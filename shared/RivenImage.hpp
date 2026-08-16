#pragma once

// On-card image formats, shared between the converter and the ARM9 runtime.
//
// Two files come out of every Riven still (a 608x392, 8bpp, CLUT-paletted tBMP):
//
//   .rpic  the display copy: 256x165 ARGB1555, ready to DMA straight into a DS
//          texture slot with no conversion at all.
//   .rpiz  the zoom twin: the ORIGINAL 608x392 pixels, kept as 8bpp indices plus
//          a 256-entry RGB555 palette.
//
// Why the twin stays paletted rather than becoming ARGB1555 like the display
// copy: the source is already 8bpp, so keeping indices is lossless AND half the
// size. Converting every twin to direct colour would cost 477 KB each, roughly
// 1.6 GB across the game. The zoom viewer decodes one image on demand into a
// 238 KB scratch buffer and windows 256x192 out of it.
//
// Compression is NDS BIOS LZ77 (swiDecompressLZSSWram / decompress(..., LZ77)).
// That choice is deliberate and partly a licensing one: liblzo in BLOCKSDSEXT is
// GPL-2.0, and statically linking it would force this Apache-2.0 project to GPL.
// The BIOS routine costs nothing, links nothing, and carries no licence at all.

#include <cstdint>

namespace rivendata
{

/// One ARGB1555 texel, matching libnds ARGB16(a,r,g,b); bit 15 is alpha.
using Texel = std::uint16_t;

inline constexpr Texel makeTexel(int r, int g, int b, bool opaque = true)
{
    return static_cast<Texel>((opaque ? 0x8000 : 0) | ((b >> 3) << 10) | ((g >> 3) << 5)
                              | (r >> 3));
}

/// How the payload after the header is stored.
enum class ImageCompression : std::uint8_t
{
    None = 0,
    /// NDS BIOS LZ77. Decompress with decompress(src, dst, LZ77) from
    /// <nds/decompress.h>; the 4-byte BIOS header is part of the payload.
    Lz77 = 1,
};

/// 20-byte header of a .rpic display still.
///
/// Pixels follow immediately: width*height ARGB1555, little-endian, row-major,
/// no padding. The DS and the file are both little-endian, so a loaded buffer
/// can be reinterpret_cast to this and the pixels used in place.
///
/// TWO sizes, and they are not the same thing. width/height are the texels that
/// follow -- what the converter resampled to. srcWidth/srcHeight are the source
/// tBMP's own dimensions, which are in Riven's 608x392 card space and are how
/// BIG the picture is on the card. A PLST rectangle says only where its top-left
/// corner goes: the original blits at the image's own size and ignores the
/// rectangle's other two edges (riven_graphics.cpp:367-381), so the runtime has
/// to know that size, and only the converter ever saw it.
struct RpicHeader
{
    char magic[4];        ///< "RPIC"
    std::uint16_t version;
    std::uint16_t width;  ///< 256
    std::uint16_t height; ///< 165
    std::uint8_t compression; ///< ImageCompression
    std::uint8_t reserved;
    std::uint32_t dataBytes;  ///< bytes of pixel payload following the header
    std::uint16_t srcWidth;   ///< the source tBMP's width, in card coordinates
    std::uint16_t srcHeight;  ///< the source tBMP's height, in card coordinates
};
static_assert(sizeof(RpicHeader) == 20, "RpicHeader must be 20 bytes");

/// 16-byte header of a .rpiz zoom twin, followed by a 256-entry RGB555 palette
/// (512 bytes, little-endian) and then `dataBytes` of 8bpp indices.
struct RpizHeader
{
    char magic[4];        ///< "RPIZ"
    std::uint16_t version;
    std::uint16_t width;  ///< 608
    std::uint16_t height; ///< 392
    std::uint8_t compression; ///< ImageCompression
    std::uint8_t reserved;
    std::uint32_t dataBytes;  ///< bytes of index payload following the palette
};
static_assert(sizeof(RpizHeader) == 16, "RpizHeader must be 16 bytes");

/// 2 added RpicHeader::srcWidth/srcHeight. The version is shared with RpizHeader
/// because one converter run writes both, so there is no state where a card holds
/// one generation of .rpic and another of .rpiz.
inline constexpr std::uint16_t kImageVersion = 2;
inline constexpr int kPaletteEntries = 256;
inline constexpr int kPaletteBytes = kPaletteEntries * 2;

inline bool magicIs(const char m[4], char a, char b, char c, char d)
{
    return m[0] == a && m[1] == b && m[2] == c && m[3] == d;
}

inline bool isRpic(const RpicHeader &h)
{
    return magicIs(h.magic, 'R', 'P', 'I', 'C') && h.version == kImageVersion;
}

inline bool isRpiz(const RpizHeader &h)
{
    return magicIs(h.magic, 'R', 'P', 'I', 'Z') && h.version == kImageVersion;
}

} // namespace rivendata
