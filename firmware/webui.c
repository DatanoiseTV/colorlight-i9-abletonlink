/*
 * Web UI route handlers for the on-device HTTP server.
 *
 * Routes:
 *   GET  /              → embedded HTML admin page
 *   GET  /api/status    → JSON status (peers, tempo, beat, sync, name, tdm)
 *   GET  /api/channels  → JSON list of remote audio channels
 *   POST /api/tempo     → body: BPM (e.g. "120.5")
 *   POST /api/play      → body: empty
 *   POST /api/stop      → body: empty
 *   POST /api/name      → body: peer name
 *   POST /api/subscribe → body: "<port> <slot> <hex-channel-id>"
 *   POST /api/unsubscribe → body: "<hex-channel-id>"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http_server.h"
#include "link.h"
#include "link_audio.h"
#include "ghost_time.h"
#include "config.h"

static char  s_resp_buf[4096];
static char  s_peer_name[32] = "linkfpga";

/* The admin page is small enough to embed as a C string. Vanilla JS,
 * no external assets, polls /api/status every 1 s. */
static const char index_html[] =
"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
"<title>LinkFPGA</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
":root{color-scheme:dark}"
"body{font:14px/1.4 ui-monospace,Menlo,Consolas,monospace;"
"background:#0b0e14;color:#d0d6e1;margin:0;padding:1.5rem;max-width:880px}"
"h1{color:#ffae57;font-size:1.4rem;margin:0 0 .2rem 0}"
"h2{color:#7fd1ff;font-size:.85rem;margin:1.4rem 0 .4rem 0;"
"text-transform:uppercase;letter-spacing:.06em}"
".s{display:grid;grid-template-columns:max-content 1fr;gap:.3rem 1.2rem}"
".s div:nth-child(odd){color:#6b7993}"
"table{width:100%;border-collapse:collapse;font-size:.9rem}"
"th,td{padding:.35rem .6rem;border-bottom:1px solid #1c2230;text-align:left}"
"th{color:#6b7993;font-weight:normal}"
".c{display:flex;gap:.6rem;flex-wrap:wrap;align-items:center}"
"input,button{background:#161c27;color:#d0d6e1;border:1px solid #2a3346;"
"padding:.4rem .7rem;font:inherit;border-radius:3px}"
"button{cursor:pointer}button:hover{background:#1f2839}"
".led{display:inline-block;width:.7rem;height:.7rem;border-radius:50%;"
"background:#2a3346;vertical-align:middle;margin-right:.3rem}"
".led.on{background:#5fdc7e;box-shadow:0 0 6px #5fdc7e}"
"small{color:#6b7993}"
"</style></head><body>"
"<h1>LinkFPGA &mdash; Ableton Link in hardware</h1>"
"<small>on-device HTTP, lwIP TCP/IPv4+IPv6 stack</small>"
"<h2>Status</h2><div class='s'>"
"<div>session</div><div id='session'>&mdash;</div>"
"<div>tempo</div><div id='tempo'>&mdash;</div>"
"<div>beat</div><div id='beat'>&mdash;</div>"
"<div>play</div><div id='play'>&mdash;</div>"
"<div>peers</div><div id='npeers'>0</div>"
"<div>sync</div><div><span id='sled' class='led'></span><span id='stxt'>&mdash;</span></div>"
"<div>name</div><div id='name'>&mdash;</div>"
"<div>node</div><div id='nid'>&mdash;</div>"
"<div>tdm</div><div id='tdm'>&mdash;</div></div>"
"<h2>Peers</h2><table id='pt'><thead><tr><th>Node</th><th>Session</th>"
"<th>Tempo</th><th>Endpoint</th></tr></thead><tbody></tbody></table>"
"<h2>Local controls</h2><div class='c'>"
"<label>BPM <input id='ti' type='number' min='20' max='999' step='.1' value='120'></label>"
"<button onclick='st()'>apply</button>"
"<button onclick='pl()'>play</button>"
"<button onclick='sp()'>stop</button>"
"<label>name <input id='ni' placeholder='linkfpga'></label>"
"<button onclick='sn()'>apply</button></div>"
"<h2>Remote audio channels</h2><table id='ct'><thead><tr><th>Channel</th>"
"<th>Peer</th><th>Subscribe to TDM</th></tr></thead><tbody></tbody></table>"
"<script>"
"const $=i=>document.getElementById(i);"
"async function r(){"
"const j=await(await fetch('/api/status')).json();"
"$('session').textContent=j.session_id;"
"$('tempo').textContent=j.tempo_bpm.toFixed(2)+' BPM';"
"$('beat').textContent=j.beats.toFixed(3);"
"$('play').textContent=j.playing?'PLAYING':'stopped';"
"$('npeers').textContent=j.peers.length;"
"$('sled').className='led '+(j.synced?'on':'');"
"$('stxt').textContent=j.synced?'locked':'free';"
"$('name').textContent=j.name;"
"$('nid').textContent=j.node_id;"
"$('tdm').textContent=j.tdm.physical+' physical + '+j.tdm.virtual+' virtual TDM16, '+j.tdm.channels_per_port+' ch '+j.tdm.fs_hz+' Hz';"
"const tb=$('pt').tBodies[0];tb.innerHTML='';"
"for(const p of j.peers){const tr=document.createElement('tr');"
"tr.innerHTML=`<td>${p.node_id}</td><td>${p.session_id}</td><td>${p.tempo_bpm.toFixed(2)}</td><td>${p.endpoint}</td>`;tb.appendChild(tr)}"
"const c=await(await fetch('/api/channels')).json();"
"const ctb=$('ct').tBodies[0];ctb.innerHTML='';"
"for(const ch of c.channels){const tr=document.createElement('tr');"
"tr.innerHTML=`<td>${ch.name}<br><small>${ch.id}</small></td><td><small>${ch.peer}</small></td><td>port <input type='number' min='0' max='15' value='0' style='width:3rem' id='p_${ch.id}'> slot <input type='number' min='0' max='15' value='0' style='width:3rem' id='s_${ch.id}'> <button onclick=\"sb('${ch.id}')\">subscribe</button> <button onclick=\"us('${ch.id}')\">stop</button></td>`;ctb.appendChild(tr)}}"
"async function P(u,b){await fetch(u,{method:'POST',body:b||''});r()}"
"function st(){P('/api/tempo',$('ti').value)}"
"function pl(){P('/api/play')}"
"function sp(){P('/api/stop')}"
"function sn(){P('/api/name',$('ni').value)}"
"function sb(id){P('/api/subscribe',$(`p_${id}`).value+' '+$(`s_${id}`).value+' '+id)}"
"function us(id){P('/api/unsubscribe',id)}"
"r();setInterval(r,1000);"
"</script></body></html>";

