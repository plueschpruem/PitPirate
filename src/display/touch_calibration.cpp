// PitPirate — XPT2046 touch calibration: NVS storage + calibration screen
//
// Step 0  : start screen — instruction text + "Start" button
// Steps 1–4 : capture one corner each (TL→TR→BR→BL, clockwise)
//             Each step shows: colored 10×10 corner marker, live raw readout,
//             disabled "Next" button until the user taps the screen once.
// After step 4 : derive xMin/xMax/yMin/yMax, persist to NVS, reload cache.

#include "touch_calibration.h"
#include "display.h"
#include "display_fonts.h"
#include "display_config.h"
#include "../shared_data.h"   // preferences

#include <Arduino.h>

// ── NVS keys ──────────────────────────────────────────────────────────────────
static const char* K_XMIN = "cal_x_min";
static const char* K_XMAX = "cal_x_max";
static const char* K_YMIN = "cal_y_min";
static const char* K_YMAX = "cal_y_max";

// ── Cached calibration values (reloaded from NVS by touchCalibInit) ──────────
static int s_xMin = 150;
static int s_xMax = 3900;
static int s_yMin = 150;
static int s_yMax = 3900;

// ── Internal session state ────────────────────────────────────────────────────
static int  s_step       = 0;       // 0=start screen, 1-4=corner steps
static int  s_raw[4][2]  = {};      // accumulated raw ADC [corner 0-3][x,y]
static int  s_curRaw[2]  = {};      // raw values for the current step
static bool s_hasPoint   = false;   // has the user tapped the screen this step?
static bool s_cancelled  = false;

// ── Button / marker geometry (private to this file) ──────────────────────────
static const int CBTN_W   = 130;
static const int CBTN_H   = 34;
static const int CBTN_X   = (SCREEN_W  - CBTN_W) / 2;   // 95
static const int CBTN_Y   = SCREEN_H - CBTN_H - 30;      // 176
static const int CANCEL_Y = SCREEN_H - 14;               // 226 — cancel tap strip

static const int MARKER   = 10;   // corner square size (px)
static const int PAD      = 2;    // gap from screen edge

// ── Per-corner descriptors ────────────────────────────────────────────────────
struct CornerInfo {
    int      mx, my;       // top-left of the 10×10 marker square
    uint16_t color;
    const char* label;     // human-readable corner name
};

// Returns a CornerInfo descriptor for the given calibration step (1–4).
// Describes the screen position, colour, and label for each corner marker.
// @param step  Calibration step number (1 = TL, 2 = TR, 3 = BR, 4 = BL).
static CornerInfo cornerInfo(int step) {
    switch (step) {
    case 1: return { PAD,                          PAD,                         TFT_YELLOW, "top-left (yellow)"    };
    case 2: return { SCREEN_W - MARKER - PAD,      PAD,                         TFT_RED,    "top-right (red)"      };
    case 3: return { SCREEN_W - MARKER - PAD,      SCREEN_H - MARKER - PAD,     TFT_GREEN,  "bottom-right (green)" };
    case 4: return { PAD,                          SCREEN_H - MARKER - PAD,     TFT_CYAN,   "bottom-left (cyan)"   };
    }
    return { 0, 0, TFT_WHITE, "?" };
}

// ── Internal helpers ──────────────────────────────────────────────────────────
// Renders the small "< Cancel" text link centred at the screen bottom.
static void drawCancelLink() {
    tft.loadFont(FoundGriBol15);
    tft.setTextDatum(BC_DATUM);
    tft.setTextColor(tft.color565(100, 100, 100), TFT_BLACK, true);
    tft.drawString("< Cancel", SCREEN_W / 2, SCREEN_H - 2);
    tft.unloadFont();
}

// Derives xMin/xMax/yMin/yMax from the four captured raw corner points,
// swaps axes if physically inverted, persists all four values to NVS,
// then immediately reloads the calibration cache via touchCalibInit().
// @param rawPoints  4×2 array of raw ADC values; [corner][0]=X, [corner][1]=Y.
//                   Corners are ordered TL, TR, BR, BL (steps 1–4).
static void saveCalibration(int rawPoints[4][2]) {
    // Average each axis edge from two corner readings
    int xMin = (rawPoints[0][0] + rawPoints[3][0]) / 2;   // TL.x + BL.x
    int xMax = (rawPoints[1][0] + rawPoints[2][0]) / 2;   // TR.x + BR.x
    int yMin = (rawPoints[0][1] + rawPoints[1][1]) / 2;   // TL.y + TR.y
    int yMax = (rawPoints[3][1] + rawPoints[2][1]) / 2;   // BL.y + BR.y
    // Swap if touch axis is physically inverted
    if (xMin > xMax) { int t = xMin; xMin = xMax; xMax = t; }
    if (yMin > yMax) { int t = yMin; yMin = yMax; yMax = t; }
    preferences.putInt(K_XMIN, xMin);
    preferences.putInt(K_XMAX, xMax);
    preferences.putInt(K_YMIN, yMin);
    preferences.putInt(K_YMAX, yMax);
    touchCalibInit(); // reload cache immediately
}

