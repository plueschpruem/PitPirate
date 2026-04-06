#pragma once

#include <stdint.h>

// Periodically POST telemetry JSON to a remote HTTP/HTTPS endpoint.

void remotePostInit();
void remotePostLoop();   // call once per second from main loop

// Update URL + token at runtime. Changes are persisted to NVS.
// Empty url disables posting. token may be empty.
void remotePostApplySettings(const char* url, const char* token);

// Copy the current runtime settings into the caller-provided buffers.
void remotePostGetSettings(char url_out[256], char token_out[64]);

// Get the bare server root URL (no trailing slash, no /php, no .php filename).
// e.g. "https://bbq.stefan-zillig.com" regardless of how the URL was stored.
void remotePostGetBaseUrl(char base_out[256]);

// POST the current NVS alarm limits to the server (telemetry.php?action=alarms).
// No-op if no server URL is configured.
void remotePostAlarmConfig();

// POST the current fan/blower settings to the server (blower.php).
// No-op if no server URL is configured.
void remotePostBlowerConfig();

// Get/set configurable telemetry post behaviour.
// interval_s: 10 | 30 | 60 | 180  (seconds between posts / heartbeat)
// on_change:  when true, only post when probe data changed; heartbeat every 2× interval.
void remotePostGetTelemetryConfig(uint32_t& interval_s_out, bool& on_change_out);
void remotePostApplyTelemetryConfig(uint32_t interval_s, bool on_change);
