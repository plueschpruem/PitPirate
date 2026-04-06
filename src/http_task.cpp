// PitPirate — Core 0 HTTP worker
//
// Runs a FreeRTOS task pinned to Core 0 (pro_cpu).  Core 1 (app_cpu) runs the
// Arduino loop() / display / touch.  All blocking HTTPClient calls happen here
// so Core 1 is never stalled waiting for a server response.
//
// Queue depths are intentionally small: the telemetry POST fires every 10 s so
// one slot is plenty; the depth-4 job queue absorbs a burst of one-off POSTs
// (alarm, blower config) that may be enqueued while a slow request is in flight.

#include "http_task.h"
#include "fan_control.h"
#include "display/display.h"   // displayProbeSetGraph
#include "config.h"
#include "shared_data.h"   // preferences
#include "debug_log.h"
#include "remote_post.h"   // remotePostGetBaseUrl

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ── Global duration counters (read by remote_post.cpp) ───────────────────────
volatile uint32_t g_postDurationMs       = 0;
volatile uint32_t g_alarmSyncDurationMs  = 0;
volatile uint32_t g_blowerSyncDurationMs = 0;

// ── Queues ────────────────────────────────────────────────────────────────────
static QueueHandle_t s_jobQueue         = nullptr;  // Core 1 → Core 0 : HttpJob
static QueueHandle_t s_resultQueue      = nullptr;  // Core 0 → Core 1 : BlowerResult
static QueueHandle_t s_graphResultQueue = nullptr;  // Core 0 → Core 1 : GraphResult

static const int JOB_QUEUE_DEPTH    = 4;
static const int RESULT_QUEUE_DEPTH = 2;

// ── Connectivity-alert rate-limit ────────────────────────────────────────────
static const uint32_t CONNECTIVITY_NOTIF_COOLDOWN_MS = 5UL * 60UL * 1000UL; // 5 min
static unsigned long  s_lastConnectivityNotifMs = 0;

// ── Per-type dedup ────────────────────────────────────────────────────────────
// Bit N is set while a job of type N is already in the queue (or being run).
// Prevents the same job type from stacking up in the queue.
static portMUX_TYPE      s_pendingMux  = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_pendingMask = 0;

// ── Parse a single int value after "key": in a JSON string ───────────────────
// Extracts a single integer field from a JSON string using a simple scan.
// @param json  JSON string to search.
// @param key   JSON key token including the opening quote, e.g. "\"pct\"".
// @param out   Receives the parsed integer value on success.
// @return true if the key was found and a number extracted; false otherwise.
static bool parseJsonInt(const String& json, const char* key, int& out) {
    int idx = json.indexOf(key);
    if (idx < 0) return false;
    idx += (int)strlen(key);
    while (idx < (int)json.length() && (json[idx] == ' ' || json[idx] == ':')) idx++;
    out = json.substring(idx).toInt();
    return true;
}

// ── Enqueue a push notification for a slow or failed HTTP request ───────────────
// Builds and enqueues a push-notification POST that reports an HTTP connectivity
// problem.  Rate-limited by CONNECTIVITY_NOTIF_COOLDOWN_MS to avoid flooding.
// @param isFailed    true when the request returned an error code (<= 0).
// @param durationMs  How long the request took in milliseconds.
// @param code        The HTTP response code (negative for transport errors).
static void enqueueConnectivityAlert(bool isFailed, uint32_t durationMs, int code)
{
    char base[256] = {};
    remotePostGetBaseUrl(base);
    if (base[0] == '\0') return;

    char alarm_url[320];
    snprintf(alarm_url, sizeof(alarm_url), "%s/php/alarm.php", base);

    char message[128];
    if (isFailed)
        snprintf(message, sizeof(message), "HTTP request failed (code %d)", code);
    else
        snprintf(message, sizeof(message), "HTTP request slow: %lu s",
                 (unsigned long)(durationMs / 1000UL));

    char payload[400];
    snprintf(payload, sizeof(payload),
             "{\"secret\":\"%s\","
             "\"title\":\"\xe2\x9a\xa0\xef\xb8\x8f PitPirate HTTP Error\","
             "\"message\":\"%s\","
             "\"tag\":\"pitpirate-http-error\","
             "\"silent\":false}",
             ALARM_SECRET, message);

    HttpJob alertJob = {};
    alertJob.type      = HTTP_JOB_CONNECTIVITY_ALERT;
    alertJob.timeoutMs = 8000;
    strlcpy(alertJob.url,  alarm_url, sizeof(alertJob.url));
    strlcpy(alertJob.body, payload,   sizeof(alertJob.body));
    DLOG("[HTTP0] connectivity alert: %s\n", message);
    httpEnqueue(alertJob);  // dedup prevents stacking if already queued
}

