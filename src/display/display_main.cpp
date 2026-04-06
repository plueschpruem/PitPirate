#include "display.h"
#include "display_fonts.h"

#include "../display/battery_icons.h"
#include "../display/cog_icon.h"
#include "../display/fan_icons.h"
#include "../display/network_lag_icon.h"

// ── Battery icon renderer ─────────────────────────────────────────────────────
// Draws an 8-bit grayscale PROGMEM battery bitmap (33×20) tinted with fg on bg.

// Renders an 8-bit greyscale PROGMEM battery bitmap tinted with the given
// foreground colour on a black background.
// @param x,y  Top-left pixel position.
// @param bmp  PROGMEM pointer to the BATTERY_ESP_0 bitmap data.
// @param fg   Tint colour (RGB565).
static void drawBatteryIcon(int x, int y, const uint8_t* bmp, uint16_t fg) {
    constexpr int W = BATTERY_ESP_0_WIDTH;
    constexpr int H = BATTERY_ESP_0_HEIGHT;
    const uint16_t bg = TFT_BLACK;
    uint16_t fg_r = (fg >> 11) & 0x1F;
    uint16_t fg_g = (fg >> 5) & 0x3F;
    uint16_t fg_b = fg & 0x1F;
    uint16_t buf[W * H]; 
    for (int i = 0; i < W * H; i++) {
        uint16_t a = 255 - pgm_read_byte(&bmp[i]);
        uint16_t r = (fg_r * a) / 255;
        uint16_t g = (fg_g * a) / 255;
        uint16_t b = (fg_b * a) / 255;
        buf[i] = (r << 11) | (g << 5) | b;
    }
    tft.setSwapBytes(true);
    tft.pushImage(x, y, W, H, buf);
    tft.setSwapBytes(false);
}

// ── Fan icon renderer ─────────────────────────────────────────────────────────

// Renders an 8-bit greyscale PROGMEM fan bitmap (FAN_ESP_0 size) tinted with
// the specified foreground colour.
// @param x,y  Top-left pixel position.
// @param bmp  PROGMEM pointer to a FAN_ESP bitmap.
// @param fg   Tint colour (RGB565).
static void drawFanIcon(int x, int y, const uint8_t* bmp, uint16_t fg) {
    constexpr int W = FAN_ESP_0_WIDTH;
    constexpr int H = FAN_ESP_0_HEIGHT;
    uint16_t fg_r = (fg >> 11) & 0x1F;
    uint16_t fg_g = (fg >> 5) & 0x3F;
    uint16_t fg_b = fg & 0x1F;
    uint16_t buf[W * H];
    for (int i = 0; i < W * H; i++) {
        uint16_t a = 255 - pgm_read_byte(&bmp[i]);
        uint16_t r = (fg_r * a) / 255;
        uint16_t g = (fg_g * a) / 255;
        uint16_t b = (fg_b * a) / 255;
        buf[i] = (r << 11) | (g << 5) | b;
    }
    tft.setSwapBytes(true);
    tft.pushImage(x, y, W, H, buf);
    tft.setSwapBytes(false);
}

// ── Network-lag icon renderer ─────────────────────────────────────────────────

// Renders the network-lag warning icon: remaps non-black pixels to red on black.
// @param x,y  Top-left pixel position.
static void drawNetworkLagIcon(int x, int y) {
    constexpr int W = NETWORK_LAG_WIDTH;
    constexpr int H = NETWORK_LAG_HEIGHT;
    uint16_t buf[W * H];
    for (int i = 0; i < W * H; i++) {
        uint16_t src = pgm_read_word(&network_lag_icon[i]);
        // Use inverted brightness: dark pixels (the icon lines) → red, light bg → black
        uint16_t g6 = (src >> 5) & 0x3F;
        uint16_t r5 = ((63 - g6) * 31) / 63;
        buf[i] = (r5 << 11);
    }
    tft.setSwapBytes(true);
    tft.pushImage(x, y, W, H, buf);
    tft.setSwapBytes(false);
}

// ── Navigation elements ───────────────────────────────────────────────────────

// Draws the cog-gear settings button in the ambient strip at the given Y offset.
// @param ay  Absolute Y pixel of the top of the ambient strip.
void drawSettingsBtn(int ay) {
    // Centre the 30×30 cog bitmap within the settings button area
    int bx = SETTINGS_BTN_X + (SETTINGS_BTN_W - COG_WIDTH) / 2;
    int by = ay + (AMB_H - COG_HEIGHT) / 2;
    uint16_t fg = tft.color565(128, 128, 128);
    uint16_t fg_r = (fg >> 11) & 0x1F;
    uint16_t fg_g = (fg >> 5)  & 0x3F;
    uint16_t fg_b =  fg        & 0x1F;
    uint16_t buf[COG_WIDTH * COG_HEIGHT];
    for (int i = 0; i < COG_WIDTH * COG_HEIGHT; i++) {
        uint16_t a = 255 - pgm_read_byte(&cog[i]);
        uint16_t r = (fg_r * a) / 255;
        uint16_t g = (fg_g * a) / 255;
        uint16_t b = (fg_b * a) / 255;
        buf[i] = (r << 11) | (g << 5) | b;
    }
    tft.setSwapBytes(true);
    tft.pushImage(bx, by, COG_WIDTH, COG_HEIGHT, buf);
    tft.setSwapBytes(false);
}

