/*
 * PS5 Bridge - PS4 homebrew bridge for ps5upload
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#include "http_server.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>

int
main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fprintf(stderr, "\n%s v%s\n", BRIDGE_NAME, BRIDGE_VERSION);
    fprintf(stderr, "ps5-bridge: Web UI  http://<PS4-IP>:%d/\n", BRIDGE_PORT);

    http_init();
    if (http_serve(BRIDGE_PORT) != 0) {
        fprintf(stderr, "ps5-bridge: could not listen on port %d\n", BRIDGE_PORT);
        return 1;
    }
    return 0;
}
