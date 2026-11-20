/*
 * PS5 Bridge - persisted settings
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "config.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char *
trim(char *s) {
    char *end;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
    return s;
}

void
config_defaults(bridge_config_t *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->port = PS5B_DEFAULT_PORT;
}

int
config_load(bridge_config_t *c) {
    static const char *paths[] = { PS5B_CONFIG_FILE, PS5B_TMP_FILE };
    size_t i;

    if (!c) return 0;
    config_defaults(c);
    for (i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        FILE *fp = fopen(paths[i], "r");
        char line[512];
        if (!fp) continue;
        while (fgets(line, sizeof line, fp)) {
            char *s = trim(line);
            char *eq;
            char *key;
            char *val;
            if (!*s || *s == '#' || *s == ';') continue;
            eq = strchr(s, '=');
            if (!eq) continue;
            *eq = '\0';
            key = trim(s);
            val = trim(eq + 1);
            if (!strcmp(key, "ip")) {
                if (!val[0]) {
                    c->ip[0] = '\0';
                } else if (util_valid_ipv4(val)) {
                    snprintf(c->ip, sizeof c->ip, "%s", val);
                }
            } else if (!strcmp(key, "port")) {
                long p = strtol(val, NULL, 10);
                if (p > 0 && p <= 65535) c->port = (int)p;
            }
        }
        fclose(fp);
        return 1;
    }
    return 0;
}

int
config_save(const bridge_config_t *c) {
    FILE *fp;

    if (!c) return -1;
    (void)mkdir(PS5B_CONFIG_DIR, 0777);
    (void)mkdir(PS5B_TMP_DIR, 0777);

    fp = fopen(PS5B_CONFIG_FILE, "w");
    if (!fp) fp = fopen(PS5B_TMP_FILE, "w");
    if (!fp) return -1;

    fprintf(fp, "# PS5 Bridge settings\n");
    fprintf(fp, "ip=%s\n", c->ip);
    fprintf(fp, "port=%d\n", c->port);
    if (fclose(fp) != 0) return -1;
    return 0;
}