static void node_id_hex(char *out, const link_id_t *id) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[i*2  ] = h[id->bytes[i] >> 4];
        out[i*2+1] = h[id->bytes[i] & 0xf];
    }
    out[16] = 0;
}

static int hex_to_id(const char *s, link_id_t *out) {
    if (strlen(s) < 16) return -1;
    for (int i = 0; i < 8; i++) {
        unsigned v;
        if (sscanf(s + i*2, "%2x", &v) != 1) return -1;
        out->bytes[i] = (uint8_t)v;
    }
    return 0;
}

static void serve_index(const char *path, const char *body, size_t blen,
                        http_response_t *resp) {
    (void)path; (void)body; (void)blen;
    resp->content_type = "text/html; charset=utf-8";
    resp->body         = (const uint8_t *)index_html;
    resp->body_len     = sizeof(index_html) - 1;
}

static void serve_status(const char *path, const char *body, size_t blen,
                         http_response_t *resp) {
    (void)path; (void)body; (void)blen;
    char self_id[17], sess_id[17];
    node_id_hex(self_id, &link_self_id);
    node_id_hex(sess_id, &link_self_session);

    int64_t now_g = (int64_t)ghost_time_us();
    int64_t bp    = link_self_timeline.tempo_us_per_beat;
    if (bp <= 0) bp = 500000;
    double cur_beats = link_self_timeline.beat_origin_microbeats / 1e6
        + ((double)(now_g - link_self_timeline.time_origin_us_ghost) /
           (double)bp);
    double bpm = 60e6 / (double)bp;
    int synced = memcmp(link_self_session.bytes, link_self_id.bytes, 8) != 0;

    int n = snprintf(s_resp_buf, sizeof(s_resp_buf),
        "{\"node_id\":\"%s\",\"session_id\":\"%s\","
        "\"tempo_bpm\":%.3f,\"beats\":%.3f,\"playing\":%s,\"synced\":%s,"
        "\"name\":\"%s\","
        "\"tdm\":{\"physical\":%d,\"virtual\":%d,\"channels_per_port\":%d,"
                  "\"fs_hz\":%d},"
        "\"peers\":[",
        self_id, sess_id, bpm, cur_beats,
        link_self_startstop.is_playing ? "true" : "false",
        synced ? "true" : "false",
        s_peer_name,
        LINK_NUM_PHYSICAL_TDM_PORTS,
        LINK_NUM_TDM_PORTS - LINK_NUM_PHYSICAL_TDM_PORTS,
        LINK_TDM_CHANNELS_PER_PORT, LINK_TDM_FS_HZ);

    int first = 1;
    for (int i = 0; i < LINK_MAX_PEERS && n < (int)sizeof(s_resp_buf) - 256; i++) {
        link_peer_t *p = &link_peers[i];
        if (!p->in_use) continue;
        char pid[17], psid[17];
        node_id_hex(pid,  &p->node_id);
        node_id_hex(psid, &p->session_id);
        double pbpm = (p->timeline.tempo_us_per_beat
                       ? 60e6 / (double)p->timeline.tempo_us_per_beat : 0.0);
        n += snprintf(s_resp_buf + n, sizeof(s_resp_buf) - n,
            "%s{\"node_id\":\"%s\",\"session_id\":\"%s\","
            "\"tempo_bpm\":%.3f,\"endpoint\":\"%u.%u.%u.%u:%u\"}",
            first ? "" : ",",
            pid, psid, pbpm,
            (p->mep4_addr >> 24) & 0xff, (p->mep4_addr >> 16) & 0xff,
            (p->mep4_addr >>  8) & 0xff,  p->mep4_addr        & 0xff,
            p->mep4_port);
        first = 0;
    }
    n += snprintf(s_resp_buf + n, sizeof(s_resp_buf) - n, "]}");

    resp->content_type = "application/json";
    resp->body         = (const uint8_t *)s_resp_buf;
    resp->body_len     = n;
}

