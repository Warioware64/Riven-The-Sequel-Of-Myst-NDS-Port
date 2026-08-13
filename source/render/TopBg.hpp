#pragma once

// The top screen's picture: Riven's own autorun splash, behind the log.
//
// The top screen was a black background with a printf console on it. Every
// diagnostic the engine produces still goes there and still reads the same way;
// this only puts something behind them.
//
// WHERE IT COMES FROM. Not the ROM. AUTORUN.BMP is Cyan's artwork, and
// docs/licensing.md is explicit that the .nds ships no game data, so the
// converter reads it out of the player's own copy and writes ui/topbg.rpiz --
// see tools/riven-convert/core/include/riven/TopBg.hpp. An install without it
// (the file belongs to the disc's autorun shell, so an installed or GOG copy
// may not have one) simply gets the black screen it had before.
//
// VRAM. The sub engine has exactly one bank -- C, 128 KB -- because VRAM_H and
// VRAM_I map inside C's window and this port gives neither to it. Everything
// below has to fit in that one bank alongside the console:
//
//     console tiles     char base 0     0 .. ~3 KB   (96 glyphs, 4bpp)
//     console map       map base 31    62 .. 64 KB
//     this picture      bitmap base 4  64 .. 128 KB  (256x256 8bpp)
//
// 8bpp is not a quality choice, it is the only one that fits. A 16-bit bitmap
// background is BgSize_B16_256x256 = 128 KB, the whole bank, and the hardware
// offers no 256x192 bitmap size to ask for instead; nor can the console's map
// move out of the way, because a screen base is a 5-bit field and cannot
// address past 62 KB.
//
// PALETTE. Shared with the console, and the split is load-bearing. libnds's
// default console font carries no palette of its own, so consoleInit fills the
// LAST entry of each of the 16 banks (BG_PALETTE_SUB[15], [31], ... [255]) with
// the 16 ANSI colours and prints through bank 15 -- that is, white text is
// entry 255. The converter therefore quantises the picture to 240 colours
// (riven::kTopBgColours) and this only ever writes BG_PALETTE_SUB[0..239], so
// the text keeps its white whatever the picture is.
//
// The one thing that would break: consoleSetColor(). Any colour other than the
// default resolves to entry (bank * 16 + 15), which below 240 is a picture
// colour rather than the ANSI one. Nothing in this port calls it, and if
// anything ever needs coloured diagnostics the answer is to drop the picture to
// 224 colours and give the console two banks, not to move the picture.

#include <string>

#include "global_header.hpp"

namespace rivenrt
{

class TopBg
{
public:
    /// Bring the layer up and load `ui/topbg.rpiz` onto it.
    ///
    /// Never fatal. A card with no ui/ (converted before this existed, or from
    /// a copy of the game with no autorun splash) leaves the top screen exactly
    /// as it was, and says so on the console once.
    ///
    /// Must run AFTER Global::Init, which is where the console claims its half
    /// of the palette, and after FAT is up.
    void load();

    bool loaded() const { return bgId_ >= 0; }

    /// Hide or show the picture without unloading it. The zoom viewer and any
    /// other full-screen top-screen mode can take the screen and give it back.
    void setVisible(bool visible);

private:
    int bgId_ = -1;
};

extern TopBg topBg;

} // namespace rivenrt