// ── Execute one HTTP job on Core 0 ───────────────────────────────────────────
// Executes a single HTTP job on Core 0.  Handles all job types defined in
// HttpJobType: telemetry/alarm/blower/alarm-config POSTs, alarm-sync and
// blower-sync GETs, and graph-column-data fetches.
// After execution, optionally enqueues a connectivity-alert if the request
// was slow (> 20 s) or failed.
// @param job  The fully populated HttpJob descriptor to execute.
static void executeJob(const HttpJob& job)
{
    if (WiFi.status() != WL_CONNECTED) {
        DLOG("[HTTP0] skip %d – WiFi not connected\n", (int)job.type);
        return;
    }

    HTTPClient http;
    static WiFiClientSecure sclient;   // one shared secure client; jobs run serially

    bool isHttps = (strncmp(job.url, "https://", 8) == 0);
    if (isHttps) {
        sclient.setInsecure();
        http.begin(sclient, job.url);
    } else {
        http.begin(job.url);
    }

    http.setReuse(false);
    http.setTimeout(job.timeoutMs);

    unsigned long t0 = millis();
    int code = -1;

    switch (job.type) {

    // ── Fire-and-forget POSTs ─────────────────────────────────────────────
    case HTTP_JOB_TELEMETRY_POST:
        if (job.token[0] != '\0') http.addHeader("X-PitPirate-Token", job.token);
        http.addHeader("Content-Type", "application/json");
        DLOG("[HTTP0] telemetry POST → %s\n", job.url);
        code = http.POST((uint8_t*)job.body, strlen(job.body));
        g_postDurationMs = (uint32_t)(millis() - t0);
        DLOG("[HTTP0] telemetry POST ← %d  (%lu ms)\n", code, g_postDurationMs);
        if (code <= 0)
            DLOG("[HTTP0] POST failed: %s\n", HTTPClient::errorToString(code).c_str());
        break;

    case HTTP_JOB_ALARM_POST:
        http.addHeader("Content-Type", "application/json");
        DLOG("[HTTP0] alarm POST → %s\n", job.url);
        code = http.POST((uint8_t*)job.body, strlen(job.body));
        DLOG("[HTTP0] alarm POST ← %d  (%lu ms)\n", code, (unsigned long)(millis() - t0));
        if (code <= 0)
            DLOG("[HTTP0] alarm POST failed: %s\n", HTTPClient::errorToString(code).c_str());
        break;

    case HTTP_JOB_BLOWER_POST:
        http.addHeader("Content-Type", "application/json");
        DLOG("[HTTP0] blower POST → %s\n", job.url);
        code = http.POST((uint8_t*)job.body, strlen(job.body));
        DLOG("[HTTP0] blower POST ← %d  (%lu ms)\n", code, (unsigned long)(millis() - t0));
        break;

    case HTTP_JOB_ALARM_CONFIG_POST:
        if (job.token[0] != '\0') http.addHeader("X-PitPirate-Token", job.token);
        http.addHeader("Content-Type", "application/json");
        DLOG("[HTTP0] alarm-config POST → %s\n", job.url);
        code = http.POST((uint8_t*)job.body, strlen(job.body));
        DLOG("[HTTP0] alarm-config POST ← %d  (%lu ms)\n", code, (unsigned long)(millis() - t0));
        break;
    case HTTP_JOB_CONNECTIVITY_ALERT:
        http.addHeader("Content-Type", "application/json");
        DLOG("[HTTP0] connectivity alert POST \u2192 %s\n", job.url);
        code = http.POST((uint8_t*)job.body, strlen(job.body));
        DLOG("[HTTP0] connectivity alert POST \u2190 %d  (%lu ms)\n", code, (unsigned long)(millis() - t0));
        if (code <= 0)
            DLOG("[HTTP0] connectivity alert POST failed: %s\n", HTTPClient::errorToString(code).c_str());
        break;
    // ── GETs with result handling ─────────────────────────────────────────
    case HTTP_JOB_ALARM_SYNC: {
        DLOG("[HTTP0] alarm-sync GET → %s\n", job.url);
        code = http.GET();
        g_alarmSyncDurationMs = (uint32_t)(millis() - t0);
        DLOG("[HTTP0] alarm-sync GET ← %d  (%lu ms)\n", code, g_alarmSyncDurationMs);
        if (code != 200) break;

        String body = http.getString();
        // body: {"1":{"lo":0,"hi":120},...}  — apply to NVS on Core 0.
        // NVS writes are infrequent (every 30 s) so the risk of a race with Core 1
        // NVS reads is negligible; both sides use the same Preferences namespace.
        bool anyChange = false;
        for (int i = 1; i <= 7; i++) {
            // Match "N": then skip optional whitespace before '{'
            String probe_key = "\"" + String(i) + "\":";
            int start = body.indexOf(probe_key);
            if (start < 0) continue;
            start += probe_key.length();
            while (start < (int)body.length() && body[start] == ' ') start++;
            if (start >= (int)body.length() || body[start] != '{') continue;
            int end = body.indexOf('}', start);
            if (end < 0) continue;
            String sub = body.substring(start, end + 1);

            int lo = 0, hi = 0;
            if (!parseJsonInt(sub, "\"lo\"", lo)) continue;
            if (!parseJsonInt(sub, "\"hi\"", hi)) continue;

            String lo_key = "alm_" + String(i) + "_lo";
            String hi_key = "alm_" + String(i) + "_hi";
            int cur_lo = preferences.getInt(lo_key.c_str(), -1);
            int cur_hi = preferences.getInt(hi_key.c_str(), -1);
            if (lo != cur_lo || hi != cur_hi) {
                preferences.putInt(lo_key.c_str(), lo);
                preferences.putInt(hi_key.c_str(), hi);
                anyChange = true;
            }
        }
        if (anyChange) DLOGLN("[HTTP0] alarm limits synced from server");
        break;
    }

    case HTTP_JOB_BLOWER_SYNC: {
        DLOG("[HTTP0] blower-sync GET → %s\n", job.url);
        code = http.GET();
        g_blowerSyncDurationMs = (uint32_t)(millis() - t0);
        DLOG("[HTTP0] blower-sync GET ← %d  (%lu ms)\n", code, g_blowerSyncDurationMs);
        if (code != 200) break;

        String body = http.getString();
        int pct = -1, startPct = -1, minPct = -1;
        if (!parseJsonInt(body, "\"pct\"",   pct))      break;
        if (!parseJsonInt(body, "\"start\"", startPct)) break;
        if (!parseJsonInt(body, "\"min\"",   minPct))   break;

        // Send result to Core 1 for safe application.
        BlowerResult res = { pct, startPct, minPct };
        xQueueSend(s_resultQueue, &res, 0);  // non-blocking; drop if full
        break;
    }

    case HTTP_JOB_GRAPH_FETCH: {
        // Parse dimensions from the URL first — used even if Content-Length is absent
        // (server may use chunked transfer or gzip, making getSize() return -1).
        // URL format: .../graph_esp.php?probe=N&w=WW&h=HH&minutes=MM
        int gw = 0, gh = 0;
        {
            const char* wp = strstr(job.url, "&w=");
            const char* hp = strstr(job.url, "&h=");
            if (wp) gw = atoi(wp + 3);
            if (hp) gh = atoi(hp + 3);
        }
        if (gw <= 0 || gh <= 0 || gw > 320) {
            DLOG("[HTTP0] graph-fetch: bad dimensions in URL w=%d h=%d\n", gw, gh);
            break;
        }
        int expected = gw;  // server sends w bytes: one bar-height value per column

        if (job.token[0] != '\0') http.addHeader("X-PitPirate-Token", job.token);
        // Force HTTP/1.0 so the server cannot use chunked transfer-encoding.
        // Chunked responses embed hex chunk-size lines in the body stream which
        // corrupt the raw binary payload when read via stream->readBytes().
        http.useHTTP10(true);
        // Opt-in to collecting the temperature-range headers sent by graph_esp.php.
        static const char* kGraphHeaders[] = { "X-Graph-TMin", "X-Graph-TMax" };
        http.collectHeaders(kGraphHeaders, 2);
        DLOG("[HTTP0] graph-fetch GET \u2192 %s\n", job.url);
        code = http.GET();
        DLOG("[HTTP0] graph-fetch GET \u2190 %d  (%lu ms)\n", code,
             (unsigned long)(millis() - t0));
        if (code != 200) break;

        // Content-Length may be -1 if the server uses chunked / gzip encoding.
        // Verify only when the header is present; otherwise trust URL dimensions.
        int contentLen = http.getSize();
        if (contentLen >= 0 && contentLen != expected) {
            DLOG("[HTTP0] graph-fetch: content-length %d != expected %d\n",
                 contentLen, expected);
            break;
        }

        uint8_t* buf = (uint8_t*)malloc(expected);
        if (!buf) {
            DLOGLN("[HTTP0] graph-fetch: malloc failed");
            break;
        }

        WiFiClient* stream = http.getStreamPtr();
        int got = stream->readBytes(buf, expected);
        if (got != expected) {
            DLOG("[HTTP0] graph-fetch: short read %d/%d\n", got, expected);
            free(buf);
            break;
        }

        GraphResult res = {};
        res.probeIdx = job.extra;       // 0–5, set by remote_post
        res.w        = (uint16_t)gw;
        res.h        = (uint16_t)gh;
        res.tMin     = http.header("X-Graph-TMin").toFloat();
        res.tMax     = http.header("X-Graph-TMax").toFloat();
        res.buf      = buf;

        if (xQueueSend(s_graphResultQueue, &res, 0) != pdTRUE) {
            DLOG("[HTTP0] graph-fetch: result queue full, dropping probe %d\n",
                 (int)job.extra + 1);
            free(buf);  // queue full — discard
        }
        break;
    }

    } // switch

    http.end();

    // ── Push notification for slow (>20 s) or failed requests ────────────────────
    // Excluded for the alert itself to avoid recursion.
    if (job.type != HTTP_JOB_CONNECTIVITY_ALERT) {
        uint32_t elapsed = (uint32_t)(millis() - t0);
        if (code <= 0 || elapsed > 20000UL) {
            unsigned long now = millis();
            if (now - s_lastConnectivityNotifMs >= CONNECTIVITY_NOTIF_COOLDOWN_MS) {
                s_lastConnectivityNotifMs = (now == 0) ? 1UL : now;
                enqueueConnectivityAlert(code <= 0, elapsed, code);
            }
        }
    }
}

