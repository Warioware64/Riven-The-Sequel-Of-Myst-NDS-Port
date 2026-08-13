#pragma once

// Loading pics/<stack>/<id>.rpic -- a card picture, already ARGB1555.
//
// The converter did the resampling and the colour conversion, so there is
// nothing to decode here beyond the LZ77 the payload may be packed with. That
// choice is a licensing one as much as a size one: the BIOS routine
// (swiDecompressLZSSWram, via libnds decompress()) links nothing and carries no
// licence, where liblzo in BLOCKSDSEXT is GPL-2.0 and would take this
// Apache-2.0 project with it. See docs/licensing.md.
//
// A .rpic is NOT always the size of the card view. The converter scales a
// picture to min(kViewW, sourceWidth) and keeps its aspect
// (ImagePipeline.cpp:247-255), so a full-card still comes out 256x165 while a
// small overlay tBMP keeps its own pixels. Where a picture goes and how big it
// is on the card is the PLST record's business, not the file's, which is why the
// caller passes a destination rectangle rather than assuming one.
//
// The zoom twin (.rpiz, 8bpp indices + a 256-entry RGB555 palette) is read here
// too, by loadRpizImage. TWO things want it and they are not both zooms: the
// zoom viewer opens the 608x392 twin of the card it is looking at, and the top
// screen's background (ui/topbg.rpiz) is a 256x192 picture in the same format
// for the reason riven/TopBg.hpp gives -- the sub engine has one VRAM bank and
// a 16bpp background would be all of it.

#include <cstdint>
#include <string>
#include <vector>

#include "RivenImage.hpp"

namespace rivenrt
{

/// Largest .rpic this will load. The converter never emits one wider than the
/// card view (ImagePipeline.cpp:250: min(kViewW, sourceWidth)) and never taller
/// than its source, so these are not a policy -- they are what the writer can
/// produce, and they are what stops a corrupt header asking for a huge
/// allocation. Spelled as numbers because this module has no business with the
/// card geometry; CardSurface.cpp static_asserts them against the real
/// constants.
inline constexpr int kMaxRpicW = 256;
inline constexpr int kMaxRpicH = 1024;

/// A .rpic as it is on the card.
struct RpicImage
{
    int width = 0;
    int height = 0;
    std::vector<rivendata::Texel> texels;

    bool valid() const
    {
        return width > 0 && height > 0
            && texels.size() >= static_cast<std::size_t>(width) * height;
    }
};

/// Read `path` at its own size. False (with `error` set) on a missing file, a
/// bad magic, a version mismatch, or a payload that does not decompress to the
/// size the header promised.
bool loadRpicImage(const std::string &path, RpicImage &out, std::string &error);

/// Largest .rpiz this will load: a full Riven still, which is the biggest thing
/// the converter emits in this format. Spelled as numbers for the same reason
/// kMaxRpicW is; ZoomView.cpp static_asserts them against the card geometry.
inline constexpr int kMaxRpizW = 608;
inline constexpr int kMaxRpizH = 392;

/// A .rpiz: 8bpp indices plus the palette to read them through.
///
/// The palette arrives as Texels rather than as the raw RGB555 halfwords the
/// file holds, because every consumer wants it that way -- the zoom viewer
/// writes ARGB1555 into a 16bpp background and the top screen copies it into
/// BG_PALETTE_SUB, where the alpha bit is ignored. The file already sets bit 15
/// on every entry (encodeRpiz goes through makeTexel), so this is a copy.
struct RpizImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> indices;
    rivendata::Texel palette[rivendata::kPaletteEntries] = {};

    bool valid() const
    {
        return width > 0 && height > 0
            && indices.size() >= static_cast<std::size_t>(width) * height;
    }
};

/// Read a .rpiz. Same failure contract as loadRpicImage.
///
/// Deliberately not folded into loadRpicImage: that function's kMaxRpicW cap is
/// 256 on purpose -- it is what the converter can produce for a display copy,
/// and it is what stops a corrupt header from asking for a huge allocation.
/// Widening it so one function could serve both would delete that check.
bool loadRpizImage(const std::string &path, RpizImage &out, std::string &error);

} // namespace rivenrt
