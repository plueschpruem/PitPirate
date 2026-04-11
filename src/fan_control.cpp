// PitPirate — PWM fan control
//
// GPIO 22 → IRF520N MOSFET gate (PWM output, 25 kHz, LEDC channel 2)
// Speed (%) is persisted in NVS under key "fan_pct" and restored on boot.

#include "fan_control.h"
#include "servo_control.h"  // valve follows fan in auto mode
#include "shared_data.h"   // for preferences (NVS)
#include <Arduino.h>
#include "debug_log.h"

#define FAN_PIN      22
#define FAN_CHANNEL   2   // LEDC channel (0–15 available)
#define FAN_FREQ  25000   // 25 kHz — inaudible for most fans
#define FAN_RES       8   // 8-bit: 0–255
#define KICK_MS     500   // how long to hold start speed before settling

static uint8_t  s_pct       = 0;
static uint8_t  s_startPct  = 40;
static uint8_t  s_minPct    = 25;
static uint32_t s_kickUntil = 0;   // millis() deadline for kick phase (0 = not kicking)

// Initialises the LEDC PWM channel for the fan and restores the last-saved speed
// from NVS.  If a non-zero speed was saved and the start percentage is higher than
// that target, a brief kick pulse at start speed is applied automatically so the
// motor overcomes static friction.  Call once from setup().
void fanInit() {
    ledcSetup(FAN_CHANNEL, FAN_FREQ, FAN_RES);
    ledcAttachPin(FAN_PIN, FAN_CHANNEL);

    s_startPct = (uint8_t)constrain(preferences.getInt("fan_start", 40), 0, 100);
    s_minPct   = (uint8_t)constrain(preferences.getInt("fan_min",   25), 0, 100);
    uint8_t saved = (uint8_t)constrain(preferences.getInt("fan_pct",  0), 0, 100);
    if (saved > 0 && saved < s_minPct) saved = s_minPct;
    s_pct = saved;
    if (s_pct > 0 && s_startPct > s_pct) {
        // Motor is off on boot — apply kick then settle
        ledcWrite(FAN_CHANNEL, (uint8_t)(s_startPct * 255 / 100));
        s_kickUntil = millis() + KICK_MS;
    } else {
        ledcWrite(FAN_CHANNEL, (uint8_t)(s_pct * 255 / 100));
    }
    DLOG("[Fan] init GPIO22 PWM, restored %d%% (start=%d%% min=%d%%)\n",
                  s_pct, s_startPct, s_minPct);
}

// Handles time-based state transitions; must be called every loop() iteration.
// Currently manages the kick-start timer: once the kick deadline elapses the
// duty cycle is reduced to the actual target percentage.
void fanLoop() {
    if (s_kickUntil && millis() >= s_kickUntil) {
        s_kickUntil = 0;
        ledcWrite(FAN_CHANNEL, (uint8_t)(s_pct * 255 / 100));
        DLOG("[Fan] kick done, settling at %d%%\n", s_pct);
    }
}

// Sets the LEDC duty cycle directly as a raw 8-bit value (0–255).
// Bypasses the percentage clamping logic; intended for low-level PID output.
// @param duty  Raw PWM duty value (0 = off, 255 = full speed).
void fanSetDuty(uint8_t duty) {
    ledcWrite(FAN_CHANNEL, duty);
}

// Sets the fan speed as a percentage (0–100) and persists it to NVS.
// A non-zero value below the configured minimum is clamped up to that minimum
// to prevent stall-prone low-speed operation.  When starting from stopped and
// the start percentage exceeds the target, a KICK_MS kick pulse is applied first.
// @param pct  Desired fan speed in percent (0 = off, 1–100 = running).
void fanSetPercent(uint8_t pct) {
    if (pct > 100) pct = 100;
    // Clamp non-zero values up to minimum sustained speed
    if (pct > 0 && pct < s_minPct) pct = s_minPct;

    bool wasOff = (s_pct == 0 && s_kickUntil == 0);
    s_pct = pct;
    preferences.putInt("fan_pct", pct);

    if (pct == 0) {
        // Turn off immediately, cancel any pending kick
        s_kickUntil = 0;
        ledcWrite(FAN_CHANNEL, 0);
    } else if (wasOff && s_startPct > pct) {
        // Starting from stopped and start speed is higher — kick to s_startPct first
        ledcWrite(FAN_CHANNEL, (uint8_t)(s_startPct * 255 / 100));
        s_kickUntil = millis() + KICK_MS;
        DLOG("[Fan] kick-start at %d%% for %dms, target %d%%\n", s_startPct, KICK_MS, pct);
    } else {
        // Already running, or start speed <= target: go straight to target
        s_kickUntil = 0;
        ledcWrite(FAN_CHANNEL, (uint8_t)(pct * 255 / 100));
    }

    servoUpdateFromFan(pct);  // valve follows fan when auto mode is enabled
}

// Returns the current fan speed setpoint in percent (0 = off, 1–100 = running).
uint8_t fanGetPercent() { return s_pct; }

// Sets the kick-start percentage and persists it to NVS.
// This is the duty applied for KICK_MS when starting the motor from rest.
// @param pct  Kick-start percentage (0–100).
void fanSetStartPct(uint8_t pct) {
    if (pct > 100) pct = 100;
    s_startPct = pct;
    preferences.putInt("fan_start", pct);
}
// Returns the configured kick-start percentage.
uint8_t fanGetStartPct() { return s_startPct; }

// Sets the minimum sustained fan speed and persists it to NVS.
// Any non-zero setPercent() call will be clamped to at least this value.
// @param pct  Minimum speed percentage (0–100).
void fanSetMinPct(uint8_t pct) {
    if (pct > 100) pct = 100;
    s_minPct = pct;
    preferences.putInt("fan_min", pct);
}
// Returns the configured minimum sustained fan speed percentage.
uint8_t fanGetMinPct() { return s_minPct; }
