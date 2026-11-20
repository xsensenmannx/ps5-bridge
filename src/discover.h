/*
 * PS5 Bridge - console discovery
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#ifndef PS5B_DISCOVER_H
#define PS5B_DISCOVER_H

#include <stddef.h>

#include "config.h"

typedef struct {
    char ip[64];
    char model[64];
    char firmware[32];
    char version[32];
    int lan;
} console_t;

/* The PS4's own LAN address, or -1 when there is none. */
int discover_local_ip(char *out, size_t cap);

/* One-shot scan of the local /24 for ps5upload's minion.  The configured
 * console is probed first, even when it is outside the local subnet. */
int discover_scan(const bridge_config_t *cfg, console_t *out, int max);

#endif /* PS5B_DISCOVER_H */
