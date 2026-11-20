/*
 * PS5 Bridge - persisted settings
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#ifndef PS5B_CONFIG_H
#define PS5B_CONFIG_H

#define PS5B_CONFIG_DIR  "/data/ps5bridge"
#define PS5B_CONFIG_FILE PS5B_CONFIG_DIR "/config.ini"
#define PS5B_TMP_DIR     "/tmp/ps5bridge"
#define PS5B_TMP_FILE    PS5B_TMP_DIR "/config.ini"
#define PS5B_DEFAULT_PORT 9180

typedef struct {
    char ip[64];
    int port;
} bridge_config_t;

void config_defaults(bridge_config_t *c);
int config_load(bridge_config_t *c);
int config_save(const bridge_config_t *c);

#endif /* PS5B_CONFIG_H */
