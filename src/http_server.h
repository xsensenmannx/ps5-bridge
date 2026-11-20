/*
 * PS5 Bridge - HTTP server
 * Copyright (C) 2026 xsensenmannx
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3, or (at your option) any later
 * version.
 */

#ifndef PS5B_HTTP_SERVER_H
#define PS5B_HTTP_SERVER_H

void http_init(void);
int http_serve(unsigned short port);

#endif /* PS5B_HTTP_SERVER_H */
