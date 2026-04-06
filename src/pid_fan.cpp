// PitPirate — PID fan controller
//
// Dual-gain PID with D-on-measurement, conditional anti-windup, lid-open
// detection, and a bias (feedforward) term.
//
// Derived from analysis of:
//   - HeaterMeter (B+PID, default params, experimental tuning guide)
//   - smoker-pid-control (dual aggressive/conservative gain sets)
//   - pitclaw (lid-open detection, D-on-meas, iAwCondition, integrator reset)
//   - PitmasterPi (conditional integral accumulation)

#include "pid_fan.h"
#include "fan_control.h"
#include "probe_data.h"
#include "shared_data.h"
#include <Arduino.h>
#include <math.h>
#include "debug_log.h"

// ── Compile-time constants ────────────────────────────────────────────────────

// How often the PID computes a new output (milliseconds).
// 10 s gives meaningful dT values and is well within the 30 s Tuya poll cadence.
#define PID_SAMPLE_MS       10000UL

// |error| threshold to switch between aggressive and conservative gain sets.
#define PID_NEAR_THRESHOLD  10.0f   // °C

// Integrator only accumulates when error is below this value.
// Prevents integrator wind-up during the long initial ramp-up phase.
#define PID_INTEG_MAX_ERR   30.0f   // °C

// Lid-open detection: temperature must drop this far below setpoint to trigger.
#define PID_LID_DROP_PCT     5.0f   // % of setpoint

// Lid-open recovery: temperature must return within this margin of setpoint.
#define PID_LID_RECOVER_PCT  2.0f   // % of setpoint

// ── Default parameters (HeaterMeter-inspired; tune per grill) ─────────────────
#define PID_DEFAULT_KP_CON   4.0f
#define PID_DEFAULT_KI_CON   0.02f
#define PID_DEFAULT_KD_CON   5.0f
#define PID_DEFAULT_KP_AGG   8.0f
#define PID_DEFAULT_KI_AGG   0.2f
#define PID_DEFAULT_KD_AGG   1.0f
#define PID_DEFAULT_BIAS     0.0f
#define PID_DEFAULT_OUT_MIN  25.0f
#define PID_DEFAULT_OUT_MAX  100.0f
#define PID_DEFAULT_SP       120.0f
#define PID_DEFAULT_PROBE    6       // 6 = ambient/pit sensor

// ── Module state (all static — no global linkage) ─────────────────────────────

static bool  s_enabled    = false;
static float s_setpoint   = PID_DEFAULT_SP;
static int   s_probeIndex = PID_DEFAULT_PROBE;

static float s_kp_con = PID_DEFAULT_KP_CON;
static float s_ki_con = PID_DEFAULT_KI_CON;
static float s_kd_con = PID_DEFAULT_KD_CON;
static float s_kp_agg = PID_DEFAULT_KP_AGG;
static float s_ki_agg = PID_DEFAULT_KI_AGG;
static float s_kd_agg = PID_DEFAULT_KD_AGG;
static float s_bias    = PID_DEFAULT_BIAS;
static float s_outMin  = PID_DEFAULT_OUT_MIN;
static float s_outMax  = PID_DEFAULT_OUT_MAX;

static bool  s_lidDetect  = true;           // lid-open detection enabled (persisted)

// Runtime state — not persisted across reboots
static float         s_integral      = 0.0f;
static float         s_lastTemp      = NAN;
static float         s_output        = 0.0f;
static float         s_lastError     = 0.0f;
static bool          s_lidOpen       = false;
static bool          s_pitReady      = false;  // true once pit has reached setpoint; gates lid-open detection
static unsigned long s_lastComputeMs = 0;

// ── Internal helpers ──────────────────────────────────────────────────────────

static void saveToNVS();   // forward declaration — defined below loadFromNVS

