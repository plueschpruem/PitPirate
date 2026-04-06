#include "debug_log.h"
#include "shared_data.h"   // for preferences (NVS)

// Returns true when the persistent debug-log flag is enabled (stored in NVS).
bool debugLogEnabled() {
    return preferences.getBool("dbg_log", false);
}

// Enables or disables the persistent debug-log flag and writes the new value to NVS.
// @param en  true to enable DLOG() output; false to suppress it.
void debugLogSetEnabled(bool en) {
    preferences.putBool("dbg_log", en);
}
