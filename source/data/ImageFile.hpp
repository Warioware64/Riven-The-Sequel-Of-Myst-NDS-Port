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
// The zoom twin (.rpiz, 608x392 8bpp + palette) is milestone 9 and is not read
// here.

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

} // namespace rivenrt