// Loads all PID parameters from NVS into module-static variables.
// Seeds NVS with compile-time defaults on the first boot (avoids ESP-IDF
// [E] log spam for missing float keys).
static void loadFromNVS() {
    // On a fresh device none of the float keys exist yet.  Preferences.getFloat()
    // uses nvs_get_blob internally and logs an [E] for every missing key even when
    // a default is provided.  Seed NVS with the compile-time defaults once so
    // subsequent boots are clean.
    if (!preferences.isKey("pid_sp")) {
        saveToNVS();   // static vars still hold #define defaults at this point
    }

    s_enabled    = preferences.getInt  ("pid_en",       0)                  != 0;
    s_setpoint   = preferences.getFloat("pid_sp",       PID_DEFAULT_SP);
    s_probeIndex = preferences.getInt  ("pid_probe",    PID_DEFAULT_PROBE);
    s_kp_con     = preferences.getFloat("pid_kp_c",     PID_DEFAULT_KP_CON);
    s_ki_con     = preferences.getFloat("pid_ki_c",     PID_DEFAULT_KI_CON);
    s_kd_con     = preferences.getFloat("pid_kd_c",     PID_DEFAULT_KD_CON);
    s_kp_agg     = preferences.getFloat("pid_kp_a",     PID_DEFAULT_KP_AGG);
    s_ki_agg     = preferences.getFloat("pid_ki_a",     PID_DEFAULT_KI_AGG);
    s_kd_agg     = preferences.getFloat("pid_kd_a",     PID_DEFAULT_KD_AGG);
    s_bias       = preferences.getFloat("pid_bias",     PID_DEFAULT_BIAS);
    s_outMin     = preferences.getFloat("pid_out_min",  PID_DEFAULT_OUT_MIN);
    s_outMax     = preferences.getFloat("pid_out_max",  PID_DEFAULT_OUT_MAX);

    // Clamp loaded values to valid ranges
    s_setpoint   = constrain(s_setpoint,   0.0f, 600.0f);
    s_probeIndex = constrain(s_probeIndex, 0, 6);
    s_outMin     = constrain(s_outMin,     0.0f, 100.0f);
    s_outMax     = constrain(s_outMax,     s_outMin, 100.0f);
    s_lidDetect  = preferences.getInt("pid_lid_det", 1) != 0;
}

// Saves all current PID parameters to NVS.
static void saveToNVS() {
    preferences.putInt  ("pid_en",      s_enabled ? 1 : 0);
    preferences.putFloat("pid_sp",      s_setpoint);
    preferences.putInt  ("pid_probe",   s_probeIndex);
    preferences.putFloat("pid_kp_c",    s_kp_con);
    preferences.putFloat("pid_ki_c",    s_ki_con);
    preferences.putFloat("pid_kd_c",    s_kd_con);
    preferences.putFloat("pid_kp_a",    s_kp_agg);
    preferences.putFloat("pid_ki_a",    s_ki_agg);
    preferences.putFloat("pid_kd_a",    s_kd_agg);
    preferences.putFloat("pid_bias",    s_bias);
    preferences.putFloat("pid_out_min", s_outMin);
    preferences.putFloat("pid_out_max", s_outMax);
    preferences.putInt  ("pid_lid_det", s_lidDetect ? 1 : 0);
}

// Reset all runtime state (call on enable, disable, setpoint change, config change).
// Resets all PID runtime state (integral, derivative terms, lid-open flag,
// pit-ready flag).  Must be called on enable/disable/setpoint/config change
// to prevent stale state from causing an initial output spike.
static void resetRuntime() {
    s_integral      = 0.0f;
    s_lastTemp      = NAN;
    s_output        = 0.0f;
    s_lastError     = 0.0f;
    s_lidOpen       = false;
    s_pitReady      = false;
    s_lastComputeMs = millis();  // start sample interval from now
}

