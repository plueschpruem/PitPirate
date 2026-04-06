#pragma once
#include <QRcodeDisplay.h>
#include <TFT_eSPI.h>

// QRcodeDisplay adapter for TFT_eSPI.
// Each QR module is rendered as a solid _scale×_scale rectangle.
// multiply=1 and offsets=0 so we control placement ourselves.
class TFTQRDisplay : public QRcodeDisplay {
    TFT_eSPI& _tft;
    int _x0, _y0, _scale;
    uint16_t _fg, _bg;

   protected:
    void drawPixel(int x, int y, int color) override {
        _tft.fillRect(_x0 + x * _scale, _y0 + y * _scale,
                      _scale, _scale, color ? _fg : _bg);
    }

   public:
    TFTQRDisplay(TFT_eSPI& tft, int x, int y, int sc, uint16_t fg, uint16_t bg)
        : _tft(tft), _x0(x), _y0(y), _scale(sc), _fg(fg), _bg(bg)
    {
        multiply     = 1;
        offsetsX     = 0;
        offsetsY     = 0;
        QRDEBUG      = false;
        screenwidth  = 0;
        screenheight = 0;
    }

    void screenwhite()  override {}  // no-op: background is pre-filled
    void screenupdate() override {}
};
