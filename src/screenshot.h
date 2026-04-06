#pragma once

// Capture the current TFT framebuffer and POST it to the server as a PNG.
// Must be called from Core 1 (which owns the TFT SPI bus).
// No-op when WiFi is down or no URL configured.
void screenshotUpload();
