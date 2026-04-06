#include "display.h"

#include <time.h>

#include "../display/png_image.h"
#include "../fan_control.h"
#include "../network/wifi_manager.h"
#include "../probe_data.h"
#include "../shared_data.h"
#include "config.h"
#include "tft_qr_display.h"
#include "../http_task.h"

// Font data — headers with raw PROGMEM arrays and no include guard.
// Each must be included in exactly one translation unit.
#include "../fonts/FoundGriBol15.h"
#include "../fonts/FoundGriBol20.h"
#include "../fonts/MinecartLCD20.h"
#include "../fonts/MinecartLCD40.h"
#include "../fonts/MinecartLCD60.h"

// ── Global state ──────────────────────────────────────────────────────────────

DisplayCache dcache;
ProbeVals lastKnownVals = {{NAN, NAN, NAN, NAN, NAN, NAN}, NAN, -1, false, false};
bool inErrorState = false;
bool errorBarOn = false;
unsigned long lastBlinkMs = 0;
bool isOnSettingsPage = false;

// ── Probe colour helpers ──────────────────────────────────────────────────────

// Returns the foreground RGB565 colour associated with a given probe number.
// Colours: 1=red, 2=orange, 3=yellow, 4=green, 5=purple, 7=gray (ambient).
// @param probeNum  1-based probe number (1–6 = food, 7 = ambient).
uint16_t probeColor(int probeNum) {
    switch (probeNum) {
        case 1:
            return tft.color565(255, 0, 0);  // red
        case 2:
            return tft.color565(255, 165, 0);  // orange
        case 3:
            return tft.color565(255, 255, 0);  // yellow
        case 4:
            return tft.color565(0, 255, 0);  // green
        case 5:
            return tft.color565(160, 32, 240);  // purple
        case 7:
            return tft.color565(160, 160, 160);  // gray (ambient/PIT)
        default:
            return TFT_WHITE;
    }
}

// Returns the dark background RGB565 colour associated with a given probe number.
// Used as the cell background and for icon tinting to create a colour-coded look.
// @param probeNum  1-based probe number (1–6 = food, 7 = ambient).
uint16_t probeColorBg(int probeNum) {
    switch (probeNum) {
        case 1:
            return tft.color565(80, 0, 2);
        case 2:
            return tft.color565(80, 60, 0);
        case 3:
            return tft.color565(60, 80, 0);
        case 4:
            return tft.color565(0, 80, 2);
        case 5:
            return tft.color565(40, 10, 76);
        case 7:
            return tft.color565(51, 51, 51);
        default:
            return TFT_WHITE;
    }
}

// ── Full-screen views ─────────────────────────────────────────────────────────

// Returns true when the integer-rounded value changed; handles NAN.
// Returns true when the integer-rounded temperature value has changed or one of
// them is NAN while the other is not (handles the disconnected-probe case).
// @param current  Most recent temperature reading.
// @param cached   Previously rendered temperature value.
static bool floatChanged(float current, float cached) {
    if (isnan(current) && isnan(cached)) return false;
    if (isnan(current) || isnan(cached)) return true;
    return (int)current != (int)cached;
}

