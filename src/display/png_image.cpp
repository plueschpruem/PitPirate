#include "png_image.h"

#include <LittleFS.h>
#include <PNGdec.h>

// ---------------------------------------------------------------------------
// LittleFS helpers
// ---------------------------------------------------------------------------

// Mounts the LittleFS filesystem; formats it if mounting fails.
// Prints used/total byte counts to Serial on success.
// @return true when the filesystem is mounted and ready; false on fatal mkfs failure.
bool fsInit()
{
    if (!LittleFS.begin(false)) {
        Serial.println("[FS] LittleFS mount failed – running mkfs…");
        if (!LittleFS.begin(true)) {   // true = format on failure
            Serial.println("[FS] mkfs failed");
            return false;
        }
    }
    Serial.printf("[FS] mounted, %u bytes used / %u total\n",
                  (unsigned)LittleFS.usedBytes(),
                  (unsigned)LittleFS.totalBytes());
    return true;
}

// ---------------------------------------------------------------------------
// PNGdec I/O callbacks (File handle stored in PNGFILE::fHandle)
// ---------------------------------------------------------------------------

static void *pngOpen(const char *filename, int32_t *size)
{
    fs::File *f = new fs::File(LittleFS.open(filename, "r"));
    if (!f || !*f) {
        Serial.printf("[PNG] cannot open %s\n", filename);
        delete f;
        return nullptr;
    }
    *size = f->size();
    return static_cast<void *>(f);
}

static void pngClose(void *handle)
{
    fs::File *f = static_cast<fs::File *>(handle);
    if (f) { f->close(); delete f; }
}

static int32_t pngRead(PNGFILE *handle, uint8_t *buf, int32_t len)
{
    return static_cast<fs::File *>(handle->fHandle)->read(buf, len);
}

static int32_t pngSeek(PNGFILE *handle, int32_t pos)
{
    fs::File *f = static_cast<fs::File *>(handle->fHandle);
    return f->seek(pos) ? pos : -1;
}

// ---------------------------------------------------------------------------
// Per-row draw callback
// ---------------------------------------------------------------------------

struct PNGCtx {
    TFT_eSPI *tft;
    PNG      *png;
    int       x, y;
    uint32_t  bgRGB;  // background in 0x00RRGGBB for PNGdec blending
    uint16_t  lineBuf[480]; // wide enough for any typical display row
};

static int pngDraw(PNGDRAW *pDraw)
{
    PNGCtx *ctx = static_cast<PNGCtx *>(pDraw->pUser);

    // getLineAsRGB565 alpha-blends transparent pixels against bgRGB.
    // PNG_RGB565_BIG_ENDIAN matches what TFT_eSPI's pushImage expects.
    ctx->png->getLineAsRGB565(pDraw, ctx->lineBuf,
                              PNG_RGB565_BIG_ENDIAN, ctx->bgRGB);

    ctx->tft->pushImage(ctx->x, ctx->y + pDraw->y,
                        pDraw->iWidth, 1, ctx->lineBuf);
    return 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// Decodes a PNG file from LittleFS and draws it to the TFT display.
// Transparent pixels are alpha-blended against bgColor.
// @param tft      Reference to the active TFT_eSPI display instance.
// @param path     LittleFS path to the .png file (e.g. "/logo.png").
// @param x,y      Top-left pixel position for the image.
// @param bgColor  Background colour (RGB565) used for alpha blending.
// @return true on success; false when the file cannot be opened or decoded.
bool drawPngFromFs(TFT_eSPI &tft, const char *path, int x, int y,
                   uint16_t bgColor)
{
    // Convert RGB565 background → 0x00RRGGBB for PNGdec
    uint32_t r = (bgColor >> 11) & 0x1F;  r = (r << 3) | (r >> 2);
    uint32_t g = (bgColor >>  5) & 0x3F;  g = (g << 2) | (g >> 4);
    uint32_t b =  bgColor        & 0x1F;  b = (b << 3) | (b >> 2);
    uint32_t bgRGB = (r << 16) | (g << 8) | b;

    static PNG png;
    PNGCtx ctx = { &tft, &png, x, y, bgRGB, {} };

    int rc = png.open(path, pngOpen, pngClose, pngRead, pngSeek, pngDraw);
    if (rc != PNG_SUCCESS) {
        Serial.printf("[PNG] open error %d\n", rc);
        return false;
    }

    rc = png.decode(&ctx, 0);
    png.close();

    if (rc != PNG_SUCCESS) {
        Serial.printf("[PNG] decode error %d\n", rc);
        return false;
    }
    return true;
}

