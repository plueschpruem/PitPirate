#pragma once

// RENAME THIS FILE TO config.h

#define PITPIRATE_VERSION  "1.0.0"	// change this as you like

// ---- WIFI configuration ----
// JUST LEAVE THOSE BLANK, YOU CAN SET THEM IN THE WEB INTERFACE
#define WIFI_SSID  ""				// e.g. "MyNetwork"
#define WIFI_PW    ""				// e.g. "MyPassword"

// ---- mDNS configuration ----
#define WIFI_MDNS  "pitpirate"		// reachable as pitpirate.local

// ---- Tuya LAN device (BBQ thermometer) ----
// JUST LEAVE THOSE BLANK, YOU CAN SET THEM IN THE WEB INTERFACE
#define TUYA_DEVICE_IP ""           // the IP of your thermometer e.g. 192.168.0.10
#define TUYA_DEVICE_ID   ""			// 22 alphanumerical chars e.g. fdb4f3d3a22bbaa2211wbp
#define TUYA_LOCAL_KEY   ""			// exactly 16 bytes from Tuya.com cloud

// ---- NTP ----
#define NTP_SERVER  "pool.ntp.org"
#define NTP_TZ      "CET-1CEST,M3.5.0,M10.5.0/3"  // Central European Time w/ DST

// ---- Remote telemetry ----
// Leave REMOTE_POST_URL empty to disable. HTTP and HTTPS both supported.
// REMOTE_POST_TOKEN must match SECRET_TOKEN in server/php/telemetry.php (empty = no auth).
#define REMOTE_POST_URL    ""		// e.g. "https://yourserver.com/php/telemetry.php"
#define REMOTE_POST_TOKEN  ""		// shared secret token e.g. AAA_BBB_CCC_222 | Same as in ./server/php/config.php

// ---- Alarm push notifications ----
// Shared secret between the ESP32 and server/php/alarm.php.
// Must match ALARM_SECRET in server/php/config.php.
#define ALARM_SECRET  ""			// e.g. tZgVso_7291_xKqB_4483 | Same as in ./server/php/config.php 
