#pragma once
#include <Arduino.h>
#include <TFT_eSPI.h>

/*
 * SmoothButton — a drop-in replacement for TFT_eSPI_Button that renders
 * anti-aliased (smooth) rounded rectangles using TFT_eSPI's smooth-graphics API.
 *
 * Differences from the stock TFT_eSPI_Button:
 *   • Uses fillSmoothRoundRect + drawSmoothRoundRect for AA-blended edges.
 *   • Accepts an explicit corner radius (instead of hard-coding min(w,h)/4).
 *   • Accepts a VLW smooth-font pointer (const uint8_t*) for the label, so it
 *     integrates with the project's loadFont/unloadFont font workflow.
 *   • Accepts a bgColor for correct AA blending against the screen background.
 *   • All coordinates reference the top-left corner (upper-left convention).
 */
class SmoothButton {
public:
    SmoothButton();

    // Initialise the button.
    //   x, y     — top-left corner on screen
    //   w, h     — button dimensions in pixels
    //   radius   — corner radius in pixels (0 = square corners)
    //   outline  — border colour
    //   fill     — background fill colour
    //   textcolor— label text colour
    //   label    — button label (up to 9 characters; longer strings use long_label in drawButton)
    //   font     — VLW smooth-font array (e.g. FoundGriBol20); nullptr → built-in font 2
    //   bgColor  — screen background colour used for AA edge blending (default TFT_BLACK)
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

    // Render the button.
    //   inverted   — swap fill and text colours (pressed visual)
    //   long_label — if non-null/non-empty, overrides the stored short label
    void     drawButton(bool inverted = false, const char* long_label = nullptr);

    // Hit-test: returns true when (x,y) falls inside the button rectangle.
    bool     contains(int16_t x, int16_t y) const;

    // Touch / click state tracking — call press(true) on touch-down, press(false) on release.
    void     press(bool p);
    bool     isPressed()    const;
    bool     justPressed()  const;
    bool     justReleased() const;

    // Fine-tune vertical label centering.
    // e.g. setTextYOffset(3) matches the +3 pixel correction used throughout this project
    // for FoundGriBol fonts.
    void     setTextYOffset(int8_t dy) { _dy = dy; }

    // Colour helpers — update individual colours without reinitialising.
    void     setColors(uint16_t outline, uint16_t fill, uint16_t text);
    void     setBgColor(uint16_t bg)       { _bgcolor      = bg;      }
    void     setFillColor(uint16_t fill)   { _fillcolor    = fill;    }
    void     setOutlineColor(uint16_t out) { _outlinecolor = out;     }
    void     setTextColor(uint16_t text)   { _textcolor    = text;    }

private:
    TFT_eSPI*      _gfx;
    int16_t        _x1, _y1;    // top-left corner
    uint16_t       _w, _h;      // width / height
    uint8_t        _radius;     // corner radius
    uint16_t       _outlinecolor, _fillcolor, _textcolor, _bgcolor;
    const uint8_t* _font;       // VLW font (nullptr = built-in font 2)
    char           _label[10];  // stored short label (9 chars + NUL)
    int8_t         _dy;         // vertical text offset applied to MC_DATUM centre
    bool           _curr, _last;
};
