/*
 * Tiny HTTP/1.0 server with optional HTTP Basic authentication.
 *
 * Single-connection at a time. The body of every response is bounded
 * (no chunked encoding); routes register a synchronous handler that
 * fills a caller-supplied buffer.
 *
 * Authentication: when `http_set_auth(user, pass)` is called with
 * non-NULL strings, every request must carry an `Authorization: Basic`
 * header that decodes to "user:pass". Otherwise the server replies
 * 401 with WWW-Authenticate. Pass NULL to disable.
 */

#ifndef LINKFPGA_HTTP_SERVER_H
#define LINKFPGA_HTTP_SERVER_H

#include <stddef.h>
#include <stdint.h>

#define HTTP_MAX_ROUTES 16

typedef enum {
    HTTP_GET  = 1,
    HTTP_POST = 2,
} http_method_t;

typedef struct {
    http_method_t method;
    const char   *path;
    const char   *body;       /* request body, NUL-terminated, may be empty */
    size_t        body_len;
} http_request_t;

typedef struct {
    int           status;     /* 200, 404, 500, ... */
    const char   *content_type;
    const uint8_t *body;
    size_t        body_len;
} http_response_t;

typedef void (*http_handler_t)(const http_request_t *req, http_response_t *resp);

void http_init(uint16_t port);
void http_set_auth(const char *user, const char *pass);
void http_route(http_method_t method, const char *path, http_handler_t fn);
void http_tick(void);
/* Hook called from net.c when a TCP segment arrives on `port`. The
 * implementation is currently UDP-only and does not parse TCP; in this
 * tree the http_server runs on top of LiteEth's stock TCP stack which
 * exposes a callback equivalent. See http_server.c for the integration
 * point. */
void http_handle_segment(uint32_t src_addr, uint16_t src_port,
                         const uint8_t *buf, size_t len);

#endif /* LINKFPGA_HTTP_SERVER_H */
