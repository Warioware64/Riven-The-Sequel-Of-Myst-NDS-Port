#pragma once

// Proportional text on the bottom screen, for the port's own screens.
//
// The Myst port drew its menu with NEA_RichTextRender3D, and that route is shut
// here: this port moved the card view onto bitmap backgrounds and switched the
// 3D engine off, so a rich-text quad would be drawn to a layer that is not
// displayed (main.cpp:14-18). What is left is NEA's Hw2D text path, which draws
// glyphs straight into a bitmap -- and for a BITMAP_16 destination it wants an
// A1RGB5 font, which is what resources/font/DejaVuSansMenu_0.ptxc builds.
//
// The destination is a BgSurface buffer, IN VRAM, with no RAM canvas in
// between. libdsf's buffer renderer is documented VRAM-safe
// (DSF_StringRenderToExistingBuffer), and the byte-store problem that forces
// the Myst port's top-screen text through a RAM strip does not arise here: a
// 16-bit pixel is a halfword store, which VRAM keeps.
//
// TEXT IS DRAWN OVER OPAQUE BLACK. A glyph is blitted as a rectangle, so the
// transparent texels inside its box are written too and anything painted
// underneath is punched out around every letter -- which is why nothing is
// painted underneath: the screen is cleared to flat black and the glyphs are the
// only thing on it. White on black is what the port's letterbox bands and its
// console already look like.
//
// OPAQUE and not transparent, which is a correction rather than a preference.
// BgSurface displays BOTH of its layers at once and "flips" by swapping their
// priority (BgSurface.cpp:applyPriorities) -- it never hides one. A card picture
// is opaque, so the layer underneath is genuinely covered; a transparent text
// buffer is not. Clearing to transparent left the two menu buffers compositing
// with each other, so every selection move showed the mark twice: once where the
// buffer being drawn had just put it and once where the other buffer still had
// it. Black fills the gaps that used to let the older pass through.

#include <string>

#include "RivenImage.hpp"
#include "global_header.hpp"

namespace rivenrt
{

class TextLayer
{
public:
    /// Load the font from NitroFS. False if the ROM filesystem is not there or
    /// the font is missing, in which case nothing else here does anything.
    bool create();
    void destroy();
    bool ready() const { return ctx_ != nullptr; }

    /// Height of one line of the font, in pixels -- the lineHeight in
    /// DejaVuSans-Bold.fnt's common block.
    static constexpr int kLineHeight = 14;

    /// Point the layer at one of BgSurface's buffers. Every draw after this
    /// lands there, so the caller writes the buffer that is not on screen and
    /// flips, the same rule the rest of the renderer follows.
    ///
    /// Cheap: it re-aims the renderer, it does not reload the font.
    bool target(int buffer);

    /// Fill the whole target buffer with opaque black. This is the layer's
    /// "clear the screen", and opacity is the part that matters -- see above.
    ///
    /// It writes past the card view, so a screen that used it owes the buffer
    /// back through BgSurface::resetBuffer on the way out.
    void clear();

    /// Draw `text` with its left edge at `x` and its top at `y`, in SCREEN
    /// pixels -- which is why the caller must have turned BgSurface's letterbox
    /// off (BgSurface::setLetterbox).
    void draw(int x, int y, const std::string &text);

private:
    NEA_Hw2DTextCtx *ctx_ = nullptr;
    void *fontTexture_ = nullptr;
    int buffer_ = -1;
};

extern TextLayer textLayer;

} // namespace rivenrt
