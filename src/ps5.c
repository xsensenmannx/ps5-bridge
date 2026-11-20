/*
 * PS5 Bridge - ps5upload minion client
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "ps5.h"
#include "json.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define PS5_CONNECT_MS   2000
#define PS5_IO_MS        6000
#define PS5_MAX_BODY     (4u * 1024u * 1024u)
#define PS5_TOKEN_TTL_MS (60LL * 60LL * 1000LL)

static pthread_mutex_t g_tok_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    int valid;
    char ip[64];
    int port;
    char token[65];
    char version[64];
    long long at;
} g_tok;

static const char *
find_bytes(const char *h, size_t n, const char *needle, size_t nl) {
    size_t i;
    if (!h || !needle || nl == 0 || n < nl) return NULL;
    for (i = 0; i + nl <= n; i++) {
        if (h[i] == needle[0] && !memcmp(h + i, needle, nl)) return h + i;
    }
    return NULL;
}

static int
tcp_connect(const char *ip, int port, int timeout_ms) {
    int fd;
    int flags;
    int rc;
    struct sockaddr_in a;
    struct timeval tv;

    if (!ip || !util_valid_ipv4(ip) || port <= 0 || port > 65535) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = inet_addr(ip);

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) flags = 0;
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc != 0) {
        fd_set wf;
        int err = 0;
        socklen_t elen = sizeof err;
        FD_ZERO(&wf);
        FD_SET(fd, &wf);
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(fd + 1, NULL, &wf, NULL, &tv);
        if (rc <= 0 || !FD_ISSET(fd, &wf)) {
            close(fd);
            return -1;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            close(fd);
            return -1;
        }
    }
    (void)fcntl(fd, F_SETFL, flags);

    tv.tv_sec = PS5_IO_MS / 1000;
    tv.tv_usec = (PS5_IO_MS % 1000) * 1000;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

void
ps5_reply_free(ps5_reply_t *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->len = 0;
    r->status = 0;
    r->err = PS5_OK;
}

static int
read_eof_body(int fd, const char *pre, size_t prelen, ps5_reply_t *out) {
    size_t cap = prelen + 8192;
    size_t len = prelen;
    char *buf;

    if (cap > PS5_MAX_BODY) cap = PS5_MAX_BODY + 1;
    buf = (char *)malloc(cap);
    if (!buf) {
        out->err = PS5_E_TOOBIG;
        return 0;
    }
    if (prelen) memcpy(buf, pre, prelen);
    while (1) {
        ssize_t n;
        if (len + 1 >= cap) {
            size_t ncap;
            char *p;
            if (cap > PS5_MAX_BODY) {
                out->err = PS5_E_TOOBIG;
                free(buf);
                return 0;
            }
            ncap = cap * 2;
            if (ncap > PS5_MAX_BODY + 1) ncap = PS5_MAX_BODY + 1;
            p = (char *)realloc(buf, ncap);
            if (!p) {
                out->err = PS5_E_TOOBIG;
                free(buf);
                return 0;
            }
            buf = p;
            cap = ncap;
        }
        n = recv(fd, buf + len, cap - len - 1, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return -1;
        }
        if (n == 0) break;
        len += (size_t)n;
        if (len > PS5_MAX_BODY) {
            out->err = PS5_E_TOOBIG;
            free(buf);
            return 0;
        }
    }
    buf[len] = '\0';
    out->body = buf;
    out->len = len;
    return 0;
}

static int
read_response(int fd, ps5_reply_t *out) {
    char head[16384];
    size_t got = 0;
    const char *hend = NULL;
    const char *line;
    size_t hlen;
    int status;
    long long clen = -1;
    int chunked = 0;

    while (got < sizeof head) {
        ssize_t n = recv(fd, head + got, sizeof head - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            out->err = PS5_E_UNREACHABLE;
            return 0;
        }
        if (n == 0) break;
        got += (size_t)n;
        hend = find_bytes(head, got, "\r\n\r\n", 4);
        if (hend) break;
    }
    if (!hend) {
        out->err = PS5_E_UNREACHABLE;
        return 0;
    }
    hlen = (size_t)(hend - head) + 4;

    {
        const char *sp = (const char *)memchr(head, ' ', got);
        if (!sp || sp >= hend) {
            out->err = PS5_E_UNREACHABLE;
            return 0;
        }
        status = atoi(sp + 1);
        if (status < 100 || status > 599) {
            out->err = PS5_E_UNREACHABLE;
            return 0;
        }
    }
    out->status = status;

    line = (const char *)memchr(head, '\n', got);
    if (line && line < hend) line++;
    while (line && line < hend) {
        const char *eol = find_bytes(line, (size_t)(hend - line), "\r\n", 2);
        size_t n;
        if (!eol) break;
        n = (size_t)(eol - line);
        if (n >= 15 && !strncasecmp(line, "Content-Length:", 15)) {
            clen = strtoll(line + 15, NULL, 10);
        } else if (n >= 18 && !strncasecmp(line, "Transfer-Encoding:", 18)) {
            if (find_bytes(line + 18, n - 18, "chunked", 7)) chunked = 1;
        }
        line = eol + 2;
    }

    if (status == 401) {
        out->err = PS5_E_TOKEN;
        return 0;
    }
    if (status == 403) {
        out->err = PS5_E_LAN;
        return 0;
    }
    if (chunked) {
        out->err = PS5_E_HTTP;
        return 0;
    }

    if (clen < 0) {
        if (status == 204) {
            out->body = (char *)malloc(1);
            if (out->body) out->body[0] = '\0';
            out->len = 0;
            return 0;
        }
        if (read_eof_body(fd, head + hlen, got - hlen, out) != 0) {
            out->err = PS5_E_UNREACHABLE;
        }
        return 0;
    }
    if (clen > (long long)PS5_MAX_BODY) {
        out->err = PS5_E_TOOBIG;
        return 0;
    }
    {
        size_t want = (size_t)clen;
        size_t have = got > hlen ? got - hlen : 0;
        char *body = (char *)malloc(want + 1);
        if (!body) {
            out->err = PS5_E_TOOBIG;
            return 0;
        }
        if (have > want) have = want;
        if (have) memcpy(body, head + hlen, have);
        while (have < want) {
            ssize_t n = recv(fd, body + have, want - have, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                free(body);
                out->err = PS5_E_UNREACHABLE;
                return 0;
            }
            if (n == 0) {
                free(body);
                out->err = PS5_E_UNREACHABLE;
                return 0;
            }
            have += (size_t)n;
        }
        body[want] = '\0';
        out->body = body;
        out->len = want;
    }
    return 0;
}

int
ps5_request(const ps5_cfg_t *cfg, const char *method, const char *path,
            const char *token, const char *body, ps5_reply_t *out) {
    char token_hdr[128];
    size_t blen;
    size_t need;
    char *req;
    int n;
    int fd;
    int rc;

    if (!out) return PS5_E_UNREACHABLE;
    memset(out, 0, sizeof *out);
    out->err = PS5_OK;
    if (!cfg || !method || !path || !cfg->ip[0]) {
        out->err = PS5_E_UNCONFIGURED;
        return out->err;
    }

    blen = body ? strlen(body) : 0;
    if (token && token[0]) snprintf(token_hdr, sizeof token_hdr, "X-PS5U-Token: %s\r\n", token);
    else token_hdr[0] = '\0';

    need = 1024 + strlen(path) + blen + strlen(token_hdr);
    req = (char *)malloc(need);
    if (!req) {
        out->err = PS5_E_UNREACHABLE;
        return out->err;
    }
    n = snprintf(req, need,
                 "%s %s HTTP/1.0\r\n"
                 "Host: %s:%d\r\n"
                 "User-Agent: ps5-bridge/" BRIDGE_VERSION "\r\n"
                 "Accept: */*\r\n"
                 "Connection: close\r\n"
                 "%s"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %lu\r\n"
                 "\r\n",
                 method, path, cfg->ip, cfg->port, token_hdr, (unsigned long)blen);
    if (n < 0 || (size_t)n >= need) {
        free(req);
        out->err = PS5_E_UNREACHABLE;
        return out->err;
    }
    if (blen) memcpy(req + n, body, blen);

    fd = tcp_connect(cfg->ip, cfg->port, PS5_CONNECT_MS);
    if (fd < 0) {
        free(req);
        out->err = PS5_E_UNREACHABLE;
        return out->err;
    }
    rc = util_write_all(fd, req, (size_t)n + blen);
    free(req);
    if (rc != 0) {
        close(fd);
        out->err = PS5_E_UNREACHABLE;
        return out->err;
    }
    (void)read_response(fd, out);
    close(fd);
    if (out->err == PS5_OK && out->status == 0) out->err = PS5_E_UNREACHABLE;
    return out->err;
}

