/*
 * PS5 Bridge - shared helpers
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#ifndef PS5B_UTIL_H
#define PS5B_UTIL_H

#include <stddef.h>

#ifndef BRIDGE_VERSION
#define BRIDGE_VERSION "1.0.0"
#endif

#define BRIDGE_PORT 9181
#define BRIDGE_NAME "PS5 Bridge"

int util_strieq(const char *a, const char *b);
int util_valid_ipv4(const char *s);
int util_valid_title_id(const char *s);
void util_uppercase(char *s);
int util_write_all(int fd, const void *buf, size_t len);
long long util_mono_ms(void);

/* A small growable string used for JSON replies. */
typedef struct {
    char *p;
    size_t len;
    size_t cap;
} strbuf_t;

void sb_init(strbuf_t *b);
void sb_free(strbuf_t *b);
int sb_reserve(strbuf_t *b, size_t extra);
int sb_putc(strbuf_t *b, char c);
int sb_puts(strbuf_t *b, const char *s);
int sb_putn(strbuf_t *b, const char *s, size_t n);
int sb_printf(strbuf_t *b, const char *fmt, ...);
int sb_json_string(strbuf_t *b, const char *s);

#endif /* PS5B_UTIL_H */