static void serve_channels(const char *path, const char *body, size_t blen,
                           http_response_t *resp) {
    (void)path; (void)body; (void)blen;
    int n = snprintf(s_resp_buf, sizeof(s_resp_buf), "{\"channels\":[");
    int first = 1;
    for (int i = 0; i < LINK_AUDIO_MAX_REMOTE && n < (int)sizeof(s_resp_buf) - 192; i++) {
        if (!link_audio_remote[i].in_use) continue;
        char cid[17], pid[17];
        node_id_hex(cid, &link_audio_remote[i].channel_id);
        node_id_hex(pid, &link_audio_remote[i].peer_id);
        n += snprintf(s_resp_buf + n, sizeof(s_resp_buf) - n,
            "%s{\"id\":\"%s\",\"name\":\"%s\",\"peer\":\"%s\"}",
            first ? "" : ",",
            cid, link_audio_remote[i].name, pid);
        first = 0;
    }
    n += snprintf(s_resp_buf + n, sizeof(s_resp_buf) - n, "]}");
    resp->content_type = "application/json";
    resp->body         = (const uint8_t *)s_resp_buf;
    resp->body_len     = n;
}

static void post_tempo(const char *path, const char *body, size_t blen,
                       http_response_t *resp) {
    (void)path;
    char tmp[16] = {0};
    size_t n = blen < 15 ? blen : 15;
    memcpy(tmp, body, n);
    double bpm = atof(tmp);
    if (bpm < 20 || bpm > 999) {
        resp->status   = 400;
        resp->body     = (const uint8_t *)"bad tempo";
        resp->body_len = 9;
        return;
    }
    link_set_local_tempo(bpm);
    resp->content_type = "application/json";
    resp->body     = (const uint8_t *)"{\"ok\":true}";
    resp->body_len = 11;
}

