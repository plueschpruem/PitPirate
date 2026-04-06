#include "smooth_button.h"
#include <string.h>

// ── Constructor ───────────────────────────────────────────────────────────────

SmoothButton::SmoothButton()
    : _gfx(nullptr)
    , _x1(0), _y1(0)
    , _w(0),  _h(0)
    , _radius(8)
    , _outlinecolor(TFT_WHITE)
    , _fillcolor(TFT_BLUE)
    , _textcolor(TFT_WHITE)
    , _bgcolor(TFT_BLACK)
    , _font(nullptr)
    , _dy(0)
    , _curr(false), _last(false)
{
    _label[0] = '\0';
    _label[9] = '\0';
}

// ── Initialisation ────────────────────────────────────────────────────────────

// Initialises all button parameters.  Must be called before drawButton().
// @param gfx        Pointer to the TFT_eSPI display instance.
// @param x,y        Top-left corner of the button in screen pixels.
// @param w,h        Button dimensions in pixels.
// @param radius     Corner radius for rounded-rect drawing (0 = sharp corners).
// @param outline    Border colour (RGB565).
// @param fill       Background fill colour (RGB565); swapped with textcolor when inverted.
// @param textcolor  Label text colour (RGB565).
// @param label      Short label string (max 9 characters, NUL-terminated).
// @param font       PROGMEM VLW font for the label, or nullptr to use built-in font 2.
// @param bgColor    Screen background colour (RGB565) used for anti-aliased edges.
void SmoothButton::initButton(TFT_eSPI*      gfx,
                               int16_t        x,
                               int16_t        y,
                               uint16_t       w,
                               uint16_t       h,
                               uint8_t        radius,
                               uint16_t       outline,
                               uint16_t       fill,
                               uint16_t       textcolor,
                               const char*    label,
                               const uint8_t* font,
                               uint16_t       bgColor)
{
    _gfx          = gfx;
    _x1           = x;
    _y1           = y;
    _w            = w;
    _h            = h;
    _radius       = radius;
    _outlinecolor = outline;
    _fillcolor    = fill;
    _textcolor    = textcolor;
    _bgcolor      = bgColor;
    _font         = font;
    _dy           = 0;
    _curr         = false;
    _last         = false;

    strncpy(_label, label, 9);
    _label[9] = '\0';
}

// ── Rendering ─────────────────────────────────────────────────────────────────

// Renders the button using stored parameters and the active TFT handle.
// Uses fillSmoothRoundRect / drawSmoothRoundRect for anti-aliased corners.
// @param inverted    When true, swaps fill and text colours (pressed state).
// @param long_label  Optional override label; falls back to the stored 9-char label.
void SmoothButton::drawButton(bool inverted, const char* long_label)
{
    uint16_t fill    = inverted ? _textcolor    : _fillcolor;
    uint16_t outline = _outlinecolor;
    uint16_t text    = inverted ? _fillcolor    : _textcolor;

    // ── Background fill ───────────────────────────────────────────────────────
    // fillSmoothRoundRect anti-aliases the corner pixels against _bgcolor,
    // producing smooth rounded edges instead of the staircase of fillRoundRect.
    if (_radius > 0) {
        _gfx->fillSmoothRoundRect(_x1, _y1, _w, _h, _radius, fill, _bgcolor);
    } else {
        _gfx->fillRect(_x1, _y1, _w, _h, fill);
    }

    // ── Border ring ───────────────────────────────────────────────────────────
    // drawSmoothRoundRect draws a ring whose corner thickness = r - ir.
    // Using ir = r-1 gives a ~1 px smooth (AA) border on the corners; the
    // straight-edge segments of the ring are 1 px wide as well.
    if (_radius > 0) {
        int32_t ir = (_radius > 1) ? (int32_t)_radius - 1 : 0;
        _gfx->drawSmoothRoundRect(_x1, _y1, _radius, ir, _w, _h, outline, _bgcolor);
    } else {
        _gfx->drawRect(_x1, _y1, _w, _h, outline);
    }

    // ── Label ─────────────────────────────────────────────────────────────────
    const char* lbl = (long_label && *long_label) ? long_label : _label;

    if (_font) {
        _gfx->loadFont(_font);
    } else {
        _gfx->setTextFont(2);
    }

    _gfx->setTextDatum(MC_DATUM);
    _gfx->setTextColor(text, fill, true);
    _gfx->drawString(lbl, _x1 + _w / 2, _y1 + _h / 2 + _dy);

    if (_font) {
        _gfx->unloadFont();
    }
}

// ── Hit test ──────────────────────────────────────────────────────────────────

// Returns true when the point (x, y) lies within the button's bounding rectangle.
bool SmoothButton::contains(int16_t x, int16_t y) const
{
    return (x >= _x1) && (x < _x1 + (int16_t)_w)
        && (y >= _y1) && (y < _y1 + (int16_t)_h);
}

// ── Touch state ───────────────────────────────────────────────────────────────

// Updates touch-press state for change-detection (justPressed / justReleased).
// Call once per loop() with the current touch state.
// @param p  true if the button is currently being pressed.
void SmoothButton::press(bool p)
{
    _last = _curr;
    _curr = p;
}

bool SmoothButton::isPressed()    const { return  _curr;           }
bool SmoothButton::justPressed()  const { return  _curr && !_last; }
bool SmoothButton::justReleased() const { return !_curr &&  _last; }

// ── Colour helpers ────────────────────────────────────────────────────────────

// Updates all three colour properties in one call.
// @param outline  New border colour (RGB565).
// @param fill     New fill colour (RGB565).
// @param text     New text colour (RGB565).
void SmoothButton::setColors(uint16_t outline, uint16_t fill, uint16_t text)
{
    _outlinecolor = outline;
    _fillcolor    = fill;
    _textcolor    = text;
}
