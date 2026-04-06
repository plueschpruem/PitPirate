#include "gif_anim.h"

#include <AnimatedGIF.h>
#include <LittleFS.h>
#include <TFT_eSPI.h>

extern TFT_eSPI tft;

// ── Placement ─────────────────────────────────────────────────────────────────
static int s_gifX = 0;
static int s_gifY = 0;

// ── Per-row pixel buffer (RGB565, big-endian for TFT_eSPI pushImage) ──────────
static uint16_t s_gifLine[256];

// ── Draw callback: one scan-line at a time ─────────────────────────────────────
static void gifDrawLine(GIFDRAW *pDraw) {
    uint8_t  *pix  = pDraw->pPixels;
    uint16_t *pal  = pDraw->pPalette;     // RGB565 palette (big-endian when BIG_ENDIAN_PIXELS)
    int       w    = pDraw->iWidth;
    int       destX = s_gifX + pDraw->iX;
    int       destY = s_gifY + pDraw->iY + pDraw->y;

    if (pDraw->ucHasTransparency) {
        uint8_t transp = pDraw->ucTransparent;
        for (int x = 0; x < w; x++) {
            uint8_t idx = pix[x];
            s_gifLine[x] = (idx == transp) ? 0x0000u : pal[idx];
        }
    } else {
        for (int x = 0; x < w; x++)
            s_gifLine[x] = pal[pix[x]];
    }

    tft.pushImage(destX, destY, w, 1, s_gifLine);
}

// ── LittleFS file callbacks ───────────────────────────────────────────────────
static void *gifOpen(const char *path, int32_t *size) {
    fs::File *f = new fs::File(LittleFS.open(path, "r"));
    if (!f || !*f) { delete f; return nullptr; }
    *size = f->size();
    return static_cast<void *>(f);
}

static void gifClose(void *handle) {
    fs::File *f = static_cast<fs::File *>(handle);
    if (f) { f->close(); delete f; }
}

static int32_t gifRead(GIFFILE *pFile, uint8_t *buf, int32_t len) {
    int32_t n = static_cast<fs::File *>(pFile->fHandle)->read(buf, len);
    pFile->iPos += n;   // library requires the callback to maintain iPos
    return n;
}

static int32_t gifSeek(GIFFILE *pFile, int32_t pos) {
    fs::File *f = static_cast<fs::File *>(pFile->fHandle);
    if (!f->seek(pos)) return -1;
    pFile->iPos = pos;  // library requires the callback to maintain iPos
    return pos;
}

// ── Task ──────────────────────────────────────────────────────────────────────
static volatile bool s_gifRunning  = false;
static volatile bool s_gifAlive    = false;

static void gifTask(void *param) {
    // AnimatedGIF's internal struct is ~20 KB — heap-allocate to avoid
    // blowing the task stack.
    AnimatedGIF *gif = new AnimatedGIF();
    gif->begin(BIG_ENDIAN_PIXELS);

    const char *path = static_cast<const char *>(param);
    if (gif->open(path, gifOpen, gifClose, gifRead, gifSeek, gifDrawLine)) {
        while (s_gifRunning) {
            int delay_ms = 0;
            if (!gif->playFrame(false, &delay_ms)) {
                gif->reset();   // last frame drawn – loop back to start
            }
            // Sleep in short slices so we respond quickly to a stop request
            int slept = 0;
            int wait  = (delay_ms > 0) ? delay_ms : 40;
            while (s_gifRunning && slept < wait) {
                int slice = (wait - slept < 20) ? (wait - slept) : 20;
                vTaskDelay(pdMS_TO_TICKS(slice));
                slept += slice;
            }
        }
        gif->close();
    } else {
        Serial.printf("[GIF] cannot open %s\n", path);
    }

    delete gif;
    s_gifAlive = false;
    vTaskDelete(nullptr);
}

// ── Public API ────────────────────────────────────────────────────────────────
// Starts the GIF animation playback task on Core 0.
// The task loops the GIF indefinitely until stopGifAnimation() is called.
// @param path  LittleFS path to the .gif file (e.g. "/StanAnim.gif").
// @param x     X pixel offset for the top-left corner of the animation.
// @param y     Y pixel offset for the top-left corner of the animation.
void startGifAnimation(const char *path, int x, int y) {
    s_gifX       = x;
    s_gifY       = y;
    s_gifRunning = true;
    s_gifAlive   = true;
    xTaskCreatePinnedToCore(gifTask, "gif", 4096,
                            const_cast<char *>(path), 1, nullptr, 0);
}

// Signals the GIF task to stop and blocks until it has fully terminated.
// Safe to call if no animation is running (returns immediately).
void stopGifAnimation() {
    s_gifRunning = false;
    while (s_gifAlive) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
