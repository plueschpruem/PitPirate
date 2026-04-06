#include "display.h"
#include "display_fonts.h"

#include <WiFi.h>
#include <LittleFS.h>

#include "../fan_control.h"
#include "../network/wifi_manager.h"
#include "../pid_fan.h"
#include "../debug_log.h"
#include "../remote_post.h"

// ── Fan settings section (lower portion of settings page) ────────────────────

// Draws (or redraws) only the PID mode toggle button, used for fast updates
// without redrawing the entire settings page.
// @param enabled  true = PID ON (green button); false = PID OFF (red button).
void drawPidModeBtn(bool enabled) {
    uint16_t bg = enabled ? tft.color565(0, 140, 0) : tft.color565(140, 0, 0);
    uint16_t bdr = enabled ? tft.color565(0, 220, 60) : tft.color565(220, 60, 60);
    tft.fillRoundRect(PIDMODE_BTN_X, PIDMODE_BTN_Y, PIDMODE_BTN_W, PIDMODE_BTN_H, 8, bg);
    tft.drawRoundRect(PIDMODE_BTN_X, PIDMODE_BTN_Y, PIDMODE_BTN_W, PIDMODE_BTN_H, 8, bdr);
    const char* label = enabled ? "PID ON" : "PID OFF";
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg, true);
    tft.drawString(label, PIDMODE_BTN_X + PIDMODE_BTN_W / 2,
                   PIDMODE_BTN_Y + PIDMODE_BTN_H / 2 + 3);
    tft.unloadFont();
}

// Draws the fan-control section of the settings page: quick-set row
// (ON/OFF, MIN, 50%, 100%), entry-speed button, drag slider, and speed readout.
// @param pct       Fan speed currently being shown/applied (0–100).
// @param entryPct  Fan speed that was active when the settings page was opened;
//                  used as the label for the "return to entry" button.
void drawFanSection(uint8_t pct, uint8_t entryPct) {
    tft.setTextFont(1);

    // Clear the fan area
    tft.fillRect(0, FAN_SECT_Y, SCREEN_W, SCREEN_H - FAN_SECT_Y, TFT_BLACK);

    // Section label
    tft.loadFont(FoundGriBol15);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(tft.color565(130, 130, 130), TFT_BLACK, true);
    tft.drawString("FAN SPEED", 5, (FAN_SECT_Y + FAN_ROW1_Y) / 2);
    tft.unloadFont();

    // ── Row 1: 4 quick-set buttons ───────────────────────────────────────────
    tft.loadFont(FoundGriBol20);
    bool fanOn = (pct > 0);
    char entryBuf[6];
    snprintf(entryBuf, sizeof(entryBuf), "%d%%", entryPct);
    char minBuf[6];
    snprintf(minBuf, sizeof(minBuf), "%d%%", fanGetMinPct());

    // [0] ON/OFF  [1] MIN  [2] 50%  [3] 100%
    const char* row1Labels[4] = {fanOn ? "ON" : "OFF", minBuf, "50%", "100%"};
    uint16_t row1Bg[4] = {
        fanOn ? tft.color565(0, 140, 0) : tft.color565(140, 0, 0),
        tft.color565(40, 70, 160),
        tft.color565(40, 70, 160),
        tft.color565(40, 70, 160)};

    for (int i = 0; i < 4; i++) {
        int bx = i * (FAN_BTN_W + FAN_BTN_GAP);
        int bw = (i == 3) ? (SCREEN_W - bx - 5) : FAN_BTN_W;  // last btn: 5 px from right edge
        uint16_t bg = row1Bg[i];
        tft.fillRoundRect(bx, FAN_ROW1_Y, bw, FAN_ROW1_H, 4, bg);
        tft.drawRoundRect(bx, FAN_ROW1_Y, bw, FAN_ROW1_H, 4, tft.color565(90, 130, 210));
        tft.loadFont(FoundGriBol20);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, bg, true);
        tft.drawString(row1Labels[i], bx + bw / 2, FAN_ROW1_Y + FAN_ROW1_H / 2 + 3);
        tft.unloadFont();
    }

    // ── Row 2: entry-speed button  +  slider  +  current speed ──────────────
    // Entry-speed button (mirrors row-1 button 2)
    {
        uint16_t bg = tft.color565(70, 70, 70);
        tft.fillRoundRect(0, FAN_ROW2_Y, FAN_BTN_W, FAN_ROW2_H, 4, bg);
        tft.drawRoundRect(0, FAN_ROW2_Y, FAN_BTN_W, FAN_ROW2_H, 4, tft.color565(90, 130, 210));
        tft.loadFont(FoundGriBol20);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, bg, true);
        tft.drawString(entryBuf, FAN_BTN_W / 2, FAN_ROW2_Y + FAN_ROW2_H / 2 + 3);
        tft.unloadFont();
    }

    // Slider track (50 % width)
    int hx = FAN_SLIDER_X + (int)pct * FAN_SLIDER_W / 100;
    int trCY = FAN_SLIDER_TY + FAN_SLIDER_TH / 2;
    if (hx > FAN_SLIDER_X)
        tft.fillRoundRect(FAN_SLIDER_X, FAN_SLIDER_TY,
                          hx - FAN_SLIDER_X, FAN_SLIDER_TH,
                          2, tft.color565(60, 120, 220));
    if (hx < FAN_SLIDER_X + FAN_SLIDER_W)
        tft.fillRoundRect(hx, FAN_SLIDER_TY,
                          FAN_SLIDER_X + FAN_SLIDER_W - hx, FAN_SLIDER_TH,
                          2, tft.color565(50, 50, 50));
    tft.fillCircle(hx, trCY, FAN_HANDLE_R, tft.color565(100, 160, 255));
    tft.drawCircle(hx, trCY, FAN_HANDLE_R, TFT_WHITE);

    // Current speed — right of the slider
    char pctbuf[6];
    snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
    int textX = FAN_SLIDER_X + FAN_SLIDER_W + FAN_BTN_GAP;
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
    tft.drawString(pctbuf, textX, FAN_ROW2_Y + FAN_ROW2_H / 2);
    tft.unloadFont();
}

