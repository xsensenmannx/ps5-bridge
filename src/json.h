/*
 * PS5 Bridge - JSON helpers
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#ifndef PS5B_JSON_H
#define PS5B_JSON_H

#include <stddef.h>

typedef struct {
    const char *js;
    size_t len;
    void *tok;
    int ntok;
} json_doc_t;

typedef struct {
    char id[16];
    char name[192];
} game_entry_t;

typedef struct {
    game_entry_t *items;
    size_t count;
    size_t cap;
} game_list_t;

int json_doc_parse(json_doc_t *d, const char *js, size_t len, size_t max_tokens);
void json_doc_free(json_doc_t *d);

int json_get_string(const json_doc_t *d, const char *key, char *out, size_t cap);
int json_get_long(const json_doc_t *d, const char *key, long *out);
int json_get_bool(const json_doc_t *d, const char *key, int *out);

int json_parse_apps(const json_doc_t *d, game_list_t *list, int *more, long *next,
                    char *err, size_t errcap);

void game_list_free(game_list_t *l);

#endif /* PS5B_JSON_H */
