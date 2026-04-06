#pragma once
#include <TFT_eSPI.h>

// Single TFT_eSPI instance — defined in main.cpp, used throughout display code.
extern TFT_eSPI tft;

// ── Touch SPI pin assignments ─────────────────────────────────────────────────
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

// ── Screen dimensions ─────────────────────────────────────────────────────────
#define SCREEN_W      320
#define SCREEN_H      240
#define HDR_H          24                          // header bar height
#define AMB_H          40                          // ambient / pit temperature strip height
#define PROBE_AREA_H  (SCREEN_H - HDR_H - AMB_H)  // 176 px for probe cells

// ── Overlay button geometry ───────────────────────────────────────────────────
#define SETTINGS_BTN_W  38
#define SETTINGS_BTN_H  28
#define SETTINGS_BTN_X  (SCREEN_W - SETTINGS_BTN_W - 4)   // 278 px from left

// "Boot AP" button — left half, 10 px from border, ends 5 px before centre
#define APMODE_BTN_X    5                                  // 10 px from left
#define APMODE_BTN_Y   (HDR_H + 65)                        // 89 px from top
#define APMODE_BTN_W   145                                  // x:10–155, 5 px left of centre
#define APMODE_BTN_H    30

// "PID on/off" button — right half, 5 px past centre to 10 px from right edge
#define PIDMODE_BTN_X  (SCREEN_W / 2 + 5)                  // 165 px from left
#define PIDMODE_BTN_Y   APMODE_BTN_Y
#define PIDMODE_BTN_W  (SCREEN_W - 10 - (SCREEN_W / 2 + 5)) // 145 px
#define PIDMODE_BTN_H   APMODE_BTN_H

// Reset button on the AP-info screen (bottom-left of left panel)
#define APRESET_BTN_X    8
#define APRESET_BTN_Y  185
#define APRESET_BTN_W  120
#define APRESET_BTN_H   28

// "Debug Log" toggle on settings page 2
#define DBGLOG_BTN_X    5
#define DBGLOG_BTN_Y   (HDR_H + 10)
#define DBGLOG_BTN_W   (SCREEN_W - 10)
#define DBGLOG_BTN_H   30

// Telemetry post settings on settings page 2
#define TEL_SECT_Y          (DBGLOG_BTN_Y + DBGLOG_BTN_H + 12)   // section label top
#define TEL_ONCHG_BTN_X      5
#define TEL_ONCHG_BTN_Y     (TEL_SECT_Y + 16)
#define TEL_ONCHG_BTN_W     (SCREEN_W - 10)
#define TEL_ONCHG_BTN_H      30
#define TEL_INT_BTN_Y        (TEL_ONCHG_BTN_Y + TEL_ONCHG_BTN_H + 18)
#define TEL_INT_BTN_H         30
#define TEL_INT_BTN_GAP        8
#define TEL_INT_BTN_W        ((SCREEN_W - 10 - 3 * TEL_INT_BTN_GAP) / 4)

// Touch calibration button on settings page 2
#define CALIB_BTN_X      5
#define CALIB_BTN_Y     (TEL_INT_BTN_Y + TEL_INT_BTN_H + 8)
#define CALIB_BTN_W     (SCREEN_W - 10)
#define CALIB_BTN_H      28

// Fan settings section (lower portion of the settings page)
#define FAN_SECT_Y     133   // top of the entire fan section
#define FAN_BTN_GAP     10   // gap between fan row buttons (px)
#define FAN_BTN_W      ((SCREEN_W - 3 * FAN_BTN_GAP) / 4)  // 72 px per button
#define FAN_ROW1_Y     153   // Y of the 4-button row
#define FAN_ROW1_H      30   // row 1 button height
#define FAN_ROW2_Y     187   // Y of row 2 (entry btn + slider + speed text)
#define FAN_ROW2_H      30   // row 2 element height
// Row-2 slider (50 % screen width, right of the row-2 button)
#define FAN_SLIDER_X   (FAN_BTN_W + FAN_BTN_GAP)                        // 82 px
#define FAN_SLIDER_W   (SCREEN_W / 2)                                    // 160 px
#define FAN_SLIDER_TH    6   // slider track height (px)
#define FAN_SLIDER_TY  (FAN_ROW2_Y + (FAN_ROW2_H - FAN_SLIDER_TH) / 2) // 199 px
#define FAN_HANDLE_R     8   // touch handle radius (px)

// Probe alarm limits page ─────────────────────────────────────────────────────
#define ALM_BTN_W         40                 // width of the fine-tune −/+ buttons
#define ALM_BTN_H         28                 // height (centred on the slider track)
#define ALM_SLIDER_X      (ALM_BTN_W + (ALM_BTN_W / 2))         // 40 px — left edge of slider track
#define ALM_SLIDER_W      (SCREEN_W - (ALM_BTN_W * 3))  // 240 px (avoids unresponsive edges)
#define ALM_SLIDER_TH     6                  // track height
#define ALM_HANDLE_R      10                 // touch handle radius
#define ALM_LO_LABEL_Y    34                 // HDR_H + 10
#define ALM_LO_SLIDER_TY  62                 // handle centre at 65, bottom at 75
#define ALM_HI_LABEL_Y    96                 // 75 + ~21 px gap
#define ALM_HI_SLIDER_TY  124                // handle centre at 127, bottom at 137
#define ALM_SET_BTN_X     5
#define ALM_SET_BTN_W     (SCREEN_W - 10)
#define ALM_SET_BTN_Y     164                // 137 + 27 px gap
#define ALM_SET_BTN_H     44                 // bottom at 208
