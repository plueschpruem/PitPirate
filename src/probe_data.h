#pragma once
#include <Arduino.h>

struct ProbeVals {
    float probe[6];   // food probes 1-6 (NAN = not connected)
    float ambient;    // pit air temperature (NAN = not connected)
    int   battery;    // 0-100, or -1 if unknown
    bool  connecting;
    bool  hasError;
    bool  apMode;     // device is in WiFi AP provisioning mode
};

ProbeVals parseProbeVals();