// Draws the status header bar: clock, battery icon+percentage, fan icon+percentage,
// and the network-lag indicator when applicable.
// @param minute     Current time expressed as (hour * 60 + minute); -1 when NTP is unavailable.
// @param battery    Battery level as an integer percentage (0–100); -1 when unknown.
// @param fanPct     Current fan speed percentage (0 = off).
// @param networkLag true when a connectivity alert is active (shows warning icon).
void drawHeader(int minute, int battery, int fanPct, bool networkLag) {
    tft.fillRect(0, 0, SCREEN_W, HDR_H, TFT_BLACK);
    tft.loadFont(FoundGriBol20);

    char clk[12] = "--:--";
    if (minute >= 0)
        snprintf(clk, sizeof(clk), "%02d:%02d Uhr", minute / 60, minute % 60);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK, false);
    tft.drawString(clk, 6, (HDR_H - 19));

    // ── Fan icon + percentage ─────────────
    {
        const uint8_t* fanBmp = (fanPct > 0) ? Fan_ESP_1 : Fan_ESP_0;
        uint16_t fanColor = (fanPct >= 1 && fanPct < 40)    ? tft.color565(0, 200, 0)
                            : (fanPct >= 40 && fanPct < 60) ? TFT_ORANGE
                            : (fanPct >= 60)                ? TFT_RED
                                                            : tft.color565(128, 128, 128);
        // Fan icon sits just to the left of the battery area
        // int fx = SCREEN_W - BATTERY_ESP_0_WIDTH - 4 - 4 - FAN_ESP_0_WIDTH;  // x=259
        int fx = ((SCREEN_W / 2) - (BATTERY_ESP_0_WIDTH / 2));
        int fy = (HDR_H - FAN_ESP_0_HEIGHT) / 2;
        drawFanIcon(fx, fy, fanBmp, fanColor);

        // Percentage text, right-aligned against the fan icon
        char fanBuf[6];
        snprintf(fanBuf, sizeof(fanBuf), "%d%%", fanPct);
        tft.setTextDatum(TL_DATUM);

        tft.setTextColor(fanColor, TFT_BLACK, false);
        tft.drawString(fanBuf, fx + 25, HDR_H - 19);
    }
    // ── Network-lag icon (between fan % and battery, only when lagging) ────────────
    {
        int bx = SCREEN_W - BATTERY_ESP_0_WIDTH - 4;
        // place immediately left of the battery icon with 4-px gap
        int nx = bx - NETWORK_LAG_WIDTH - 4 - 15;
        int ny = (HDR_H - NETWORK_LAG_HEIGHT) / 2;
        if (networkLag) {
            drawNetworkLagIcon(nx, ny);
        } else {
            tft.fillRect(nx, ny, NETWORK_LAG_WIDTH, NETWORK_LAG_HEIGHT, TFT_BLACK);
        }
    }
    // ── Battery icon (far right) ──────────────────────────────────────────────
    if (battery >= 0) {
        uint16_t bc;
        const uint8_t* bmp;
        if (battery <= 0) {
            bc = tft.color565(220, 0, 0);  // red
            bmp = Battery_ESP_0;
        } else if (battery <= 33) {
            bc = tft.color565(255, 140, 0);  // orange
            bmp = Battery_ESP_33;
        } else if (battery <= 66) {
            bc = tft.color565(0, 200, 0);  // green
            bmp = Battery_ESP_66;
        } else {
            bc = tft.color565(0, 200, 0);  // green
            bmp = Battery_ESP_100;
        }
        int bx = SCREEN_W - BATTERY_ESP_0_WIDTH - 4;
        int by = (HDR_H - BATTERY_ESP_0_HEIGHT) / 2;
        drawBatteryIcon(bx, by, bmp, bc);
    }
    tft.unloadFont();
}

void drawFooterStrip(float ambient, int almLo, int almHi) {
    int ay = HDR_H + PROBE_AREA_H;
    tft.fillRect(0, ay, SCREEN_W, AMB_H, TFT_BLACK);

    tft.loadFont(MinecartLCD40);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(tft.color565(128, 128, 128), TFT_BLACK, true);
    tft.drawString("PIT", 8, 225);
    tft.unloadFont();

    int segCX = 210;
    tft.loadFont(MinecartLCD40);
    tft.setTextDatum(MR_DATUM);
    if (!isnan(ambient)) {
        char abuf[16];
        snprintf(abuf, sizeof(abuf), "%d\xb0", (int)ambient);
        tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
        tft.drawString(abuf, segCX, 225);
    } else {
        tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
        tft.drawString("---", segCX, 225);
    }
    tft.unloadFont();

    tft.loadFont(FoundGriBol15);
    char lobuf[8];
    if (almLo > 0) {
        tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
        snprintf(lobuf, sizeof(lobuf), "%d\xb0", almLo);
    } else {
        tft.setTextColor(tft.color565(128, 128, 128), TFT_BLACK, true);
        snprintf(lobuf, sizeof(lobuf), "--- ");
    }
    tft.setTextDatum(BR_DATUM);
    tft.drawString(lobuf, SETTINGS_BTN_X - 20, ay + AMB_H);

    char hibuf[8];
    if (almHi > 0) {
        tft.setTextColor(TFT_WHITE, TFT_BLACK, true);
        snprintf(hibuf, sizeof(hibuf), "%d\xb0", almHi);
    } else {
        tft.setTextColor(tft.color565(128, 128, 128), TFT_BLACK, true);
        snprintf(hibuf, sizeof(hibuf), "--- ");
    }
    tft.setTextDatum(BR_DATUM);
    tft.drawString(hibuf, SETTINGS_BTN_X - 20, ay + AMB_H - 20);
    tft.unloadFont();

    drawSettingsBtn(ay + 3);
}
