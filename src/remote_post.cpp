// PitPirate — remote telemetry poster
// Enqueues HTTP jobs to the Core 0 http_task worker; never blocks Core 1.
//
// POST_INTERVAL_MS  — how often the telemetry POST job is enqueued (10 s)
// ALARM_SYNC_MS     — how often the alarm-limits GET job is enqueued (30 s)
// BLOWER_SYNC_MS    — how often the blower-settings GET job is enqueued (30 s)

#include "remote_post.h"
#include "http_task.h"
#include "shared_data.h"
#include "config.h"
#include "fan_control.h"
#include "display/display.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include "debug_log.h"

static const uint32_t ALARM_SYNC_MS         = 30000UL;  // 30 s alarm poll interval
static const uint32_t BLOWER_SYNC_MS        = 30000UL;  // 30 s blower/fan poll interval
static const uint32_t GRAPH_SYNC_MS         = 30000UL;  // 30 s between per-probe fetches (steady-state)
static const uint32_t GRAPH_BOOT_INTERVAL_MS =  5000UL;  // 5 s stagger during boot burst

// Look-back window for graph_esp.php.  Width and height are computed at request
// time from the current probe-grid layout so the bitmap maps 1:1 to the cell.
static const int GRAPH_MINUTES = 60;

static char          s_url[256]    = {};
static char          s_token[64]   = {};
static unsigned long s_last_post_ms     = 0;
static unsigned long s_last_alarm_sync  = 0;
static unsigned long s_last_blower_sync = 0;
static unsigned long s_last_graph_sync  = 0;
static int           s_graph_probe_next = 0;    // 0..5, cycles through all probes
static int           s_graph_burst_left = -1;   // -1 = discover on first sync; >0 = burst in progress; 0 = steady-state
static bool          s_first            = true;   // post immediately on first loop

// Configurable telemetry behaviour (persisted in NVS)
static uint32_t      s_post_interval_ms = 10000UL;  // 10 s default; valid: 10/30/60/180 s
static bool          s_post_on_change   = false;    // when true: only post if data changed
static char          s_last_json[600]   = {};       // last-enqueued probe JSON for change detection

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

// Adaptive interval: always at least base_ms, but at least last_ms + 2 s
// (so the next request cannot fire until the previous one has been done for
// 2 s). Never more than 5x the base to avoid extreme back-off.
// Computes a rate-limited poll interval that is at least base_ms but stretched
// by 2 s past the last observed server response time (last_ms), capped at 5x
// the base to avoid runaway back-off.
// @param base_ms  The nominal interval in milliseconds.
// @param last_ms  The duration of the most recent completed request in milliseconds.
// @return  Effective interval in milliseconds.
static uint32_t adaptiveInterval(uint32_t base_ms, uint32_t last_ms)
{
    uint32_t stretched = last_ms + 2000UL;
    uint32_t interval  = (stretched > base_ms) ? stretched : base_ms;
    uint32_t cap       = base_ms * 5UL;
    return (interval < cap) ? interval : cap;
}

// Strip any .php filename and trailing /php directory from s_url so that all
// endpoint builders start from the bare server root (e.g. "https://host.com").
// Strips any .php filename and trailing /php path segment from s_url to produce
// the bare server root (e.g. "https://host.com").
// @param out  256-byte buffer that receives the server base URL.
static void extractServerBase(char out[256]) {
    strlcpy(out, s_url, 256);
    int len = (int)strlen(out);
    while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
    char* lastSlash = strrchr(out, '/');
    if (lastSlash && strstr(lastSlash, ".php")) {
        *lastSlash = '\0';
        len = (int)strlen(out);
    }
    if (len >= 4 && strcmp(out + len - 4, "/php") == 0) {
        out[len - 4] = '\0';
    }
}

