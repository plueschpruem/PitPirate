#include "display.h"
#include "display_fonts.h"

#include "../display/circle_numbers.h"
#include "../probe_data.h"
#include "../shared_data.h"
#include "debug_log.h"

// ── Per-probe graph bitmap cache ──────────────────────────────────────────────
// Heap-allocated 8-bit greyscale buffers received from graph_esp.php.
// Pixel encoding: 0 = fully fg-tinted, 255 = transparent (bg shows through).
// Updated by httpTaskDrainGraphResults() on Core 1; free'd and replaced atomically.
static uint8_t*  s_graphBuf[6] = {};
static uint16_t  s_graphW[6]   = {};
static uint16_t  s_graphH[6]   = {};
static float     s_graphTMin[6] = {NAN, NAN, NAN, NAN, NAN, NAN};
static float     s_graphTMax[6] = {NAN, NAN, NAN, NAN, NAN, NAN};

// ── Graph bar-chart renderer ──────────────────────────────────────────────────
// Draws a bottom-anchored bar chart from the compact data sent by graph_esp.php.
// bars[i] = bar height for column i in [0, srcH]; 0xFF = no data, 0 = empty.
// Scales from srcW×srcH to dstW×dstH without any heap allocation.
// Renders a bottom-anchored bar chart from the compact column-height data sent
// by graph_esp.php.  Scales from the source grid (srcW×srcH) to the destination
// rectangle (dstW×dstH) using integer nearest-neighbour mapping.
// @param x,y       Top-left corner of the destination rectangle in pixels.
// @param dstW,dstH Width and height of the destination rectangle in pixels.
// @param bars      Array of srcW bytes: each byte is bar height in [0, srcH];
//                  0xFF means no data for that column.
// @param srcW,srcH Dimensions of the source data grid.
// @param fg        Foreground colour (RGB565) used for bar tinting.
// @param bg        Background colour (RGB565) for blending.
static void drawGraphBars(int x, int y, int dstW, int dstH,
                           const uint8_t* bars, int srcW, int srcH,
                           uint16_t fg, uint16_t bg) {
    if (!bars || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    // Pre-blend two colour levels:
    //   lineCol — 70 % fg : top-of-bar "line" marker
    //   fillCol — 30 % fg : area fill below the line
    uint16_t fg_r = (fg >> 11) & 0x1F;
    uint16_t fg_g = (fg >>  5) & 0x3F;
    uint16_t fg_b =  fg        & 0x1F;
    uint16_t bg_r = (bg >> 11) & 0x1F;
    uint16_t bg_g = (bg >>  5) & 0x3F;
    uint16_t bg_b =  bg        & 0x1F;
    uint16_t lineCol = (uint16_t)(((fg_r * 70 + bg_r * 30) / 100) << 11)
                     | (uint16_t)(((fg_g * 70 + bg_g * 30) / 100) <<  5)
                     | (uint16_t) ((fg_b * 70 + bg_b * 30) / 100);
    uint16_t fillCol = (uint16_t)(((fg_r * 30 + bg_r * 70) / 100) << 11)
                     | (uint16_t)(((fg_g * 30 + bg_g * 70) / 100) <<  5)
                     | (uint16_t) ((fg_b * 30 + bg_b * 70) / 100);

    for (int dx = 0; dx < dstW; dx++) {
        int sx    = (int)((long)dx * srcW / dstW);
        uint8_t v = bars[sx];
        if (v == 0xFF || v == 0) continue;                         // no data / empty
        int barH  = (int)((long)v * dstH + srcH / 2) / srcH;       // rounded scale
        if (barH <= 0) continue;
        if (barH > dstH) barH = dstH;
        int barY  = y + dstH - barH;
        tft.drawPixel(x + dx, barY, lineCol);                      // line marker 
        tft.drawPixel(x + dx, barY - 1, lineCol);                  // line marker 2nd pixel
        if (barH > 1)
            tft.drawFastVLine(x + dx, barY + 1, barH - 1, fillCol); // area fill
    }
}

// ── Circle-number bitmap renderer ────────────────────────────────────────────
// Draws an 8-bit grayscale PROGMEM bitmap tinted with fg colour on bg colour.

// Renders an 8-bit greyscale PROGMEM circle-number bitmap, tinting non-zero
// pixels with fg blended against bg.
// @param x,y  Top-left pixel position for the CIRCLE_SIZE×CIRCLE_SIZE bitmap.
// @param bmp  Pointer to the PROGMEM bitmap data.
// @param fg   Foreground tint colour (RGB565).
// @param bg   Background colour (RGB565) for transparent (white) pixels.
static void drawCircleNum(int x, int y, const uint8_t* bmp, uint16_t fg, uint16_t bg) {
    uint16_t fg_r = (fg >> 11) & 0x1F;
    uint16_t fg_g = (fg >> 5) & 0x3F;
    uint16_t fg_b = fg & 0x1F;
    uint16_t bg_r = (bg >> 11) & 0x1F;
    uint16_t bg_g = (bg >> 5) & 0x3F;
    uint16_t bg_b = bg & 0x1F;
    uint16_t buf[CIRCLE_SIZE * CIRCLE_SIZE];
    for (int i = 0; i < CIRCLE_SIZE * CIRCLE_SIZE; i++) {
        // Invert: 0 = circle (fg), 255 = background
        uint16_t a = 255 - pgm_read_byte(&bmp[i]);
        uint16_t r = (fg_r * a + bg_r * (255 - a)) / 255;
        uint16_t g = (fg_g * a + bg_g * (255 - a)) / 255;
        uint16_t b = (fg_b * a + bg_b * (255 - a)) / 255;
        buf[i] = (r << 11) | (g << 5) | b;
    }
    tft.setSwapBytes(true);
    tft.pushImage(x, y, CIRCLE_SIZE, CIRCLE_SIZE, buf);
    tft.setSwapBytes(false);
}

// ── Probe cell ────────────────────────────────────────────────────────────────
// Draws one probe cell; x/y/w/h are absolute pixel coordinates.

// Draws a single probe cell including background, graph bars, circle-number
// badge, temperature readout, and hi/lo alarm limit labels.
// @param x,y         Top-left corner of the cell in screen pixels.
// @param w,h         Cell dimensions in pixels.
// @param probeNum    1-based probe number (determines colour scheme).
// @param value       Current temperature in °C.
// @param totalProbes Total number of active probes (affects font/layout sizes).
// @param lo          Low alarm limit in °C (0 = disabled, shown as "--").
// @param hi          High alarm limit in °C (0 = disabled, shown as "--").
void drawProbeCell(int x, int y, int w, int h, int probeNum, float value,
                   int totalProbes, int lo, int hi) {
    tft.fillRect(x, y, w - 1, h - 1, probeColorBg(probeNum));

    // Draw graph background if a cached bitmap is available for this probe.
    // probeNum is 1-based; s_graphBuf[] is 0-based.
    uint8_t pi = (uint8_t)(probeNum - 1);
    if (pi < 6 && s_graphBuf[pi]) {
        drawGraphBars(x, y, w - 1, h - 1,
                      s_graphBuf[pi], s_graphW[pi], s_graphH[pi],
                      probeColor(probeNum), probeColorBg(probeNum));
    }

    if (probeNum >= 1 && probeNum <= 7) {
		// uint8_t circle_pad = (totalProbes <= 2) ? 8 : 0;
        uint8_t circle_pad_x = (w / 2) - 10;
        uint8_t circle_pad_y = (totalProbes <= 2) ? 8 : 0;
        drawCircleNum(x + circle_pad_x, y + circle_pad_y,
                      circleNums[probeNum], probeColor(probeNum), probeColorBg(probeNum));
    }

    int cx = x + w / 2;
    int cy = y + 14 + (h - 24) / 2;
    char tbuf[10];
    const char* tempSign = ((int)value >= 100) ? "" : "\xb0";
    snprintf(tbuf, sizeof(tbuf), "%d%s", (int)value, tempSign);

    (totalProbes <= 2) ? tft.loadFont(MinecartLCD60) : tft.loadFont(MinecartLCD40);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(probeColor(probeNum), probeColorBg(probeNum), false);
    tft.drawString(tbuf, cx, cy);
    tft.unloadFont();

    uint16_t limColor = TFT_WHITE;
    uint8_t hilo_x = (totalProbes <= 2) ? 8 : 6;
    uint8_t hilo_y = (totalProbes <= 2) ? 3 : 3;
    (totalProbes <= 2) ? tft.loadFont(FoundGriBol20) : tft.loadFont(FoundGriBol15);

    // HIGH limit (top-left)
    char hibuf[8];
    if (hi > 0) {
        tft.setTextColor(limColor, probeColorBg(probeNum), false);
        snprintf(hibuf, sizeof(hibuf), "%d\xb0", hi);
    } else {
        tft.setTextColor(tft.color565(128, 128, 128), probeColorBg(probeNum), false);
        snprintf(hibuf, sizeof(hibuf), "--");
    }
    tft.setTextDatum(TL_DATUM);
    tft.drawString(hibuf, x + hilo_x, y + (totalProbes <= 2 ? hilo_y + 7 : hilo_y + 2));

    // HIGH graph value (top-right)
    if (pi < 6 && !isnan(s_graphTMax[pi])) {
        char tmaxbuf[8];
        snprintf(tmaxbuf, sizeof(tmaxbuf), "%d\xb0", (int)s_graphTMax[pi]);
        tft.setTextColor(probeColor(probeNum), probeColorBg(probeNum), false);
        tft.setTextDatum(TR_DATUM);
        tft.drawString(tmaxbuf, x + w - hilo_x, y + (totalProbes <= 2 ? hilo_y + 7 : hilo_y + 2));
    }

    // LOW limit (bottom-left)
    char lobuf[8];
    if (lo > 0) {
        tft.setTextColor(limColor, probeColorBg(probeNum), false);
        snprintf(lobuf, sizeof(lobuf), "%d\xb0", lo);
    } else {
        tft.setTextColor(tft.color565(128, 128, 128), probeColorBg(probeNum), false);
        snprintf(lobuf, sizeof(lobuf), "--");
    }
    tft.setTextDatum(BL_DATUM);
    tft.drawString(lobuf, x + hilo_x, y + h - hilo_y);

    // LOW graph value (bottom-right)
    if (pi < 6 && !isnan(s_graphTMin[pi])) {
        char tminbuf[8];
        snprintf(tminbuf, sizeof(tminbuf), "%d\xb0", (int)s_graphTMin[pi]);
        tft.setTextColor(probeColor(probeNum), probeColorBg(probeNum), false);
        tft.setTextDatum(BR_DATUM);
        tft.drawString(tminbuf, x + w - hilo_x, y + h - hilo_y);
    }
    tft.unloadFont();
}

// ── Probe-area helpers ────────────────────────────────────────────────────────

// Maps the number of active probes to a grid layout (cols × rows).
// Layout rules: 1→1×1, 2→2×1, 3→3×1, 4→2×2, 5–6→3×2.
// @param n     Number of active probes (0–6).
// @param cols  Output: number of grid columns.
// @param rows  Output: number of grid rows.
void getProbeLayout(int n, int& cols, int& rows) {
    if (n <= 1) {
        cols = 1;
        rows = 1;
    } else if (n <= 2) {
        cols = 2;
        rows = 1;
    } else if (n <= 3) {
        cols = 3;
        rows = 1;
    } else if (n <= 4) {
        cols = 2;
        rows = 2;
    } else {
        cols = 3;
        rows = 2;
    }
}

// Clears and redraws the entire probe area from the supplied ProbeVals snapshot.
// Computes the grid layout from the number of active probes, draws each cell,
// and updates the display cache (dcache) with the new values and layout.
// @param v  Snapshot of probe temperatures and state flags to render.
void redrawProbeArea(const ProbeVals& v) {
    tft.setTextFont(1);
    int active[6];
    int n = 0;
    for (int i = 0; i < 6; i++)
        if (!isnan(v.probe[i])) active[n++] = i;

    int cols, rows;
    getProbeLayout(n, cols, rows);
    int cellW = SCREEN_W / cols;
    int cellH = PROBE_AREA_H / rows;

    tft.fillRect(0, HDR_H, SCREEN_W, PROBE_AREA_H, TFT_BLACK);

    if (n == 0) {
        tft.setTextColor(TFT_ORANGE, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(96, HDR_H + PROBE_AREA_H / 2 - 8);
        tft.print("No probes");
    }

    for (int i = 0; i < n; i++) {
        int pi = active[i];
        int lo = preferences.getInt(("alm_" + String(pi + 1) + "_lo").c_str(), 0);
        int hi = preferences.getInt(("alm_" + String(pi + 1) + "_hi").c_str(), 0);
        int col = i % cols;
        int row = i / cols;
        int x = col * cellW;
        int w = (i == n - 1 || col == cols - 1) ? SCREEN_W - x : cellW;
        drawProbeCell(x, HDR_H + row * cellH, w, cellH, pi + 1, v.probe[pi], n, lo, hi);
        dcache.probeVal[pi] = v.probe[pi];
        dcache.almLo[pi] = lo;
        dcache.almHi[pi] = hi;
    }
    for (int i = 0; i < 6; i++)
        if (isnan(v.probe[i])) dcache.probeVal[i] = NAN;
    dcache.numProbes = n;
}

// ── Error-bar overlay (blinking warning) ─────────────────────────────────────

static constexpr int ERR_BAR_H = 34;
static constexpr int ERR_BAR_Y = HDR_H + (PROBE_AREA_H - ERR_BAR_H) / 2;

// Draws (show=true) or clears (show=false) the blinking error bar overlay.
// When cleared, the probe area is redrawn from the lastKnownVals cache.
// @param show  true to render the red "Device not found" bar; false to clear it.
void drawErrorBar(bool show) {
    if (show) {
        uint16_t ebg = tft.color565(180, 20, 20);
        tft.fillRect(0, ERR_BAR_Y, SCREEN_W, ERR_BAR_H, ebg);
        tft.setTextColor(TFT_WHITE, ebg);
        tft.setTextSize(2);
        tft.setCursor(64, ERR_BAR_Y + (ERR_BAR_H - 16) / 2);
        tft.print("NC01 not found!");
    } else {
        redrawProbeArea(lastKnownVals);
    }
}

// ── Graph bitmap cache management ────────────────────────────────────────────
// Called on Core 1 by httpTaskDrainGraphResults() after a successful graph fetch.
// Takes ownership of buf (frees any previously cached buffer for this probe).
// Invalidates the display cache so the probe area redraws with the new graph.
// Stores a new graph bitmap for one probe and immediately redraws the probe area.
// Called on Core 1 by httpTaskDrainGraphResults() after a successful graph fetch.
// Takes ownership of buf (frees any previously cached buffer for that probe index).
// @param probeIdx  0-based probe index (0–5); calls with idx >= 6 are ignored.
// @param buf       Heap-allocated column-height array from graph_esp.php (caller must not free).
// @param w,h       Dimensions of the payload in source grid pixels.
// @param tMin      Minimum temperature visible in the graph (°C).
// @param tMax      Maximum temperature visible in the graph (°C).
void displayProbeSetGraph(uint8_t probeIdx, uint8_t* buf, uint16_t w, uint16_t h, float tMin, float tMax) {
    if (probeIdx >= 6) {
        free(buf);
        return;
    }
    free(s_graphBuf[probeIdx]);
    s_graphBuf[probeIdx]  = buf;
    s_graphW[probeIdx]    = w;
    s_graphH[probeIdx]    = h;
    s_graphTMin[probeIdx] = tMin;
    s_graphTMax[probeIdx] = tMax;
    // Redraw immediately — this runs on Core 1 so TFT access is safe.
    // updateDisplay() only fires when jsonData changes, so we can't rely on it.
    if (!isOnSettingsPage)
        redrawProbeArea(lastKnownVals);
}
