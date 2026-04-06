#pragma once
#include <Arduino.h>

// Runtime debug-log flag — persisted in NVS under key "dbg_log" (default: off).
// Toggle from the TFT settings page 2 "Debug Log" button.
bool debugLogEnabled();
void debugLogSetEnabled(bool en);

// Drop-in conditional replacements for tagged Serial.printf / Serial.println calls.
// Usage: DLOG("[Tag] fmt\n", args...)   or   DLOGLN("[Tag] message")
#define DLOG(...)   do { if (debugLogEnabled()) Serial.printf(__VA_ARGS__); } while(0)
#define DLOGLN(s)   do { if (debugLogEnabled()) Serial.println(s); } while(0)
