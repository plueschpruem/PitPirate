#pragma once

// PitPirate — alarm checker + push notification dispatcher
//
// alarmNotifyLoop() should be called once per second from the main loop.
// It reads alarm limits from NVS, compares them against the current probe
// temperatures, logs any breach to Serial, and POSTs a push-notification
// request to <server_base_url>/alarm.php.
//
// Alarms re-arm automatically 5 minutes after the triggering condition clears.

void alarmNotifyInit();
void alarmNotifyLoop();
void alarmNotifyTest();   // send a test push notification immediately
