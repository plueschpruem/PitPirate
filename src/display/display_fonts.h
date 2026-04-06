#pragma once
#include <pgmspace.h>

// Forward declarations for font PROGMEM arrays.
// The actual array definitions are #included exactly once in display.cpp.
// Other translation units use these externs to pass font pointers to tft.loadFont().

extern const uint8_t FoundGriBol15[];
extern const uint8_t FoundGriBol20[];
extern const uint8_t MinecartLCD20[];
extern const uint8_t MinecartLCD40[];
extern const uint8_t MinecartLCD60[];
