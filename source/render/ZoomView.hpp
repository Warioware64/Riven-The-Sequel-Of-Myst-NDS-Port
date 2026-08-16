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
// The window is 1:1 and is the WHOLE SCREEN, 256x192 -- the letterbox goes off
// and the 27 rows the card view spends on black bands become picture, which is
// 16% more of it. It was the card view's 256x165 for a while and the note on
// kWindowH in the .cpp records why that changed and what it cost. There is no
// intermediate zoom level either: the hardware could scale the background, but
// everything between 0.42x and 1.0x is an interpolation of pixels that already
// exist at both ends, and the point of the mode is the ones that do not.
//
// That 192 is load-bearing outside this file exactly once: Engine::captureNote
// reads the front buffer to make a notebook page, and has to take all 192 rows
// here where it takes 165 from a letterboxed card, or the page silently loses
// the bottom of what the player was looking at.
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
//
// THE PATCHES. A card is a still plus whatever its scripts drew on top of it --
// a slider's position, five D'ni numerals, a marble on a square -- and the base
// picture above is only the still. Those overlays are stamped over it as small
// ARGB1555 patches in the same 608x392 space, out of the FULL-resolution twin of
// the overlay's own tBMP (the converter writes one for every tBMP, not just the
// full-card stills), and they are re-blitted after the base window on every
// redraw. Engine owns the list of what to stamp -- it is the only thing that
// sees the draws -- and stamps again as they happen, which is what lets a dome's
// slider move under the stylus while the viewer is up.
//
// A patch is small: a dome's twenty-five slots are 12 KB and a journal page's
// combination 8 KB, against the 238 KB base. They are kept as texels rather than
// as indices into the base's palette because they are not from the base's
// picture and its 256 colours are not theirs -- matching them into it would be a
// nearest-colour search per pixel, and would show.

#include <cstdint>
#include <string>
#include <vector>

#include "RivenData.hpp" // Rect: a patch is a rectangle of the card
#include "RivenImage.hpp"
#include "data/ImageFile.hpp"

namespace rivenrt
{

class ZoomView
{
public:
    /// Decode `path` and take the screen. False (having printed why) if the
    /// file is not on the card -- a conversion run with --no-hires has no
    /// pics_hi/ at all -- or if there is no memory for the plane.
    bool open(const std::string &path);

    /// A new base picture under the same window: the card drew a new full-card
    /// still while the viewer was up, which is what turning a journal's page is.
    /// Keeps the pan position and drops the patches, because they belonged to
    /// the picture that has just been replaced.
    ///
    /// False, with the old picture untouched and still on screen, if the new one
    /// cannot be read: a page that will not load is not a reason to throw away
    /// the one the player is looking at.
    bool reopen(const std::string &path);

    /// Give the screen back and free the plane.
    void close();

    bool active() const { return !indices_.empty(); }

    /// Move the window by whole pixels, clamped to the picture.
    void pan(int dx, int dy);

    /// The window's top-left corner in the picture, which -- because the picture
    /// IS the card at its original size -- is the offset between a screen pixel
    /// and a card coordinate. Engine::pointerCardX is the reader: it is what
    /// makes the pointer mean the same thing in here as it does on the card.
    int originX() const { return originX_; }
    int originY() const { return originY_; }

    /// Draw `src` of the .rpiz at `rpizPath` onto the picture at `dstCard`, both
    /// in the source tBMP's own pixels and in card coordinates respectively.
    ///
    /// A stamp whose destination repeats one already held replaces it in place,
    /// which is the whole of what a drag costs: the dome redraws the same
    /// twenty-five rectangles on every slot the slider crosses.
    bool stamp(const std::string &rpizPath, const rivendata::Rect &src,
               const rivendata::Rect &dstCard);

    /// The same, for art that has no full-resolution twin -- today only the
    /// marble strip, which the converter writes from extras.MHK at DS size.
    /// Blocky, being a 2.375x nearest upscale of the card view's own pixels, but
    /// by construction exactly what the player is looking at.
    void stampFromCard(const rivendata::Texel *cardPlane, const rivendata::Rect &dstCard);

    void clearPatches();

    /// Draw the window into the back buffer if anything moved. Called from the
    /// engine's frame tail, in the same vblank window as every other upload.
    void publish();

private:
    /// Decode `path` into the base plane. `keepOrigin` is reopen()'s: hold the
    /// window where it is (clamped to the new picture) rather than re-centring.
    /// Nothing is committed until the file has decoded.
    bool load(const std::string &path, bool keepOrigin);

    void redraw(int buffer);

    /// A decoded overlay source, kept because a drag asks for the same two files
    /// on every tick and each ask is an SD read plus an LZ77 inflate.
    const RpizImage *source(const std::string &path);

    /// Whether a patch this size can be held, asked BEFORE anything is read or
    /// allocated for it -- what it costs depends on the destination alone, and a
    /// whole-card overlay must be refused rather than attempted.
    bool affordable(const rivendata::Rect &dst) const;

    /// Take the finished pixels: replacing the patch with the same destination
    /// if there is one, appending otherwise.
    void commit(const rivendata::Rect &dst, std::vector<rivendata::Texel> &&texels);

    /// One overlay, in the picture's own 608x392 space.
    struct Patch
    {
        rivendata::Rect dst;
        std::vector<rivendata::Texel> texels;
    };

    struct Source
    {
        std::string path;
        RpizImage img;
        std::uint32_t used = 0; ///< a clock, for the least-recently-used eviction
    };

    std::vector<std::uint8_t> indices_;
    rivendata::Texel palette_[rivendata::kPaletteEntries] = {};
    int width_ = 0;
    int height_ = 0;
    int originX_ = 0;
    int originY_ = 0;
    std::vector<Patch> patches_;
    std::size_t patchBytes_ = 0;
    std::vector<Source> sources_;
    std::size_t sourceBytes_ = 0;
    std::uint32_t sourceClock_ = 0;
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
