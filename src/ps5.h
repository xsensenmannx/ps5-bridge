/*
 * PS5 Bridge - ps5upload minion client
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#ifndef PS5B_PS5_H
#define PS5B_PS5_H

#include <stddef.h>

typedef struct {
    char ip[64];
    int port;
} ps5_cfg_t;

enum {
    PS5_OK = 0,
    PS5_E_UNCONFIGURED,
    PS5_E_UNREACHABLE,
    PS5_E_LAN,
    PS5_E_TOKEN,
    PS5_E_STARTING,
    PS5_E_HTTP,
    PS5_E_TOOBIG,
    PS5_E_PARSE
};

typedef struct {
    int status;
    char *body;
    size_t len;
    int err;
} ps5_reply_t;

void ps5_reply_free(ps5_reply_t *r);

/* One raw HTTP request against the minion.  The Host header uses cfg->ip, which
 * is exactly what ps5upload's LAN check expects. */
int ps5_request(const ps5_cfg_t *cfg, const char *method, const char *path,
                const char *token, const char *body, ps5_reply_t *out);

/* POST an API route with the cached token; refreshes the token once on 401. */
int ps5_api(const ps5_cfg_t *cfg, const char *path, const char *body, ps5_reply_t *out);

/* GET /api/token without a token (the route is outside the token check). */
int ps5_token_fetch(const ps5_cfg_t *cfg, char *token, size_t tcap,
                    char *version, size_t vcap);

void ps5_token_reset(void);

const char *ps5_error_name(int err, int status);

#endif /* PS5B_PS5_H */
