#pragma once
#include <stdint.h>

// PitPirate — SG-90 servo control via GPIO 27
//
// servoInit()               — setup LEDC PWM channel and move to 0° (call once from setup)
// servoSetAngle(0–180)      — move to angle in degrees
// servoGetAngle()           — last commanded angle
// servoSetUs(500–2400)      — raw pulse-width override in microseconds

void    servoInit();
void    servoSetAngle(uint8_t deg);   // clamps to [min,max], persists to NVS
uint8_t servoGetAngle();
void    servoSetUs(uint16_t us);

void    servoSetMinAngle(uint8_t deg);  // persist min limit to NVS
uint8_t servoGetMinAngle();
void    servoSetMaxAngle(uint8_t deg);  // persist max limit to NVS
uint8_t servoGetMaxAngle();

// Auto mode: valve follows fan — open when fan>0, closed when fan=0
void    servoSetAuto(bool enable);      // persist to NVS (key: srv_auto)
bool    servoIsAuto();
void    servoUpdateFromFan(uint8_t fan_pct);  // called by fanSetPercent()
