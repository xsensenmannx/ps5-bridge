/*
 * PS5 Bridge - JSON helpers
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "json.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsmn.h"

static jsmntok_t *
toks(const json_doc_t *d) {
    return (jsmntok_t *)d->tok;
}

static int
tok_eq(const json_doc_t *d, int i, const char *s) {
    const jsmntok_t *t = &toks(d)[i];
    size_t n = strlen(s);
    if (t->type != JSMN_STRING) return 0;
    return (size_t)(t->end - t->start) == n && !memcmp(d->js + t->start, s, n);
}

/* A string token followed by ':' is an object key, not a value. */
static int
tok_is_key(const json_doc_t *d, int i) {
    const jsmntok_t *t = &toks(d)[i];
    size_t p = (size_t)t->end;
    while (p < d->len && isspace((unsigned char)d->js[p])) p++;
    return p < d->len && d->js[p] == ':';
}

static int
skip_subtree(const json_doc_t *d, int i) {
    const jsmntok_t *t = &toks(d)[i];
    int j = i + 1;
    while (j < d->ntok && toks(d)[j].start < t->end) j++;
    return j;
}

static int
find_value(const json_doc_t *d, const char *key) {
    int i;
    for (i = 0; i + 1 < d->ntok; i++) {
        if (tok_eq(d, i, key) && tok_is_key(d, i)) return i + 1;
    }
    return -1;
}

static int
hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static size_t
put_utf8(char *out, size_t cap, size_t o, unsigned cp) {
    if (cp < 0x80) {
        if (o + 1 < cap) out[o++] = (char)cp;
    } else if (cp < 0x800) {
        if (o + 2 < cap) {
            out[o++] = (char)(0xc0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    } else if (cp < 0x10000) {
        if (o + 3 < cap) {
            out[o++] = (char)(0xe0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    } else {
        if (o + 4 < cap) {
            out[o++] = (char)(0xf0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3f));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (cp & 0x3f));
        }
    }
    return o;
}

static void
json_unescape(const char *s, int len, char *out, size_t cap) {
    size_t o = 0;
    int i;

    if (!out || cap == 0) return;
    for (i = 0; i < len; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < len) {
            char e = s[++i];
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                unsigned cp;
                if (i + 4 < len && hex4(s + i + 1, &cp)) {
                    int consumed = 4;
                    if (cp >= 0xd800 && cp <= 0xdbff && i + 10 < len &&
                        s[i + 5] == '\\' && s[i + 6] == 'u') {
                        unsigned lo;
                        if (hex4(s + i + 7, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
                            cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                            consumed = 10;
                        }
                    }
                    i += consumed;
                    o = put_utf8(out, cap, o, cp);
                    continue;
                }
                c = '?';
                break;
            }
            default:
                c = e;
                break;
            }
        }
        if (o + 1 < cap) out[o++] = c;
    }
    out[o] = '\0';
}

int
json_doc_parse(json_doc_t *d, const char *js, size_t len, size_t max_tokens) {
    jsmn_parser p;
    jsmntok_t *t;
    int n;

    if (!d) return 0;
    memset(d, 0, sizeof *d);
    if (!js || len == 0 || max_tokens == 0) return 0;
    t = (jsmntok_t *)calloc(max_tokens, sizeof(jsmntok_t));
    if (!t) return 0;
    jsmn_init(&p);
    n = jsmn_parse(&p, js, len, t, (unsigned int)max_tokens);
    if (n <= 0 || t[0].type != JSMN_OBJECT) {
        free(t);
        return 0;
    }
    d->js = js;
    d->len = len;
    d->tok = t;
    d->ntok = n;
    return 1;
}

void
json_doc_free(json_doc_t *d) {
    if (!d) return;
    free(d->tok);
    memset(d, 0, sizeof *d);
}

int
json_get_string(const json_doc_t *d, const char *key, char *out, size_t cap) {
    int i;
    if (!d || !key || !out || cap == 0) return 0;
    out[0] = '\0';
    i = find_value(d, key);
    if (i < 0 || toks(d)[i].type != JSMN_STRING) return 0;
    json_unescape(d->js + toks(d)[i].start, toks(d)[i].end - toks(d)[i].start, out, cap);
    return 1;
}

int
json_get_long(const json_doc_t *d, const char *key, long *out) {
    int i;
    char buf[64];
    char *end = NULL;
    long v;

    if (!d || !key || !out) return 0;
    i = find_value(d, key);
    if (i < 0 || toks(d)[i].type != JSMN_PRIMITIVE) return 0;
    if ((size_t)(toks(d)[i].end - toks(d)[i].start) >= sizeof buf) return 0;
    memcpy(buf, d->js + toks(d)[i].start, (size_t)(toks(d)[i].end - toks(d)[i].start));
    buf[toks(d)[i].end - toks(d)[i].start] = '\0';
    v = strtol(buf, &end, 10);
    if (end == buf) return 0;
    *out = v;
    return 1;
}