static void post_play(const char *path, const char *body, size_t blen,
                      http_response_t *resp) {
    (void)path; (void)body; (void)blen;
    link_set_play(1);
    resp->content_type = "application/json";
    resp->body     = (const uint8_t *)"{\"ok\":true}";
    resp->body_len = 11;
}

static void post_stop(const char *path, const char *body, size_t blen,
                      http_response_t *resp) {
    (void)path; (void)body; (void)blen;
    link_set_play(0);
    resp->content_type = "application/json";
    resp->body     = (const uint8_t *)"{\"ok\":true}";
    resp->body_len = 11;
}

static void post_name(const char *path, const char *body, size_t blen,
                      http_response_t *resp) {
    (void)path;
    size_t n = blen < sizeof(s_peer_name) - 1 ? blen : sizeof(s_peer_name) - 1;
    memcpy(s_peer_name, body, n);
    s_peer_name[n] = 0;
    resp->content_type = "application/json";
    resp->body     = (const uint8_t *)"{\"ok\":true}";
    resp->body_len = 11;
}

static void post_subscribe(const char *path, const char *body, size_t blen,
                           http_response_t *resp) {
    (void)path;
    char tmp[80] = {0};
    size_t n = blen < 79 ? blen : 79;
    memcpy(tmp, body, n);
    int port, slot;
    char hex[20] = {0};
    if (sscanf(tmp, "%d %d %16s", &port, &slot, hex) == 3) {
        link_id_t id;
        if (hex_to_id(hex, &id) == 0 &&
            link_audio_subscribe(port, slot, &id) == 0) {
            resp->content_type = "application/json";
            resp->body     = (const uint8_t *)"{\"ok\":true}";
            resp->body_len = 11;
            return;
        }
    }
    resp->status   = 400;
    resp->body     = (const uint8_t *)"subscribe failed";
    resp->body_len = 16;
}

static void post_unsubscribe(const char *path, const char *body, size_t blen,
                             http_response_t *resp) {
    (void)path;
    char tmp[20] = {0};
    size_t n = blen < 19 ? blen : 19;
    memcpy(tmp, body, n);
    link_id_t id;
    if (hex_to_id(tmp, &id) == 0 && link_audio_unsubscribe(&id) == 0) {
        resp->content_type = "application/json";
        resp->body     = (const uint8_t *)"{\"ok\":true}";
        resp->body_len = 11;
        return;
    }
    resp->status   = 400;
    resp->body     = (const uint8_t *)"unsubscribe failed";
    resp->body_len = 18;
}

void webui_init(void) {
    http_route("GET",  "/",               serve_index);
    http_route("GET",  "/api/status",     serve_status);
    http_route("GET",  "/api/channels",   serve_channels);
    http_route("POST", "/api/tempo",      post_tempo);
    http_route("POST", "/api/play",       post_play);
    http_route("POST", "/api/stop",       post_stop);
    http_route("POST", "/api/name",       post_name);
    http_route("POST", "/api/subscribe",  post_subscribe);
    http_route("POST", "/api/unsubscribe",post_unsubscribe);
}
