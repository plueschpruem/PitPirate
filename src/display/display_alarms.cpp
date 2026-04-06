#include "display.h"
#include "display_fonts.h"

// ── Probe alarm limits page ───────────────────────────────────────────────────

// Draws (or redraws) one label-row + slider section.
// Draws (or redraws) one alarm-limit row: label, numeric value, slider track,
// drag-handle circle, and flanking −/+ fine-tune buttons.
// @param probeNum  1-based probe number (controls colour scheme).
// @param labelY    Top Y pixel of the label row.
// @param sliderTY  Top Y pixel of the slider track rectangle.
// @param name      Label string displayed on the left (e.g. "LOW:" or "HIGH:").
// @param val       Current limit value in °C (0 = limit disabled, shown as "OFF").
// @param col       Foreground colour (RGB565) used for the handle and value text.
static void sDrawAlarmSection(int probeNum, int labelY, int sliderTY,
                               const char* name, int val, uint16_t col) {
    uint16_t btnBg = probeColorBg(probeNum);
    // Clear the section (label + slider + handles + buttons)
    int clearH = (sliderTY + ALM_SLIDER_TH / 2 + ALM_BTN_H / 2 + 3) - labelY;
    tft.fillRect(0, labelY, SCREEN_W, clearH, TFT_BLACK);

    // Label on the left (gray), value on the right (probe colour)
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(tft.color565(130, 130, 130), TFT_BLACK, true);
    tft.drawString(name, ALM_SLIDER_X, labelY);

    char vbuf[8];
    if (val == 0) snprintf(vbuf, sizeof(vbuf), "OFF");
    else          snprintf(vbuf, sizeof(vbuf), "%d\xb0", val);
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(col, TFT_BLACK, true);
    tft.drawString(vbuf, SCREEN_W - ALM_SLIDER_X, labelY);
    tft.unloadFont();

    // Slider track
    int hx   = ALM_SLIDER_X + val * ALM_SLIDER_W / 200;
    int trCY = sliderTY + ALM_SLIDER_TH / 2;
    if (hx > ALM_SLIDER_X)
        tft.fillRoundRect(ALM_SLIDER_X, sliderTY,
                          hx - ALM_SLIDER_X, ALM_SLIDER_TH, 2, col);
    if (hx < ALM_SLIDER_X + ALM_SLIDER_W)
        tft.fillRoundRect(hx, sliderTY,
                          ALM_SLIDER_X + ALM_SLIDER_W - hx, ALM_SLIDER_TH, 2, btnBg);
    tft.fillCircle(hx, trCY, ALM_HANDLE_R, col);
    tft.drawCircle(hx, trCY, ALM_HANDLE_R, TFT_BLACK);

    // Fine-tune buttons flanking the slider
    int btnY  = trCY - ALM_BTN_H / 2;
    tft.fillRoundRect(0, btnY, ALM_BTN_W, ALM_BTN_H, 4, btnBg);
    tft.drawRoundRect(0, btnY, ALM_BTN_W, ALM_BTN_H, 4, col);
    tft.fillRoundRect(SCREEN_W - ALM_BTN_W, btnY, ALM_BTN_W, ALM_BTN_H, 4, btnBg);
    tft.drawRoundRect(SCREEN_W - ALM_BTN_W, btnY, ALM_BTN_W, ALM_BTN_H, 4, col);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, btnBg, true);
    tft.drawString("-", ALM_BTN_W / 2, btnY + ALM_BTN_H / 2 + 1);
    tft.drawString("+", SCREEN_W - ALM_BTN_W / 2, btnY + ALM_BTN_H / 2 + 1);
    tft.unloadFont();
}

// Redraws the LOW alarm slider section on the probe limits page.
// @param probeNum  1-based probe number (controls colour).
// @param val       Current low-limit value in °C (0 = off).
void drawAlarmLoSlider(int probeNum, int val) {
    sDrawAlarmSection(probeNum, ALM_LO_LABEL_Y, ALM_LO_SLIDER_TY, "LOW:",  val, probeColor(probeNum));
}

// Redraws the HIGH alarm slider section on the probe limits page.
// @param probeNum  1-based probe number (controls colour).
// @param val       Current high-limit value in °C (0 = off).
void drawAlarmHiSlider(int probeNum, int val) {
    sDrawAlarmSection(probeNum, ALM_HI_LABEL_Y, ALM_HI_SLIDER_TY, "HIGH:", val, probeColor(probeNum));
}

// Draws the full probe alarm limits page: header with back button and probe
// name, LOW and HIGH slider sections, and the "Set Limits" confirm button.
// @param probeNum  1-based probe number (1–6 = food probe, 7 = ambient/PIT).
// @param lo        Initial LOW limit in °C (0 = off).
// @param hi        Initial HIGH limit in °C (0 = off).
void drawProbeLimitsPage(int probeNum, int lo, int hi) {
    tft.setTextFont(1);
    tft.fillScreen(TFT_BLACK);

    uint16_t col = probeColor(probeNum);
    uint16_t hbg = tft.color565(30, 30, 30);
    tft.fillRect(0, 0, SCREEN_W, HDR_H, hbg);

    tft.loadFont(FoundGriBol20);
    // "< Back"
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, hbg, true);
    tft.drawString("< Back", 8, 4);

    // Title centred
    char title[10];
    if (probeNum == 7)
        snprintf(title, sizeof(title), "PIT");
    else
        snprintf(title, sizeof(title), "Probe %d", probeNum);
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(col, hbg, true);
    tft.drawString(title, SCREEN_W / 2, 4);
    tft.unloadFont();

    // Coloured separator under header
    tft.drawFastHLine(0, HDR_H, SCREEN_W, col);

    // LO + HI slider sections
    sDrawAlarmSection(probeNum, ALM_LO_LABEL_Y, ALM_LO_SLIDER_TY, "LOW:",  lo, col);
    sDrawAlarmSection(probeNum, ALM_HI_LABEL_Y, ALM_HI_SLIDER_TY, "HIGH:", hi, col);

    // "Set Limits" button
    uint16_t btnBg = probeColorBg(probeNum);
    tft.fillRoundRect(ALM_SET_BTN_X, ALM_SET_BTN_Y, ALM_SET_BTN_W, ALM_SET_BTN_H, 6, btnBg);
    tft.drawRoundRect(ALM_SET_BTN_X, ALM_SET_BTN_Y, ALM_SET_BTN_W, ALM_SET_BTN_H, 6, col);
    tft.loadFont(FoundGriBol20);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(col, btnBg, true);
    tft.drawString("Set Limits", ALM_SET_BTN_X + ALM_SET_BTN_W / 2,
                   ALM_SET_BTN_Y + ALM_SET_BTN_H / 2 + 3);
    tft.unloadFont();
}
