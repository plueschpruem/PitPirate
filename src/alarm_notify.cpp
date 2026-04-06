// PitPirate — alarm checker + push notification dispatcher
//
// Reads lo/hi limits from NVS (same keys written by web_server.cpp).
// When a probe temperature crosses a limit the condition is:
//   1. Printed to Serial immediately.
//   2. POSTed to <server_url>/alarm.php so the server can push a web
//      notification to all browser subscribers.
//
// Alarm rules
// ───────────
//  • Alarm fires immediately when a limit is breached.
//  • Notification is re-sent every ALARM_RESEND_MS (2 min) while the
//    alarm condition persists.
//  • Alarm clears (and state resets) as soon as EITHER:
//      – the temperature is no longer tripping the limit, OR
//      – the limit setting has been changed so it no longer applies
//        (limit set to 0, or temp no longer exceeds the new value).
//  • Once cleared, the next breach fires a fresh notification immediately.

#include "alarm_notify.h"

#include <Arduino.h>
#include <WiFi.h>

#include "config.h"
#include "debug_log.h"
#include "http_task.h"
#include "probe_data.h"
#include "remote_post.h"
#include "shared_data.h"

// ── Constants ─────────────────────────────────────────────────────────────────

static const uint32_t ALARM_RESEND_MS = 2UL * 60UL * 1000UL;  // re-send interval while alarm active

// ── Per-probe debounce state ──────────────────────────────────────────────────
// Index: [probe_index 0..6][0=lo, 1=hi]
// Value: 0 = ready to fire; non-0 = millis() timestamp of last fire.

static unsigned long s_lastFiredMs[7][2];

// ── Battery warning state ─────────────────────────────────────────────────────
// Fires once when battery first drops to 33%; re-arms when it rises above 33%.

static bool s_batteryWarnFired = false;

// ── Internal: build alarm URL from remote-post base URL ──────────────────────

// Constructs the full alarm endpoint URL from the configured remote server base URL.
// @param out     Output buffer to receive the built URL string.
// @param outLen  Size of the output buffer in bytes.
// @return true if a server URL is configured and the URL was written; false if no URL is set.
static bool buildAlarmUrl(char* out, size_t outLen) {
    char base[256] = {};
    remotePostGetBaseUrl(base);
    if (base[0] == '\0') return false;
    snprintf(out, outLen, "%s/php/alarm.php", base);
    return true;
}

// ── Internal: send one alarm POST ────────────────────────────────────────────

