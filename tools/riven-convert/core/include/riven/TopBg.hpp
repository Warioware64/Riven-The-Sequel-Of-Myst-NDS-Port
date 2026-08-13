#pragma once

// The port's top-screen background: Autorun/AUTORUN.BMP turned into the file
// the ARM9 draws behind its console (source/render/TopBg.cpp).
//
// THE OUTPUT IS A .rpiz, and that is not a pun on the zoom twins -- it is the
// same file format, used because it already describes exactly this asset:
// "16-byte header + 256-entry RGB555 palette + LZ77 8bpp indices"
// (shared/RivenImage.hpp:63-75). Reusing it means no new schema, no new
// version to keep in step, and ONE reader on the DS side serving both the zoom
// viewer and this.
//
// WHY PALETTED AT ALL, when every other still on the card is ARGB1555: the sub
// engine has one VRAM bank (C, 128 KB) and the console has to keep living in
// it. A 16bpp background is BgSize_B16_256x256 = 128 KB, the whole bank, and
// there is no 256x192 bitmap size to ask for instead. 8bpp is 64 KB and fits
// above the console's tiles and map. See source/render/TopBg.hpp for the
// layout that falls out of that.

#include <cstddef>
#include <filesystem>
#include <string>

namespace riven
{

/// The DS screen. Not taken from RivenData.hpp's kScreenW/kScreenH on purpose:
/// those describe the card VIEW, which is letterboxed to 165 rows, and this
/// picture covers the whole screen.
inline constexpr int kTopBgW = 256;
inline constexpr int kTopBgH = 192;

/// Colours the picture may use. The console's 4bpp font palette is loaded at
/// BG_PALETTE_SUB[240..255] (consoleInitEx's palIndex 15), so 0..239 is what is
/// left -- and index 0 is pinned to black so the letterbox bands, the rows
/// below the picture and the backdrop all read the same.
inline constexpr int kTopBgColours = 240;

struct TopBgResult
{
    bool ok = false;
    std::string error;
    std::size_t bytes = 0;
    int colours = 0; ///< palette entries actually used, <= kTopBgColours
};

/// Read `bmp`, fit it to 256x192 and write `outPath` as a .rpiz.
///
/// The picture is scaled to fit whole (319x214 becomes 256x172) and centred on
/// a black canvas rather than cropped: it is a title card, and cutting the
/// edges off one to fill a screen nothing else is using would be a strange
/// trade.
TopBgResult convertTopBackground(const std::filesystem::path &bmp,
                                 const std::filesystem::path &outPath);

} // namespace riven