static int
token_shape_ok(const char *t) {
    size_t i;
    if (!t || strlen(t) != 64) return 0;
    for (i = 0; i < 64; i++) {
        char c = t[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0;
    }
    return 1;
}

int
ps5_token_fetch(const ps5_cfg_t *cfg, char *token, size_t tcap,
                char *version, size_t vcap) {
    ps5_reply_t r;
    json_doc_t d;
    char tok[80] = "";
    char ver[96] = "";
    int rc;

    if (token && tcap) token[0] = '\0';
    if (version && vcap) version[0] = '\0';
    if (!cfg || !cfg->ip[0]) return PS5_E_UNCONFIGURED;

    rc = ps5_request(cfg, "GET", "/api/token", NULL, NULL, &r);
    if (rc != PS5_OK) {
        ps5_reply_free(&r);
        return rc;
    }
    if (r.status == 503) {
        ps5_reply_free(&r);
        return PS5_E_STARTING;
    }
    if (r.status != 200 || !r.body) {
        ps5_reply_free(&r);
        return PS5_E_HTTP;
    }
    if (!json_doc_parse(&d, r.body, r.len, 64)) {
        ps5_reply_free(&r);
        return PS5_E_PARSE;
    }
    if (!json_get_string(&d, "token", tok, sizeof tok) || !token_shape_ok(tok)) {
        json_doc_free(&d);
        ps5_reply_free(&r);
        return PS5_E_PARSE;
    }
    (void)json_get_string(&d, "version", ver, sizeof ver);
    if (token && tcap) snprintf(token, tcap, "%s", tok);
    if (version && vcap) snprintf(version, vcap, "%s", ver);
    json_doc_free(&d);
    ps5_reply_free(&r);
    return PS5_OK;
}

static int
token_get(const ps5_cfg_t *cfg, char *token, size_t cap) {
    long long now;
    int rc;

    if (!cfg || !cfg->ip[0]) return PS5_E_UNCONFIGURED;
    pthread_mutex_lock(&g_tok_mu);
    now = util_mono_ms();
    if (g_tok.valid && !strcmp(g_tok.ip, cfg->ip) && g_tok.port == cfg->port &&
        now - g_tok.at < PS5_TOKEN_TTL_MS) {
        snprintf(token, cap, "%s", g_tok.token);
        pthread_mutex_unlock(&g_tok_mu);
        return PS5_OK;
    }
    g_tok.valid = 0;
    rc = ps5_token_fetch(cfg, g_tok.token, sizeof g_tok.token, g_tok.version, sizeof g_tok.version);
    if (rc == PS5_OK) {
        snprintf(g_tok.ip, sizeof g_tok.ip, "%s", cfg->ip);
        g_tok.port = cfg->port;
        g_tok.at = now;
        g_tok.valid = 1;
        snprintf(token, cap, "%s", g_tok.token);
    } else {
        g_tok.token[0] = '\0';
    }
    pthread_mutex_unlock(&g_tok_mu);
    return rc;
}

void
ps5_token_reset(void) {
    pthread_mutex_lock(&g_tok_mu);
    g_tok.valid = 0;
    g_tok.token[0] = '\0';
    g_tok.ip[0] = '\0';
    pthread_mutex_unlock(&g_tok_mu);
}

int
ps5_api(const ps5_cfg_t *cfg, const char *path, const char *body, ps5_reply_t *out) {
    char token[65];
    int rc;

    if (!out) return PS5_E_UNREACHABLE;
    memset(out, 0, sizeof *out);
    if (!cfg || !cfg->ip[0]) {
        out->err = PS5_E_UNCONFIGURED;
        return out->err;
    }
    rc = token_get(cfg, token, sizeof token);
    if (rc != PS5_OK) {
        out->err = rc;
        return rc;
    }
    rc = ps5_request(cfg, "POST", path, token, body, out);
    if (rc == PS5_E_TOKEN) {
        ps5_reply_free(out);
        ps5_token_reset();
        rc = token_get(cfg, token, sizeof token);
        if (rc != PS5_OK) {
            out->err = rc;
            return rc;
        }
        rc = ps5_request(cfg, "POST", path, token, body, out);
    }
    return rc;
}

const char *
ps5_error_name(int err, int status) {
    switch (err) {
    case PS5_OK: break;
    case PS5_E_UNCONFIGURED: return "unconfigured";
    case PS5_E_UNREACHABLE: return "unreachable";
    case PS5_E_LAN: return "lan_off";
    case PS5_E_TOKEN: return "token";
    case PS5_E_STARTING: return "starting";
    case PS5_E_TOOBIG: return "too_big";
    case PS5_E_PARSE: return "bad_reply";
    case PS5_E_HTTP: return "ps5_error";
    default: return "ps5_error";
    }
    if (status == 401) return "token";
    if (status == 403) return "lan_off";
    if (status == 503) return "starting";
    return "ps5_error";
}