// Update lid-open state machine.
// Triggered when temperature drops sharply below setpoint (lid opened mid-cook).
// Suspends PID output until temperature recovers, then resets integrator.
// Updates the lid-open state machine each PID sample.
// Arms only after the pit has reached setpoint at least once (s_pitReady).
// When a lid-open event is detected the PID output is frozen at bias/outMin;
// the integrator is reset once the temperature recovers.
// @param currentTemp  Latest temperature reading from the monitored probe (°C).
static void updateLidState(float currentTemp) {
    if (s_setpoint <= 0.0f) return;

    float dropThresh    = s_setpoint * (1.0f - PID_LID_DROP_PCT    / 100.0f);
    float recoverThresh = s_setpoint * (1.0f - PID_LID_RECOVER_PCT / 100.0f);

    if (!s_lidOpen) {
        if (currentTemp < dropThresh) {
            s_lidOpen = true;
            DLOG("[PID] Lid open detected: T=%.1f threshold=%.1f\n",
                 currentTemp, dropThresh);
        }
    } else {
        if (currentTemp >= recoverThresh) {
            s_lidOpen  = false;
            s_integral = 0.0f;  // prevent integrator wind-up built up during lid-open
            DLOG("[PID] Lid recovered: T=%.1f\n", currentTemp);
        }
    }
}

// ── Public API ────────────────────────────────────────────────────────────────

// Initialises the PID controller: loads parameters from NVS, enforces the
// fan-hardware minimum output floor, and resets runtime state.
// Must be called once from setup(), after fanInit().
void pidFanInit() {
    loadFromNVS();

    // Enforce outMin >= fan hardware minimum so PID never requests a stall-prone speed
    float fanMin = (float)fanGetMinPct();
    if (s_outMin < fanMin) s_outMin = fanMin;

    resetRuntime();

    DLOG("[PID] init: enabled=%d SP=%.1f probe=%d "
                  "Kp=%.2f/%.2f Ki=%.3f/%.3f Kd=%.1f/%.1f "
                  "bias=%.1f out=[%.1f-%.1f%%]\n",
                  s_enabled, s_setpoint, s_probeIndex,
                  s_kp_con, s_kp_agg,
                  s_ki_con, s_ki_agg,
                  s_kd_con, s_kd_agg,
                  s_bias, s_outMin, s_outMax);
}

