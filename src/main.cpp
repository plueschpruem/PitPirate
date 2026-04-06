#include <ArduinoOTA.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#include "alarm_notify.h"
#include "config.h"
#include "display/display.h"
#include "display/touch_calibration.h"
#include "display/gif_anim.h"
#include "display/png_image.h"
#include "fan_control.h"
#include "http_task.h"
#include "pid_fan.h"
#include "network/tuya_lan.h"
#include "network/web_server.h"
#include "network/wifi_manager.h"
#include "remote_post.h"
#include "screenshot.h"
#include "shared_data.h"
#include "debug_log.h"

// ── Hardware objects ──────────────────────────────────────────────────────────
SPIClass touchscreenSpi(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft;

// ---------- Setup ----------
void setup() {
    Serial.begin(115200);
    Serial.println("PitPirate starting...");

    // GPIO_0 (BOOT button) — used as screenshot trigger; INPUT_PULLUP, active-LOW
    pinMode(0, INPUT_PULLUP);

    // RGB LED (active-low common-anode) — drive HIGH to keep all channels off
#if defined(BOARD_HAS_RGB_LED) && BOARD_HAS_RGB_LED != 0
    pinMode(RGB_LED_R, OUTPUT); digitalWrite(RGB_LED_R, HIGH);
    pinMode(RGB_LED_G, OUTPUT); digitalWrite(RGB_LED_G, HIGH);
    pinMode(RGB_LED_B, OUTPUT); digitalWrite(RGB_LED_B, HIGH);
#endif

    // Display
    tft.init();
    tft.setRotation(3);  // landscape: 320x240
    tft.setTextFont(1);  // built-in GLCD 8px font (requires LOAD_GLCD)
    tft.fillScreen(TFT_BLACK);

    // Backlight PWM – LEDC channel 1, 5 kHz, 8-bit resolution (0–255)
    // tft.init() has already driven TFT_BL HIGH; attach LEDC to take over the pin.
    ledcSetup(1, 5000, 8);
    ledcAttachPin(TFT_BL, 1);
    ledcWrite(1, 255);  // 100% brightness on boot

    // Touchscreen
    touchscreenSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touchscreen.begin(touchscreenSpi);
    touchscreen.setRotation(3);

    // Filesystem (LittleFS)
    fsInit();

    // Boot splash (5 s animated title screen)
    showSplash();

    // Persistent settings
    preferences.begin("pitpirate", false);
    touchCalibInit();  // load saved calibration (or use defaults 150/3900)

    // Show "Connecting…" on screen before WiFi blocks
    jsonData = "{\"connecting\":true}";
    updateDisplay();
    // GIF: 95×128 px, right side bottom
    startGifAnimation("/StanAnim.gif", SCREEN_W - 95 - 8, SCREEN_H - 128);

    // WiFi + mDNS (may enter AP mode if no credentials or connection fails)
    wifiInit();

    if (wifiIsAPMode()) {
        // In AP mode: just start the web server so users can configure WiFi.
        // Tuya, remote telemetry, alarms, and NTP are not meaningful without a network.
        stopGifAnimation();
        webServerInit();
        jsonData = "{\"ap_mode\":true}";
        updateDisplay();
        Serial.println("Setup done (AP provisioning mode)");
        return;
    }

    // NTP
    configTzTime(NTP_TZ, NTP_SERVER);

    // HTTP server
    webServerInit();

    // Tuya LAN client
    tuyaLanInit();

    // Remote telemetry poster
    remotePostInit();

    // Core 0 HTTP worker (must start after remotePostInit so URL is ready)
    httpTaskInit();

    // Alarm checker
    alarmNotifyInit();

    // Fan control (GPIO 22, PWM)
    fanInit();
    pidFanInit();

    // OTA firmware updates
    ArduinoOTA.setHostname("pitpirate");
    ArduinoOTA.begin();

    stopGifAnimation();
    updateDisplay();  // draw main screen

    Serial.println("Setup done");
}

// ---------- Loop ----------
static String lastJsonData;
static int lastMinute = -1;
// GPIO_0 (BOOT button) screenshot trigger state
static bool          s_gpio0Prev       = HIGH;
static unsigned long s_gpio0DebounceMs = 0;
static bool lastTouched = false;
static bool    s_showSettings     = false;  // true while the settings page is visible
static uint8_t s_settingsEntryPct = 0;      // fan % captured when settings was opened
static unsigned long lastDisplayMs = 0;     // non-blocking display-refresh gate
static unsigned long lastRssiMs   = 0;      // non-blocking RSSI refresh gate (settings page)
static bool s_showSettings2     = false;    // true while settings page 2 is visible
static bool s_showSettings3     = false;    // true while settings page 3 (system info) is visible
static bool s_showProbeLimits   = false;    // true while probe limits page is shown
static bool s_showCalibration   = false;    // true while touch calibration is active
static int  s_limitsProbeNum    = 0;        // 1-based probe number being edited
static int  s_limitsLo          = 0;        // working LO limit (°C, 0 = off)
static int  s_limitsHi          = 0;        // working HI limit (°C, 0 = off)
// Backlight auto-dim state
#define BACKLIGHT_MIN 5						 // minimum backlinght brightness
#define BACKLIGHT_MAX 100					 // maximum backlinght brightness
static int  		 s_blPct          = BACKLIGHT_MAX; // current brightness (10–100%)
static unsigned long s_lastActivityMs = 0;   // millis() of last touch
static unsigned long s_lastDimMs      = 0;   // millis() of last 1%-step dim

// Maps screen-space touch coordinates to a probe number by identifying which
// probe cell the tap landed in.
// @param tx  Touch X coordinate in screen pixels (0–SCREEN_W).
// @param ty  Touch Y coordinate in screen pixels (0–SCREEN_H).
// @return  1-based probe number (1–6) when the touch is inside an active probe
//          cell; 0 when the tap is outside the probe area or no probes are active.
static int probeAtTouch(int tx, int ty) {
    if (ty < HDR_H || ty >= HDR_H + PROBE_AREA_H) return 0;
    int active[6], n = 0;
    for (int i = 0; i < 6; i++)
        if (!isnan(dcache.probeVal[i])) active[n++] = i;
    if (n == 0) return 0;
    int cols, rows;
    if      (n <= 1) { cols = 1; rows = 1; }
    else if (n <= 2) { cols = 2; rows = 1; }
    else if (n <= 3) { cols = 3; rows = 1; }
    else if (n <= 4) { cols = 2; rows = 2; }
    else             { cols = 3; rows = 2; }
    int col = tx / (SCREEN_W / cols);
    int row = (ty - HDR_H) / (PROBE_AREA_H / rows);
    int idx = row * cols + col;
    if (idx < 0 || idx >= n) return 0;
    return active[idx] + 1;
}

void loop() {
    wifiCheck();
    ArduinoOTA.handle();

    // ── Screenshot: BOOT button (GPIO_0) + debug log enabled ─────────────────
    {
        bool gpio0 = digitalRead(0);
        if (gpio0 == LOW && s_gpio0Prev == HIGH
                && millis() - s_gpio0DebounceMs > 50UL) {
            s_gpio0DebounceMs = millis();
            if (debugLogEnabled()) {
                DLOGLN("[SHOT] GPIO_0 pressed – capturing screenshot");
                screenshotUpload();
            }
        }
        s_gpio0Prev = gpio0;
    }

    isOnSettingsPage = s_showSettings || s_showSettings2 || s_showSettings3 || s_showProbeLimits || s_showCalibration;

    // Touch and network-dependent services only run in normal STA mode
    bool nowTouched = touchscreen.touched();

    // Restore backlight to 100% on any new touch and reset the idle timer.
    // If the screen was dimmed, consume this touch entirely — do not pass it to the GUI.
    if (nowTouched && !lastTouched) {
        s_lastActivityMs = millis();
        if (s_blPct < BACKLIGHT_MAX) {
            s_blPct = BACKLIGHT_MAX;
            ledcWrite(1, (uint8_t)(s_blPct * 255 / 100));
            lastTouched = nowTouched;
            DLOG("[Touch] Wakeup touch. Setting backlight to 100%\n");
            return;
        }
    }

    // AP mode: handle the Reset button
    if (wifiIsAPMode() && nowTouched && !lastTouched) {
        TS_Point p = touchscreen.getPoint();
        int tx, ty;
        touchCalibMap(p.x, p.y, tx, ty);
        if (tx >= APRESET_BTN_X && tx <= APRESET_BTN_X + APRESET_BTN_W &&
            ty >= APRESET_BTN_Y && ty <= APRESET_BTN_Y + APRESET_BTN_H) {
            ESP.restart();
        }
    }

    if (!wifiIsAPMode()) {
        if (nowTouched && !lastTouched) {
            TS_Point p = touchscreen.getPoint();
            // Map raw XPT2046 ADC values to screen pixels using saved calibration.
            int tx, ty;
            touchCalibMap(p.x, p.y, tx, ty);
            DLOG("[Touch] raw=(%d,%d) screen=(%d,%d)\n", p.x, p.y, tx, ty);

            if (s_showCalibration) {
                // ── Touch calibration flow ───────────────────────────────────
                if (touchCalibHandleTouch(tx, ty, p.x, p.y)) {
                    s_showCalibration = false;
                    s_showSettings2   = true;
                    drawSettingsPage2();
                }
            } else if (s_showSettings3) {
                // ── Settings page 3 (System Info) touch ──────────────────────
                if (ty <= HDR_H && tx <= 110) {
                    // "< Back" → return to page 2
                    s_showSettings3 = false;
                    drawSettingsPage2();
                }
            } else if (s_showSettings2) {
                // ── Settings page 2 touch ────────────────────────────────────
                if (ty <= HDR_H && tx <= 110) {
                    // "< Back" → return to page 1
                    s_showSettings2 = false;
                    drawSettingsPage();
                } else if (tx >= DBGLOG_BTN_X && tx <= DBGLOG_BTN_X + DBGLOG_BTN_W &&
                           ty >= DBGLOG_BTN_Y && ty <= DBGLOG_BTN_Y + DBGLOG_BTN_H) {
                    debugLogSetEnabled(!debugLogEnabled());
                    drawDebugLogBtn(debugLogEnabled());
                } else if (tx >= TEL_ONCHG_BTN_X && tx <= TEL_ONCHG_BTN_X + TEL_ONCHG_BTN_W &&
                           ty >= TEL_ONCHG_BTN_Y && ty <= TEL_ONCHG_BTN_Y + TEL_ONCHG_BTN_H) {
                    uint32_t iv; bool oc;
                    remotePostGetTelemetryConfig(iv, oc);
                    remotePostApplyTelemetryConfig(iv, !oc);
                    drawTelOnChangeBtn(!oc);
                } else if (ty >= TEL_INT_BTN_Y && ty <= TEL_INT_BTN_Y + TEL_INT_BTN_H) {
                    static const uint32_t telOpts[4] = {10, 30, 60, 180};
                    int idx = constrain((tx - 5) / (TEL_INT_BTN_W + TEL_INT_BTN_GAP), 0, 3);
                    uint32_t iv; bool oc;
                    remotePostGetTelemetryConfig(iv, oc);
                    remotePostApplyTelemetryConfig(telOpts[idx], oc);
                    drawTelIntervalBtns(telOpts[idx]);
                } else if (tx >= CALIB_BTN_X && tx <= CALIB_BTN_X + CALIB_BTN_W &&
                           ty >= CALIB_BTN_Y && ty <= CALIB_BTN_Y + CALIB_BTN_H) {
                    s_showSettings2   = false;
                    s_showCalibration = true;
                    touchCalibBegin();
                } else if (tx >= SCREEN_W - 80 && ty > CALIB_BTN_Y + CALIB_BTN_H) {
                    // "More >" link — navigate to page 3 (System Info)
                    s_showSettings3 = true;
                    drawSettingsPage3();
                }
            } else if (s_showSettings) {
                // ── Settings page touch ──────────────────────────────────────
                if (ty <= HDR_H && tx <= 110) {
                    // "< Back" button
                    s_showSettings = false;
                    dcache.invalidate();
                    updateDisplay();
                } else if (tx >= APMODE_BTN_X && tx <= APMODE_BTN_X + APMODE_BTN_W &&
                           ty >= APMODE_BTN_Y && ty <= APMODE_BTN_Y + APMODE_BTN_H) {
                    // "Boot AP" button — keeps saved WiFi credentials intact
                    wifiForceAPMode();
                    jsonData = "{\"ap_mode\":true}";
                    inErrorState = false;  // suppress Tuya error blink
                    s_showSettings = false;
                    updateDisplay();
                } else if (tx >= PIDMODE_BTN_X && tx <= PIDMODE_BTN_X + PIDMODE_BTN_W &&
                           ty >= PIDMODE_BTN_Y && ty <= PIDMODE_BTN_Y + PIDMODE_BTN_H) {
                    // PID blower control toggle
                    pidFanSetEnabled(!pidFanIsEnabled());
                    drawPidModeBtn(pidFanIsEnabled());
                } else if (tx >= SCREEN_W - 80 && ty >= FAN_ROW2_Y) {
                    // "More >" link — right 80 px column from FAN_ROW2 downward
                    s_showSettings2 = true;
                    drawSettingsPage2();
                } else if (ty >= FAN_ROW1_Y && ty <= FAN_ROW1_Y + FAN_ROW1_H) {
                    // Row 1: ON/OFF | MIN | entry speed | 100%
                    int btnIdx = constrain(tx / (FAN_BTN_W + FAN_BTN_GAP), 0, 3);
                    uint8_t newPct = 0;
                    if (btnIdx == 0) {
                        // Toggle on/off
                        newPct = (fanGetPercent() > 0)
                                     ? 0
                                     : (uint8_t)max((int)s_settingsEntryPct,
                                                    (int)fanGetMinPct());
                    } else if (btnIdx == 1) {
                        newPct = fanGetMinPct();
                    } else if (btnIdx == 2) {
                        newPct = 50;
                    } else {
                        newPct = 100;
                    }
                    fanSetPercent(newPct);
                    remotePostBlowerConfig();
                    drawFanSection(fanGetPercent(), s_settingsEntryPct);
                    drawSettingsMoreLink();
                } else if (ty >= FAN_ROW2_Y && ty <= FAN_ROW2_Y + FAN_ROW2_H) {
                    if (tx < FAN_SLIDER_X) {
                        // Row 2 entry-speed button
                        fanSetPercent(s_settingsEntryPct);
                        remotePostBlowerConfig();
                        drawFanSection(fanGetPercent(), s_settingsEntryPct);
                        drawSettingsMoreLink();
                    } else if (tx >= FAN_SLIDER_X && tx <= FAN_SLIDER_X + FAN_SLIDER_W) {
                        // Slider
                        uint8_t pct = (uint8_t)constrain(
                            (tx - FAN_SLIDER_X) * 100 / FAN_SLIDER_W, 0, 100);
                        fanSetPercent(pct);
                        remotePostBlowerConfig();
                        drawFanSection(fanGetPercent(), s_settingsEntryPct);
                        drawSettingsMoreLink();
                    }
                }
            } else if (s_showProbeLimits) {
                // ── Probe alarm limits page touch ────────────────────────────
                if (ty <= HDR_H && tx <= 110) {
                    // "< Back"
                    s_showProbeLimits = false;
                    dcache.invalidate();
                    updateDisplay();
                } else if (tx < ALM_SLIDER_X || tx > ALM_SLIDER_X + ALM_SLIDER_W) {
                    // Fine-tune −/+ buttons flanking each slider
                    bool isLo = (ty >= ALM_LO_LABEL_Y &&
                                 ty <= ALM_LO_SLIDER_TY + ALM_SLIDER_TH / 2 + ALM_BTN_H / 2);
                    bool isHi = (ty >= ALM_HI_LABEL_Y &&
                                 ty <= ALM_HI_SLIDER_TY + ALM_SLIDER_TH / 2 + ALM_BTN_H / 2);
                    int  delta = (tx < ALM_SLIDER_X) ? -1 : 1;
                    if (isLo) {
                        s_limitsLo = constrain(s_limitsLo + delta, 0, 200);
                        drawAlarmLoSlider(s_limitsProbeNum, s_limitsLo);
                    } else if (isHi) {
                        s_limitsHi = constrain(s_limitsHi + delta, 0, 200);
                        drawAlarmHiSlider(s_limitsProbeNum, s_limitsHi);
                    }
                } else if (tx >= ALM_SLIDER_X && tx <= ALM_SLIDER_X + ALM_SLIDER_W &&
                           ty >= ALM_LO_SLIDER_TY - ALM_HANDLE_R - 2 &&
                           ty <= ALM_LO_SLIDER_TY + ALM_SLIDER_TH + ALM_HANDLE_R + 2) {
                    s_limitsLo = constrain((tx - ALM_SLIDER_X) * 200 / ALM_SLIDER_W, 0, 200);
                    drawAlarmLoSlider(s_limitsProbeNum, s_limitsLo);
                } else if (tx >= ALM_SLIDER_X && tx <= ALM_SLIDER_X + ALM_SLIDER_W &&
                           ty >= ALM_HI_SLIDER_TY - ALM_HANDLE_R - 2 &&
                           ty <= ALM_HI_SLIDER_TY + ALM_SLIDER_TH + ALM_HANDLE_R + 2) {
                    s_limitsHi = constrain((tx - ALM_SLIDER_X) * 200 / ALM_SLIDER_W, 0, 200);
                    drawAlarmHiSlider(s_limitsProbeNum, s_limitsHi);
                } else if (tx >= ALM_SET_BTN_X && tx <= ALM_SET_BTN_X + ALM_SET_BTN_W &&
                           ty >= ALM_SET_BTN_Y  && ty <= ALM_SET_BTN_Y  + ALM_SET_BTN_H) {
                    // Persist to NVS and push to remote server
                    preferences.putInt(("alm_" + String(s_limitsProbeNum) + "_lo").c_str(), s_limitsLo);
                    preferences.putInt(("alm_" + String(s_limitsProbeNum) + "_hi").c_str(), s_limitsHi);
                    remotePostAlarmConfig();
                    s_showProbeLimits = false;
                    dcache.invalidate();
                    updateDisplay();
                }
            } else {
                // ── Main screen touch ───────────────────────────────────────────
                constexpr int ay = HDR_H + PROBE_AREA_H;
                if (ty >= ay && tx >= SETTINGS_BTN_X) {
                    // Hamburger / settings button in the ambient strip
                    s_settingsEntryPct = fanGetPercent();
                    lastRssiMs = millis();
                    s_showSettings = true;
                    drawSettingsPage();
                } else if (ty >= ay) {
                    // Ambient / PIT strip tap → open alarm limits for ambient (probe 7)
                    s_limitsProbeNum = 7;
                    s_limitsLo = preferences.getInt("alm_7_lo", 0);
                    s_limitsHi = preferences.getInt("alm_7_hi", 0);
                    s_showProbeLimits = true;
                    drawProbeLimitsPage(7, s_limitsLo, s_limitsHi);
                } else {
                    // Probe cell tap → open alarm limits page
                    int pNum = probeAtTouch(tx, ty);
                    if (pNum > 0) {
                        s_limitsProbeNum = pNum;
                        s_limitsLo = preferences.getInt(("alm_" + String(pNum) + "_lo").c_str(), 0);
                        s_limitsHi = preferences.getInt(("alm_" + String(pNum) + "_hi").c_str(), 0);
                        s_showProbeLimits = true;
                        drawProbeLimitsPage(pNum, s_limitsLo, s_limitsHi);
                    }
                }
            }
        }
        // Re-check: wifiForceAPMode() may have been called during touch handling above.
        if (!wifiIsAPMode()) {
            tuyaLanLoop();
            remotePostLoop();
            alarmNotifyLoop();
            pidFanLoop();
            httpTaskDrainResults();       // apply blower results from Core 0
            httpTaskDrainGraphResults();  // apply graph bitmaps from Core 0
        }
    }
    lastTouched = nowTouched;

    webServerLoop();
    fanLoop();

    // Error bar blink: toggle every 2 s while error persists (skip while any overlay is open)
    if (!wifiIsAPMode() && !s_showSettings && !s_showSettings2 && !s_showSettings3 && !s_showProbeLimits && !s_showCalibration && inErrorState) {
        if (millis() - lastBlinkMs >= 2000UL) {
            lastBlinkMs = millis();
            errorBarOn = !errorBarOn;
            drawErrorBar(errorBarOn);
        }
    }

    struct tm ti;
    int curMinute = getLocalTime(&ti, 0) ? ti.tm_hour * 60 + ti.tm_min : -1;

    // Update RSSI readout on the settings pages every 5 s
    if ((s_showSettings || s_showSettings2 || s_showSettings3) && millis() - lastRssiMs >= 5000UL) {
        lastRssiMs = millis();
        drawSettingsRssi();
    }

    // Suppress periodic redraws while any overlay page is shown.
    // Rate-limit to once per second without blocking.
    if (!s_showSettings && !s_showSettings2 && !s_showSettings3 && !s_showProbeLimits && !s_showCalibration && millis() - lastDisplayMs >= 1000UL) {
        lastDisplayMs = millis();
        if (jsonData != lastJsonData || curMinute != lastMinute) {
            lastJsonData = jsonData;
            lastMinute = curMinute;
            updateDisplay();
        }
    }

    // Backlight auto-dim: after 60 s idle ramp down 1%/s; floor at 10%
    if (s_blPct > BACKLIGHT_MIN && millis() - s_lastActivityMs >= 60000UL) {
        if (millis() - s_lastDimMs >= 1000UL) {
            s_lastDimMs = millis();
            s_blPct = max(BACKLIGHT_MIN, s_blPct - 1);
            ledcWrite(1, (uint8_t)(s_blPct * 255 / 100));
        }
    }
}
