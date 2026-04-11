#include "web_server.h"
#include "shared_data.h"
#include "tuya_lan.h"
#include "remote_post.h"
#include "wifi_manager.h"
#include "../fan_control.h"
#include "../pid_fan.h"
#include "../servo_control.h"

#include <WebServer.h>
#include <WiFi.h>
#include <Arduino.h>
#include <LittleFS.h>

static WebServer server(80);

// ---------------------------------------------------------------------------
// Static file serving from LittleFS (Vue SPA + assets)
// ---------------------------------------------------------------------------
// Maps a file path's extension to the appropriate HTTP Content-Type string.
// Used by handleNotFoundFs() when streaming files from LittleFS.
// @param path  URL path or filename (extension is checked as a suffix)
// @return      MIME type string; defaults to "text/plain" for unknown extensions
static String getMimeType(const String& path) {
    if (path.endsWith(".html"))        return "text/html";
    if (path.endsWith(".css"))         return "text/css";
    if (path.endsWith(".js"))          return "application/javascript";
    if (path.endsWith(".json"))        return "application/json";
    if (path.endsWith(".png"))         return "image/png";
    if (path.endsWith(".ico"))         return "image/x-icon";
    if (path.endsWith(".svg"))         return "image/svg+xml";
    if (path.endsWith(".webmanifest")) return "application/manifest+json";
    return "text/plain";
}

// Serves static files from LittleFS for the Vue SPA.
// Applies smart cache-control headers: no-cache for index.html and /data,
// immutable for versioned assets, no-cache for unversioned JS/CSS.
// Detects gzip magic bytes (0x1f 0x8b) and adds Content-Encoding: gzip if present.
// Falls back to serving /index.html for unknown paths (Vue Router history mode).
// Registered as the onNotFound handler and explicitly for "/" and "/index.html".
static void handleNotFoundFs() {
    String path = server.uri();
    // Treat bare "/" as "/index.html"
    if (path.endsWith("/")) path += "index.html";

    if (LittleFS.exists(path)) {
        File f = LittleFS.open(path, "r");
        if (f) {
            // Cache policy:
            //  - index.html and /data → no-cache (always fetch fresh)
            //  - JS/CSS with a ?v=<hash> query param (e.g. index.js from the
            //    <script> tag in index.html) → immutable (content-hash locked)
            //  - All other JS/CSS (dynamic chunks, no version param) → no-cache
            //    so the browser always re-fetches after a firmware update
            //  - Everything else (icons, images, fonts) → long-lived cache
            if (path.startsWith("/data") || path == "/index.html") {
                server.sendHeader("Cache-Control", "no-cache");
            } else {
                server.sendHeader("Cache-Control", "public, max-age=31536000, immutable");
            }
            // Only send Content-Encoding: gzip if the file is actually gzip-compressed
            // (magic bytes 0x1f 0x8b). Self-adapts whether or not the build script ran gzip.
            if (path.endsWith(".js") || path.endsWith(".css") || path.endsWith(".html")) {
                uint8_t magic[2] = {0, 0};
                f.read(magic, 2);
                f.seek(0);
                if (magic[0] == 0x1f && magic[1] == 0x8b) {
                    server.sendHeader("Content-Encoding", "gzip");
                }
            }
            server.streamFile(f, getMimeType(path));
            f.close();
            return;
        }
    }
    // SPA fallback: unknown paths (Vue Router history-mode) get index.html
    File fallback = LittleFS.open("/index.html", "r");
    if (fallback) {
        server.sendHeader("Cache-Control", "no-cache");
        uint8_t magic[2] = {0, 0};
        fallback.read(magic, 2);
        fallback.seek(0);
        if (magic[0] == 0x1f && magic[1] == 0x8b) {
            server.sendHeader("Content-Encoding", "gzip");
        }
        server.streamFile(fallback, "text/html");
        fallback.close();
    } else {
        server.send(503, "text/plain", "UI not found - run: pio run -t uploadfs");
    }
}