// Builds and enqueues an alarm push-notification POST to the remote server.
// @param probeId    0-based probe index (0–5 = food probes, 6 = ambient).
// @param probeName  Human-readable probe label, e.g. "Probe 1" or "Ambient".
// @param isHigh     true when the high-limit was breached; false for a low-limit breach.
// @param temp       Current probe temperature in °C at the time of the alarm.
// @param limit      The limit value (°C) that was exceeded.
static void postAlarm(int probeId, const char* probeName, bool isHigh,
                      float temp, int limit) {
    char alarm_url[320];
    if (!buildAlarmUrl(alarm_url, sizeof(alarm_url))) return;

    // Build title
    char title[30];
    snprintf(title, sizeof(title),
             "%s",
             isHigh ? "🔥 HIGH Temperature Alarm! 🔥" : "❄️ LOW Temperature Alarm! ❄️");

    // Probe colour emoji: index 0-6 → Probe 1-7 (index 6 = Ambient)
    static const char* const probeEmoji[7] = {
        "\xf0\x9f\x94\xb4",          // 🔴 Probe 1
        "\xf0\x9f\x9f\xa0",          // 🟠 Probe 2
        "\xf0\x9f\x9f\xa1",          // 🟡 Probe 3
        "\xf0\x9f\x9f\xa2",          // 🟢 Probe 4
        "\xf0\x9f\x9f\xa3",          // 🟣 Probe 5
        "\xf0\x9f\x94\xb5",          // 🔵 Probe 6
        "\xe2\x9a\xaa\xef\xb8\x8f",  // ⚪️ Probe 7 / Ambient
    };
    const char* emoji = (probeId >= 0 && probeId < 7) ? probeEmoji[probeId] : "";

    // Build human-readable message
    char message[128];
    snprintf(message, sizeof(message),
             "%s  %s: %.0f\xc2\xb0 (Limit %d\xc2\xb0)",  // °C in UTF-8
             emoji,
             probeName,
             temp, limit);

    // JSON tag is used by the browser to group / replace notifications
    char tag[32];
    snprintf(tag, sizeof(tag), "pitpirate-%s-%s",
             isHigh ? "hi" : "lo",
             probeName);  // unique per probe+direction

    // Build JSON payload (matches esp32-notify.php interface)
    char payload[320];
    snprintf(payload, sizeof(payload),
             "{\"secret\":\"%s\",\"title\":\"%s\","
             "\"message\":\"%s\",\"tag\":\"%s\",\"silent\":false}",
             ALARM_SECRET, title, message, tag);

    HttpJob job = {};
    job.type      = HTTP_JOB_ALARM_POST;
    job.timeoutMs = 8000;
    strlcpy(job.url,  alarm_url, sizeof(job.url));
    strlcpy(job.body, payload,   sizeof(job.body));
    DLOG("[Alarm] enqueue POST → %s  (%s)\n", alarm_url, message);
    if (!httpEnqueue(job))
        DLOGLN("[Alarm] job queue full - alarm POST dropped");
}

// ── Public API ────────────────────────────────────────────────────────────────

// Initialises the alarm subsystem: clears all per-probe fired-timestamps and
// resets the battery-warning latch.  Must be called once during setup().
void alarmNotifyInit() {
    memset(s_lastFiredMs, 0, sizeof(s_lastFiredMs));
    s_batteryWarnFired = false;
    DLOGLN("[Alarm] alarm checker initialised");
}

// Sends a test push notification to the configured remote server.
// Silently skips when WiFi is not connected or no server URL is configured.
void alarmNotifyTest() {
    if (WiFi.status() != WL_CONNECTED) {
        DLOGLN("[Alarm] test skipped – WiFi not connected");
        return;
    }
    char alarm_url[320];
    if (!buildAlarmUrl(alarm_url, sizeof(alarm_url))) {
        DLOGLN("[Alarm] test skipped – no server URL configured");
        return;
    }
    const char* payload =
        "{\"secret\":\"" ALARM_SECRET
        "\","
        "\"title\":\"🏴‍☠️ PitPirate Test 🏴‍☠️\","
        "\"message\":\"Test notification from the device\","
        "\"tag\":\"pitpirate-test\","
        "\"silent\":false}";

    HttpJob job = {};
    job.type      = HTTP_JOB_ALARM_POST;
    job.timeoutMs = 8000;
    strlcpy(job.url,  alarm_url, sizeof(job.url));
    strlcpy(job.body, payload,   sizeof(job.body));
    DLOG("[Alarm] enqueue test POST → %s\n", alarm_url);
    if (!httpEnqueue(job))
        DLOGLN("[Alarm] job queue full - test POST dropped");
}