// ── Public: connectivity alert state (readable from Core 1) ──────────────────
// Returns true if a connectivity-alert push notification was sent within the
// past CONNECTIVITY_NOTIF_COOLDOWN_MS window.  Callable from Core 1 (display).
bool httpConnectivityAlertActive()
{
    unsigned long last = s_lastConnectivityNotifMs;
    if (last == 0) return false;
    return (millis() - last) < CONNECTIVITY_NOTIF_COOLDOWN_MS;
}

// ── Core 0 task ──────────────────────────────────────────────────────────────
static void httpTaskFn(void* /*pvParameters*/)
{
    HttpJob job;
    for (;;) {
        // Block until a job arrives (no timeout → task sleeps when idle)
        if (xQueueReceive(s_jobQueue, &job, portMAX_DELAY) == pdTRUE) {
            // Clear the pending bit before executing so a new job of this
            // type can be enqueued while this one runs.
            taskENTER_CRITICAL(&s_pendingMux);
            s_pendingMask &= ~(1u << (uint32_t)job.type);
            taskEXIT_CRITICAL(&s_pendingMux);
            executeJob(job);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

// Creates the job, result, and graph-result queues and pins the HTTP worker
// task to Core 0 at priority 5.  Must be called once from setup() after
// remotePostInit() so the server URL is ready before the first job fires.
void httpTaskInit()
{
    s_jobQueue         = xQueueCreate(JOB_QUEUE_DEPTH,    sizeof(HttpJob));
    s_resultQueue      = xQueueCreate(RESULT_QUEUE_DEPTH, sizeof(BlowerResult));
    s_graphResultQueue = xQueueCreate(6,                  sizeof(GraphResult));  // one slot per probe

    // Pin to Core 0, priority 5, 12 KB stack (TLS handshake needs headroom)
    xTaskCreatePinnedToCore(
        httpTaskFn,
        "http_task",
        12288,       // stack bytes
        nullptr,
        5,           // priority
        nullptr,
        0            // Core 0
    );

    DLOGLN("[HTTP0] HTTP worker task started on Core 0");
}

// Enqueues an HTTP job for execution on Core 0.  Thread-safe; callable from any core.
// Uses a per-type deduplication bitmask so only one job of each type is ever
// in the queue simultaneously (prevents stacking on slow servers).
// @param job  The HttpJob to enqueue; must be fully populated before calling.
// @return true if the job was accepted; false if a job of the same type is
//         already queued or the queue is full.
bool httpEnqueue(const HttpJob& job)
{
    if (!s_jobQueue) return false;
    uint32_t bit = 1u << (uint32_t)job.type;
    taskENTER_CRITICAL(&s_pendingMux);
    bool already = (s_pendingMask & bit) != 0;
    if (!already) s_pendingMask |= bit;
    taskEXIT_CRITICAL(&s_pendingMux);
    if (already) return false;  // same type already queued – drop
    bool ok = xQueueSend(s_jobQueue, &job, 0) == pdTRUE;
    if (!ok) {
        // Queue genuinely full (shouldn't happen often with dedup)
        taskENTER_CRITICAL(&s_pendingMux);
        s_pendingMask &= ~bit;
        taskEXIT_CRITICAL(&s_pendingMux);
    }
    return ok;
}

// Drains at most one BlowerResult from the inter-core result queue and applies
// the contained pct/startPct/minPct values to the fan-control module.
// Must be called from Core 1 (loop()) to ensure fan state is updated safely.
void httpTaskDrainResults()
{
    if (!s_resultQueue) return;
    BlowerResult res;
    // Process at most one result per loop() tick
    if (xQueueReceive(s_resultQueue, &res, 0) == pdTRUE) {
        if (res.pct < 0) return;  // sentinel: no-op

        bool anyChange = false;
        if (res.pct      != (int)fanGetPercent())  { fanSetPercent((uint8_t)res.pct);           anyChange = true; }
        if (res.startPct != (int)fanGetStartPct()) { fanSetStartPct((uint8_t)res.startPct);     anyChange = true; }
        if (res.minPct   != (int)fanGetMinPct())   { fanSetMinPct((uint8_t)res.minPct);         anyChange = true; }
        if (anyChange)
            DLOG("[HTTP0] blower applied: pct=%d start=%d min=%d\n",
                          res.pct, res.startPct, res.minPct);
    }
}

// Drains at most one GraphResult from the inter-core graph queue and hands the
// pixel buffer to displayProbeSetGraph().  The display module takes ownership
// of the buffer.  Must be called from Core 1 (loop()).
void httpTaskDrainGraphResults()
{
    if (!s_graphResultQueue) return;
    GraphResult res;
    // Process at most one graph result per loop() tick to avoid stalling Core 1
    if (xQueueReceive(s_graphResultQueue, &res, 0) == pdTRUE) {
        // displayProbeSetGraph takes ownership of res.buf (frees old buffer, stores new one)
        displayProbeSetGraph(res.probeIdx, res.buf, res.w, res.h, res.tMin, res.tMax);
    }
}
