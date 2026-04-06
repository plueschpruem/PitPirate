#pragma once

// Start looping a GIF from LittleFS on a background FreeRTOS task.
// `path`   – LittleFS path, e.g. "/StanAnim.gif"
// `x`, `y` – top-left corner on the display
// Returns immediately; frames loop until stopGifAnimation() is called.
void startGifAnimation(const char *path, int x, int y);

// Signal the animation task to stop and block until it has fully exited.
void stopGifAnimation();
