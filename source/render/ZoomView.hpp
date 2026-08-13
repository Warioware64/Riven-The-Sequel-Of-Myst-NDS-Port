#pragma once

// The zoom viewer: the card you are looking at, at its original resolution.
//
// Riven's stills are 608x392 and the DS screen is 256x192, so the card view
// shows every card downscaled 0.42x -- the whole picture, and 2.375x less of
// it than Cyan drew. Half of what the game asks you to look at is writing on
// a wall, a dial position or a mark on a stone, and at 0.42x a good deal of
// that is a smudge.
//
// The converter has been writing the other copy all along: pics_hi/<stack>/
// <id>.rpiz is the ORIGINAL 608x392 pixels as 8bpp indices plus a palette,
// LZ77-packed, and shared/RivenImage.hpp:15-17 wrote down what to do with it
// before anything could -- "decodes one image on demand into a 238 KB scratch
// buffer and windows 256x192 out of it". This is that.
//
// The window is 1:1 and is the card view's own 256x165, so it covers 42% of the
// card's width and its height, and the D-pad or the stylus moves it. Framing it
// as the card view rather than as the whole screen is not cosmetic -- see the
// note on kWindowH in the .cpp. There is no intermediate zoom level either: the
// hardware could scale the background, but everything between 0.42x and 1.0x is
// an interpolation of pixels that already exist at both ends, and the point of
// the mode is the ones that do not.
//
// THE SCREEN. Taken with BgSurface::beginMovieTakeover, which is the mechanism
// a fullscreen movie already uses and which exists precisely for this: the card
// stays parked, untouched, in the buffer neither layer is showing, so leaving
// the viewer is a rebind and not another 84 KB upload.
//
// COST. The index plane is 238 KB and the file is read on top of that, so the
// buffer is allocated on entry and freed on exit rather than held. It cannot
// coexist with a fullscreen movie's two 258 KB planes, and the engine's mode
// gate already refuses to open it during one.

#include <cstdint>
#include <string>
#include <vector>

#include "RivenImage.hpp"

namespace rivenrt
{

class ZoomView
{
public:
    /// Decode `path` and take the screen. False (having printed why) if the
    /// file is not on the card -- a conversion run with --no-hires has no
    /// pics_hi/ at all -- or if there is no memory for the plane.
    bool open(const std::string &path);

    /// Give the screen back and free the plane.
    void close();

    bool active() const { return !indices_.empty(); }

    /// Move the window by whole pixels, clamped to the picture.
    void pan(int dx, int dy);

    /// Draw the window into the back buffer if anything moved. Called from the
    /// engine's frame tail, in the same vblank window as every other upload.
    void publish();

private:
    void redraw(int buffer);

    std::vector<std::uint8_t> indices_;
    rivendata::Texel palette_[rivendata::kPaletteEntries] = {};
    int width_ = 0;
    int height_ = 0;
    int originX_ = 0;
    int originY_ = 0;
    /// Redraws still owed, counted rather than tracked per buffer.
    ///
    /// Two, because the display ping-pongs between two buffers and a single
    /// redraw would show the old window every other frame. Counted rather than
    /// held as a per-buffer bitmask because the THIRD buffer is the parked card
    /// and must never be written -- a mask would keep a bit set for a buffer
    /// this is not allowed to touch, and so would never reach zero.
    int redrawsLeft_ = 0;
};

extern ZoomView zoomView;

} // namespace rivenrt