// Periodic alarm checker — call from loop() on every iteration.
// Rate-limited internally to once per second.  Reads probe temperatures from
// the shared jsonData, compares them against NVS-stored lo/hi limits for each
// probe (1–7), and enqueues push-notification POSTs when a limit is breached.
// Notifications are re-sent every ALARM_RESEND_MS while the condition persists.
void alarmNotifyLoop() {
    // Rate-limit to once per second: NVS reads + string ops on every loop()
    // iteration are expensive and alarm conditions don't change sub-second.
    static unsigned long s_lastAlarmCheck = 0;
    unsigned long now = millis();
    if (now - s_lastAlarmCheck < 1000UL) return;
    s_lastAlarmCheck = now;

    if (WiFi.status() != WL_CONNECTED) return;

    ProbeVals v = parseProbeVals();
    if (v.connecting || v.hasError) return;

    // ── Battery low warning (33%) ──────────────────────────────────────
    if (v.battery == 33) {
        if (!s_batteryWarnFired) {
            s_batteryWarnFired = true;
            char alarm_url[320];
            if (buildAlarmUrl(alarm_url, sizeof(alarm_url))) {
                const char* payload =
                    "{\"secret\":\"" ALARM_SECRET "\","
                    "\"title\":\"\xf0\x9f\xaa\xa7 Battery Low\","
                    "\"message\":\"PitPirate battery at 33%\","
                    "\"tag\":\"pitpirate-battery-low\","
                    "\"silent\":false}";
                HttpJob job = {};
                job.type      = HTTP_JOB_ALARM_POST;
                job.timeoutMs = 8000;
                strlcpy(job.url,  alarm_url, sizeof(job.url));
                strlcpy(job.body, payload,   sizeof(job.body));
                DLOG("[Alarm] enqueue battery LOW POST → %s\n", alarm_url);
                if (!httpEnqueue(job))
                    DLOGLN("[Alarm] job queue full - battery LOW POST dropped");
            }
        }
    } else if (v.battery > 33 || v.battery < 0) {
        // Re-arm once battery recovers or is unknown
        s_batteryWarnFired = false;
    }

    for (int i = 0; i < 7; i++) {
        float temp = (i < 6) ? v.probe[i] : v.ambient;
        if (isnan(temp)) continue;

        // Probe name
        char probeName[16];
        if (i < 6)
            snprintf(probeName, sizeof(probeName), "Probe %d", i + 1);
        else
            snprintf(probeName, sizeof(probeName), "Ambient");

        // Read limits from NVS
        char lo_key[12], hi_key[12];
        snprintf(lo_key, sizeof(lo_key), "alm_%d_lo", i + 1);
        snprintf(hi_key, sizeof(hi_key), "alm_%d_hi", i + 1);
        int lo = preferences.getInt(lo_key, 0);
        int hi = preferences.getInt(hi_key, 0);

        // ── Low alarm ──────────────────────────────────────────────────────
        if (lo > 0) {
            bool inAlarm = (temp < (float)lo);
            if (inAlarm) {
                bool canFire = (s_lastFiredMs[i][0] == 0) ||
                               (now - s_lastFiredMs[i][0] >= ALARM_RESEND_MS);
                if (canFire) {
                    s_lastFiredMs[i][0] = (now == 0) ? 1 : now;
                    DLOG("[Alarm] LOW  %s: %.1f° < %d°\n",
                                  probeName, temp, lo);
                    postAlarm(i, probeName, false, temp, lo);
                }
            } else {
                // temp >= lo: condition cleared (temp recovered or limit raised)
                s_lastFiredMs[i][0] = 0;
            }
        } else {
            // limit disabled (lo == 0): clear any active alarm state
            s_lastFiredMs[i][0] = 0;
        }

        // ── High alarm ─────────────────────────────────────────────────────
        if (hi > 0) {
            bool inAlarm = (temp > (float)hi);
            if (inAlarm) {
                bool canFire = (s_lastFiredMs[i][1] == 0) ||
                               (now - s_lastFiredMs[i][1] >= ALARM_RESEND_MS);
                if (canFire) {
                    s_lastFiredMs[i][1] = (now == 0) ? 1 : now;
                    DLOG("[Alarm] HIGH %s: %.1f° > %d°\n",
                                  probeName, temp, hi);
                    postAlarm(i, probeName, true, temp, hi);
                }
            } else {
                // temp <= hi: condition cleared (temp recovered or limit lowered)
                s_lastFiredMs[i][1] = 0;
            }
        } else {
            // limit disabled (hi == 0): clear any active alarm state
            s_lastFiredMs[i][1] = 0;
        }
    }
}
