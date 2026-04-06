#include "wifi_manager.h"
#include "shared_data.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Arduino.h>
#include "../include/config.h"

static bool      s_apMode = false;
static DNSServer s_dns;

// ── Credential helpers (NVS with config.h fallback) ───────────────────────────

// Returns the stored WiFi SSID from NVS, or falls back to the compile-time
// WIFI_SSID value from config.h if no credential has been saved yet.
// @return  Arduino String containing the SSID to use for STA connection
static String nvSSID() {
    return preferences.isKey("wifi_ssid")
        ? preferences.getString("wifi_ssid")
        : String(WIFI_SSID);
}

// Returns the stored WiFi password from NVS, or falls back to the compile-time
// WIFI_PW value from config.h if no credential has been saved yet.
// @return  Arduino String containing the password to use for STA connection
static String nvPW() {
    return preferences.isKey("wifi_pw")
        ? preferences.getString("wifi_pw")
        : String(WIFI_PW);
}

// ── STA connect attempt (20 s timeout) ───────────────────────────────────────

// Attempts to connect to the stored STA network with a 20-second timeout.
// Reads credentials from NVS (via nvSSID/nvPW); returns immediately if SSID is empty.
// Resets the WiFi stack (disconnect + mode WIFI_OFF) before each attempt
// to avoid stale association state.
// @return  true if WL_CONNECTED is reached within 20 s; false on timeout or empty SSID
static bool tryConnectSTA() {
    String ssid = nvSSID();
    String pw   = nvPW();
    if (ssid.isEmpty()) return false;

    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid.c_str(), pw.c_str());

    Serial.print("Connecting to WiFi: ");
    Serial.println(ssid);
    for (int i = 0; i < 40; i++) {   // 40 × 500 ms = 20 s
        if (WiFi.status() == WL_CONNECTED) {
            Serial.println();
            return true;
        }
        Serial.print('.');
        delay(500);
    }
    Serial.println("\nWiFi timeout");
    return false;
}

// ── AP mode setup ─────────────────────────────────────────────────────────────

// Starts the device as a WiFi access point using AP_SSID / AP_PW from config.h.
// Also starts a captive DNS server (port 53) that redirects all hostnames to
// the AP gateway IP so connecting browsers automatically open the config portal.
static void startAP() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PW);

    Serial.print("AP mode started, IP: ");
    Serial.println(WiFi.softAPIP());

    // Redirect all DNS queries to the AP gateway so browsers open the portal
    s_dns.start(53, "*", WiFi.softAPIP());
    s_apMode = true;
}

// ── Public API ────────────────────────────────────────────────────────────────

// Initialises WiFi: attempts STA connection and, on success, registers an mDNS
// hostname (WIFI_MDNS.local). Falls back to AP mode with captive DNS if STA fails.
// Must be called once from setup() before any network-dependent code runs.
void wifiInit() {
    if (tryConnectSTA()) {
        s_apMode = false;
        MDNS.end();
        if (!MDNS.begin(WIFI_MDNS))
            Serial.println("Error starting mDNS");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        Serial.print("mDNS: ");
        Serial.println(String(WIFI_MDNS) + ".local");
    } else {
        startAP();
    }
}

// Returns true if the device is currently operating as an access point (AP mode).
// AP mode is active when STA connection failed during wifiInit(), or after
// wifiForceAPMode() is called.
bool wifiIsAPMode() { return s_apMode; }

// Returns the active network name.
// In STA mode: returns the SSID from NVS (or config.h default).
// In AP mode: returns "PitPirate" (the AP SSID used for the config portal).
String wifiGetSSID() {
    return s_apMode ? String("PitPirate") : nvSSID();
}

// Persists new WiFi credentials to NVS and immediately reboots.
// The reboot causes wifiInit() to run again on startup and attempt STA
// connection with the new credentials.
// @param ssid  New WiFi SSID (null-terminated, max 63 chars)
// @param pw    New WiFi password (null-terminated, max 63 chars; empty for open)
void wifiSaveCredentials(const char* ssid, const char* pw) {
    preferences.putString("wifi_ssid", ssid);
    preferences.putString("wifi_pw",   pw);
    Serial.println("WiFi credentials saved — restarting");
    delay(300);
    ESP.restart();
}

// Switches the device to AP mode without modifying NVS credentials.
// On the next reboot, the saved STA credentials are still used for wifiInit().
// Intended for the user to manually trigger AP mode from the touch UI.
void wifiForceAPMode() {
    // Switches to AP mode without touching NVS credentials.
    // On the next reboot, saved credentials are still used for STA connect.
    startAP();
}

static int           s_errorCounter = 0;
static unsigned long s_lastCheckMs  = 0;   // time-gate: only count one tick per second

// Reconnect watchdog; must be called every loop() iteration.
// In AP mode: processes pending captive DNS queries.
// In STA mode: tracks consecutive seconds of WiFi disconnect and retries
//   tryConnectSTA() after 10 consecutive failed seconds. Never falls back
//   to AP mid-session — AP mode is only used if startup STA connect fails.
void wifiCheck() {
    if (s_apMode) {
        s_dns.processNextRequest();
        return;
    }
    unsigned long now = millis();
    if (WiFi.status() != WL_CONNECTED) {
        // Only increment the counter once per second so a momentary status
        // blip (common right after connect, or during DHCP renewal) doesn't
        // immediately trigger the 20-second blocking reconnect attempt.
        if (now - s_lastCheckMs >= 1000UL) {
            s_lastCheckMs = now;
            s_errorCounter++;
            if (s_errorCounter >= 10) {
                // Keep retrying STA forever — never fall to AP mid-session.
                // AP mode is only triggered during startup (wifiInit).
                if (tryConnectSTA()) {
                    MDNS.end();
                    MDNS.begin(WIFI_MDNS);
                }
                s_errorCounter = 0;
            }
        }
    } else {
        s_errorCounter = 0;
        s_lastCheckMs  = now;
    }
}
