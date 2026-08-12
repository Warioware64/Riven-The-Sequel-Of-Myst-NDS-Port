#pragma once

// Turning one Riven tBMP into the two files the DS reads.
//
// Both outputs come from a single libvaht decode: the truecolour view feeds the
// downscale, the indexed view and its palette feed the zoom twin.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenImage.hpp"
#include "riven/Archive.hpp"

namespace riven
{

/// Compress with the NDS BIOS LZ77 format (swiDecompressLZSSWram / decompress
/// with LZ77). Returns the complete payload including the 4-byte BIOS header.
///
/// Written here rather than pulled in as a dependency because the BIOS routine
/// on the other end is free, and the two candidates that would have saved the
/// work are both wrong for this project: liblzo is GPL-2.0 (it would relicense
/// the ROM) and zlib is not shipped for the ARM9 by BlocksDS.
std::vector<std::uint8_t> compressLz77(const std::uint8_t *data, std::size_t size);

/// Decompress an NDS LZ77 payload. Only used by tests -- the DS uses the BIOS --
/// but a compressor nothing can verify is a compressor nobody should trust.
std::vector<std::uint8_t> decompressLz77(const std::uint8_t *data, std::size_t size);

/// Box-filter downscale of an RGB888 image, then ARGB1555 with gamma-correct
/// ordered dithering.
///
/// Area averaging is the correct filter for a 2.375x reduction (608 -> 256) and
/// costs a dozen lines. The dither matters more than it sounds: Riven is full of
/// slow gradients -- skies, water, lamplight on stone -- and truncating 8 bits
/// to 5 bands them visibly. A 4x4 Bayer threshold applied in LINEAR light (not
/// on the sRGB values) breaks the bands up without the speckle that naive
/// dithering in gamma space produces in dark areas.
std::vector<rivendata::Texel> downscaleToTexels(const std::uint8_t *rgb, int srcW, int srcH,
                                                int dstW, int dstH);

/// Encode a finished .rpic file (header + pixels).
std::vector<std::uint8_t> encodeRpic(const std::vector<rivendata::Texel> &texels, int w, int h);

/// Encode a finished .rpiz file (header + RGB555 palette + 8bpp indices).
/// `compress` picks LZ77; the result falls back to uncompressed when
/// compression does not actually help, which happens on tiny images.
std::vector<std::uint8_t> encodeRpiz(const std::uint8_t *indices, const std::uint8_t *paletteRgb,
                                     int w, int h, bool compress);

/// What convertBitmap produced, for progress reporting and size accounting.
struct ImageResult
{
    bool ok = false;
    std::string error;
    std::size_t rpicBytes = 0;
    std::size_t rpizBytes = 0;
    int width = 0;
    int height = 0;
};

/// Decode tBMP `id` from `set` and write whichever outputs are requested.
/// Paths are the final ones; writes are atomic.
ImageResult convertBitmap(const ArchiveSet &set, std::uint16_t id,
                          const std::filesystem::path &rpicPath, bool wantRpic,
                          const std::filesystem::path &rpizPath, bool wantRpiz);

} // namespace riven
