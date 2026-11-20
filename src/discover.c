/*
 * PS5 Bridge - console discovery
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "discover.h"
#include "json.h"
#include "ps5.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define SCAN_BATCH 48
#define SCAN_WAIT_MS 220

static int
private_v4(unsigned int ip) {
    if ((ip & 0xff000000u) == 0x0a000000u) return 1;
    if ((ip & 0xfff00000u) == 0xac100000u) return 1;
    if ((ip & 0xffff0000u) == 0xc0a80000u) return 1;
    return 0;
}

int
discover_local_ip(char *out, size_t cap) {
    struct ifaddrs *ifa = NULL;
    struct ifaddrs *p;
    unsigned int best = 0;
    unsigned int fallback = 0;

    if (!out || cap == 0) return -1;
    out[0] = '\0';

    if (getifaddrs(&ifa) == 0) {
        for (p = ifa; p; p = p->ifa_next) {
            unsigned int ip;
            struct sockaddr_in *sin;
            if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
            if (!(p->ifa_flags & IFF_UP)) continue;
            sin = (struct sockaddr_in *)p->ifa_addr;
            ip = ntohl(sin->sin_addr.s_addr);
            if (ip == 0 || (ip >> 24) == 127) continue;
            if ((ip & 0xffff0000u) == 0xa9fe0000u) continue;
            if (private_v4(ip)) {
                best = ip;
                break;
            }
            if (!fallback) fallback = ip;
        }
        freeifaddrs(ifa);
    }
    if (!best) best = fallback;
    if (best) {
        snprintf(out, cap, "%u.%u.%u.%u",
                 (best >> 24) & 0xff, (best >> 16) & 0xff,
                 (best >> 8) & 0xff, best & 0xff);
        return 0;
    }

    /* No usable interface list: ask the routing table without sending a packet. */
    {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in dst;
        struct sockaddr_in me;
        socklen_t mlen = sizeof me;
        if (fd < 0) return -1;
        memset(&dst, 0, sizeof dst);
        dst.sin_family = AF_INET;
        dst.sin_port = htons(53);
        dst.sin_addr.s_addr = inet_addr("8.8.8.8");
        if (connect(fd, (struct sockaddr *)&dst, sizeof dst) == 0 &&
            getsockname(fd, (struct sockaddr *)&me, &mlen) == 0) {
            unsigned int ip = ntohl(me.sin_addr.s_addr);
            if (ip && (ip >> 24) != 127) {
                snprintf(out, cap, "%u.%u.%u.%u",
                         (ip >> 24) & 0xff, (ip >> 16) & 0xff,
                         (ip >> 8) & 0xff, ip & 0xff);
                close(fd);
                return 0;
            }
        }
        close(fd);
    }
    return -1;
}

static void
collect_batch(int *fds, int *hosts, int n, unsigned char *hits) {
    fd_set wf;
    struct timeval tv;
    int maxfd = -1;
    int rc;
    int i;

    if (n <= 0) return;
    FD_ZERO(&wf);
    for (i = 0; i < n; i++) {
        FD_SET(fds[i], &wf);
        if (fds[i] > maxfd) maxfd = fds[i];
    }
    tv.tv_sec = 0;
    tv.tv_usec = SCAN_WAIT_MS * 1000;
    rc = select(maxfd + 1, NULL, &wf, NULL, &tv);
    for (i = 0; i < n; i++) {
        if (rc > 0 && FD_ISSET(fds[i], &wf)) {
            int err = 0;
            socklen_t elen = sizeof err;
            if (getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &elen) == 0 && err == 0) {
                hits[hosts[i]] = 1;
            }
        }
        close(fds[i]);
    }
}