// ── Public API ────────────────────────────────────────────────────────────────

// Loads calibration min/max values from NVS into the module-static cache.
// Falls back to 150/3900 defaults when no saved calibration exists.
// Must be called once from setup() and again after saveCalibration() completes.
void touchCalibInit() {
    s_xMin = preferences.getInt(K_XMIN, 150);
    s_xMax = preferences.getInt(K_XMAX, 3900);
    s_yMin = preferences.getInt(K_YMIN, 150);
    s_yMax = preferences.getInt(K_YMAX, 3900);
}

// Maps raw XPT2046 ADC coordinates to screen pixel coordinates using the
// cached calibration values.
// @param rawX  Raw ADC X reading from the XPT2046 driver.
// @param rawY  Raw ADC Y reading from the XPT2046 driver.
// @param tx    Output: mapped X position in screen pixels (0–SCREEN_W−1), clamped.
// @param ty    Output: mapped Y position in screen pixels (0–SCREEN_H−1), clamped.
void touchCalibMap(int rawX, int rawY, int& tx, int& ty) {
    tx = (int)constrain(map(rawX, s_xMin, s_xMax, 0, SCREEN_W - 1), 0, SCREEN_W - 1);
    ty = (int)constrain(map(rawY, s_yMin, s_yMax, 0, SCREEN_H - 1), 0, SCREEN_H - 1);
}

// Returns true if the most recent calibration session was cancelled by the user
// (tapped "< Cancel") rather than completed normally.
bool touchCalibWasCancelled() { return s_cancelled; }

// ── Draw: start screen ────────────────────────────────────────────────────────
// Draws the introductory calibration start-screen with preview corner markers
// and a "Start" button.  Called by touchCalibBegin().
static void drawCalibStartScreen() {
    tft.fillScreen(TFT_BLACK);

    // Preview all 4 corner markers
    for (int s = 1; s <= 4; s++) {
        CornerInfo c = cornerInfo(s);
        tft.fillRect(c.mx, c.my, MARKER, MARKER, c.color);
    }

    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
    tft.drawString("Touch Calibration", SCREEN_W / 2, 30);
    tft.unloadFont();

    tft.loadFont(FoundGriBol15);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(tft.color565(160, 160, 160), TFT_BLACK, true);
    tft.drawString("Touch corners as far on the", SCREEN_W / 2, 62);
    tft.drawString("outside as possible.", SCREEN_W / 2, 80);
    tft.drawString("4 corners will be captured,", SCREEN_W / 2, 105);
    tft.drawString("one at a time.", SCREEN_W / 2, 123);
    tft.unloadFont();

    // Start button
    uint16_t bg  = tft.color565(40, 60, 150);
    uint16_t bdr = tft.color565(80, 120, 220);
    tft.fillRoundRect(CBTN_X, CBTN_Y, CBTN_W, CBTN_H, 8, bg);
    tft.drawRoundRect(CBTN_X, CBTN_Y, CBTN_W, CBTN_H, 8, bdr);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg, true);
    tft.drawString("Start", CBTN_X + CBTN_W / 2, CBTN_Y + CBTN_H / 2 + 3);
    tft.unloadFont();

    drawCancelLink();
}

