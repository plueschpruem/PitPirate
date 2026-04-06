#pragma once
// PitPirate — XPT2046 touch-screen calibration
//
// Stores four corner raw ADC readings in NVS and derives linear x/y mappings.
// Integrates with the existing edge-triggered touch loop in main.cpp.

// ── Boot-time init ────────────────────────────────────────────────────────────
// Load saved calibration from NVS (falls back to 150/3900 defaults).
// Call once from setup() after preferences.begin().
void touchCalibInit();

// ── Touch coordinate mapping ──────────────────────────────────────────────────
// Map raw XPT2046 ADC values to screen pixels using saved calibration.
// Replaces the hard-coded map(p.x, 150, 3900, ...) in main.cpp.
void touchCalibMap(int rawX, int rawY, int& tx, int& ty);

// ── Calibration flow ──────────────────────────────────────────────────────────
// Begin a new calibration session and draw the start screen.
void touchCalibBegin();

// Handle one touch event during an active calibration session.
//   tx, ty   — already-mapped screen coordinates (used for button hit-tests)
//   rawX/rawY — raw ADC values from the touchscreen (recorded as corner data)
// Returns true when the session ends (completed or cancelled).
// Call touchCalibWasCancelled() afterwards to distinguish the two outcomes.
bool touchCalibHandleTouch(int tx, int ty, int rawX, int rawY);

// Returns true if the last completed session was cancelled by the user.
bool touchCalibWasCancelled();
