// PitPirate — SG-90 servo control
//
// GPIO 27 → servo signal wire (5 V power supplied externally)
// LEDC channel 4, 50 Hz, 16-bit resolution
//
// ESP32 LEDC timer sharing: ch0-1→timer0, ch2-3→timer1, ch4-5→timer2, ch6-7→timer3
// ch1=backlight(5kHz), ch2=fan(25kHz) — servo must be on a different timer, so ch4.
//
// Standard SG-90 pulse timing:
//   500 µs →   0°
//  1500 µs →  90° (neutral)
//  2400 µs → 180°
//
// At 50 Hz, 16-bit: 1 µs = 65536 / 20000 = 3.2768 counts

#include "servo_control.h"
#include <Arduino.h>
#include "debug_log.h"
#include "shared_data.h"  // preferences (NVS)

#define SERVO_PIN      27
#define SERVO_CHANNEL   4   // LEDC ch4 → timer2 (ch1=BL/timer0, ch2=fan/timer1 — no overlap)
#define SERVO_FREQ     50   // 50 Hz standard servo frequency
#define SERVO_RES      16   // 16-bit for fine pulse resolution

// Pulse width limits in microseconds (SG-90 datasheet)
#define SERVO_US_MIN  500
#define SERVO_US_MAX 2400
#define SERVO_US_PERIOD 20000  // 20 ms = 1/50 Hz

static uint8_t s_angleDeg  =   0;
static uint8_t s_minAngle  =   0;  // NVS: srv_min
static uint8_t s_maxAngle  = 180;  // NVS: srv_max
static bool    s_auto      = false; // NVS: srv_auto — valve follows fan

// Converts a pulse width in microseconds to a 16-bit LEDC duty value.
static inline uint32_t usToDuty(uint16_t us) {
    // duty = us / period_us * 2^resolution
    return (uint32_t)us * (1UL << SERVO_RES) / SERVO_US_PERIOD;
}

// Converts an angle in degrees to a pulse width in microseconds.
static inline uint16_t angleToUs(uint8_t deg) {
    return (uint16_t)(SERVO_US_MIN + (uint32_t)(SERVO_US_MAX - SERVO_US_MIN) * deg / 180);
}

// Initialises LEDC channel for the servo, restores persisted limits and angle
// from NVS, and drives the servo to the saved position.  Call once from setup().
void servoInit() {
    ledcSetup(SERVO_CHANNEL, SERVO_FREQ, SERVO_RES);
    ledcAttachPin(SERVO_PIN, SERVO_CHANNEL);
    s_minAngle = (uint8_t)constrain(preferences.getInt("srv_min",   0), 0, 180);
    s_maxAngle = (uint8_t)constrain(preferences.getInt("srv_max", 180), 0, 180);
    if (s_maxAngle < s_minAngle) s_maxAngle = 180;  // sanity guard
    s_angleDeg = (uint8_t)constrain(preferences.getInt("srv_ang",  90), s_minAngle, s_maxAngle);
    s_auto     = preferences.getBool("srv_auto", false);
    ledcWrite(SERVO_CHANNEL, usToDuty(angleToUs(s_angleDeg)));
    DLOG("[Servo] init GPIO%d LEDC ch%d, pos %ddeg (min=%d max=%d auto=%d)\n",
         SERVO_PIN, SERVO_CHANNEL, s_angleDeg, s_minAngle, s_maxAngle, (int)s_auto);
}

// Moves the servo to the requested angle, clamped to [min,max], and persists
// the new position to NVS (key "srv_ang").
// @param deg  Target angle in degrees.
void servoSetAngle(uint8_t deg) {
    deg = (uint8_t)constrain((int)deg, s_minAngle, s_maxAngle);
    s_angleDeg = deg;
    preferences.putInt("srv_ang", deg);
    uint16_t us = angleToUs(deg);
    ledcWrite(SERVO_CHANNEL, usToDuty(us));
    DLOG("[Servo] angle %ddeg → %dus\n", deg, us);
}

// Returns the last commanded angle in degrees.
uint8_t servoGetAngle() {
    return s_angleDeg;
}

// Applies a raw pulse-width value in microseconds.
// Clamped to SERVO_US_MIN–SERVO_US_MAX to protect the servo.
// Does NOT persist to NVS — use servoSetAngle() for user-facing commands.
// @param us  Pulse width in microseconds.
void servoSetUs(uint16_t us) {
    us = (uint16_t)constrain((int)us, SERVO_US_MIN, SERVO_US_MAX);
    s_angleDeg = (uint8_t)((uint32_t)(us - SERVO_US_MIN) * 180 / (SERVO_US_MAX - SERVO_US_MIN));
    ledcWrite(SERVO_CHANNEL, usToDuty(us));
    DLOG("[Servo] raw %dus → angle ~%ddeg\n", us, s_angleDeg);
}

// Returns the configured minimum angle limit.
uint8_t servoGetMinAngle() { return s_minAngle; }

// Returns the configured maximum angle limit.
uint8_t servoGetMaxAngle() { return s_maxAngle; }

// Saves the minimum angle limit to NVS.  Does not move the servo.
void servoSetMinAngle(uint8_t deg) {
    if (deg > 180) deg = 180;
    s_minAngle = deg;
    preferences.putInt("srv_min", deg);
}

// Saves the maximum angle limit to NVS.  Does not move the servo.
void servoSetMaxAngle(uint8_t deg) {
    if (deg > 180) deg = 180;
    s_maxAngle = deg;
    preferences.putInt("srv_max", deg);
}

// Enables or disables automatic mode.  In auto mode the valve tracks the fan:
// open (max angle) when fan is running, closed (min angle) when fan is off.
void servoSetAuto(bool enable) {
    s_auto = enable;
    preferences.putBool("srv_auto", enable);
    DLOG("[Servo] auto mode %s\n", enable ? "ON" : "OFF");
}

// Returns whether automatic (fan-coupled) mode is active.
bool servoIsAuto() { return s_auto; }

// Called by fanSetPercent() whenever the fan speed changes.
// In auto mode: drives the valve fully open when the fan is on, fully closed when off.
void servoUpdateFromFan(uint8_t fan_pct) {
    if (!s_auto) return;
    uint8_t target = (fan_pct > 0) ? s_maxAngle : s_minAngle;
    // Use servoSetAngle so the position is persisted and logged
    servoSetAngle(target);
    DLOG("[Servo] auto: fan=%d%% -> %ddeg\n", fan_pct, target);
}
