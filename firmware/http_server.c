/*
 * Tiny HTTP/1.0 server.
 *
 * Designed to sit on top of LiteEth's TCP socket layer. Each request is
 * parsed in one shot from a fixed-size buffer; pipelined / chunked /
 * keep-alive requests are not supported.
 *
 * Auth: HTTP Basic.
 */

#include <string.h>
#include <stdio.h>

#include "http_server.h"
#include "config.h"

/* Forward decls into LiteEth's TCP stack — see net.c notes about the
 * exact symbol names matching the generated LiteEth integration. */
extern int  liteeth_tcp_listen(uint16_t port);
extern int  liteeth_tcp_send(uint32_t client, const void *data, size_t len);
extern void liteeth_tcp_close(uint32_t client);
extern void liteeth_tcp_set_callback(
                void (*cb)(uint32_t client, const void *data, size_t len));

typedef struct {
    http_method_t  method;
    const char    *path;
    http_handler_t fn;
} route_t;

static route_t s_routes[HTTP_MAX_ROUTES];
static int     s_n_routes;
static char    s_auth[64];          /* "user:pass" or "" */
static char    s_auth_b64[128];     /* "Basic <base64>" */

/* ---- minimal base64 encoder ---- */
static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64enc(const char *in, size_t inlen, char *out) {
    size_t i = 0, j = 0;
    while (i + 3 <= inlen) {
        uint32_t v = ((uint32_t)(uint8_t)in[i] << 16) |
                     ((uint32_t)(uint8_t)in[i+1] << 8) |
                                (uint8_t)in[i+2];
        out[j++] = b64tab[(v >> 18) & 0x3f];
        out[j++] = b64tab[(v >> 12) & 0x3f];
        out[j++] = b64tab[(v >>  6) & 0x3f];
        out[j++] = b64tab[ v        & 0x3f];
        i += 3;
    }
    if (i < inlen) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        if (i + 1 < inlen) v |= (uint32_t)(uint8_t)in[i+1] << 8;
        out[j++] = b64tab[(v >> 18) & 0x3f];
        out[j++] = b64tab[(v >> 12) & 0x3f];
        out[j++] = (i + 1 < inlen) ? b64tab[(v >> 6) & 0x3f] : '=';
        out[j++] = '=';
    }
    out[j] = 0;
    return j;
}

void http_init(uint16_t port) {
    s_n_routes = 0;
    s_auth[0] = 0;
    s_auth_b64[0] = 0;
    liteeth_tcp_listen(port);
    extern void http_handle_segment(uint32_t, uint16_t, const uint8_t *, size_t);
    /* Wire LiteEth callback to our segment handler */
    liteeth_tcp_set_callback((void(*)(uint32_t, const void*, size_t))
                             http_handle_segment);
}

void http_set_auth(const char *user, const char *pass) {
    if (!user || !pass) { s_auth[0] = 0; s_auth_b64[0] = 0; return; }
    int n = snprintf(s_auth, sizeof(s_auth), "%s:%s", user, pass);
    if (n <= 0) return;
    char tmp[96];
    size_t len = b64enc(s_auth, n, tmp);
    snprintf(s_auth_b64, sizeof(s_auth_b64), "Basic %.*s", (int)len, tmp);
}

void http_route(http_method_t method, const char *path, http_handler_t fn) {
    if (s_n_routes >= HTTP_MAX_ROUTES) return;
    s_routes[s_n_routes++] = (route_t){ method, path, fn };
}

/* ---- request parser + dispatcher ---- */

static int parse_method(const char *s, size_t n, http_method_t *out) {
    if (n >= 4 && memcmp(s, "GET ",  4) == 0) { *out = HTTP_GET;  return 4; }
    if (n >= 5 && memcmp(s, "POST ", 5) == 0) { *out = HTTP_POST; return 5; }
    return 0;
}

static int auth_ok(const char *headers, size_t n) {
    if (s_auth_b64[0] == 0) return 1;     /* auth disabled */
    /* Look for "Authorization: <s_auth_b64>" */
    const char *needle = "Authorization:";
    const char *pos = NULL;
    for (size_t i = 0; i + 14 < n; i++) {
        if (memcmp(headers + i, needle, 14) == 0) { pos = headers + i; break; }
    }
    if (!pos) return 0;
    pos += 14;
    while (*pos == ' ' || *pos == '\t') pos++;
    return strncmp(pos, s_auth_b64, strlen(s_auth_b64)) == 0;
}

static void send_status(uint32_t client, int status, const char *reason,
                        const char *extra_headers,
                        const char *content_type,
                        const void *body, size_t body_len) {
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Server: LinkFPGA\r\n"
        "%s"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason,
        extra_headers ? extra_headers : "",
        content_type ? content_type : "text/plain",
        (unsigned)body_len);
    if (n > 0) liteeth_tcp_send(client, hdr, n);
    if (body_len > 0) liteeth_tcp_send(client, body, body_len);
    liteeth_tcp_close(client);
}

void http_handle_segment(uint32_t client, uint16_t src_port,
                         const uint8_t *buf, size_t len) {
    (void)src_port;
    if (len < 16) return;     /* not a complete request line */

    http_method_t method;
    int off = parse_method((const char *)buf, len, &method);
    if (!off) {
        send_status(client, 400, "Bad Request", NULL, "text/plain", "bad", 3);
        return;
    }

    /* Path is from `off` until next space. */
    const char *path = (const char *)buf + off;
    size_t      maxp = len - off;
    size_t      pathlen = 0;
    while (pathlen < maxp && path[pathlen] != ' ' && path[pathlen] != '\r')
        pathlen++;
    char pathbuf[128];
    if (pathlen >= sizeof(pathbuf)) pathlen = sizeof(pathbuf) - 1;
    memcpy(pathbuf, path, pathlen);
    pathbuf[pathlen] = 0;

    /* Headers begin after CRLF */
    const char *body = NULL;
    size_t body_len = 0;
    const char *p = (const char *)buf;
    const char *end = p + len;
    while (p + 4 <= end) {
        if (memcmp(p, "\r\n\r\n", 4) == 0) {
            body = p + 4;
            body_len = end - body;
            break;
        }
        p++;
    }

    if (!auth_ok((const char *)buf, len)) {
        send_status(client, 401, "Unauthorized",
                    "WWW-Authenticate: Basic realm=\"LinkFPGA\"\r\n",
                    "text/plain", "auth", 4);
        return;
    }

    for (int i = 0; i < s_n_routes; i++) {
        route_t *r = &s_routes[i];
        if (r->method == method && strcmp(r->path, pathbuf) == 0) {
            http_request_t req = {
                .method = method,
                .path   = pathbuf,
                .body   = body ? body : "",
                .body_len = body_len,
            };
            http_response_t resp = { 200, "text/plain", (const uint8_t *)"", 0 };
            r->fn(&req, &resp);
            send_status(client, resp.status,
                        (resp.status == 200 ? "OK" : "Error"),
                        NULL,
                        resp.content_type, resp.body, resp.body_len);
            return;
        }
    }

    send_status(client, 404, "Not Found", NULL, "text/plain", "404", 3);
}

void http_tick(void) {
    /* Pull-mode tick is unused; LiteEth's TCP callback drives us. */
}