// Runs one PID sample iteration; must be called every loop() iteration.
// Returns immediately when PID is disabled or the sample interval has not
// elapsed.  Selects aggressive or conservative gains based on error magnitude,
// performs D-on-measurement differentiation, applies conditional anti-windup,
// and writes the new output with fanSetPercent().
void pidFanLoop() {
    if (!s_enabled) return;

    unsigned long now     = millis();
    unsigned long elapsed = now - s_lastComputeMs;
    if (elapsed < PID_SAMPLE_MS) return;

    // ── Read temperature from the configured probe ────────────────────────────
    ProbeVals v       = parseProbeVals();
    float currentTemp = (s_probeIndex == 6) ? v.ambient : v.probe[s_probeIndex];

    // Safety: invalid/disconnected probe → turn fan off and wait
    if (isnan(currentTemp)) {
        DLOGLN("[PID] Probe NaN — suspending fan output");
        fanSetPercent(0);
        s_output        = 0.0f;
        s_lastError     = 0.0f;
        s_lastTemp      = NAN;
        s_lastComputeMs = now;
        return;
    }

    // ── Lid-open detection ────────────────────────────────────────────────────
    // Only arm after the pit has reached setpoint at least once; prevents a
    // false trigger during the initial heat-up ramp (e.g. 20°C → 120°C target).
    if (!s_pitReady && currentTemp >= s_setpoint * (1.0f - PID_LID_RECOVER_PCT / 100.0f)) {
        s_pitReady = true;
        DLOG("[PID] Pit reached setpoint — lid-open detection armed\n");
    }
    if (s_lidDetect && s_pitReady) updateLidState(currentTemp);

    if (s_lidOpen) {
        // Hold at bias/outMin while lid is open to keep embers alive without
        // blasting fresh air in that would cause a temperature spike when closed.
        float holdPct = constrain(s_bias > 0.0f ? s_bias : s_outMin,
                                  s_outMin, s_outMax);
        fanSetPercent((uint8_t)roundf(holdPct));
        s_output        = holdPct;
        s_lastTemp      = currentTemp;
        s_lastComputeMs = now;
        return;
    }

    float error = s_setpoint - currentTemp;

    // ── Above setpoint: kill fan, reset integrator ────────────────────────────
    // The fire can only be throttled, not actively cooled.  Once the pit is
    // over-temp there is no benefit to keeping integrator wind-up around.
    if (error < 0.0f) {
        fanSetPercent(0);
        s_integral      = 0.0f;
        s_output        = 0.0f;
        s_lastError     = error;
        s_lastTemp      = currentTemp;
        s_lastComputeMs = now;
        DLOG("[PID] Over-target: T=%.1f SP=%.1f → fan off\n",
                      currentTemp, s_setpoint);
        return;
    }

    float dt = elapsed / 1000.0f;

    // ── Select gain set ───────────────────────────────────────────────────────
    bool  near = (fabsf(error) <= PID_NEAR_THRESHOLD);
    float kp   = near ? s_kp_con : s_kp_agg;
    float ki   = near ? s_ki_con : s_ki_agg;
    float kd   = near ? s_kd_con : s_kd_agg;

    // ── P term ────────────────────────────────────────────────────────────────
    float p_term = kp * error;

    // ── D term — on measurement, not error ───────────────────────────────────
    // Using dTemp/dt instead of dError/dt prevents a derivative spike when the
    // setpoint is changed.  Skipped on the first compute (no previous reading).
    float d_term = 0.0f;
    if (!isnan(s_lastTemp)) {
        float dTemp = (currentTemp - s_lastTemp) / dt;
        d_term = -kd * dTemp;
    }

    // ── Conditional anti-windup / integrator update ───────────────────────────
    // Compute a tentative output to detect saturation before updating integral.
    float tentative = s_bias + p_term + ki * s_integral + d_term;
    bool  satHigh   = (tentative >= s_outMax);

    // Accumulate only when:
    //   1. Error is positive (below setpoint — more heat needed)
    //   2. Error is below the ramp-up threshold (don't integrate during cold start)
    //   3. Output is not already saturated at max (classic anti-windup)
    if (error > 0.0f && error < PID_INTEG_MAX_ERR && !satHigh) {
        s_integral += error * dt;
    }

    // ── Final output ──────────────────────────────────────────────────────────
    float output = s_bias + p_term + ki * s_integral + d_term;
    output = constrain(output, s_outMin, s_outMax);

    s_output    = output;
    s_lastError = error;
    s_lastTemp  = currentTemp;
    s_lastComputeMs = now;

    fanSetPercent((uint8_t)roundf(output));

    DLOG("[PID] T=%.1f SP=%.1f err=%.1f [%s] | "
                  "P=%.1f I=%.2f D=%.1f | out=%.1f%%\n",
                  currentTemp, s_setpoint, error, near ? "CON" : "AGG",
                  p_term, ki * s_integral, d_term, output);
}

// ── Config setters ────────────────────────────────────────────────────────────

// Enables or disables PID control.  On state change, resets all runtime state
// so the integrator and derivative terms start fresh.  Persists the setting to NVS.
// @param en  true to activate PID control; false to release the fan to manual control.
void pidFanSetEnabled(bool en) {
    if (en == s_enabled) return;
    s_enabled = en;
    preferences.putInt("pid_en", en ? 1 : 0);
    resetRuntime();
    if (en) {
        DLOG("[PID] Enabled — SP=%.1f°C probe=%d\n", s_setpoint, s_probeIndex);
    } else {
        DLOGLN("[PID] Disabled — fan returned to manual control");
    }
}

