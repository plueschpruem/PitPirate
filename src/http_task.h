#pragma once
// PitPirate — Core 0 HTTP worker
//
// All network HTTP/HTTPS calls are executed on Core 0 so that the UI/display
// loop on Core 1 is never blocked.
//
// Usage (Core 1 side):
//   httpTaskInit();         — call once from setup(), after WiFi is up
//   httpEnqueue(job);       — enqueue a job; returns false if queue is full
//   httpTaskDrainResults(); — call from loop() to apply blower results on Core 1

#include <stdint.h>
#include <stdbool.h>

// ── Job types ─────────────────────────────────────────────────────────────────
enum HttpJobType : uint8_t {
    HTTP_JOB_TELEMETRY_POST = 0,  // POST probe data to telemetry.php
    HTTP_JOB_ALARM_SYNC,          // GET  telemetry_get.php?probe=alarms
    HTTP_JOB_BLOWER_SYNC,         // GET  blower.php  (result applied via result queue)
    HTTP_JOB_ALARM_POST,          // POST alarm.php   (fire-and-forget)
    HTTP_JOB_BLOWER_POST,         // POST blower.php  (fire-and-forget config push)
    HTTP_JOB_ALARM_CONFIG_POST,   // POST telemetry.php?action=alarms
    HTTP_JOB_CONNECTIVITY_ALERT,  // POST alarm.php   (slow/failed request notification)
    HTTP_JOB_GRAPH_FETCH,         // GET  graph_esp.php (raw 8-bit greyscale bitmap)
};

// ── Job struct ────────────────────────────────────────────────────────────────
// All fields are copied by the enqueuer; Core 0 owns the struct after enqueue.
struct HttpJob {
    HttpJobType type;
    char        url[300];    // full endpoint URL (already built by enqueuer)
    char        token[64];   // X-PitPirate-Token (empty = omit header)
    char        body[600];   // pre-serialised JSON body; empty for GETs
    uint32_t    timeoutMs;   // HTTP client timeout
    uint8_t     extra;       // job-specific byte; for GRAPH_FETCH: probe index 0–5
};

// ── Result struct (blower sync only) ─────────────────────────────────────────
// Written by Core 0, consumed by Core 1 in httpTaskDrainResults().
struct BlowerResult {
    int pct;       // -1 means "no usable result, don't apply"
    int startPct;
    int minPct;
};

// ── Result struct (graph fetch) ───────────────────────────────────────────────
// Core 0 allocates buf and sends ownership to Core 1 via httpTaskDrainGraphResults().
// If buf is non-null, Core 1 must eventually free it (displayProbeSetGraph does this).
struct GraphResult {
    uint8_t  probeIdx;  // 0–5
    uint16_t w;         // image width  (matches the ?w= param; up to SCREEN_W = 320)
    uint16_t h;         // image height (matches the ?h= param; up to PROBE_AREA_H = 176)
    float    tMin;      // temperature at the bottom of the graph (from X-Graph-TMin header)
    float    tMax;      // temperature at the top    of the graph (from X-Graph-TMax header)
    uint8_t* buf;       // heap-allocated raw 8-bit greyscale pixels; NULL = no data
};

// ── Public API ────────────────────────────────────────────────────────────────

// Initialise queues and start the Core 0 task. Call once from setup().
void httpTaskInit();

// Enqueue one HTTP job. Non-blocking; returns false if the queue is full.
bool httpEnqueue(const HttpJob& job);

// Drain the blower-result queue and apply any pending fan settings.
// Call from loop() on Core 1. Applies at most one result per call.
void httpTaskDrainResults();

// Drain the graph-result queue and hand graph bitmaps to the display layer.
// Call from loop() on Core 1, after httpTaskDrainResults().
void httpTaskDrainGraphResults();

// Written by Core 0 after each telemetry POST; used by remote_post.cpp for
// adaptive interval calculation. volatile — no mutex needed for a single 32-bit write.
extern volatile uint32_t g_postDurationMs;
extern volatile uint32_t g_alarmSyncDurationMs;
extern volatile uint32_t g_blowerSyncDurationMs;

// Returns true while within the 5-minute cooldown window after a slow (>20 s)
// or failed HTTP request triggered a push notification. Safe to call from Core 1.
bool httpConnectivityAlertActive();
