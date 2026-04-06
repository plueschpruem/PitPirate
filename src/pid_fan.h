#pragma once
#include <stdint.h>

// PitPirate — PID fan controller for BBQ pit temperature regulation.
//
// Algorithm:
//   output = bias + Kp*error - Kd*(dTemp/dt) + Ki*integrator
//
//   error     = setpoint - currentTemp
//   D term    computed on measurement (avoids derivative kick on setpoint change)
//   Integrator accumulates only when 0 < error < INTEG_MAX_ERR AND output not saturated
//   Dual-gain: aggressive gains when |error| > NEAR_THRESHOLD, conservative otherwise
//   Lid-open:  if temp drops > LID_DROP_PCT% below setpoint → suspend output
//
// Usage:
//   pidFanInit()  — call once from setup() after fanInit() and preferences.begin()
//   pidFanLoop()  — call every loop() iteration (internally gated to PID_SAMPLE_MS)

struct PidConfig {
    bool    enabled;
    float   setpoint;    // target pit temperature (°C)
    int     probeIndex;  // 0-5 = food probes 1-6, 6 = ambient/pit probe

    // Conservative gains — used when |error| <= NEAR_THRESHOLD (close to setpoint)
    float   kp_con;
    float   ki_con;
    float   kd_con;

    // Aggressive gains — used when |error| > NEAR_THRESHOLD (far from setpoint)
    float   kp_agg;
    float   ki_agg;
    float   kd_agg;

    float   bias;        // base fan % feedforward (keeps fire alive at steady state)
    float   outMin;      // minimum fan % when PID is actively controlling
    float   outMax;      // maximum fan %
    bool    lidDetect;   // enable lid-open detection (suspend output on temp drop)
};

// Lifecycle
void  pidFanInit();  // call from setup()
void  pidFanLoop();  // call from loop() — internally rate-limited

// Enable / disable
void  pidFanSetEnabled(bool en);
bool  pidFanIsEnabled();

// Setpoint (also resets integrator to prevent integral wind-up transient)
void  pidFanSetSetpoint(float sp);
float pidFanGetSetpoint();

// Probe selection
void  pidFanSetProbeIndex(int idx);
int   pidFanGetProbeIndex();

// Full config read / write (persists to NVS)
void  pidFanApplyConfig(const PidConfig& cfg);
void  pidFanGetConfig(PidConfig& cfg);

// Runtime diagnostics (read-only)
float pidFanGetOutput();    // last computed fan % (0-100)
float pidFanGetError();     // last temperature error (setpoint - currentTemp)
float pidFanGetIntegral();  // raw integral accumulator
bool  pidFanIsLidOpen();    // true while lid-open is active