int
json_get_bool(const json_doc_t *d, const char *key, int *out) {
    int i;
    const char *s;
    int n;

    if (!d || !key || !out) return 0;
    i = find_value(d, key);
    if (i < 0 || toks(d)[i].type != JSMN_PRIMITIVE) return 0;
    s = d->js + toks(d)[i].start;
    n = toks(d)[i].end - toks(d)[i].start;
    if (n == 4 && !memcmp(s, "true", 4)) {
        *out = 1;
        return 1;
    }
    if (n == 5 && !memcmp(s, "false", 5)) {
        *out = 0;
        return 1;
    }
    return 0;
}

void
game_list_free(game_list_t *l) {
    if (!l) return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap = 0;
}

static int
list_has(const game_list_t *l, const char *id) {
    size_t i;
    for (i = 0; i < l->count; i++) {
        if (!strcmp(l->items[i].id, id)) return 1;
    }
    return 0;
}

static int
list_add(game_list_t *l, const char *id, const char *name) {
    game_entry_t *p;
    size_t cap;

    if (list_has(l, id)) return 0;
    if (l->count == l->cap) {
        cap = l->cap ? l->cap * 2 : 128;
        p = (game_entry_t *)realloc(l->items, cap * sizeof *p);
        if (!p) return -1;
        l->items = p;
        l->cap = cap;
    }
    memset(&l->items[l->count], 0, sizeof l->items[l->count]);
    snprintf(l->items[l->count].id, sizeof l->items[l->count].id, "%s", id);
    snprintf(l->items[l->count].name, sizeof l->items[l->count].name, "%s",
             (name && name[0]) ? name : id);
    l->count++;
    return 1;
}

/*
 * One /api/apps/db page: {"apps":[{"title_id","app_id","name"}],"next":N,"more":bool}
 * or {"err":"..."}.  Returns 1 on success, 0 on a malformed reply, -1 when the
 * console reported an error (err receives the reason).
 */
int
json_parse_apps(const json_doc_t *d, game_list_t *list, int *more, long *next,
                char *err, size_t errcap) {
    char e[128];
    int trunc = 0;
    int apps_i;
    int i;
    int array_end;

    if (!d || !list) return 0;
    if (err && errcap) err[0] = '\0';
    if (more) *more = 0;
    if (next) *next = 0;

    if (json_get_string(d, "err", e, sizeof e) && e[0]) {
        if (err && errcap) snprintf(err, errcap, "%s", e);
        return -1;
    }
    if (json_get_bool(d, "truncated", &trunc) == 1 && trunc) {
        if (err && errcap) snprintf(err, errcap, "truncated");
        return -1;
    }
    if (more) json_get_bool(d, "more", more);
    if (next) {
        long n = 0;
        if (json_get_long(d, "next", &n)) *next = n;
    }

    apps_i = find_value(d, "apps");
    if (apps_i < 0) return 0;
    if (toks(d)[apps_i].type != JSMN_ARRAY) return 0;

    array_end = skip_subtree(d, apps_i);
    i = apps_i + 1;
    while (i < array_end) {
        char id[24] = "";
        char name[192] = "";
        int j;
        int obj_end;

        if (toks(d)[i].type != JSMN_OBJECT) {
            i = skip_subtree(d, i);
            continue;
        }
        obj_end = skip_subtree(d, i);
        j = i + 1;
        while (j < obj_end) {
            if (toks(d)[j].type == JSMN_STRING && tok_is_key(d, j) && j + 1 < obj_end) {
                int v = j + 1;
                if (tok_eq(d, j, "title_id") && toks(d)[v].type == JSMN_STRING) {
                    json_unescape(d->js + toks(d)[v].start, toks(d)[v].end - toks(d)[v].start,
                                  id, sizeof id);
                } else if (tok_eq(d, j, "name") && toks(d)[v].type == JSMN_STRING) {
                    json_unescape(d->js + toks(d)[v].start, toks(d)[v].end - toks(d)[v].start,
                                  name, sizeof name);
                }
                j = skip_subtree(d, v);
            } else {
                j = skip_subtree(d, j);
            }
        }
        util_uppercase(id);
        if (util_valid_title_id(id)) {
            if (list_add(list, id, name) < 0) return 0;
        }
        i = obj_end;
    }
    return 1;
}