// Redraws only the RSSI readout in the top-right of the settings header.
// Colour-coded: green (≥−60 dBm), yellow (≥−75 dBm), orange (weaker).
void drawSettingsRssi() {
    int32_t rssi = WiFi.RSSI();
    char buf[10];
    snprintf(buf, sizeof(buf), "%ddBm", (int)rssi);
    uint16_t hbg = tft.color565(30, 30, 30);
    uint16_t col = (rssi >= -60) ? tft.color565(0, 220, 80)
                 : (rssi >= -75) ? TFT_YELLOW
                                 : tft.color565(220, 80, 0);
    // Clear right half of header (safe past the centred "Settings" label)
    tft.fillRect(SCREEN_W / 2 + 50, 0, SCREEN_W / 2 - 50, HDR_H, hbg);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(col, hbg, true);
    tft.drawString(buf, SCREEN_W - 4, 4);
    tft.unloadFont();
}

// Draws the full settings page 1: header with back button and SSID/IP info,
// Boot AP button, PID toggle, and fan speed section.
void drawSettingsPage() {
    tft.setTextFont(1);
    tft.fillScreen(TFT_BLACK);

    uint16_t hbg = tft.color565(30, 30, 30);
    tft.fillRect(0, 0, SCREEN_W, HDR_H, hbg);

    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, hbg, true);
    tft.drawString("< Back", 8, 4);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, hbg, true);
    tft.drawString("Settings", SCREEN_W / 2, 4);
    tft.unloadFont();
    drawSettingsRssi();

    constexpr int INFO_Y = HDR_H + 10;
    String ssid = wifiGetSSID();
    String ip = WiFi.localIP().toString();

    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(130, 130, 130), TFT_BLACK, true);
    tft.drawString("SSID:", 5, INFO_Y);
    tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
    tft.drawString(ssid, 5 + tft.textWidth("SSID:") + 8, INFO_Y);
    tft.setTextColor(tft.color565(130, 130, 130), TFT_BLACK, true);
    tft.drawString("IP:", 5, INFO_Y + 22);
    tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
    tft.drawString(ip, 5 + tft.textWidth("SSID:") + 8, INFO_Y + 22);
    tft.unloadFont();

    // "Boot AP" button (left half)
    uint16_t btnBg = tft.color565(40, 60, 150);
    uint16_t btnBdr = tft.color565(80, 120, 220);
    tft.fillRoundRect(APMODE_BTN_X, APMODE_BTN_Y, APMODE_BTN_W, APMODE_BTN_H, 8, btnBg);
    tft.drawRoundRect(APMODE_BTN_X, APMODE_BTN_Y, APMODE_BTN_W, APMODE_BTN_H, 8, btnBdr);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, btnBg, true);
    tft.drawString("Boot AP", APMODE_BTN_X + APMODE_BTN_W / 2,
                   APMODE_BTN_Y + APMODE_BTN_H / 2 + 3);
    tft.unloadFont();

    // "PID on/off" button (right half)
    drawPidModeBtn(pidFanIsEnabled());

    uint8_t curPct = fanGetPercent();
    drawFanSection(curPct, curPct);
    drawSettingsMoreLink();
}

// Draws the "More >" link in the bottom-right corner of settings page 1,
// used to navigate to settings page 2.
void drawSettingsMoreLink() {
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(BR_DATUM);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK, true);
    tft.drawString("More >", SCREEN_W - 6, SCREEN_H - 4);
    tft.unloadFont();
}

