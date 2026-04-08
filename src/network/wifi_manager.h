#pragma once

#include <Arduino.h>

// AP provisioning credentials (displayed on TFT and encoded in QR code)
#define AP_SSID  "PitPirate"
#define AP_PW    "Guybrush"

// Connect WiFi and start mDNS (call once in setup).
// Falls back to AP mode "PitPirate" if no credentials or connection fails.
void   wifiInit();

// Check WiFi connection; reconnects if dropped.
// In AP mode: pumps the DNS server for the captive portal.
void   wifiCheck();

// Returns true when the device is in AP provisioning mode.
bool   wifiIsAPMode();

// Returns the currently targeted SSID (NVS value, config.h fallback, or "PitPirate" in AP mode).
String wifiGetSSID();

// Save WiFi credentials to NVS and immediately restart the device.
void   wifiSaveCredentials(const char* ssid, const char* pw);

// Enter AP provisioning mode immediately WITHOUT erasing saved WiFi credentials.
// Device will serve the setup portal; use wifiInit() on next reboot to reconnect.
void   wifiForceAPMode();

// Returns true exactly once after WiFi first becomes connected (rising edge).
// Subsequent calls return false until the next reconnect event.
// Safe to call every loop() iteration; cheap — just a bool comparison.
bool   wifiJustConnected();