static void
scan_subnet(const char *local, int port, unsigned char *hits) {
    unsigned int a, b, c, d;
    int fds[SCAN_BATCH];
    int hosts[SCAN_BATCH];
    int n = 0;
    int h;

    if (!local || !hits) return;
    if (sscanf(local, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return;
    (void)d;
    if (a == 127 || port <= 0 || port > 65535) return;

    for (h = 1; h <= 254; h++) {
        char ip[64];
        struct sockaddr_in sa;
        int fd;
        int flags;
        int rc;

        snprintf(ip, sizeof ip, "%u.%u.%u.%u", a, b, c, (unsigned)h);
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;
        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0) flags = 0;
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)port);
        sa.sin_addr.s_addr = inet_addr(ip);
        rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
        if (rc == 0) {
            hits[h] = 1;
            close(fd);
            continue;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            continue;
        }
        fds[n] = fd;
        hosts[n] = h;
        n++;
        if (n == SCAN_BATCH) {
            collect_batch(fds, hosts, n, hits);
            n = 0;
        }
    }
    collect_batch(fds, hosts, n, hits);
}

static int
probe_console(const char *ip, int port, console_t *c) {
    ps5_cfg_t pc;
    char tok[65];
    char ver[64] = "";
    int rc;

    if (!ip || !c || port <= 0 || port > 65535) return 0;
    memset(c, 0, sizeof *c);
    snprintf(c->ip, sizeof c->ip, "%s", ip);
    snprintf(c->model, sizeof c->model, "PlayStation 5");

    memset(&pc, 0, sizeof pc);
    snprintf(pc.ip, sizeof pc.ip, "%s", ip);
    pc.port = port;

    rc = ps5_token_fetch(&pc, tok, sizeof tok, ver, sizeof ver);
    if (rc == PS5_E_LAN) {
        snprintf(c->model, sizeof c->model, "PlayStation 5 (LAN-Zugriff pr\u00fcfen)");
        c->lan = 0;
        return 1;
    }
    if (rc == PS5_E_STARTING) {
        snprintf(c->model, sizeof c->model, "PlayStation 5 (startet)");
        c->lan = 1;
        return 1;
    }
    if (rc != PS5_OK) return 0;

    c->lan = 1;
    snprintf(c->version, sizeof c->version, "%s", ver);
    {
        ps5_reply_t r;
        rc = ps5_request(&pc, "POST", "/api/status", tok, NULL, &r);
        if (rc == PS5_OK && r.status == 200 && r.body) {
            json_doc_t d;
            if (json_doc_parse(&d, r.body, r.len, 512)) {
                char model[96] = "";
                char fw[64] = "";
                if (json_get_string(&d, "model", model, sizeof model) && model[0]) {
                    snprintf(c->model, sizeof c->model, "%s", model);
                }
                if (json_get_string(&d, "firmware", fw, sizeof fw) && fw[0]) {
                    snprintf(c->firmware, sizeof c->firmware, "%s", fw);
                }
                json_doc_free(&d);
            }
        }
        ps5_reply_free(&r);
    }
    return 1;
}

int
discover_scan(const bridge_config_t *cfg, console_t *out, int max) {
    unsigned char hits[256];
    char local[64] = "";
    int port = (cfg && cfg->port) ? cfg->port : PS5B_DEFAULT_PORT;
    int n = 0;
    int h;

    if (!out || max <= 0) return 0;
    memset(hits, 0, sizeof hits);

    if (cfg && cfg->ip[0] && n < max) {
        console_t c;
        if (probe_console(cfg->ip, port, &c)) out[n++] = c;
    }

    if (n >= max) return n;
    if (discover_local_ip(local, sizeof local) != 0) return n;

    scan_subnet(local, port, hits);
    for (h = 1; h <= 254 && n < max; h++) {
        char ip[64];
        unsigned int a, b, c, d;
        console_t found;

        if (!hits[h]) continue;
        if (sscanf(local, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return n;
        (void)d;
        snprintf(ip, sizeof ip, "%u.%u.%u.%u", a, b, c, (unsigned)h);
        if (cfg && cfg->ip[0] && !strcmp(ip, cfg->ip)) continue;
        if (probe_console(ip, port, &found)) out[n++] = found;
    }
    return n;
}
