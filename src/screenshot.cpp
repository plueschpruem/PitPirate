// PitPirate — screenshot capture & upload
//
// Reads the TFT display one row at a time via tft.readRect() and POSTs the
// raw pixel data to the server.  The server converts to PNG via GD.
//
// Wire format (no padding, all little-endian):
//   [0..3]  magic  0x52 0x41 0x57 0x31  ('RAW1')
//   [4..5]  width  as uint16_t LE
//   [6..7]  height as uint16_t LE
//   [8..]   raw uint16_t pixels, row-major, as returned by tft.readRect()
//           (each uint16_t is byte-swapped by the library for pushRect compat)
//
// Memory cost per call:
//   s_lineBuf  640 B  – one row of raw pixels, streamed straight to TCP
//   8-byte header built on the stack
//
// The total payload size is known before the first byte is sent, so a plain
// Content-Length POST is used — no chunked encoding needed.

#include "screenshot.h"
#include "config.h"
#include "debug_log.h"
#include "remote_post.h"
#include "display/display_config.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

static const char* ENDPOINT_PATH = "/php/screenshot.php";

// ── Geometry & payload size ───────────────────────────────────────────────────
static const int      PX_W          = SCREEN_W;                     // 320
static const int      PX_H          = SCREEN_H;                     // 240
static const int      ROW_BYTES     = PX_W * 2;                     // 640 B per row
static const uint32_t PAYLOAD_SZ    = 8u + (uint32_t)PX_H * ROW_BYTES; // 153 608 B

// One row buffer — kept static so the stack stays shallow.
static uint16_t s_lineBuf[PX_W];  // 640 B

// ── Parse "scheme://host[:port]" out of a base URL ───────────────────────────
static bool parseBaseUrl(const char* base,
                         bool& isHttps, char* host, size_t hostLen, uint16_t& port)
{
    if (!base || base[0] == '\0') return false;

    const char* rest;
    if      (strncmp(base, "https://", 8) == 0) { isHttps = true;  rest = base + 8; }
    else if (strncmp(base, "http://",  7) == 0) { isHttps = false; rest = base + 7; }
    else return false;

    strlcpy(host, rest, hostLen);

    // Strip any trailing path
    char* slash = strchr(host, '/');
    if (slash) *slash = '\0';

    // Extract optional explicit port
    char* colon = strchr(host, ':');
    if (colon) {
        port = (uint16_t)atoi(colon + 1);
        *colon = '\0';
    } else {
        port = isHttps ? 443 : 80;
    }
    return host[0] != '\0';
}

// ── Public entry point ────────────────────────────────────────────────────────
void screenshotUpload()
{
    if (WiFi.status() != WL_CONNECTED) {
        DLOGLN("[SHOT] WiFi not connected – skipping");
        return;
    }

    char base[256];
    remotePostGetBaseUrl(base);
    if (base[0] == '\0') {
        DLOGLN("[SHOT] No server URL configured – skipping");
        return;
    }

    bool     isHttps = false;
    char     host[128] = {};
    uint16_t port = 80;
    if (!parseBaseUrl(base, isHttps, host, sizeof(host), port)) {
        DLOGLN("[SHOT] Could not parse server URL");
        return;
    }

    // Host header value: include port only when it is non-standard
    char hostHdr[160];
    bool stdPort = (isHttps && port == 443) || (!isHttps && port == 80);
    if (stdPort) snprintf(hostHdr, sizeof(hostHdr), "%s",     host);
    else         snprintf(hostHdr, sizeof(hostHdr), "%s:%u",  host, (unsigned)port);

    // Get the runtime auth token (may differ from the compile-time default)
    char urlTmp[256] = {};
    char token[64]   = {};
    remotePostGetSettings(urlTmp, token);

    // ── Open TCP connection ───────────────────────────────────────────────────
    // Mirror http_task.cpp: shared static secure client, setInsecure() for HTTPS.
    static WiFiClientSecure sClient;
    WiFiClient              plainClient;
    WiFiClient*             conn;

    if (isHttps) {
        sClient.setInsecure();
        conn = &sClient;
    } else {
        conn = &plainClient;
    }

    DLOG("[SHOT] Connecting to %s:%u ...\n", host, (unsigned)port);
    if (!conn->connect(host, port)) {
        DLOGLN("[SHOT] Connection failed");
        return;
    }

    // ── HTTP request-line + headers ───────────────────────────────────────────
    char reqBuf[512];
    int  reqLen = snprintf(reqBuf, sizeof(reqBuf),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n",
        ENDPOINT_PATH, hostHdr, (unsigned long)PAYLOAD_SZ);
    conn->write((uint8_t*)reqBuf, (size_t)reqLen);

    if (token[0] != '\0') {
        char tokHdr[96];
        int  tLen = snprintf(tokHdr, sizeof(tokHdr),
                             "X-PitPirate-Token: %s\r\n", token);
        conn->write((uint8_t*)tokHdr, (size_t)tLen);
    }
    conn->write((uint8_t*)"\r\n", 2);   // blank line — end of headers

    // ── 8-byte binary header: magic + width + height ──────────────────────────
    uint8_t hdr[8] = {
        'R','A','W','1',
        (uint8_t)(PX_W),      (uint8_t)(PX_W >> 8),
        (uint8_t)(PX_H),      (uint8_t)(PX_H >> 8)
    };
    conn->write(hdr, 8);

    // ── Pixel rows: TFT → TCP, one row at a time, zero conversion ────────────
    DLOG("[SHOT] Streaming %d\xc3\x97%d raw565 (%lu B) ...\n",
         PX_W, PX_H, (unsigned long)PAYLOAD_SZ);

    for (int y = 0; y < PX_H; y++) {
        tft.readRect(0, y, PX_W, 1, s_lineBuf);
        conn->write((uint8_t*)s_lineBuf, (size_t)ROW_BYTES);
    }
    conn->flush();

    // ── Log the server response status line, then close ───────────────────────
    unsigned long deadline = millis() + 5000UL;
    while (conn->connected() && millis() < deadline) {
        if (conn->available()) {
            String line = conn->readStringUntil('\n');
            DLOG("[SHOT] ← %s\n", line.c_str());
            if (line == "\r" || line.length() == 0) break;  // end of headers
        }
    }
    conn->stop();
    DLOGLN("[SHOT] Done");
}
