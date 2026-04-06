#pragma once

#include <Arduino.h>
#include <Preferences.h>

// Shared mutable state accessed by BLE, web server, and main loop

extern String       jsonData;          // JSON payload served at /data
extern bool         requestConnection; // web client wants data → trigger immediate query
extern Preferences  preferences;       // NVS for persisting device settings
