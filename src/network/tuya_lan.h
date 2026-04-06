#pragma once

// Tuya LAN v3.4 client — replaces ble_inkbird
// Polls the BBQ thermometer over WiFi TCP (port 6668) and updates
// jsonData / stringBattery in shared_data.

void tuyaLanInit();
void tuyaLanLoop();   // call once per second from main loop

// Update device IP / device ID / local key at runtime, persist to NVS,
// and force an immediate reconnect. local_key must be exactly 16 chars.
void tuyaApplyNewSettings(const char* ip, const char* device_id, const char* local_key);

// Copy the current runtime settings into the caller-provided buffers.
void tuyaGetSettings(char ip_out[64], char id_out[32], char key_out[17]);
