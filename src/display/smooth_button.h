#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

/**
 * @brief Anti-aliased rounded-rectangle button for TFT_eSPI displays.
 *
 * Drop-in replacement for TFT_eSPI_Button that uses the smooth-graphics API
 * (`fillSmoothRoundRect` / `drawSmoothRoundRect`) so corners are AA-blended
 * instead of jagged.
 *
 * Typical usage:
 * @code
 *   SmoothButton btn;
 *   btn.initButton(&tft, x, y, w, h, 8,
 *                  tft.color565(80,120,220),  // outline
 *                  tft.color565(40, 60,150),  // fill
 *                  TFT_WHITE, "Boot AP", FoundGriBol20);
 *   btn.setTextYOffset(3);
 *   btn.drawButton();
 *
 *   // in touch handler:
 *   if (btn.contains(tx, ty)) btn.press(true);
 *   if (btn.justPressed()) { ... }
 * @endcode
 */
class SmoothButton {
public:
    SmoothButton();

    /**
     * @brief Initialise all button parameters.  Must be called before drawButton().
     *
     * @param gfx       Pointer to the TFT_eSPI display instance.
     * @param x         Left edge of the button in screen pixels.
     * @param y         Top edge of the button in screen pixels.
     * @param w         Button width in pixels.
     * @param h         Button height in pixels.
     * @param radius    Corner radius in pixels (0 = sharp corners).
     * @param outline   Border colour (RGB565).
     * @param fill      Background fill colour (RGB565).
     * @param textcolor Label text colour (RGB565).
     * @param label     Short label string (max 30 characters).
     *                  Longer text can be passed at draw-time via drawButton(false, longStr).
     * @param font      PROGMEM VLW font array (e.g. FoundGriBol20),
     *                  or nullptr to fall back to built-in font 2.
     * @param bgColor   Screen background colour (RGB565) used for AA edge blending.
     *                  Defaults to TFT_BLACK.
     */
    void     initButton(TFT_eSPI*      gfx,
                        int16_t        x,
                        int16_t        y,
                        uint16_t       w,
                        uint16_t       h,
                        uint8_t        radius,
                        uint16_t       outline,
                        uint16_t       fill,
                        uint16_t       textcolor,
                        const char*    label,
                        const uint8_t* font    = nullptr,
                        uint16_t       bgColor = TFT_BLACK);

    /**
     * @brief Render the button onto the display.
     *
     * @param inverted   When true, swaps fill and text colours to show a pressed state.
     * @param long_label Optional label override; falls back to the 30-char stored label
     *                   when null or empty.  Use for labels longer than 9 characters.
     */
    void     drawButton(bool inverted = false, const char* long_label = nullptr);

    /**
     * @brief Hit-test — returns true when (x, y) is inside the button rectangle.
     *
     * @param x  Touch X coordinate in screen pixels.
     * @param y  Touch Y coordinate in screen pixels.
     */
    bool     contains(int16_t x, int16_t y) const;

    /**
     * @brief Update the pressed state.  Call once per touch event with the
     *        current touch-down status; used by justPressed() / justReleased().
     *
     * @param p  true if the button is currently being touched/pressed.
     */
    void     press(bool p);

    /** @brief Returns true while the button is held down. */
    bool     isPressed()    const;

    /** @brief Returns true on the first frame the button is pressed (falling edge). */
    bool     justPressed()  const;

    /** @brief Returns true on the first frame after the button is released (rising edge). */
    bool     justReleased() const;

    /**
     * @brief Fine-tune the vertical label position.
     *
     * TFT_eSPI's MC_DATUM centre does not match all fonts' baseline well.
     * Pass a small positive offset (e.g. 3) to shift the text down into the
     * visual centre of the button.
     *
     * @param dy  Pixel offset added to the Y centre coordinate (positive = down).
     */
    void     setTextYOffset(int8_t dy) { _dy = dy; }

    /**
     * @brief Update outline, fill, and text colours in one call (without reinitialising).
     * @param outline  New border colour (RGB565).
     * @param fill     New fill colour (RGB565).
     * @param text     New text colour (RGB565).
     */
    void     setColors(uint16_t outline, uint16_t fill, uint16_t text);

    /** @brief Replace the AA-blend background colour (must match the screen background). */
    void     setBgColor(uint16_t bg)       { _bgcolor      = bg;      }

    /** @brief Replace the button fill colour. */
    void     setFillColor(uint16_t fill)   { _fillcolor    = fill;    }

    /** @brief Replace the border/outline colour. */
    void     setOutlineColor(uint16_t out) { _outlinecolor = out;     }

    /** @brief Replace the label text colour. */
    void     setTextColor(uint16_t text)   { _textcolor    = text;    }

private:
    TFT_eSPI*      _gfx;
    int16_t        _x1, _y1;    ///< top-left corner
    uint16_t       _w, _h;      ///< width / height
    uint8_t        _radius;     ///< corner radius
    uint16_t       _outlinecolor, _fillcolor, _textcolor, _bgcolor;
    const uint8_t* _font;       ///< VLW font (nullptr = built-in font 2)
    char           _label[31];  ///< stored short label (30 chars + NUL)
    int8_t         _dy;         ///< vertical text offset applied to MC_DATUM centre
    bool           _curr, _last;
};