// ── Draw: one calibration step ────────────────────────────────────────────────
// Draws one calibration step screen: corner marker, step progress label,
// instructions, optional raw-value readout, and Next/Save button.
// @param step      Current step number (1–4).
// @param rawX,rawY Most recent raw ADC touch values (displayed when hasPoint=true).
// @param hasPoint  true once the user has tapped the screen for this step.
static void drawCalibStepScreen(int step, int rawX, int rawY, bool hasPoint) {
    tft.fillScreen(TFT_BLACK);

    CornerInfo c = cornerInfo(step);

    // Corner marker + white outline so it's visible at screen edges
    tft.fillRect(c.mx, c.my, MARKER, MARKER, c.color);
    tft.drawRect(c.mx - 1, c.my - 1, MARKER + 2, MARKER + 2, TFT_WHITE);

    // Step label
    char stepBuf[12];
    snprintf(stepBuf, sizeof(stepBuf), "Step %d / 4", step);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
    tft.drawString(stepBuf, SCREEN_W / 2, 16);
    tft.unloadFont();

    // Instructions
    tft.loadFont(FoundGriBol15);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(tft.color565(190, 190, 190), TFT_BLACK, true);
    tft.drawString("Tap the corner square:", SCREEN_W / 2, 47);
    tft.setTextColor(c.color, TFT_BLACK, true);
    tft.drawString(c.label, SCREEN_W / 2, 65);
    tft.setTextColor(tft.color565(160, 160, 160), TFT_BLACK, true);
    tft.drawString("as far outside as possible.", SCREEN_W / 2, 83);
    tft.unloadFont();

    // Raw readout
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TC_DATUM);
    if (hasPoint) {
        char buf[28];
        snprintf(buf, sizeof(buf), "Raw X: %d", rawX);
        tft.setTextColor(TFT_GREEN, TFT_BLACK, true);
        tft.drawString(buf, SCREEN_W / 2, 112);
        snprintf(buf, sizeof(buf), "Raw Y: %d", rawY);
        tft.drawString(buf, SCREEN_W / 2, 135);
    } else {
        tft.setTextColor(tft.color565(80, 80, 80), TFT_BLACK, true);
        tft.drawString("Touch the corner first", SCREEN_W / 2, 123);
    }
    tft.unloadFont();

    // Next / Save button (greyed out until a point is recorded)
    uint16_t bg  = hasPoint ? tft.color565(0, 130, 0)   : tft.color565(50, 50, 50);
    uint16_t bdr = hasPoint ? tft.color565(0, 220, 60)  : tft.color565(80, 80, 80);
    tft.fillRoundRect(CBTN_X, CBTN_Y, CBTN_W, CBTN_H, 8, bg);
    tft.drawRoundRect(CBTN_X, CBTN_Y, CBTN_W, CBTN_H, 8, bdr);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, bg, true);
    tft.drawString(step < 4 ? "Next" : "Save", CBTN_X + CBTN_W / 2, CBTN_Y + CBTN_H / 2 + 3);
    tft.unloadFont();

    drawCancelLink();
}

// ── Public: begin session ─────────────────────────────────────────────────────
// Resets session state and draws the calibration start screen.
// Call from main.cpp when the user navigates to the calibration flow.
void touchCalibBegin() {
    s_step       = 0;
    s_hasPoint   = false;
    s_cancelled  = false;
    drawCalibStartScreen();
}

// ── Public: handle touch ──────────────────────────────────────────────────────
// Processes one touch event during an active calibration session.
// Must be called from the main touch-handling block in loop() whenever
// s_showCalibration is true and a new touch is detected.
// @param tx,ty     Calibrated screen pixel coordinates of the touch.
// @param rawX,rawY Raw ADC values from the XPT2046 driver (recorded as reference points).
// @return true when the session has ended (successfully saved OR cancelled);
//         false while the session is still in progress.
bool touchCalibHandleTouch(int tx, int ty, int rawX, int rawY) {
    s_cancelled = false;

    if (s_step == 0) {
        // Start screen ────────────────────────────────────────────────────────
        if (ty >= CANCEL_Y && tx >= 60 && tx <= SCREEN_W - 60) {
            s_cancelled = true;
            return true;
        }
        if (tx >= CBTN_X && tx <= CBTN_X + CBTN_W &&
            ty >= CBTN_Y && ty <= CBTN_Y + CBTN_H) {
            s_step = 1;
            s_hasPoint = false;
            drawCalibStepScreen(1, 0, 0, false);
        }
    } else {
        // Steps 1–4 ───────────────────────────────────────────────────────────
        if (ty >= CANCEL_Y && tx >= 60 && tx <= SCREEN_W - 60) {
            s_cancelled = true;
            return true;
        }
        if (s_hasPoint &&
            tx >= CBTN_X && tx <= CBTN_X + CBTN_W &&
            ty >= CBTN_Y && ty <= CBTN_Y + CBTN_H) {
            // Confirm point and advance
            s_raw[s_step - 1][0] = s_curRaw[0];
            s_raw[s_step - 1][1] = s_curRaw[1];
            s_step++;
            if (s_step > 4) {
                // All 4 corners captured — save and finish
                saveCalibration(s_raw);
                s_step = 0;
                return true;
            }
            s_hasPoint = false;
            drawCalibStepScreen(s_step, 0, 0, false);
        } else {
            // Any other touch → record raw values and refresh
            s_curRaw[0] = rawX;
            s_curRaw[1] = rawY;
            s_hasPoint  = true;
            drawCalibStepScreen(s_step, rawX, rawY, true);
        }
    }
    return false;  // session still in progress
}
