/*
 * HTTP/1.0 server using lwIP raw TCP.
 *
 * Connection lifecycle:
 *   accept() →
 *      Accumulate received bytes into a per-connection request buffer
 *      until either:
 *        - we see "\r\n\r\n" with Content-Length=0, OR
 *        - we have accumulated `Content-Length` bytes after "\r\n\r\n",
 *          OR
 *        - the buffer fills up.
 *      Then dispatch to the matching route, write the response inline,
 *      and close.
 *
 * No keep-alive, no chunked transfer, no TLS — just enough to serve
 * a small admin UI from the device itself. Both GET and POST work;
 * POST bodies up to a few KiB are supported (including across multiple
 * TCP segments).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/tcp.h"
#include "lwip/err.h"

#include "http_server.h"

#define MAX_ROUTES   16
#define REQ_BUF_MAX  4096
#define HDR_BUF_MAX  256

typedef struct {
    char            method[8];
    char            path[64];
    http_handler_t  fn;
} route_t;

typedef struct {
    char    buf[REQ_BUF_MAX];
    size_t  used;
    int     have_headers;
    int     content_length;
    size_t  body_offset;
} conn_state_t;

static route_t s_routes[MAX_ROUTES];
static int     s_n_routes;

void http_route(const char *method, const char *path, http_handler_t fn) {
    if (s_n_routes >= MAX_ROUTES) return;
    route_t *r = &s_routes[s_n_routes++];
    strncpy(r->method, method, sizeof(r->method) - 1);
    r->method[sizeof(r->method) - 1] = 0;
    strncpy(r->path, path, sizeof(r->path) - 1);
    r->path[sizeof(r->path) - 1] = 0;
    r->fn = fn;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

static void send_response(struct tcp_pcb *pcb, const http_response_t *resp) {
    char hdr[HDR_BUF_MAX];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.0 %d %s\r\n"
        "Server: LinkFPGA\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "\r\n",
        resp->status, reason(resp->status),
        resp->content_type ? resp->content_type : "text/plain",
        (unsigned)resp->body_len);

    tcp_write(pcb, hdr, hlen, TCP_WRITE_FLAG_COPY);
    if (resp->body_len)
        tcp_write(pcb, resp->body, resp->body_len, TCP_WRITE_FLAG_COPY);
    tcp_output(pcb);
}

static void parse_headers(conn_state_t *cs) {
    cs->buf[cs->used] = 0;
    char *eol = strstr(cs->buf, "\r\n\r\n");
    if (!eol) return;
    cs->have_headers = 1;
    cs->body_offset  = (eol - cs->buf) + 4;

    /* Find Content-Length, case-insensitive (lwip's clients send
     * "Content-Length"). Hand-rolled to avoid bringing in strcasestr. */
    cs->content_length = 0;
    for (char *p = cs->buf; p < eol; p++) {
        if ((*p == 'C' || *p == 'c') &&
            strncmp(p, "Content-Length", 14) == 0 || strncmp(p, "content-length", 14) == 0) {
            char *colon = strchr(p, ':');
            if (colon && colon < eol) {
                cs->content_length = atoi(colon + 1);
            }
            break;
        }
    }
}

static int request_complete(const conn_state_t *cs) {
    if (!cs->have_headers) return 0;
    if (cs->content_length == 0) return 1;
    return cs->used >= cs->body_offset + (size_t)cs->content_length;
}

static void dispatch(struct tcp_pcb *pcb, conn_state_t *cs) {
    /* Parse request line */
    char method[8] = {0};
    char path[64]  = {0};
    int  off = 0;
    while (cs->buf[off] && cs->buf[off] != ' ' && off < 7) {
        method[off] = cs->buf[off]; off++;
    }
    method[off] = 0;
    if (cs->buf[off] != ' ') {
        http_response_t resp = { 400, "text/plain",
                                 (const uint8_t *)"bad", 3 };
        send_response(pcb, &resp);
        return;
    }
    off++;
    int p1 = 0;
    while (cs->buf[off] && cs->buf[off] != ' ' && p1 < 63) {
        path[p1++] = cs->buf[off++];
    }
    path[p1] = 0;

    const char *body = cs->buf + cs->body_offset;
    size_t body_len  = cs->content_length;

    http_response_t resp = { 404, "text/plain",
                             (const uint8_t *)"not found", 9 };
    for (int i = 0; i < s_n_routes; i++) {
        if (strcmp(s_routes[i].method, method) == 0 &&
            strcmp(s_routes[i].path,   path)   == 0) {
            resp.status   = 200;
            resp.content_type = "text/plain";
            resp.body     = (const uint8_t *)"";
            resp.body_len = 0;
            s_routes[i].fn(path, body, body_len, &resp);
            break;
        }
    }
    send_response(pcb, &resp);
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p,
                     err_t err) {
    conn_state_t *cs = (conn_state_t *)arg;
    if (err != ERR_OK || p == NULL) {
        if (p) pbuf_free(p);
        if (cs) free(cs);
        tcp_arg(pcb, NULL);
        tcp_close(pcb);
        return ERR_OK;
    }

    if (cs->used + p->tot_len > REQ_BUF_MAX) {
        http_response_t resp = { 413, "text/plain",
                                 (const uint8_t *)"too large", 9 };
        send_response(pcb, &resp);
        pbuf_free(p);
        free(cs);
        tcp_arg(pcb, NULL);
        tcp_close(pcb);
        return ERR_OK;
    }

    pbuf_copy_partial(p, cs->buf + cs->used, p->tot_len, 0);
    cs->used += p->tot_len;
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (!cs->have_headers) parse_headers(cs);
    if (!request_complete(cs)) return ERR_OK;       /* keep accumulating */

    dispatch(pcb, cs);
    free(cs);
    tcp_arg(pcb, NULL);
    tcp_close(pcb);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || newpcb == NULL) return ERR_VAL;
    conn_state_t *cs = (conn_state_t *)calloc(1, sizeof(*cs));
    if (!cs) return ERR_MEM;
    tcp_setprio(newpcb, TCP_PRIO_MIN);
    tcp_arg(newpcb, cs);
    tcp_recv(newpcb, on_recv);
    return ERR_OK;
}

void http_init(uint16_t port) {
    s_n_routes = 0;
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return;
    tcp_bind(pcb, IP_ANY_TYPE, port);
    pcb = tcp_listen(pcb);
    tcp_accept(pcb, on_accept);
}