// Returns true when PID control is currently active.
bool pidFanIsEnabled() { return s_enabled; }

// Sets the temperature setpoint in °C and resets the integrator term to prevent
// a wind-up transient after a sudden setpoint jump.  Persists the value to NVS.
// @param sp  Desired pit temperature in °C (clamped to 0–600).
void pidFanSetSetpoint(float sp) {
    sp         = constrain(sp, 0.0f, 600.0f);
    s_setpoint = sp;
    preferences.putFloat("pid_sp", sp);
    s_integral = 0.0f;  // prevent integrator wind-up transient after setpoint jump
    DLOG("[PID] Setpoint → %.1f°C (integrator reset)\n", sp);
}

// Returns the current PID temperature setpoint in °C.
float pidFanGetSetpoint() { return s_setpoint; }

// Sets the probe index used as the PID process variable and resets runtime
// state (prevents derivative spikes from a probe switch).
// @param idx  0-based probe index: 0–5 = food probes, 6 = ambient/pit.
void pidFanSetProbeIndex(int idx) {
    idx          = constrain(idx, 0, 6);
    s_probeIndex = idx;
    preferences.putInt("pid_probe", idx);
    resetRuntime();
}

// Returns the 0-based probe index currently used as the PID process variable.
int pidFanGetProbeIndex() { return s_probeIndex; }

// Applies a complete PID configuration struct atomically: copies all fields,
// saves to NVS, and resets runtime state.
// @param cfg  Configuration to apply (see PidConfig struct in pid_fan.h).
void pidFanApplyConfig(const PidConfig& cfg) {
    s_enabled    = cfg.enabled;
    s_setpoint   = constrain(cfg.setpoint,   0.0f, 600.0f);
    s_probeIndex = constrain(cfg.probeIndex, 0, 6);
    s_kp_con     = cfg.kp_con;
    s_ki_con     = cfg.ki_con;
    s_kd_con     = cfg.kd_con;
    s_kp_agg     = cfg.kp_agg;
    s_ki_agg     = cfg.ki_agg;
    s_kd_agg     = cfg.kd_agg;
    s_bias       = constrain(cfg.bias,   0.0f, 100.0f);
    s_outMin     = constrain(cfg.outMin, 0.0f, 100.0f);
    s_outMax     = constrain(cfg.outMax, s_outMin, 100.0f);
    s_lidDetect  = cfg.lidDetect;
    saveToNVS();
    resetRuntime();
}

// Fills cfg with the current PID configuration (parameters + enabled state).
// Used by the web server to serialise the settings to JSON.
// @param cfg  Output struct to populate.
void pidFanGetConfig(PidConfig& cfg) {
    cfg.enabled    = s_enabled;
    cfg.setpoint   = s_setpoint;
    cfg.probeIndex = s_probeIndex;
    cfg.kp_con     = s_kp_con;
    cfg.ki_con     = s_ki_con;
    cfg.kd_con     = s_kd_con;
    cfg.kp_agg     = s_kp_agg;
    cfg.ki_agg     = s_ki_agg;
    cfg.kd_agg     = s_kd_agg;
    cfg.bias       = s_bias;
    cfg.outMin     = s_outMin;
    cfg.outMax     = s_outMax;
    cfg.lidDetect  = s_lidDetect;
}

// ── Runtime diagnostics ───────────────────────────────────────────────────────

// Returns the most recent PID output value in percent (0–100).
float pidFanGetOutput()   { return s_output; }
// Returns the most recent PID error (setpoint − measured temperature) in °C.
float pidFanGetError()    { return s_lastError; }
// Returns the current value of the integral accumulator (in °C·s).
float pidFanGetIntegral() { return s_integral; }
// Returns true if the lid-open condition is currently active.
bool  pidFanIsLidOpen()   { return s_lidOpen; }
