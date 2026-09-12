/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* Performs one blocking HTTP/1.1 GET request for path on host (a
   hostname or a dotted-decimal literal - resolved via dns_resolve())
   at port, and copies up to body_buf_len-1 bytes of the RESPONSE BODY
   (headers stripped) into body_buf, null-terminated (pass NULL /0 to
   discard the body and just get the status code).

   Always sends "Connection: close" and reads until the peer closes
   its end (or a timeout), so there's no need to trust - or even look
   at - a Content-Length header to know the response is complete.
   Simple, at the cost of not reusing the connection for a second
   request the way a real client would. Chunked transfer-encoding
   isn't decoded either - a chunked response's chunk-size lines will
   show up as literal text inside body_buf; that's the next piece of
   this to build if it turns out to matter.

   Returns the HTTP status code (e.g. 200, 404) on success. Negative
   on failure before any status line was received: -1 DNS resolution
   failed, -2 the TCP connection failed or was refused, -3 no valid
   HTTP response arrived before the connection closed or the overall
   timeout was reached. */
int http_get(const char *host, uint16_t port, const char *path,
             char *body_buf, uint32_t body_buf_len);

#endif