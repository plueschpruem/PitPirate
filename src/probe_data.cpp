#include "probe_data.h"
#include "shared_data.h"

// Scans the JSON string `s` for the given literal key token and returns the
// float value that follows it.  Returns NAN if the key is absent.
// @param s    The JSON string to search (typically the global jsonData).
// @param key  The key token including any colon/quote, e.g. "\"1\":".
static float parseFloatKey(const String& s, const char* key)
{
    int idx = s.indexOf(key);
    if (idx < 0) return NAN;
    return s.substring(idx + strlen(key)).toFloat();
}

// Parses the global jsonData string into a ProbeVals snapshot.
// Probe temperatures are returned in °C as floats; NAN indicates
// a disconnected or absent probe.  battery is −1 when not reported.
// Flags connecting, hasError, and apMode reflect the device’s operating state.
// @return  A populated ProbeVals struct representing the latest device state.
ProbeVals parseProbeVals()
{
    ProbeVals v;
    for (int i = 0; i < 6; i++) v.probe[i] = NAN;
    v.ambient    = NAN;
    v.battery    = -1;
    v.connecting = jsonData.indexOf("\"connecting\"") >= 0;
    v.hasError   = jsonData.indexOf("\"error\"") >= 0;
    v.apMode     = jsonData.indexOf("\"ap_mode\"") >= 0;
    if (v.connecting || v.hasError || v.apMode) return v;

    for (int i = 0; i < 6; i++) {
        char key[8];
        snprintf(key, sizeof(key), "\"%d\":", i + 1);
        v.probe[i] = parseFloatKey(jsonData, key);
    }
    v.ambient = parseFloatKey(jsonData, "\"7\":");

    int bpos = jsonData.indexOf("\"battery\":");
    if (bpos >= 0) v.battery = jsonData.substring(bpos + 10).toInt();
    return v;
}