// Draws (or redraws) the UART debug-log toggle button on settings page 2.
// @param enabled  true = logging ON (green); false = logging OFF (blue).
void drawDebugLogBtn(bool enabled) {
    uint16_t bg  = enabled ? tft.color565(0, 140, 0)   : tft.color565(40, 60, 150);
    uint16_t bdr = enabled ? tft.color565(0, 220, 60)  : tft.color565(80, 120, 220);
    tft.fillRoundRect(DBGLOG_BTN_X, DBGLOG_BTN_Y, DBGLOG_BTN_W, DBGLOG_BTN_H, 8, bg);
    tft.drawRoundRect(DBGLOG_BTN_X, DBGLOG_BTN_Y, DBGLOG_BTN_W, DBGLOG_BTN_H, 8, bdr);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg, true);
    tft.drawString(enabled ? "UART Logging: ON" : "UART Logging: OFF",
                   DBGLOG_BTN_X + DBGLOG_BTN_W / 2,
                   DBGLOG_BTN_Y + DBGLOG_BTN_H / 2 + 3);
    tft.unloadFont();
}

// Draws (or redraws) the telemetry "Send on change" toggle button.
// @param on_change  true = send-on-change ON (green); false = OFF (blue).
void drawTelOnChangeBtn(bool on_change) {
    uint16_t bg  = on_change ? tft.color565(0, 140, 0)  : tft.color565(40, 60, 150);
    uint16_t bdr = on_change ? tft.color565(0, 220, 60) : tft.color565(80, 120, 220);
    tft.fillRoundRect(TEL_ONCHG_BTN_X, TEL_ONCHG_BTN_Y, TEL_ONCHG_BTN_W, TEL_ONCHG_BTN_H, 8, bg);
    tft.drawRoundRect(TEL_ONCHG_BTN_X, TEL_ONCHG_BTN_Y, TEL_ONCHG_BTN_W, TEL_ONCHG_BTN_H, 8, bdr);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg, true);
    tft.drawString(on_change ? "Send on change: ON" : "Send on change: OFF",
                   TEL_ONCHG_BTN_X + TEL_ONCHG_BTN_W / 2,
                   TEL_ONCHG_BTN_Y + TEL_ONCHG_BTN_H / 2 + 3);
    tft.unloadFont();
}

// Draws the four telemetry interval selector buttons (10 s / 30 s / 1 min / 3 min),
// highlighting the currently active interval in green.
// @param interval_s  Currently selected interval in seconds (10, 30, 60, or 180).
void drawTelIntervalBtns(uint32_t interval_s) {
    static const uint32_t opts[4]   = {10, 30, 60, 180};
    static const char*    labels[4] = {"10s", "30s", "1min", "3min"};
    tft.loadFont(FoundGriBol20);
    for (int i = 0; i < 4; i++) {
        int bx  = 5 + i * (TEL_INT_BTN_W + TEL_INT_BTN_GAP);
        bool sel = (interval_s == opts[i]);
        uint16_t bg  = sel ? tft.color565(0, 140, 0)  : tft.color565(40, 60, 150);
        uint16_t bdr = sel ? tft.color565(0, 220, 60) : tft.color565(80, 120, 220);
        tft.fillRoundRect(bx, TEL_INT_BTN_Y, TEL_INT_BTN_W, TEL_INT_BTN_H, 6, bg);
        tft.drawRoundRect(bx, TEL_INT_BTN_Y, TEL_INT_BTN_W, TEL_INT_BTN_H, 6, bdr);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(TFT_WHITE, bg, true);
        tft.drawString(labels[i], bx + TEL_INT_BTN_W / 2, TEL_INT_BTN_Y + TEL_INT_BTN_H / 2 + 3);
    }
    tft.unloadFont();
}

// Draws the "Touch Calibration" button on settings page 2.
void drawCalibBtn() {
    uint16_t bg  = tft.color565(40, 60, 150);
    uint16_t bdr = tft.color565(80, 120, 220);
    tft.fillRoundRect(CALIB_BTN_X, CALIB_BTN_Y, CALIB_BTN_W, CALIB_BTN_H, 8, bg);
    tft.drawRoundRect(CALIB_BTN_X, CALIB_BTN_Y, CALIB_BTN_W, CALIB_BTN_H, 8, bdr);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg, true);
    tft.drawString("Touch Calibration",
                   CALIB_BTN_X + CALIB_BTN_W / 2,
                   CALIB_BTN_Y + CALIB_BTN_H / 2 + 2);
    tft.unloadFont();
}

