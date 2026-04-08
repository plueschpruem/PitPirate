#pragma once

// Capture the current TFT framebuffer and POST it to the server as a PNG.
// Must be called from Core 1 (which owns the TFT SPI bus).
// Falls back to screenshotSaveToFs() when WiFi is down.
void screenshotUpload();

// Save the current framebuffer to LittleFS in RAW1 format.
// @param path  Full LittleFS path, e.g. "/screenshot.raw".
// Overwrites any existing file at that path.
void screenshotSaveToFs(const char* path = "/screenshot.raw");

// If a screenshot file exists at `path` in LittleFS and the server is reachable,
// streams it to the server exactly like screenshotUpload() would, then deletes the file.
// Call this once after WiFi reconnects (e.g. when wifiJustConnected() returns true).
void screenshotUploadFromFs(const char* path = "/screenshot.raw");
