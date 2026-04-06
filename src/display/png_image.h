#pragma once

#include <TFT_eSPI.h>

// Initialise LittleFS. Call once in setup() before any drawPng* call.
// Returns true on success.
bool fsInit();

// Draw a PNG stored in LittleFS at the given screen position.
// Transparent pixels are alpha-blended against bgColor (RGB565).
// Returns true on success.
bool drawPngFromFs(TFT_eSPI &tft, const char *path, int x, int y,
                   uint16_t bgColor = TFT_BLACK);

