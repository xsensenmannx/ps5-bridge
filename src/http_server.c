/*
 * PS5 Bridge - HTTP server
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "http_server.h"

#include "config.h"
#include "discover.h"
#include "json.h"
#include "ps5.h"
#include "util.h"
#include "webui_assets.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HDR_CAP       16384
#define BODY_CAP      65536
#define MAX_CONNS     32
#define CONN_STACK    (256u * 1024u)

static bridge_config_t g_cfg;
static pthread_mutex_t g_cfg_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_conn_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_conns = 0;
static unsigned short g_port = BRIDGE_PORT;

typedef struct {
    char method[8];
    char path[512];
    char query[1024];
    char host[256];
    size_t content_length;
} request_t;

static const char *
find_bytes(const char *h, size_t n, const char *needle, size_t nl) {
    size_t i;
    if (!h || !needle || nl == 0 || n < nl) return NULL;
    for (i = 0; i + nl <= n; i++) {
        if (h[i] == needle[0] && !memcmp(h + i, needle, nl)) return h + i;
    }
    return NULL;
}

static const char *
reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    default: return "Error";
    }
}

static void
send_response(int fd, int status, const char *ctype, const void *body, size_t len,
              const char *extra) {
    char head[1024];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %lu\r\n"
                     "Connection: close\r\n"
                     "X-Content-Type-Options: nosniff\r\n"
                     "X-Frame-Options: DENY\r\n"
                     "%s"
                     "\r\n",
                     status, reason_phrase(status), ctype ? ctype : "application/octet-stream",
                     (unsigned long)len, extra ? extra : "");
    if (n < 0 || (size_t)n >= sizeof head) return;
    if (util_write_all(fd, head, (size_t)n) != 0) return;
    if (body && len) (void)util_write_all(fd, body, len);
}

static void
send_json(int fd, int status, const char *body) {
    send_response(fd, status, "application/json; charset=utf-8",
                  body ? body : "{}", body ? strlen(body) : 2,
                  "Cache-Control: no-store\r\n");
}

static void
send_error(int fd, int status, const char *error) {
    char b[160];
    snprintf(b, sizeof b, "{\"error\":\"%s\"}", error ? error : "error");
    send_json(fd, status, b);
}

static void
copy_cfg(bridge_config_t *out) {
    pthread_mutex_lock(&g_cfg_mu);
    *out = g_cfg;
    pthread_mutex_unlock(&g_cfg_mu);
}

static void
set_cfg(const bridge_config_t *in) {
    pthread_mutex_lock(&g_cfg_mu);
    g_cfg = *in;
    pthread_mutex_unlock(&g_cfg_mu);
    ps5_token_reset();
}

void
http_init(void) {
    config_defaults(&g_cfg);
    (void)config_load(&g_cfg);
}

static int
host_ok(const char *host) {
    char name[256];
    char *colon;
    char *p;

    if (!host || !host[0]) return 1;
    snprintf(name, sizeof name, "%s", host);
    colon = strrchr(name, ':');
    if (colon && colon != name) {
        int digits = 1;
        for (p = colon + 1; *p; p++) {
            if (!isdigit((unsigned char)*p)) {
                digits = 0;
                break;
            }
        }
        if (digits) *colon = '\0';
    }
    if (!strcmp(name, "[::1]") || !strcmp(name, "::1")) return 1;
    if (util_strieq(name, "localhost") || !strcmp(name, "127.0.0.1")) return 1;
    return util_valid_ipv4(name);
}

static void
url_decode(char *s) {
    char *r = s;
    char *w = s;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            int hi = isdigit((unsigned char)r[1]) ? r[1] - '0' : (tolower((unsigned char)r[1]) - 'a' + 10);
            int lo = isdigit((unsigned char)r[2]) ? r[2] - '0' : (tolower((unsigned char)r[2]) - 'a' + 10);
            *w++ = (char)((hi << 4) | lo);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static int
query_get(const char *query, const char *key, char *out, size_t cap) {
    size_t klen;
    const char *p;

    if (!query || !key || !out || cap == 0) return 0;
    out[0] = '\0';
    klen = strlen(key);
    p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            size_t vlen = seg - klen - 1;
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, p + klen + 1, vlen);
            out[vlen] = '\0';
            url_decode(out);
            return 1;
        }
        p = amp ? amp + 1 : NULL;
    }
    return 0;
}

static int
read_request(int fd, request_t *r, char **body_out) {
    char *hdr;
    size_t got = 0;
    const char *hend = NULL;
    const char *eol;
    const char *sp1;
    const char *sp2;
    char target[1600];
    size_t hlen;
    size_t tlen;
    const char *line;
    int rc = 0;

    *body_out = NULL;
    memset(r, 0, sizeof *r);
    hdr = (char *)malloc(HDR_CAP + 1);
    if (!hdr) return 500;

    while (got < HDR_CAP) {
        ssize_t n = recv(fd, hdr + got, HDR_CAP - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(hdr);
            return 400;
        }
        if (n == 0) break;
        got += (size_t)n;
        hend = find_bytes(hdr, got, "\r\n\r\n", 4);
        if (hend) break;
    }
    if (!hend) {
        free(hdr);
        return 400;
    }
    hdr[got] = '\0';
    hlen = (size_t)(hend - hdr) + 4;

    eol = find_bytes(hdr, (size_t)(hend - hdr), "\r\n", 2);
    if (!eol) {
        free(hdr);
        return 400;
    }
    sp1 = (const char *)memchr(hdr, ' ', (size_t)(eol - hdr));
    sp2 = sp1 ? (const char *)memchr(sp1 + 1, ' ', (size_t)(eol - (sp1 + 1))) : NULL;
    if (!sp1 || !sp2 || sp1 == hdr) {
        free(hdr);
        return 400;
    }
    if ((size_t)(sp1 - hdr) >= sizeof r->method) {
        free(hdr);
        return 400;
    }
    memcpy(r->method, hdr, (size_t)(sp1 - hdr));
    r->method[sp1 - hdr] = '\0';

    tlen = (size_t)(sp2 - sp1 - 1);
    if (tlen == 0 || tlen >= sizeof target) {
        free(hdr);
        return 400;
    }
    memcpy(target, sp1 + 1, tlen);
    target[tlen] = '\0';
    if (target[0] != '/') {
        free(hdr);
        return 400;
    }
    {
        char *q = strchr(target, '?');
        if (q) {
            *q = '\0';
            snprintf(r->query, sizeof r->query, "%s", q + 1);
        }
    }
    if (strlen(target) >= sizeof r->path) {
        free(hdr);
        return 400;
    }
    snprintf(r->path, sizeof r->path, "%s", target);

    line = eol + 2;
    while (line < hend) {
        const char *le = find_bytes(line, (size_t)(hend - line), "\r\n", 2);
        size_t ll;
        if (!le) break;
        ll = (size_t)(le - line);
        if (ll == 0) break;
        if (ll >= 5 && !strncasecmp(line, "Host:", 5)) {
            const char *v = line + 5;
            const char *ve = le;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
            if ((size_t)(ve - v) < sizeof r->host) {
                memcpy(r->host, v, (size_t)(ve - v));
                r->host[ve - v] = '\0';
            }
        } else if (ll >= 15 && !strncasecmp(line, "Content-Length:", 15)) {
            char num[32];
            const char *v = line + 15;
            const char *ve = le;
            size_t nl;
            char *endp = NULL;
            long long cl;
            while (v < ve && (*v == ' ' || *v == '\t')) v++;
            nl = (size_t)(ve - v);
            if (nl == 0 || nl >= sizeof num) {
                free(hdr);
                return 400;
            }
            memcpy(num, v, nl);
            num[nl] = '\0';
            cl = strtoll(num, &endp, 10);
            if (!endp || *endp != '\0' || cl < 0) {
                free(hdr);
                return 400;
            }
            r->content_length = (size_t)cl;
        }
        line = le + 2;
    }

    if (r->content_length > BODY_CAP) {
        free(hdr);
        return 413;
    }
    {
        char *body = (char *)malloc(r->content_length + 1);
        size_t have;
        if (!body) {
            free(hdr);
            return 500;
        }
        have = got > hlen ? got - hlen : 0;
        if (have > r->content_length) have = r->content_length;
        if (have) memcpy(body, hdr + hlen, have);
        while (have < r->content_length) {
            ssize_t n = recv(fd, body + have, r->content_length - have, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                free(body);
                free(hdr);
                return 400;
            }
            if (n == 0) {
                free(body);
                free(hdr);
                return 400;
            }
            have += (size_t)n;
        }
        body[r->content_length] = '\0';
        *body_out = body;
    }
    free(hdr);
    (void)rc;
    return 0;
}

static void
handle_state(int fd) {
    bridge_config_t cfg;
    ps5_cfg_t pc;
    ps5_reply_t r;
    strbuf_t b;

    copy_cfg(&cfg);
    sb_init(&b);
    sb_printf(&b, "{\"bridge\":{\"version\":\"%s\",\"port\":%u},", BRIDGE_VERSION,
              (unsigned)g_port);
    sb_puts(&b, "\"config\":{\"ip\":");
    sb_json_string(&b, cfg.ip);
    sb_printf(&b, ",\"port\":%d},\"ps5\":", cfg.port);

    if (!cfg.ip[0]) {
        sb_puts(&b, "{\"online\":false,\"error\":\"unconfigured\"}}");
        send_json(fd, 200, b.p ? b.p : "{}");
        sb_free(&b);
        return;
    }

    memset(&pc, 0, sizeof pc);
    snprintf(pc.ip, sizeof pc.ip, "%s", cfg.ip);
    pc.port = cfg.port;

    memset(&r, 0, sizeof r);
    {
        int rc = ps5_api(&pc, "/api/status", NULL, &r);
        if (rc != PS5_OK || r.status != 200 || !r.body) {
            sb_printf(&b, "{\"online\":false,\"error\":\"%s\"}}",
                      ps5_error_name(rc, r.status));
            ps5_reply_free(&r);
            send_json(fd, 200, b.p ? b.p : "{}");
            sb_free(&b);
            return;
        }
    }

    {
        json_doc_t d;
        char version[96] = "";
        char firmware[64] = "";
        char model[128] = "";
        char title[32] = "";
        long temp = 0;
        long app_id = 0;
        int have_temp = 0;
        int have_run = 0;

        if (!json_doc_parse(&d, r.body, r.len, 512)) {
            sb_puts(&b, "{\"online\":false,\"error\":\"bad_reply\"}}");
            ps5_reply_free(&r);
            send_json(fd, 200, b.p ? b.p : "{}");
            sb_free(&b);
            return;
        }
        (void)json_get_string(&d, "version", version, sizeof version);
        (void)json_get_string(&d, "firmware", firmware, sizeof firmware);
        (void)json_get_string(&d, "model", model, sizeof model);
        have_temp = json_get_long(&d, "temp_c", &temp) == 1;
        have_run = json_get_string(&d, "title_id", title, sizeof title) == 1 && title[0];
        (void)json_get_long(&d, "app_id", &app_id);

        sb_puts(&b, "{\"online\":true,\"version\":");
        sb_json_string(&b, version[0] ? version : NULL);
        sb_puts(&b, ",\"firmware\":");
        sb_json_string(&b, firmware[0] ? firmware : NULL);
        sb_puts(&b, ",\"model\":");
        sb_json_string(&b, model[0] ? model : NULL);
        if (have_temp) sb_printf(&b, ",\"temp_c\":%ld", temp);
        else sb_puts(&b, ",\"temp_c\":null");
        if (have_run) {
            sb_puts(&b, ",\"running\":{\"title_id\":");
            sb_json_string(&b, title);
            sb_printf(&b, ",\"app_id\":%ld,\"name\":null}", app_id);
        } else {
            sb_puts(&b, ",\"running\":null");
        }
        sb_puts(&b, ",\"error\":null}}");
        json_doc_free(&d);
    }

    ps5_reply_free(&r);
    send_json(fd, 200, b.p ? b.p : "{}");
    sb_free(&b);
}

static const char *
platform_of(const char *id) {
    if (!strncmp(id, "PPSA", 4) || !strncmp(id, "PPSX", 4)) return "ps5";
    if (!strncmp(id, "CUSA", 4)) return "ps4";
    return "other";
}

static int
cmp_games(const void *a, const void *b) {
    const game_entry_t *x = (const game_entry_t *)a;
    const game_entry_t *y = (const game_entry_t *)b;
    int n = strcasecmp(x->name, y->name);
    if (n) return n;
    return strcmp(x->id, y->id);
}

static void
handle_games(int fd) {
    bridge_config_t cfg;
    ps5_cfg_t pc;
    game_list_t list;
    long offset = 0;
    int page;

    copy_cfg(&cfg);
    if (!cfg.ip[0]) {
        send_error(fd, 409, "unconfigured");
        return;
    }
    memset(&pc, 0, sizeof pc);
    snprintf(pc.ip, sizeof pc.ip, "%s", cfg.ip);
    pc.port = cfg.port;
    memset(&list, 0, sizeof list);

    for (page = 0; page < 20; page++) {
        char req[96];
        ps5_reply_t r;
        json_doc_t d;
        int more = 0;
        long next = 0;
        char perr[128] = "";
        int rc;
        int ok;

        snprintf(req, sizeof req, "{\"offset\":%ld,\"limit\":500}", offset);
        memset(&r, 0, sizeof r);
        rc = ps5_api(&pc, "/api/apps/db", req, &r);
        if (rc != PS5_OK || r.status != 200 || !r.body) {
            const char *e = ps5_error_name(rc, r.status);
            fprintf(stderr, "ps5-bridge: games: %s (status %d)\n", e, r.status);
            ps5_reply_free(&r);
            game_list_free(&list);
            send_error(fd, 502, e);
            return;
        }
        if (!json_doc_parse(&d, r.body, r.len, 16384)) {
            ps5_reply_free(&r);
            game_list_free(&list);
            send_error(fd, 502, "bad_reply");
            return;
        }
        ok = json_parse_apps(&d, &list, &more, &next, perr, sizeof perr);
        json_doc_free(&d);
        ps5_reply_free(&r);
        if (ok <= 0) {
            fprintf(stderr, "ps5-bridge: apps/db: %s\n", perr[0] ? perr : "bad reply");
            game_list_free(&list);
            send_error(fd, 502, "ps5_error");
            return;
        }
        if (!more || next <= offset) break;
        offset = next;
    }

    if (list.count > 1) qsort(list.items, list.count, sizeof list.items[0], cmp_games);    {
        strbuf_t b;
        size_t i;
        sb_init(&b);
        sb_puts(&b, "{\"games\":[");
        for (i = 0; i < list.count; i++) {
            if (i) sb_putc(&b, ',');
            sb_puts(&b, "{\"id\":");
            sb_json_string(&b, list.items[i].id);
            sb_puts(&b, ",\"name\":");
            sb_json_string(&b, list.items[i].name);
            sb_printf(&b, ",\"platform\":\"%s\"}", platform_of(list.items[i].id));
        }
        sb_puts(&b, "]}");
        send_json(fd, 200, b.p ? b.p : "{\"games\":[]}");
        sb_free(&b);
    }
    game_list_free(&list);
}

static int
parse_title_id(const char *body, size_t len, char *out, size_t cap) {
    json_doc_t d;
    if (!body || len == 0 || !json_doc_parse(&d, body, len, 64)) return 0;
    if (!json_get_string(&d, "title_id", out, cap)) {
        json_doc_free(&d);
        return 0;
    }
    json_doc_free(&d);
    util_uppercase(out);
    return util_valid_title_id(out);
}

static void
handle_launch(int fd, const char *body, size_t len) {
    bridge_config_t cfg;
    ps5_cfg_t pc;
    char tid[32];
    char req[64];
    ps5_reply_t r;
    int rc;

    if (!parse_title_id(body, len, tid, sizeof tid)) {
        send_error(fd, 400, "bad_title_id");
        return;
    }
    copy_cfg(&cfg);
    if (!cfg.ip[0]) {
        send_error(fd, 409, "unconfigured");
        return;
    }
    memset(&pc, 0, sizeof pc);
    snprintf(pc.ip, sizeof pc.ip, "%s", cfg.ip);
    pc.port = cfg.port;
    snprintf(req, sizeof req, "{\"title_id\":\"%s\"}", tid);

    fprintf(stderr, "ps5-bridge: launch %s\n", tid);
    memset(&r, 0, sizeof r);
    rc = ps5_api(&pc, "/api/apps/launch", req, &r);
    if (rc == PS5_OK && r.status == 200) {
        send_json(fd, 200, "{\"ok\":true}");
    } else if (rc == PS5_OK && r.status == 409) {
        send_error(fd, 409, "other_game_running");
    } else {
        const char *e = ps5_error_name(rc, r.status);
        fprintf(stderr, "ps5-bridge: launch failed: %s (status %d)\n", e, r.status);
        send_error(fd, 502, e);
    }
    ps5_reply_free(&r);
}

static void
handle_close(int fd, const char *body, size_t len) {
    bridge_config_t cfg;
    ps5_cfg_t pc;
    json_doc_t d;
    long app_id = 0;
    char req[64];
    ps5_reply_t r;
    int rc;

    if (!body || len == 0 || !json_doc_parse(&d, body, len, 64)) {
        send_error(fd, 400, "bad_json");
        return;
    }
    if (!json_get_long(&d, "app_id", &app_id) || app_id <= 0 || app_id > 0xffffffffL) {
        json_doc_free(&d);
        send_error(fd, 400, "bad_app_id");
        return;
    }
    json_doc_free(&d);

    copy_cfg(&cfg);
    if (!cfg.ip[0]) {
        send_error(fd, 409, "unconfigured");
        return;
    }
    memset(&pc, 0, sizeof pc);
    snprintf(pc.ip, sizeof pc.ip, "%s", cfg.ip);
    pc.port = cfg.port;
    snprintf(req, sizeof req, "{\"app_id\":%ld}", app_id);

    memset(&r, 0, sizeof r);
    rc = ps5_api(&pc, "/api/app/close", req, &r);
    if (rc == PS5_OK && r.status == 200) send_json(fd, 200, "{\"ok\":true}");
    else send_error(fd, 502, ps5_error_name(rc, r.status));
    ps5_reply_free(&r);
}

static void
handle_config(int fd, const char *body, size_t len) {
    json_doc_t d;
    char ip[64] = "";
    long port = PS5B_DEFAULT_PORT;
    bridge_config_t next;
    int saved;

    if (!body || len == 0 || !json_doc_parse(&d, body, len, 64)) {
        send_error(fd, 400, "bad_json");
        return;
    }
    if (!json_get_string(&d, "ip", ip, sizeof ip)) {
        json_doc_free(&d);
        send_error(fd, 400, "bad_ip");
        return;
    }
    (void)json_get_long(&d, "port", &port);
    json_doc_free(&d);

    {
        char *s = ip;
        char *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = '\0';
        if (s != ip) memmove(ip, s, strlen(s) + 1);
    }
    if (ip[0] && !util_valid_ipv4(ip)) {
        send_error(fd, 400, "bad_ip");
        return;
    }
    if (port < 1 || port > 65535) {
        send_error(fd, 400, "bad_port");
        return;
    }

    config_defaults(&next);
    if (ip[0]) snprintf(next.ip, sizeof next.ip, "%s", ip);
    next.port = (int)port;
    saved = config_save(&next);
    set_cfg(&next);
    fprintf(stderr, "ps5-bridge: settings -> %s:%d%s\n",
            next.ip[0] ? next.ip : "(none)", next.port, saved == 0 ? "" : " (not persisted)");
    send_json(fd, 200, "{\"ok\":true}");
}

static void
handle_discover(int fd) {
    bridge_config_t cfg;
    console_t list[16];
    int n;
    int i;
    strbuf_t b;

    copy_cfg(&cfg);
    n = discover_scan(&cfg, list, (int)(sizeof list / sizeof list[0]));
    sb_init(&b);
    sb_puts(&b, "{\"consoles\":[");
    for (i = 0; i < n; i++) {
        if (i) sb_putc(&b, ',');
        sb_puts(&b, "{\"ip\":");
        sb_json_string(&b, list[i].ip);
        sb_puts(&b, ",\"model\":");
        sb_json_string(&b, list[i].model);
        sb_puts(&b, ",\"firmware\":");
        sb_json_string(&b, list[i].firmware);
        sb_puts(&b, ",\"version\":");
        sb_json_string(&b, list[i].version);
        sb_printf(&b, ",\"lan\":%s}", list[i].lan ? "true" : "false");
    }
    sb_puts(&b, "]}");
    send_json(fd, 200, b.p ? b.p : "{\"consoles\":[]}");
    sb_free(&b);
}

static int
image_signature(const char *body, size_t len, const char **ctype) {
    static const unsigned char png[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    if (len >= 8 && !memcmp(body, png, 8)) {
        *ctype = "image/png";
        return 1;
    }
    if (len >= 3 && (unsigned char)body[0] == 0xff && (unsigned char)body[1] == 0xd8 &&
        (unsigned char)body[2] == 0xff) {
        *ctype = "image/jpeg";
        return 1;
    }
    return 0;
}

static void
handle_icon(int fd, const char *query) {
    bridge_config_t cfg;
    ps5_cfg_t pc;
    char id[64];
    char p1[160];
    char p2[160];
    const char *paths[2];
    int i;

    if (!query_get(query, "title_id", id, sizeof id)) {
        send_error(fd, 400, "bad_title_id");
        return;
    }
    util_uppercase(id);
    if (!util_valid_title_id(id)) {
        send_error(fd, 400, "bad_title_id");
        return;
    }
    copy_cfg(&cfg);
    if (!cfg.ip[0]) {
        send_error(fd, 404, "no_console");
        return;
    }
    memset(&pc, 0, sizeof pc);
    snprintf(pc.ip, sizeof pc.ip, "%s", cfg.ip);
    pc.port = cfg.port;

    snprintf(p1, sizeof p1, "/user/appmeta/%s/icon0.png", id);
    snprintf(p2, sizeof p2, "/user/app/%s/sce_sys/icon0.png", id);
    paths[0] = p1;
    paths[1] = p2;

    for (i = 0; i < 2; i++) {
        char req[256];
        ps5_reply_t r;
        int rc;
        snprintf(req, sizeof req, "{\"path\":\"%s\"}", paths[i]);
        memset(&r, 0, sizeof r);
        rc = ps5_api(&pc, "/api/fs/read", req, &r);
        if (rc == PS5_OK && r.status == 200 && r.body) {
            const char *ctype = NULL;
            if (image_signature(r.body, r.len, &ctype)) {
                send_response(fd, 200, ctype, r.body, r.len,
                              "Cache-Control: public, max-age=86400\r\n");
                ps5_reply_free(&r);
                return;
            }
        }
        ps5_reply_free(&r);
    }
    send_error(fd, 404, "no_icon");
}

static void
handle_conn(int fd) {
    request_t r;
    char *body = NULL;
    int rc;

    {
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        tv.tv_sec = 30;
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }

    rc = read_request(fd, &r, &body);
    if (rc != 0) {
        send_error(fd, rc, rc == 413 ? "too_large" : "bad_request");
        free(body);
        return;
    }
    if (!host_ok(r.host)) {
        send_error(fd, 403, "forbidden");
        free(body);
        return;
    }

    if (!strcmp(r.method, "GET")) {
        if (!strcmp(r.path, "/") || !strcmp(r.path, "/index.html")) {
            send_response(fd, 200, "text/html; charset=utf-8", k_webui_html, k_webui_html_len,
                          "Cache-Control: no-cache\r\n"
                          "Content-Security-Policy: default-src 'self' 'unsafe-inline' data: blob:; "
                          "frame-ancestors 'none'\r\n");
        } else if (!strcmp(r.path, "/favicon.ico") || !strcmp(r.path, "/icon.png")) {
            if (k_webui_icon_len) {
                send_response(fd, 200, "image/png", k_webui_icon, k_webui_icon_len,
                              "Cache-Control: public, max-age=86400\r\n");
            } else {
                send_response(fd, 204, "image/png", NULL, 0, NULL);
            }
        } else if (!strcmp(r.path, "/api/state")) {
            handle_state(fd);
        } else if (!strcmp(r.path, "/api/games")) {
            handle_games(fd);
        } else if (!strcmp(r.path, "/api/icon")) {
            handle_icon(fd, r.query);
        } else {
            send_error(fd, 404, "not_found");
        }
    } else if (!strcmp(r.method, "POST")) {
        if (!strcmp(r.path, "/api/launch")) {
            handle_launch(fd, body, r.content_length);
        } else if (!strcmp(r.path, "/api/close")) {
            handle_close(fd, body, r.content_length);
        } else if (!strcmp(r.path, "/api/config")) {
            handle_config(fd, body, r.content_length);
        } else if (!strcmp(r.path, "/api/discover")) {
            handle_discover(fd);
        } else {
            send_error(fd, 404, "not_found");
        }
    } else {
        send_error(fd, 405, "method_not_allowed");
    }
    free(body);
}

static void
conn_release(void) {
    pthread_mutex_lock(&g_conn_mu);
    if (g_conns > 0) g_conns--;
    pthread_mutex_unlock(&g_conn_mu);
}

static void *
conn_thread(void *arg) {
    int fd = (int)(intptr_t)arg;
    handle_conn(fd);
    close(fd);
    conn_release();
    return NULL;
}

int
http_serve(unsigned short port) {
    int lfd;
    int one = 1;
    struct sockaddr_in a;

    g_port = port;
    lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) return -1;
    (void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) != 0 || listen(lfd, 64) != 0) {
        close(lfd);
        return -1;
    }

    fprintf(stderr, "ps5-bridge: listening on 0.0.0.0:%u\n", (unsigned)port);
    for (;;) {
        int fd = accept(lfd, NULL, NULL);
        pthread_attr_t attr;
        pthread_t th;
        int allow = 0;

        if (fd < 0) {
            if (errno == EINTR) continue;
            close(lfd);
            return -1;
        }
        pthread_mutex_lock(&g_conn_mu);
        if (g_conns < MAX_CONNS) {
            g_conns++;
            allow = 1;
        }
        pthread_mutex_unlock(&g_conn_mu);
        if (!allow) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "Content-Length: 22\r\n"
                "Connection: close\r\n\r\n"
                "{\"error\":\"too_many\"}\n";
            (void)util_write_all(fd, busy, sizeof busy - 1);
            close(fd);
            continue;
        }

        (void)pthread_attr_init(&attr);
        (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        (void)pthread_attr_setstacksize(&attr, CONN_STACK);
        if (pthread_create(&th, &attr, conn_thread, (void *)(intptr_t)fd) != 0) {
            pthread_attr_destroy(&attr);
            close(fd);
            conn_release();
            continue;
        }
        pthread_attr_destroy(&attr);
    }
}
