/*
 * HTTP/1.0 server for the LinkFPGA admin UI.
 *
 * Built on top of lwIP's raw TCP API (no sockets, no threads).
 * One connection at a time, response-then-close. Both GET and POST
 * are supported; POST bodies up to a few KiB work, including bodies
 * that arrive split across multiple TCP segments — the recv handler
 * accumulates segments until either Content-Length bytes have arrived
 * or the request buffer fills up. Requests are dispatched to a small
 * fixed route table that is wired up by `webui_init()`.
 */

#ifndef LINKFPGA_HTTP_SERVER_H
#define LINKFPGA_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int           status;             /* 200, 404, ... */
    const char   *content_type;
    const uint8_t *body;
    size_t        body_len;
} http_response_t;

typedef void (*http_handler_t)(const char *path, const char *body,
                               size_t body_len, http_response_t *resp);

void http_init(uint16_t port);
void http_route(const char *method, const char *path, http_handler_t fn);

#endif /* LINKFPGA_HTTP_SERVER_H */