// Draws the full settings page 2: header with back button, UART debug-log
// toggle, telemetry section (on-change toggle, interval buttons), and touch
// calibration button.
void drawSettingsPage2() {
    tft.setTextFont(1);
    tft.fillScreen(TFT_BLACK);

    uint16_t hbg = tft.color565(30, 30, 30);
    tft.fillRect(0, 0, SCREEN_W, HDR_H, hbg);

    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, hbg, true);
    tft.drawString("< Back", 8, 4);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, hbg, true);
    tft.drawString("Settings 2", SCREEN_W / 2, 4);
    tft.unloadFont();
    drawSettingsRssi();
    drawDebugLogBtn(debugLogEnabled());

    // Section label
    tft.loadFont(FoundGriBol15);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(tft.color565(130, 130, 130), TFT_BLACK, true);
    tft.drawString("TELEMETRY POST", 5, TEL_SECT_Y + 8);
    tft.unloadFont();

    uint32_t telInterval;
    bool     telOnChange;
    remotePostGetTelemetryConfig(telInterval, telOnChange);
    drawTelOnChangeBtn(telOnChange);
    drawTelIntervalBtns(telInterval);
    drawCalibBtn();

    drawSettingsMoreLink();
}

// Draws the full settings page 3 (System Info): header with back button and
// a list of runtime diagnostics — heap usage, PSRAM (if present), CPU speed,
// flash size, sketch footprint, LittleFS usage, uptime, and MAC address.
void drawSettingsPage3() {
    tft.setTextFont(1);
    tft.fillScreen(TFT_BLACK);

    // ── Header ───────────────────────────────────────────────────────────────
    uint16_t hbg = tft.color565(30, 30, 30);
    tft.fillRect(0, 0, SCREEN_W, HDR_H, hbg);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, hbg, true);
    tft.drawString("< Back", 8, 4);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_YELLOW, hbg, true);
    tft.drawString("System Info", SCREEN_W / 2, 4);
    tft.unloadFont();
    drawSettingsRssi();

    // ── Info rows (FoundGriBol15, label dimmed / value white) ────────────────
    constexpr int LBL_X = 5;
    constexpr int VAL_X = SCREEN_W - 5;
    constexpr int ROW_H = 20;
    int r = 0;
    uint16_t lblCol = tft.color565(130, 130, 130);

    auto drawRow = [&](const char* lbl, const char* val) {
        int y = HDR_H + 8 + r * ROW_H;
        tft.loadFont(FoundGriBol15);
        tft.setTextDatum(ML_DATUM);
        tft.setTextColor(lblCol, TFT_BLACK, true);
        tft.drawString(lbl, LBL_X, y + ROW_H / 2);
        tft.setTextDatum(MR_DATUM);
        tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
        tft.drawString(val, VAL_X, y + ROW_H / 2);
        tft.unloadFont();
        r++;
    };

    // Heap free / total
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "%lu / %lu KB",
                 ESP.getFreeHeap() / 1024, ESP.getHeapSize() / 1024);
        drawRow("Free Heap:", buf);
    }
    // Heap low-watermark
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu KB", ESP.getMinFreeHeap() / 1024);
        drawRow("Heap Low Wtr:", buf);
    }
    // PSRAM (skip row entirely when absent)
    {
        uint32_t psTotal = ESP.getPsramSize();
        if (psTotal > 0) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%lu / %lu KB",
                     ESP.getFreePsram() / 1024, psTotal / 1024);
            drawRow("Free PSRAM:", buf);
        }
    }
    // CPU frequency
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%u MHz", ESP.getCpuFreqMHz());
        drawRow("CPU Freq:", buf);
    }
    // Flash chip size
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%lu MB",
                 ESP.getFlashChipSize() / (1024UL * 1024UL));
        drawRow("Flash:", buf);
    }
    // Sketch used / partition total
    {
        char buf[24];
        uint32_t used = ESP.getSketchSize();
        uint32_t tot  = used + ESP.getFreeSketchSpace();
        snprintf(buf, sizeof(buf), "%lu / %lu KB", used / 1024, tot / 1024);
        drawRow("Sketch:", buf);
    }
    // LittleFS usage
    {
        char buf[24];
        size_t used  = LittleFS.usedBytes();
        size_t total = LittleFS.totalBytes();
        snprintf(buf, sizeof(buf), "%u / %u KB",
                 (unsigned)(used / 1024), (unsigned)(total / 1024));
        drawRow("LittleFS:", buf);
    }
    // Uptime
    {
        uint32_t s = millis() / 1000;
        uint32_t m = s / 60; s %= 60;
        uint32_t h = m / 60; m %= 60;
        uint32_t d = h / 24; h %= 24;
        char buf[20];
        if (d > 0) snprintf(buf, sizeof(buf), "%lud %luh %02lum", d, h, m);
        else       snprintf(buf, sizeof(buf), "%luh %02lum %02lus", h, m, s);
        drawRow("Uptime:", buf);
    }
    // MAC address
    {
        drawRow("MAC:", WiFi.macAddress().c_str());
    }
}
