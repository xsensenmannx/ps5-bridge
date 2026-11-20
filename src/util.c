/*
 * PS5 Bridge - shared helpers
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

int
util_strieq(const char *a, const char *b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int
util_valid_ipv4(const char *s) {
    int part = 0;
    int digits = 0;
    int value = 0;

    if (!s || !*s) return 0;
    for (;;) {
        unsigned char c = (unsigned char)*s++;
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            if (++digits > 3 || value > 255) return 0;
        } else if (c == '.' || c == '\0') {
            if (digits == 0) return 0;
            part++;
            if (c == '\0') break;
            if (part >= 4) return 0;
            digits = 0;
            value = 0;
        } else {
            return 0;
        }
    }
    return part == 4;
}

int
util_valid_title_id(const char *s) {
    size_t i;
    if (!s || strlen(s) != 9) return 0;
    for (i = 0; i < 4; i++) {
        if (s[i] < 'A' || s[i] > 'Z') return 0;
    }
    for (i = 4; i < 9; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    if (!strncmp(s, "NPXS", 4)) return 0;
    if (!strcmp(s, "PSUP00001")) return 0;
    return 1;
}

void
util_uppercase(char *s) {
    if (!s) return;
    while (*s) {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

int
util_write_all(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

long long
util_mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

void
sb_init(strbuf_t *b) {
    if (!b) return;
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

void
sb_free(strbuf_t *b) {
    if (!b) return;
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

int
sb_reserve(strbuf_t *b, size_t extra) {
    size_t want;
    size_t cap;
    char *p;

    if (!b) return -1;
    if (extra > (size_t)-1 - b->len - 1) return -1;
    want = b->len + extra + 1;
    if (want <= b->cap) return 0;
    cap = b->cap ? b->cap : 256;
    while (cap < want) {
        if (cap > ((size_t)-1) / 2) {
            cap = want;
            break;
        }
        cap *= 2;
    }
    p = (char *)realloc(b->p, cap);
    if (!p) return -1;
    b->p = p;
    b->cap = cap;
    if (b->len == 0) b->p[0] = '\0';
    return 0;
}

int
sb_putc(strbuf_t *b, char c) {
    if (sb_reserve(b, 1) != 0) return -1;
    b->p[b->len++] = c;
    b->p[b->len] = '\0';
    return 0;
}

int
sb_putn(strbuf_t *b, const char *s, size_t n) {
    if (n == 0) return 0;
    if (!s) return -1;
    if (sb_reserve(b, n) != 0) return -1;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
    return 0;
}

int
sb_puts(strbuf_t *b, const char *s) {
    return s ? sb_putn(b, s, strlen(s)) : 0;
}

int
sb_printf(strbuf_t *b, const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int n;
    size_t avail;

    if (!b || !fmt) return -1;
    if (sb_reserve(b, 128) != 0) return -1;
    avail = b->cap - b->len;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    n = vsnprintf(b->p + b->len, avail, fmt, ap2);
    va_end(ap2);
    if (n < 0) {
        va_end(ap);
        return -1;
    }
    if ((size_t)n >= avail) {
        if (sb_reserve(b, (size_t)n) != 0) {
            va_end(ap);
            return -1;
        }
        n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
        if (n < 0) {
            va_end(ap);
            return -1;
        }
    }
    va_end(ap);
    b->len += (size_t)n;
    return 0;
}

int
sb_json_string(strbuf_t *b, const char *s) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *p;

    if (sb_putc(b, '"') != 0) return -1;
    if (!s) s = "";
    for (p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        switch (c) {
        case '"':  if (sb_putn(b, "\\\"", 2) != 0) return -1; break;
        case '\\': if (sb_putn(b, "\\\\", 2) != 0) return -1; break;
        case '\b': if (sb_putn(b, "\\b", 2) != 0) return -1; break;
        case '\f': if (sb_putn(b, "\\f", 2) != 0) return -1; break;
        case '\n': if (sb_putn(b, "\\n", 2) != 0) return -1; break;
        case '\r': if (sb_putn(b, "\\r", 2) != 0) return -1; break;
        case '\t': if (sb_putn(b, "\\t", 2) != 0) return -1; break;
        default:
            if (c < 0x20) {
                char esc[6] = { '\\', 'u', '0', '0', 0, 0 };
                esc[4] = hex[(c >> 4) & 0xf];
                esc[5] = hex[c & 0xf];
                if (sb_putn(b, esc, 6) != 0) return -1;
            } else {
                if (sb_putc(b, (char)c) != 0) return -1;
            }
            break;
        }
    }
    return sb_putc(b, '"');
}
