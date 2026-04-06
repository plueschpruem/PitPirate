#pragma once
#include <stdint.h>

// PitPirate — PWM fan control via GPIO 22 + IRF520N MOSFET adapter
//
// fanInit()             — setup PWM and restore persisted speed (call once from setup)
// fanSetDuty(0–255)     — raw 8-bit duty cycle
// fanSetPercent(0–100)  — set speed and persist to NVS (clamped to min if on)
// fanGetPercent()       — last set percentage
// fanSetStartPct/fanGetStartPct — kick-start threshold (fan won't start below this)
// fanSetMinPct/fanGetMinPct     — minimum sustained speed once running

void    fanInit();
void    fanLoop();   // call every loop() — handles kick-start settling
void    fanSetDuty(uint8_t duty);
void    fanSetPercent(uint8_t pct);
uint8_t fanGetPercent();

void    fanSetStartPct(uint8_t pct);
uint8_t fanGetStartPct();
void    fanSetMinPct(uint8_t pct);
uint8_t fanGetMinPct();