// Build an endpoint URL from the stored base.
// Appends suffix to the extracted server base URL and writes the result to out.
// @param out     Output buffer to receive the full endpoint URL.
// @param outLen  Size of out in bytes.
// @param suffix  Path suffix to append, e.g. "/php/telemetry.php".
// @return true on success; false if the server URL is not configured.
static bool buildUrl(char* out, size_t outLen, const char* suffix)
{
    char base[256];
    extractServerBase(base);
    if (base[0] == '\0') return false;
    snprintf(out, outLen, "%s%s", base, suffix);
    return true;
}

// ---------------------------------------------------------------------------
// Job enqueuers  (run on Core 1, return immediately)
// ---------------------------------------------------------------------------

// Builds the telemetry POST payload (timestamp, uptime, RSSI + probe JSON)
// and enqueues it for Core 0 execution.  No-op when no URL is configured or
// WiFi is not connected.
static void enqueuePost()
{
    if (s_url[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    // Snapshot all volatile data on Core 1 before handing to Core 0.
    time_t now_ts = 0;
    struct tm ti;
    if (getLocalTime(&ti, 0)) now_ts = mktime(&ti);
    unsigned long uptime_s = millis() / 1000;
    int32_t rssi = WiFi.RSSI();

    String jd = jsonData;
    if (jd.startsWith("[") && jd.endsWith("]"))
        jd = jd.substring(1, jd.length() - 1);

    // Capture the probe JSON for change detection in remotePostLoop.
    strlcpy(s_last_json, jd.c_str(), sizeof(s_last_json));

    String payload;
    if (jd.startsWith("{") && jd.endsWith("}")) {
        payload = String("{\"ts\":")     + String((long)now_ts)
                + String(",\"uptime\":") + String(uptime_s)
                + String(",\"rssi\":")   + String(rssi)
                + String(",")
                + jd.substring(1);
    } else {
        payload = String("{\"ts\":")     + String((long)now_ts)
                + String(",\"uptime\":") + String(uptime_s)
                + String(",\"rssi\":")   + String(rssi)
                + String(",\"status\":\"no_data\"}");
    }

    HttpJob job = {};
    job.type      = HTTP_JOB_TELEMETRY_POST;
    job.timeoutMs = 5000;
    if (!buildUrl(job.url, sizeof(job.url), "/php/telemetry.php")) return;
    strlcpy(job.token, s_token, sizeof(job.token));
    strlcpy(job.body,  payload.c_str(), sizeof(job.body));

    if (!httpEnqueue(job))
        DLOGLN("[Remote] telemetry skipped - already pending");
}

// Enqueues an alarm-limits sync GET for Core 0.  The server response is parsed
// on Core 0; any changed limits are written directly to NVS.
static void enqueueAlarmSync()
{
    if (s_url[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    HttpJob job = {};
    job.type      = HTTP_JOB_ALARM_SYNC;
    job.timeoutMs = 5000;
    if (!buildUrl(job.url, sizeof(job.url), "/php/telemetry_get.php?probe=alarms")) return;

    if (!httpEnqueue(job))
        DLOGLN("[Remote] alarm-sync skipped - already pending");
}

// Enqueues a blower-settings sync GET for Core 0.  The server response is
// forwarded to Core 1 via the result queue for safe fan-state application.
static void enqueueBlowerSync()
{
    if (s_url[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    HttpJob job = {};
    job.type      = HTTP_JOB_BLOWER_SYNC;
    job.timeoutMs = 5000;
    if (!buildUrl(job.url, sizeof(job.url), "/php/blower.php")) return;

    if (!httpEnqueue(job))
        DLOGLN("[Remote] blower-sync skipped - already pending");
}

// Enqueues a graph-data GET for the specified probe index.
// Cell pixel dimensions are computed from the current probe layout so the
// server renders a bitmap that maps 1:1 to the on-screen cell.
// @param probeIdx  0-based probe index (0–5).
static void enqueueGraphSync(int probeIdx)
{
    if (s_url[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    // Compute the actual probe-cell pixel dimensions for the current layout.
    // The PHP server renders at exactly this size so no scaling is needed on-device.
    int n = 0;
    for (int i = 0; i < 6; i++)
        if (!isnan(lastKnownVals.probe[i])) n++;
    if (n < 1) n = 1;  // safe default before first data arrives
    int cols, rows;
    getProbeLayout(n, cols, rows);
    int gw = SCREEN_W / cols;          // cell width  (e.g. 160 for 2 probes)
    int gh = PROBE_AREA_H / rows;      // cell height (e.g. 176 for 1 row)

    HttpJob job = {};
    job.type      = HTTP_JOB_GRAPH_FETCH;
    job.timeoutMs = 10000;
    job.extra     = (uint8_t)probeIdx;  // 0-5
    char suffix[96];
    snprintf(suffix, sizeof(suffix),
             "/php/graph_esp.php?probe=%d&w=%d&h=%d&minutes=%d",
             probeIdx + 1, gw, gh, GRAPH_MINUTES);
    if (!buildUrl(job.url, sizeof(job.url), suffix)) return;
    strlcpy(job.token, s_token, sizeof(job.token));

    if (!httpEnqueue(job))
        DLOG("[Remote] graph-sync probe %d skipped - already pending\n", probeIdx + 1);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Initialises the remote-post module: loads the server URL and auth token from
// NVS (falling back to config.h compile-time defaults), and reads the configurable
// telemetry interval and on-change flag.  Must be called once from setup().
void remotePostInit()
{
    String url   = preferences.isKey("rpost_url")   ? preferences.getString("rpost_url")   : String(REMOTE_POST_URL);
    String token = preferences.isKey("rpost_token") ? preferences.getString("rpost_token") : String(REMOTE_POST_TOKEN);
    strlcpy(s_url,   url.c_str(),   sizeof(s_url));
    strlcpy(s_token, token.c_str(), sizeof(s_token));
    s_last_post_ms  = 0;
    s_last_graph_sync = 0;   // ensures first burst fires on the very first background tick
    s_graph_burst_left = -1; // re-discover active probes after each remotePostInit
    s_graph_probe_next = 0;
    s_first         = true;

    // Load configurable telemetry settings from NVS.
    uint32_t interval_s = preferences.isKey("rpost_interval") ? preferences.getUInt("rpost_interval") : 10U;
    if (interval_s != 10 && interval_s != 30 && interval_s != 60 && interval_s != 180) interval_s = 10;
    s_post_interval_ms = interval_s * 1000UL;
    s_post_on_change   = preferences.isKey("rpost_on_change") ? (preferences.getUChar("rpost_on_change") != 0) : false;
}

// Periodic scheduler; must be called every loop() iteration.
// Enqueues telemetry, alarm-sync, blower-sync, and graph jobs according to
// their respective adaptive intervals.  At most one background-sync job is
// enqueued per iteration to avoid saturating the HTTP worker queue.
void remotePostLoop()
{
    if (s_url[0] == '\0') return;
    unsigned long now = millis();

    // Adaptive intervals: stretch when the server was slow.
    uint32_t postInterval   = adaptiveInterval(s_post_interval_ms, (uint32_t)g_postDurationMs);
    uint32_t alarmInterval  = adaptiveInterval(ALARM_SYNC_MS,      (uint32_t)g_alarmSyncDurationMs);
    uint32_t blowerInterval = adaptiveInterval(BLOWER_SYNC_MS,     (uint32_t)g_blowerSyncDurationMs);

    static uint32_t s_lastLoggedInterval = 0;
    if (postInterval != s_post_interval_ms && postInterval != s_lastLoggedInterval) {
        s_lastLoggedInterval = postInterval;
        DLOG("[Remote] adaptive post interval: %lu ms (last %lu ms)\n",
                      (unsigned long)postInterval, (unsigned long)g_postDurationMs);
    } else if (postInterval == s_post_interval_ms && s_lastLoggedInterval != 0) {
        s_lastLoggedInterval = 0;
        DLOGLN("[Remote] post interval back to normal");
    }

    if (!s_first && (now - s_last_post_ms) < postInterval) {
        // on_change mode: post immediately whenever probe data differs from last post.
        if (s_post_on_change) {
            String cur = jsonData;
            if (cur.startsWith("[") && cur.endsWith("]"))
                cur = cur.substring(1, cur.length() - 1);
            if (cur != String(s_last_json)) {
                s_last_post_ms = now;
                enqueuePost();
                return;
            }
        }
        // Only enqueue one background sync per loop tick.
        if ((now - s_last_alarm_sync) >= alarmInterval) {
            s_last_alarm_sync = now;
            enqueueAlarmSync();
        } else if ((now - s_last_blower_sync) >= blowerInterval) {
            s_last_blower_sync = now;
            enqueueBlowerSync();
        } else if ((now - s_last_graph_sync) >= (s_graph_burst_left > 0 ? GRAPH_BOOT_INTERVAL_MS : GRAPH_SYNC_MS)) {
            s_last_graph_sync = now;

            // On first sync: discover how many active probes we have and kick off the
            // boot burst so all of them get a graph as quickly as possible after startup.
            if (s_graph_burst_left < 0) {
                int n = 0;
                for (int i = 0; i < 6; i++)
                    if (!isnan(lastKnownVals.probe[i])) n++;
                if (n > 0) {
                    s_graph_burst_left  = n;
                    s_graph_probe_next  = 0;  // always start from probe 0 at boot
                    DLOG("[Remote] graph boot burst: fetching %d active probe(s)\n", n);
                } else {
                    // No probe data yet — stay in discovery state and retry next tick.
                    s_graph_burst_left = -1;
                    return;
                }
            }

            // Advance past inactive slots.
            int checked = 0;
            while (checked < 6 && isnan(lastKnownVals.probe[s_graph_probe_next])) {
                s_graph_probe_next = (s_graph_probe_next + 1) % 6;
                checked++;
            }
            if (!isnan(lastKnownVals.probe[s_graph_probe_next])) {
                enqueueGraphSync(s_graph_probe_next);
                if (s_graph_burst_left > 0) s_graph_burst_left--;
            }
            s_graph_probe_next = (s_graph_probe_next + 1) % 6;
        }
        return;
    }

    // Interval fired — always post (serves as heartbeat in on_change mode too).
    s_first        = false;
    s_last_post_ms = now;
    enqueuePost();
}

// Persists a new server URL and auth token to NVS and updates the in-memory
// copies so the next telemetry post uses the new endpoint immediately.
// @param url    Full server URL (e.g. "https://host.com/php/telemetry.php").
// @param token  Optional Bearer / API token sent as X-PitPirate-Token header.
void remotePostApplySettings(const char* url, const char* token)
{
    preferences.putString("rpost_url",   url);
    preferences.putString("rpost_token", token);
    strlcpy(s_url,   url,   sizeof(s_url));
    strlcpy(s_token, token, sizeof(s_token));
    s_last_post_ms = 0;
    s_first        = true;
    DLOG("[Remote] settings updated -> %s\n", url);
}

// Copies the current server URL and auth token into the caller-supplied buffers.
// @param url_out    256-byte output buffer for the server URL.
// @param token_out  64-byte output buffer for the auth token.
void remotePostGetSettings(char url_out[256], char token_out[64])
{
    strlcpy(url_out,   s_url,   256);
    strlcpy(token_out, s_token, 64);
}

// Writes the bare server root URL (stripped of any /php prefix and .php filename)
// into base_out.  Used by alarm_notify.cpp to construct the alarm endpoint.
// @param base_out  256-byte output buffer for the base URL.
void remotePostGetBaseUrl(char base_out[256])
{
    extractServerBase(base_out);
}

// Builds the current alarm-limits JSON (all 7 probes) and enqueues a POST
// to /php/telemetry.php?action=alarms so the server stores the latest limits.
// No-op when no URL is configured or WiFi is not connected.
void remotePostAlarmConfig()
{
    if (s_url[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    // Build JSON on Core 1 before enqueue.
    String json = "{";
    for (int i = 1; i <= 7; i++) {
        int lo = preferences.getInt(("alm_" + String(i) + "_lo").c_str(), 0);
        int hi = preferences.getInt(("alm_" + String(i) + "_hi").c_str(), 0);
        if (i > 1) json += ",";
        json += "\"" + String(i) + "\":{\"lo\":" + String(lo) + ",\"hi\":" + String(hi) + "}";
    }
    json += "}";

    HttpJob job = {};
    job.type      = HTTP_JOB_ALARM_CONFIG_POST;
    job.timeoutMs = 5000;
    if (!buildUrl(job.url, sizeof(job.url), "/php/telemetry.php?action=alarms")) return;
    strlcpy(job.token, s_token, sizeof(job.token));
    strlcpy(job.body,  json.c_str(), sizeof(job.body));

    if (!httpEnqueue(job))
        Serial.println("[Remote] alarm-config job queue full - dropped");
}

// Snapshots the current fan pct/start/min settings and enqueues a POST to
// /php/blower.php so the server stores the latest blower configuration.
// No-op when no URL is configured or WiFi is not connected.
void remotePostBlowerConfig()
{
    if (s_url[0] == '\0') return;
    if (WiFi.status() != WL_CONNECTED) return;

    // Snapshot fan state on Core 1 before handing to Core 0.
    String json = String("{\"pct\":")   + String(fanGetPercent())  +
                  String(",\"start\":") + String(fanGetStartPct()) +
                  String(",\"min\":")   + String(fanGetMinPct())   + "}";

    HttpJob job = {};
    job.type      = HTTP_JOB_BLOWER_POST;
    job.timeoutMs = 5000;
    if (!buildUrl(job.url, sizeof(job.url), "/php/blower.php")) return;
    strlcpy(job.body, json.c_str(), sizeof(job.body));

    if (!httpEnqueue(job))
        Serial.println("[Remote] blower-config job queue full - dropped");
}

// ---------------------------------------------------------------------------
// Telemetry post settings: configurable interval + on-change mode
// ---------------------------------------------------------------------------

// Reads the current telemetry interval and on-change flag into the caller's variables.
// @param interval_s_out  Receives the telemetry POST interval in seconds (10/30/60/180).
// @param on_change_out   Receives true when only post-on-data-change mode is active.
void remotePostGetTelemetryConfig(uint32_t& interval_s_out, bool& on_change_out)
{
    interval_s_out = s_post_interval_ms / 1000UL;
    on_change_out  = s_post_on_change;
}

// Applies a new telemetry interval and on-change flag, persisting both to NVS.
// Invalid interval values are silently rounded to 10 s.
// @param interval_s  New POST interval in seconds; must be 10, 30, 60, or 180.
// @param on_change   When true, also post immediately whenever probe data changes.
void remotePostApplyTelemetryConfig(uint32_t interval_s, bool on_change)
{
    // Only allow the four permitted values; fall back to 10 s on anything else.
    if (interval_s != 10 && interval_s != 30 && interval_s != 60 && interval_s != 180)
        interval_s = 10;
    s_post_interval_ms = interval_s * 1000UL;
    s_post_on_change   = on_change;
    preferences.putUInt("rpost_interval",  interval_s);
    preferences.putUChar("rpost_on_change", on_change ? 1 : 0);
    DLOG("[Remote] telemetry config: interval=%lus on_change=%d\n",
         (unsigned long)interval_s, (int)on_change);
}