// ---------------------------------------------------------------------------
// HTTP request handlers
// ---------------------------------------------------------------------------
// GET /data — Returns the current device JSON with live RSSI injected.
// Prepends {"rssi":<dBm>, before the existing top-level keys so the
// frontend always has a fresh signal-strength reading.
// Also resets disconnectCounter and sets requestConnection to trigger
// an immediate Tuya LAN poll on the next tuyaLanLoop() tick.
static void handleData() {
    // Inject live RSSI into the JSON object before serving it.
    String jd = jsonData;
    if (jd.startsWith("[{") && jd.endsWith("}]"))
        jd = String("[{\"rssi\":") + String(WiFi.RSSI()) + "," + jd.substring(2);
    server.send(200, "application/json", jd);
    requestConnection = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Escapes a C string for embedding inside a JSON string literal.
// Replaces '"' with '\"' and '\' with '\\'.
// @param s  Null-terminated input string
// @return   Arduino String safe for use inside JSON double-quoted values
static String jsonEscape(const char* s) {
    String out;
    for (; *s; s++) {
        if      (*s == '"')  out += "\\\"";
        else if (*s == '\\') out += "\\\\";
        else                 out += *s;
    }
    return out;
}

// GET /tuya-config — Returns the current Tuya LAN credentials as JSON.
// Response: {"ip":"...","id":"...","key":"..."}
// Values are read from runtime state (NVS-overridden or config.h defaults).
static void handleTuyaConfig() {
    char ip[64], id[32], key[17];
    tuyaGetSettings(ip, id, key);
    String json = "{\"ip\":\""  + jsonEscape(ip)
                + "\",\"id\":\"" + jsonEscape(id)
                + "\",\"key\":\"" + jsonEscape(key)
                + "\"}";
    server.send(200, "application/json", json);
}

// GET /settings — Validates and applies new Tuya LAN credentials.
// Query params:
//   @param ip   Device LAN IP address (1–63 chars)
//   @param id   Tuya device ID (1–31 chars)
//   @param key  Tuya local key (exactly 16 chars)
// Persists to NVS and forces an immediate reconnect via tuyaApplyNewSettings().
// Returns 400 with a descriptive error if any parameter fails validation.
static void handleSettings() {
    if (!server.hasArg("ip") || !server.hasArg("id") || !server.hasArg("key")) {
        server.send(400, "text/plain", "Missing parameters");
        return;
    }
    String ip  = server.arg("ip");
    String id  = server.arg("id");
    String key = server.arg("key");
    if (ip.isEmpty()  || ip.length()  > 63) { server.send(400, "text/plain", "Invalid IP");          return; }
    if (id.isEmpty()  || id.length()  > 31) { server.send(400, "text/plain", "Invalid device ID");    return; }
    if (key.length() != 16)                 { server.send(400, "text/plain", "Key must be 16 chars"); return; }
    tuyaApplyNewSettings(ip.c_str(), id.c_str(), key.c_str());
    server.send(200, "text/plain", "OK");
}

// GET /remote-config — Returns the current remote telemetry server settings as JSON.
// Response: {"url":"...","token":"..."}
// Empty strings indicate telemetry posting is disabled.
static void handleRemoteConfig() {
    char url[256], token[64];
    remotePostGetSettings(url, token);
    String json = "{\"url\":\""   + jsonEscape(url)
                + "\",\"token\":\"" + jsonEscape(token)
                + "\"}";
    server.send(200, "application/json", json);
}

// GET /save-remote — Saves remote telemetry server URL and auth token.
// Query params:
//   @param url    Target URL (max 255 chars); must start with http:// or https:// if non-empty
//   @param token  Bearer/auth token (max 63 chars); may be empty
// Passing an empty url disables telemetry posting.
static void handleSaveRemote() {
    String url   = server.hasArg("url")   ? server.arg("url")   : String("");
    String token = server.hasArg("token") ? server.arg("token") : String("");
    if (url.length() > 255) { server.send(400, "text/plain", "URL too long");   return; }
    if (token.length() > 63){ server.send(400, "text/plain", "Token too long"); return; }
    if (url.length() > 0 && !url.startsWith("http://") && !url.startsWith("https://")) {
        server.send(400, "text/plain", "URL must start with http:// or https://");
        return;
    }
    remotePostApplySettings(url.c_str(), token.c_str());
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Alarm settings: /alarm-config (GET) and /save-alarms (GET with query params)
// Probe IDs 1-6 → max 200°C, probe 7 (ambient) → max 600°C
// A value of 0 means the limit is disabled (OFF).
// NVS keys: alm_N_lo / alm_N_hi  (N = 1..7, keys ≤ 15 chars)
// ---------------------------------------------------------------------------
// GET /alarm-config — Returns alarm limits for all 7 probes as JSON.
// Response: {"1":{"lo":N,"hi":N}, ... "7":{"lo":N,"hi":N}}
// A value of 0 means the limit is disabled. Probe 7 is the ambient sensor.
static void handleAlarmConfig() {
    String json = "{";
    for (int i = 1; i <= 7; i++) {
        String lo_key = "alm_" + String(i) + "_lo";
        String hi_key = "alm_" + String(i) + "_hi";
        int lo = preferences.getInt(lo_key.c_str(), 0);
        int hi = preferences.getInt(hi_key.c_str(), 0);
        if (i > 1) json += ",";
        json += "\"" + String(i) + "\":{\"lo\":" + String(lo) + ",\"hi\":" + String(hi) + "}";
    }
    json += "}";
    server.send(200, "application/json", json);
}

// GET /save-alarms — Updates alarm limits for any/all probes.
// Query params (all optional): N_lo and N_hi for each probe N (1–7).
//   Probe 1–6 (food): valid range 0–200°C; 0 disables the limit.
//   Probe 7 (ambient): valid range 0–600°C; 0 disables the limit.
// Out-of-range values are silently ignored. Updated limits are also
// pushed to the remote server via remotePostAlarmConfig().
static void handleSaveAlarms() {
    for (int i = 1; i <= 7; i++) {
        int maxT = (i == 7) ? 600 : 200;
        String lo_arg = String(i) + "_lo";
        String hi_arg = String(i) + "_hi";
        String lo_key = "alm_" + String(i) + "_lo";
        String hi_key = "alm_" + String(i) + "_hi";
        if (server.hasArg(lo_arg)) {
            int lo = server.arg(lo_arg).toInt();
            if (lo >= 0 && lo <= maxT) preferences.putInt(lo_key.c_str(), lo);
        }
        if (server.hasArg(hi_arg)) {
            int hi = server.arg(hi_arg).toInt();
            if (hi >= 0 && hi <= maxT) preferences.putInt(hi_key.c_str(), hi);
        }
    }
    // Push the updated limits to the remote server so the hosted app stays in sync.
    remotePostAlarmConfig();
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Fan speed: /fan-config (GET) and /save-fan (GET with query param)
// ---------------------------------------------------------------------------
// GET /fan-config — Returns current fan speed settings as JSON.
// Response: {"pct":N,"start":N,"min":N}
//   pct:   current manual fan speed (0–100%)
//   start: kick-start speed used briefly when fan first turns on (0–100%)
//   min:   minimum sustained speed while fan is running (0–100%)
static void handleFanConfig() {
    String json = "{\"pct\":"   + String(fanGetPercent())  +
                  ",\"start\":" + String(fanGetStartPct()) +
                  ",\"min\":"   + String(fanGetMinPct())   + "}";
    server.send(200, "application/json", json);
}

// GET /save-fan — Sets the manual fan speed.
// Query params:
//   @param pct  Fan duty cycle percentage (0–100, required)
// Also syncs the new value to the remote server via remotePostBlowerConfig().
static void handleSaveFan() {
    if (!server.hasArg("pct")) { server.send(400, "text/plain", "Missing pct"); return; }
    int pct = server.arg("pct").toInt();
    if (pct < 0 || pct > 100) { server.send(400, "text/plain", "pct must be 0-100"); return; }
    fanSetPercent((uint8_t)pct);
    remotePostBlowerConfig();
    server.send(200, "text/plain", "OK");
}

// GET /save-fan-settings — Updates kick-start and minimum fan speed settings.
// Query params:
//   @param start  Kick-start speed percentage (0–100, required)
//   @param min    Minimum sustained speed percentage (0–100, required); must be <= start
// Also syncs the updated settings to the remote server via remotePostBlowerConfig().
static void handleSaveFanSettings() {
    if (!server.hasArg("start") || !server.hasArg("min")) {
        server.send(400, "text/plain", "Missing start or min"); return;
    }
    int startPct = server.arg("start").toInt();
    int minPct   = server.arg("min").toInt();
    if (startPct < 0 || startPct > 100 || minPct < 0 || minPct > 100) {
        server.send(400, "text/plain", "Values must be 0-100"); return;
    }
    if (minPct > startPct) {
        server.send(400, "text/plain", "min must be <= start"); return;
    }
    fanSetStartPct((uint8_t)startPct);
    fanSetMinPct((uint8_t)minPct);
    remotePostBlowerConfig();
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Telemetry post settings: /telemetry-config (GET) and /save-telemetry-config
// NVS keys: rpost_interval (uint32, seconds), rpost_on_change (uint8)
// ---------------------------------------------------------------------------
// GET /telemetry-config — Returns remote telemetry posting settings as JSON.
// Response: {"interval_s":N,"on_change":true|false}
//   interval_s:  how often (in seconds) to post probe data regardless of change
//   on_change:   if true, also posts immediately whenever probe values change
static void handleTelemetryConfig() {
    uint32_t interval_s;
    bool on_change;
    remotePostGetTelemetryConfig(interval_s, on_change);
    String json = "{\"interval_s\":" + String(interval_s)
                + ",\"on_change\":"  + (on_change ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

// GET /save-telemetry-config — Updates remote telemetry posting settings.
// Query params (all optional):
//   @param interval_s  Post interval in seconds (must be > 0; defaults to 10)
//   @param on_change   "1" or "true" to also post on value change; else false
static void handleSaveTelemetryConfig() {
    uint32_t interval_s = 10;
    bool on_change = false;
    if (server.hasArg("interval_s")) {
        long v = server.arg("interval_s").toInt();
        if (v > 0) interval_s = (uint32_t)v;
    }
    if (server.hasArg("on_change")) {
        String v = server.arg("on_change");
        on_change = (v == "1" || v == "true");
    }
    remotePostApplyTelemetryConfig(interval_s, on_change);
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// PID fan controller: /pid-config (GET) and /save-pid (GET with query params)
//
// GET /pid-config  → full config JSON including live runtime diagnostics
// GET /save-pid    → update any subset of config fields (all params optional)
//
// Probe index: 0-5 = food probes 1-6, 6 = ambient/pit sensor
// ---------------------------------------------------------------------------
// GET /pid-config — Returns full PID fan controller configuration plus live runtime diagnostics.
// Response includes static config fields (enabled, setpoint, probe, gains, limits, lid_detect)
// and live read-only fields (output, error, integral, lid_open).
// Probe index 0–5 maps to food probes 1–6; index 6 is the ambient/pit sensor.
static void handlePidConfig() {
    PidConfig cfg;
    pidFanGetConfig(cfg);
    char json[512];
    snprintf(json, sizeof(json),
        "{\"enabled\":%s,\"setpoint\":%.1f,\"probe\":%d,"
        "\"kp_con\":%.3f,\"ki_con\":%.4f,\"kd_con\":%.3f,"
        "\"kp_agg\":%.3f,\"ki_agg\":%.4f,\"kd_agg\":%.3f,"
        "\"bias\":%.1f,\"out_min\":%.1f,\"out_max\":%.1f,"
        "\"lid_detect\":%s,"
        "\"output\":%.1f,\"error\":%.1f,\"integral\":%.2f,\"lid_open\":%s}",
        cfg.enabled ? "true" : "false",
        cfg.setpoint, cfg.probeIndex,
        cfg.kp_con, cfg.ki_con, cfg.kd_con,
        cfg.kp_agg, cfg.ki_agg, cfg.kd_agg,
        cfg.bias, cfg.outMin, cfg.outMax,
        cfg.lidDetect ? "true" : "false",
        pidFanGetOutput(), pidFanGetError(), pidFanGetIntegral(),
        pidFanIsLidOpen() ? "true" : "false");
    server.send(200, "application/json", json);
}

// GET /save-pid — Updates any subset of PID fan controller config fields.
// All query params are optional; unrecognised/omitted fields keep their current values.
// Query params: enabled, setpoint (0–600°C), probe (0–6), kp_con, ki_con, kd_con,
//               kp_agg, ki_agg, kd_agg, bias, out_min (0–100), out_max (out_min–100),
//               lid_detect ("1" or "true" to enable).
// Gains are clamped to ±1000 to prevent runaway fan behaviour.
static void handleSavePid() {
    PidConfig cfg;
    pidFanGetConfig(cfg);  // start from current values; only overwrite provided fields

    if (server.hasArg("enabled")) {
        String v = server.arg("enabled");
        cfg.enabled = (v == "1" || v == "true");
    }
    if (server.hasArg("setpoint")) cfg.setpoint   = server.arg("setpoint").toFloat();
    if (server.hasArg("probe"))    cfg.probeIndex = server.arg("probe").toInt();
    if (server.hasArg("kp_con"))   cfg.kp_con     = server.arg("kp_con").toFloat();
    if (server.hasArg("ki_con"))   cfg.ki_con     = server.arg("ki_con").toFloat();
    if (server.hasArg("kd_con"))   cfg.kd_con     = server.arg("kd_con").toFloat();
    if (server.hasArg("kp_agg"))   cfg.kp_agg     = server.arg("kp_agg").toFloat();
    if (server.hasArg("ki_agg"))   cfg.ki_agg     = server.arg("ki_agg").toFloat();
    if (server.hasArg("kd_agg"))   cfg.kd_agg     = server.arg("kd_agg").toFloat();
    if (server.hasArg("bias"))     cfg.bias       = server.arg("bias").toFloat();
    if (server.hasArg("out_min"))     cfg.outMin     = server.arg("out_min").toFloat();
    if (server.hasArg("out_max"))     cfg.outMax     = server.arg("out_max").toFloat();
    if (server.hasArg("lid_detect")) {
        String v = server.arg("lid_detect");
        cfg.lidDetect = (v == "1" || v == "true");
    }

    // Input validation
    if (cfg.setpoint < 0.0f || cfg.setpoint > 600.0f) {
        server.send(400, "text/plain", "Invalid setpoint (0-600 C)"); return;
    }
    if (cfg.probeIndex < 0 || cfg.probeIndex > 6) {
        server.send(400, "text/plain", "Invalid probe index (0-6)"); return;
    }
    if (cfg.outMin < 0.0f || cfg.outMin > 100.0f) {
        server.send(400, "text/plain", "Invalid out_min (0-100)"); return;
    }
    if (cfg.outMax < cfg.outMin || cfg.outMax > 100.0f) {
        server.send(400, "text/plain", "Invalid out_max (out_min-100)"); return;
    }
    // Clamp gain values to a sane range to prevent runaway fan behaviour
    const float kMax = 1000.0f;
    if (fabsf(cfg.kp_con) > kMax || fabsf(cfg.ki_con) > kMax || fabsf(cfg.kd_con) > kMax ||
        fabsf(cfg.kp_agg) > kMax || fabsf(cfg.ki_agg) > kMax || fabsf(cfg.kd_agg) > kMax) {
        server.send(400, "text/plain", "Gain out of range (max 1000)"); return;
    }

    pidFanApplyConfig(cfg);
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// Servo: /servo-config (GET) and /save-servo (GET with query params)
// NVS keys: srv_ang (current angle), srv_min (min limit), srv_max (max limit)
// ---------------------------------------------------------------------------
// GET /servo-config — Returns current servo angle and limits as JSON.
// Response: {"angle":N,"min":N,"max":N,"auto":true|false}
static void handleServoConfig() {
    String json = "{\"angle\":" + String(servoGetAngle())
                + ",\"min\":"   + String(servoGetMinAngle())
                + ",\"max\":"   + String(servoGetMaxAngle())
                + ",\"auto\":"  + (servoIsAuto() ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

// GET /save-servo — Moves and/or re-limits the servo.
// Query params (all optional):
//   @param angle  Target angle in degrees (0–180); clamped to [min,max] and persisted.
//   @param min    New minimum angle limit (0–180); must be <= max if both provided.
//   @param max    New maximum angle limit (0–180); must be >= min if both provided.
//   @param auto   "1"/"true" to enable auto (fan-coupled) mode, "0"/"false" to disable.
static void handleSaveServo() {
    if (server.hasArg("min") || server.hasArg("max")) {
        int mn = server.hasArg("min") ? server.arg("min").toInt() : (int)servoGetMinAngle();
        int mx = server.hasArg("max") ? server.arg("max").toInt() : (int)servoGetMaxAngle();
        if (mn < 0 || mn > 180 || mx < 0 || mx > 180) {
            server.send(400, "text/plain", "min/max must be 0-180"); return;
        }
        if (mn > mx) {
            server.send(400, "text/plain", "min must be <= max"); return;
        }
        servoSetMinAngle((uint8_t)mn);
        servoSetMaxAngle((uint8_t)mx);
    }
    if (server.hasArg("angle")) {
        int a = server.arg("angle").toInt();
        if (a < 0 || a > 180) { server.send(400, "text/plain", "angle must be 0-180"); return; }
        servoSetAngle((uint8_t)a);  // clamps to [min,max] and persists
    }
    if (server.hasArg("auto")) {
        String v = server.arg("auto");
        servoSetAuto(v == "1" || v == "true");
    }
    server.send(200, "text/plain", "OK");
}

// ---------------------------------------------------------------------------
// WiFi provisioning: /wifi-config (GET), /wifi-scan (GET), /save-wifi (GET)
// ---------------------------------------------------------------------------

// GET /wifi-config — Returns current WiFi connection info as JSON.
// Response: {"ssid":"...","ap_mode":true|false}
//   ssid:     connected SSID (STA mode) or "PitPirate" (AP mode)
//   ap_mode:  true when the device is running as a hotspot/access point
static void handleWifiConfig() {
    String ssid = wifiGetSSID();
    bool   ap   = wifiIsAPMode();
    String json = "{\"ssid\":\"" + jsonEscape(ssid.c_str())
                + "\",\"ap_mode\":" + (ap ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

// GET /wifi-scan — Performs a blocking WiFi scan and returns nearby networks as JSON.
// Response: [{"ssid":"...","rssi":N,"secure":true|false}, ...]
// In AP mode, the ESP32 SDK automatically enables dual AP+STA for scanning.
// The scan result is deleted after building the JSON to free memory.
static void handleWifiScan() {
    // On AP mode the ESP32 Arduino lib enables dual AP+STA for the scan automatically.
    int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        bool secure = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i).c_str())
             + "\",\"rssi\":"  + String(WiFi.RSSI(i))
             + ",\"secure\":" + (secure ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    server.send(200, "application/json", json);
}

// GET /save-wifi — Saves new WiFi credentials to NVS and reboots.
// Query params:
//   @param ssid  Network SSID (1–63 chars, required)
//   @param pw    Password (0–63 chars, optional; empty means open network)
// Sends "OK - rebooting" before flushing and calling wifiSaveCredentials(),
// which persists to NVS then calls ESP.restart().
static void handleSaveWifi() {
    if (!server.hasArg("ssid")) {
        server.send(400, "text/plain", "Missing ssid");
        return;
    }
    String ssid = server.arg("ssid");
    String pw   = server.hasArg("pw") ? server.arg("pw") : String("");
    if (ssid.isEmpty() || ssid.length() > 63) {
        server.send(400, "text/plain", "Invalid SSID");
        return;
    }
    if (pw.length() > 63) {
        server.send(400, "text/plain", "Password too long");
        return;
    }
    server.send(200, "text/plain", "OK - rebooting");
    server.client().flush();
    wifiSaveCredentials(ssid.c_str(), pw.c_str());  // saves to NVS then ESP.restart()
}

// Registers all HTTP routes and starts the server on port 80.
// Also mounts LittleFS (with auto-format on first use) for SPA file serving.
// Must be called once from setup() after WiFi is initialised.
void webServerInit() {
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS mount failed - UI will not be available");
    }
    // Explicit routes for the SPA entry-points prevent the WebServer library
    // from logging a false "request handler not found" error for every page
    // load (it fires whenever _currentHandler is null, even though onNotFound
    // would handle it correctly).
    server.on("/",            handleNotFoundFs);
    server.on("/index.html",  handleNotFoundFs);
    server.on("/data",          handleData);
    server.on("/tuya-config",   handleTuyaConfig);
    server.on("/settings",      handleSettings);
    server.on("/remote-config", handleRemoteConfig);
    server.on("/save-remote",   handleSaveRemote);
    server.on("/alarm-config",  handleAlarmConfig);
    server.on("/save-alarms",   handleSaveAlarms);
    server.on("/fan-config",         handleFanConfig);
    server.on("/save-fan",           handleSaveFan);
    server.on("/save-fan-settings",  handleSaveFanSettings);
    server.on("/telemetry-config",       handleTelemetryConfig);
    server.on("/save-telemetry-config",  handleSaveTelemetryConfig);
    server.on("/wifi-config",   handleWifiConfig);
    server.on("/wifi-scan",     handleWifiScan);
    server.on("/save-wifi",     handleSaveWifi);
    server.on("/pid-config",    handlePidConfig);
    server.on("/save-pid",      handleSavePid);
    server.on("/servo-config",  handleServoConfig);
    server.on("/save-servo",    handleSaveServo);
    server.onNotFound(handleNotFoundFs);
    server.begin();
    Serial.println("HTTP server started");
}

// Processes one pending HTTP request per call.
// Must be called from loop() on every iteration to keep the web server responsive.
void webServerLoop() {
    server.handleClient();
}