// Top-level display refresh: routes to the correct full-screen view (AP setup,
// connecting, or normal operating screen with header + probe area + footer).
// Must be called from Core 1 only (not from Core 0 or ISR context).
void updateDisplay() {
    tft.setTextFont(1);
    ProbeVals v = parseProbeVals();

    // ── AP provisioning screen ────────────────────────────────────────────────
    if (v.apMode) {
        tft.fillScreen(TFT_BLACK);

        constexpr int QR_PANEL_X = 165;
        constexpr int QR_PANEL_W = SCREEN_W - QR_PANEL_X;  // 134 px
        tft.fillRect(QR_PANEL_X, 0, QR_PANEL_W, QR_PANEL_W, TFT_WHITE);

        // WiFi QR — format: WIFI:T:WPA;S:<ssid>;P:<pw>;;
        String qrStr = String("WIFI:T:WPA;S:") + AP_SSID + ";P:" + AP_PW + ";;";
        constexpr int QR_SCALE = 3;
        constexpr int QR_X = QR_PANEL_X + 10;
        constexpr int QR_Y = 12;
        TFTQRDisplay qr(tft, QR_X, QR_Y, QR_SCALE, TFT_BLACK, TFT_WHITE);
        qr.create(qrStr);

        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(8, 10);
        tft.print("WiFi Setup");

        tft.setTextColor(tft.color565(150, 150, 150), TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(8, 48);
        tft.print("Scan QR or connect to:");

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setCursor(8, 70);
        tft.print("Network:");
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(8, 82);
        tft.print(AP_SSID);

        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(8, 106);
        tft.print("Password:");
        tft.setTextColor(TFT_CYAN, TFT_BLACK);
        tft.setTextSize(2);
        tft.setCursor(8, 118);
        tft.print(AP_PW);

        tft.setTextColor(tft.color565(150, 150, 150), TFT_BLACK);
        tft.setTextSize(1);
        tft.setCursor(8, 148);
        tft.print("Then open 192.168.4.1");

        uint16_t rstBg = tft.color565(120, 20, 20);
        uint16_t rstBdr = tft.color565(200, 60, 60);
        tft.fillRoundRect(APRESET_BTN_X, APRESET_BTN_Y, APRESET_BTN_W, APRESET_BTN_H, 5, rstBg);
        tft.drawRoundRect(APRESET_BTN_X, APRESET_BTN_Y, APRESET_BTN_W, APRESET_BTN_H, 5, rstBdr);
        tft.setTextColor(TFT_WHITE, rstBg);
        tft.setTextSize(2);
        tft.setCursor(APRESET_BTN_X + (APRESET_BTN_W - 5 * 12) / 2,
                      APRESET_BTN_Y + (APRESET_BTN_H - 16) / 2);
        tft.print("Restart");

        dcache.invalidate();
        return;
    }

    // ── Connecting screen ─────────────────────────────────────────────────────
    if (v.connecting) {
        tft.fillScreen(TFT_BLACK);
        // Text centred in the left column (right side reserved for the GIF)
        constexpr int GIF_COL_W = 95 + 8;
        constexpr int LEFT_W = SCREEN_W - GIF_COL_W;  // 217 px (unused but kept for clarity)

        tft.loadFont(FoundGriBol20);
        tft.setTextSize(3);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(tft.color565(200, 0, 200));
        tft.drawString("Connecting to AP...", SCREEN_W / 2, 15);

        String connSSID = wifiGetSSID();
        // tft.setTextDatum(ML_DATUM);
        tft.setTextColor(TFT_GREEN);
        tft.setTextSize(2);
        tft.drawString(connSSID, SCREEN_W / 2, 50);
        tft.setTextFont(1);
        dcache.invalidate();
        return;
    }

    // ── Header ────────────────────────────────────────────────────────────────
    {
        struct tm ti;
        int curMinute = getLocalTime(&ti, 0) ? ti.tm_hour * 60 + ti.tm_min : -1;
        int curFan = (int)fanGetPercent();
        bool curNetLag = httpConnectivityAlertActive();
        if (curMinute != dcache.hdrMinute || v.battery != dcache.hdrBattery || curFan != dcache.hdrFan || curNetLag != dcache.hdrNetLag) {
            drawHeader(curMinute, v.battery, curFan, curNetLag);
            dcache.hdrMinute = curMinute;
            dcache.hdrBattery = v.battery;
            dcache.hdrFan = curFan;
            dcache.hdrNetLag = curNetLag;
        }
    }

    // ── Probe area ────────────────────────────────────────────────────────────
    if (!v.hasError) lastKnownVals = v;
    {
        const ProbeVals& vals = v.hasError ? lastKnownVals : v;
        int active[6], n = 0;
        for (int i = 0; i < 6; i++)
            if (!isnan(vals.probe[i])) active[n++] = i;

        bool errorCleared = inErrorState && !v.hasError;
        bool needFullRedraw = (n != dcache.numProbes) || errorCleared || v.hasError;

        if (needFullRedraw) {
            redrawProbeArea(vals);
        } else {
            int cols, rows;
            getProbeLayout(n, cols, rows);
            int cellW = SCREEN_W / cols;
            int cellH = PROBE_AREA_H / rows;
            for (int i = 0; i < n; i++) {
                int pi = active[i];
                int lo = preferences.getInt(("alm_" + String(pi + 1) + "_lo").c_str(), 0);
                int hi = preferences.getInt(("alm_" + String(pi + 1) + "_hi").c_str(), 0);
                if (floatChanged(vals.probe[pi], dcache.probeVal[pi]) ||
                    lo != dcache.almLo[pi] || hi != dcache.almHi[pi]) {
                    int col = i % cols;
                    int row = i / cols;
                    int x = col * cellW;
                    int w = (i == n - 1 || col == cols - 1) ? SCREEN_W - x : cellW;
                    drawProbeCell(x, HDR_H + row * cellH, w, cellH,
                                  pi + 1, vals.probe[pi], n, lo, hi);
                    dcache.probeVal[pi] = vals.probe[pi];
                    dcache.almLo[pi] = lo;
                    dcache.almHi[pi] = hi;
                }
            }
        }
    }

    // ── Error bar (drawn on top of probe area) ────────────────────────────────
    inErrorState = v.hasError;
    if (v.hasError) {
        errorBarOn = true;
        lastBlinkMs = millis();
        drawErrorBar(true);
    }

    // ── Footer (ambient / pit temperature strip) ──────────────────────────────
    {
        int ambLo = preferences.getInt("alm_7_lo", 0);
        int ambHi = preferences.getInt("alm_7_hi", 0);
        if (floatChanged(v.ambient, dcache.ambVal) ||
            ambLo != dcache.ambAlmLo || ambHi != dcache.ambAlmHi) {
            drawFooterStrip(v.ambient, ambLo, ambHi);
            dcache.ambVal = v.ambient;
            dcache.ambAlmLo = ambLo;
            dcache.ambAlmHi = ambHi;
        }
    }
}

void showSplash() {
    tft.fillScreen(TFT_BLACK);
    drawPngFromFs(tft, "/BootSplash.png", 0, 0, TFT_BLACK);
    tft.setTextColor(TFT_ORANGE);
    tft.setTextSize(2);
    tft.setTextDatum(BR_DATUM);
    tft.loadFont(FoundGriBol15);
    tft.setTextColor(TFT_GOLD);
    char versBuff[10];

    snprintf(versBuff, sizeof(versBuff), "%s%s", "v", PITPIRATE_VERSION);
    delay(2000);
    tft.drawString(versBuff, TFT_HEIGHT - 25, TFT_WIDTH - 5);
    tft.unloadFont();
    delay(4000);
}
