#pragma once
#include <Arduino.h>
#include "display_config.h"
#include "../probe_data.h"

// ── Per-section shadow cache for partial display refreshes ────────────────────
// Tracks what is currently drawn on screen so unchanged regions are skipped.
struct DisplayCache {
    int   hdrMinute  = -2;  // -2 = never drawn; -1 = time unavailable
    int   hdrBattery = -2;
    int   hdrFan     = -2;
    bool  hdrNetLag  = false;
    float probeVal[6];      // NAN = never drawn / not connected
    int   numProbes  = -1;
    int   almLo[6];
    int   almHi[6];
    float ambVal     = NAN;
    int   ambAlmLo   = -2;
    int   ambAlmHi   = -2;

    DisplayCache() {
        for (int i = 0; i < 6; i++) {
            probeVal[i] = NAN;
            almLo[i]    = -2;
            almHi[i]    = -2;
        }
    }

    void invalidate() {
        hdrMinute = hdrBattery = hdrFan = -2;
        numProbes = -1;
        for (int i = 0; i < 6; i++) {
            probeVal[i] = NAN;
            almLo[i]    = almHi[i] = -2;
        }
        ambVal = NAN;
        ambAlmLo = ambAlmHi = -2;
    }
};

// Global display state — defined in display.cpp
extern DisplayCache   dcache;
extern ProbeVals      lastKnownVals;
extern bool           inErrorState;
extern bool           errorBarOn;
extern unsigned long  lastBlinkMs;
extern bool           isOnSettingsPage;

// ── Shared colour helpers (defined in display.cpp) ───────────────────────────
uint16_t probeColor(int probeNum);
uint16_t probeColorBg(int probeNum);

// ── Probe-area helpers (defined in display_probes.cpp) ───────────────────────
void drawProbeCell(int x, int y, int w, int h, int probeNum, float value, int totalProbes, int lo, int hi);
void getProbeLayout(int n, int& cols, int& rows);
void redrawProbeArea(const ProbeVals& v);

// Store a fetched graph bitmap for probe probeIdx (0-5). Takes ownership of buf.
// Call from Core 1 only (e.g. inside httpTaskDrainGraphResults).
void displayProbeSetGraph(uint8_t probeIdx, uint8_t* buf, uint16_t w, uint16_t h, float tMin, float tMax);

// ── Screen drawing functions ──────────────────────────────────────────────────
void drawHeader(int minute, int battery, int fanPct, bool networkLag);
void drawFooterStrip(float ambient, int almLo, int almHi);
void drawSettingsBtn(int ay);
void drawErrorBar(bool show);
void drawCalibBtn();
void showSplash();
void drawPidModeBtn(bool enabled);
void drawSettingsRssi();
void drawFanSection(uint8_t pct, uint8_t entryPct);
void drawSettingsMoreLink();
void drawSettingsPage();
void drawSettingsPage2();
void drawSettingsPage3();
void drawAlarmLoSlider(int probeNum, int val);
void drawAlarmHiSlider(int probeNum, int val);
void drawProbeLimitsPage(int probeNum, int lo, int hi);
void drawDebugLogBtn(bool enabled);
void drawTelOnChangeBtn(bool on_change);
void drawTelIntervalBtns(uint32_t interval_s);
void updateDisplay();
