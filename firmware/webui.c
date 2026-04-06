/*
 * Web UI route handlers (serves /, /api/status, /api/tempo, /api/play,
 * /api/auth). The HTML page lives in webui/index.html and is embedded
 * here as a byte array via a generated header.
 *
 * To regenerate the embedded blob without `xxd`:
 *   python3 -c "import sys;
 *   d=open('webui/index.html','rb').read();
 *   print('static const unsigned char index_html[] = {' +
 *         ','.join(str(b) for b in d) + '};');
 *   print('static const unsigned int index_html_len = %d;' % len(d))" \
 *     > firmware/index_html.h
 *
 * The Makefile runs the same command before compiling webui.c.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "http_server.h"
#include "link.h"
#include "ghost_time.h"
#include "config.h"
#include "net.h"

#if __has_include("index_html.h")
#  include "index_html.h"
#else
   /* Tiny fallback so the firmware still links if the embed step is
    * skipped during early bring-up. */
   static const unsigned char index_html[] =
       "<!doctype html><title>LinkFPGA</title><h1>LinkFPGA OK</h1>";
   static const unsigned int index_html_len = sizeof(index_html) - 1;
#endif

static char     resp_buf[2048];

static void serve_index(const http_request_t *req, http_response_t *resp) {
    (void)req;
    resp->status       = 200;
    resp->content_type = "text/html; charset=utf-8";
    resp->body         = index_html;
    resp->body_len     = index_html_len;
}

static void node_id_hex(char *out, const link_id_t *id) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i*2  ] = h[id->bytes[i] >> 4];
        out[i*2+1] = h[id->bytes[i] & 0xf];
    }
    out[16] = 0;
}

static void serve_status(const http_request_t *req, http_response_t *resp) {
    (void)req;
    char self_id[17], sess_id[17];
    node_id_hex(self_id, &link_self_id);
    node_id_hex(sess_id, &link_self_session);

    int64_t now_g = (int64_t)ghost_time_us();
    double cur_beats = link_self_timeline.beat_origin_microbeats / 1e6
        + ((double)(now_g - link_self_timeline.time_origin_us_ghost) /
           (double)link_self_timeline.tempo_us_per_beat);
    double bpm = 60e6 / (double)link_self_timeline.tempo_us_per_beat;
    int synced = memcmp(link_self_session.bytes, link_self_id.bytes, 8) != 0;

    int n = snprintf(resp_buf, sizeof(resp_buf),
        "{\"node_id\":\"%s\",\"session_id\":\"%s\","
        "\"tempo_bpm\":%.3f,\"beats\":%.3f,\"playing\":%s,\"synced\":%s,"
        "\"tdm\":{\"physical\":%d,\"virtual\":%d,\"channels_per_port\":%d,"
                  "\"fs_hz\":%d},"
        "\"peers\":[",
        self_id, sess_id, bpm, cur_beats,
        link_self_startstop.is_playing ? "true" : "false",
        synced ? "true" : "false",
        LINK_NUM_PHYSICAL_TDM_PORTS,
        LINK_NUM_TDM_PORTS - LINK_NUM_PHYSICAL_TDM_PORTS,
        LINK_TDM_CHANNELS_PER_PORT, LINK_TDM_FS_HZ);

    int first = 1;
    for (int i = 0; i < LINK_MAX_PEERS && n < (int)sizeof(resp_buf) - 256; i++) {
        link_peer_t *p = &link_peers[i];
        if (!p->in_use) continue;
        char pid[17], psid[17];
        node_id_hex(pid,  &p->node_id);
        node_id_hex(psid, &p->session_id);
        double pbpm = (p->timeline.tempo_us_per_beat
                       ? 60e6 / (double)p->timeline.tempo_us_per_beat : 0.0);
        n += snprintf(resp_buf + n, sizeof(resp_buf) - n,
            "%s{\"node_id\":\"%s\",\"session_id\":\"%s\","
            "\"tempo_bpm\":%.3f,\"endpoint\":\"%u.%u.%u.%u:%u\"}",
            first ? "" : ",",
            pid, psid, pbpm,
            (p->mep4_addr >> 24) & 0xff, (p->mep4_addr >> 16) & 0xff,
            (p->mep4_addr >>  8) & 0xff,  p->mep4_addr        & 0xff,
            p->mep4_port);
        first = 0;
    }
    n += snprintf(resp_buf + n, sizeof(resp_buf) - n, "]}");

    resp->status       = 200;
    resp->content_type = "application/json";
    resp->body         = (const uint8_t *)resp_buf;
    resp->body_len     = n;
}

static void serve_tempo(const http_request_t *req, http_response_t *resp) {
    double bpm = 0;
    if (req->body_len > 0) {
        char tmp[16] = {0};
        size_t n = req->body_len < 15 ? req->body_len : 15;
        memcpy(tmp, req->body, n);
        bpm = atof(tmp);
    }
    if (bpm < 20 || bpm > 999) {
        resp->status = 400;
        resp->body   = (const uint8_t *)"bad tempo";
        resp->body_len = 9;
        return;
    }
    link_set_local_tempo(bpm);
    resp->status = 200;
    resp->body   = (const uint8_t *)"ok";
    resp->body_len = 2;
}

static void serve_play(const http_request_t *req, http_response_t *resp) {
    int play = (req->body_len > 0 && req->body[0] == '1');
    link_set_play(play);
    resp->status = 200;
    resp->body   = (const uint8_t *)"ok";
    resp->body_len = 2;
}

static void serve_auth(const http_request_t *req, http_response_t *resp) {
    /* Body is "user:pass". Empty user clears auth. */
    char buf[64] = {0};
    size_t n = req->body_len < 63 ? req->body_len : 63;
    memcpy(buf, req->body, n);
    char *colon = strchr(buf, ':');
    if (!colon || colon == buf) {
        http_set_auth(NULL, NULL);
    } else {
        *colon = 0;
        http_set_auth(buf, colon + 1);
    }
    resp->status = 200;
    resp->body   = (const uint8_t *)"ok";
    resp->body_len = 2;
}

void webui_init(void) {
    http_route(HTTP_GET,  "/",            serve_index);
    http_route(HTTP_GET,  "/api/status",  serve_status);
    http_route(HTTP_POST, "/api/tempo",   serve_tempo);
    http_route(HTTP_POST, "/api/play",    serve_play);
    http_route(HTTP_POST, "/api/auth",    serve_auth);
}
