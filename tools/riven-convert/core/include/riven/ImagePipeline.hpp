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
///
/// This is the conversion's inner loop -- every movie frame in the game goes
/// through it -- so the out-parameter form exists to be called in a loop with one
/// reused buffer. The returning form is for callers that convert one image.
void downscaleToTexels(const std::uint8_t *rgb, int srcW, int srcH, int dstW, int dstH,
                       std::vector<rivendata::Texel> &out);

std::vector<rivendata::Texel> downscaleToTexels(const std::uint8_t *rgb, int srcW, int srcH,
                                                int dstW, int dstH);

/// Where a movie overlay sits on the card, for resampling it on the CARD's pixel
/// grid rather than its own.
///
/// THE THIRD THING BETWEEN A tMOV AND THE SCREEN, after the track matrix and the
/// span (docs/video.md). Those two fixed the movie's SIZE; this fixes its
/// PHASE. `scale=w:h` resamples on the movie's own grid -- period srcW/dstW,
/// anchored at the movie's pixel 0 -- while the still underneath went through
/// the card's: period 608/256, anchored at card x=0. The two coincide only when
/// left/2.375 is an integer AND the movie's own ratio happens to be 2.375, and
/// across a 5-CD install 716 of the 874 overlays failed that by half a DS pixel
/// or more. jspit's gallows carriage (tMOV 116, 152x336 at 224,56) is 0.32 px
/// out in x and 0.58 in y with a 0.37% vertical stretch, which reads as a seam
/// along the top of the animation.
///
/// The mapping is exact in integers. Destination column `dx` of a movie of card
/// width `W` at card left `L`, drawn at `viewX = toDsX(L)`, covers source pixels
///
///     sx0 = ((viewX + dx)     * kCardW - kViewW*L) * srcW / (kViewW * W)
///     sx1 = ((viewX + dx + 1) * kCardW - kViewW*L) * srcW / (kViewW * W)
///
/// clamped to [0, srcW] -- and THE CLAMP IS THE EDGE REPLICATION the span's
/// outward rounding needs: at dx = 0 the first destination column starts before
/// the movie does, so sx0 goes negative and clamps to the edge.
///
/// THE TWO AXES ARE NOT THE SAME RATIO, which is the one part of this that had
/// to be measured rather than reasoned about. Across, the card view is 608 to
/// 256 -- exactly 19/8. Down, it is 392 to 165, which is 2.37576 and not 2.375,
/// because 165 is where 392 * 256/608 got rounded. A full-card still is
/// resampled 608x392 -> 256x165, so 392/165 IS the card's vertical grid whatever
/// toDsY says about positions, and an overlay sampled at 19/8 would sit off it.
///
/// A FULL movie degenerates to the plain box filter exactly -- L and viewX are
/// zero and W is the whole card -- and there is a test that says so, because if
/// it ever stops being true every fullscreen movie in the game has moved.
struct CardPlacement
{
    int left = 0;   ///< the MLST position, in Riven's 608x392 card space
    int top = 0;
    int cardW = 0;  ///< the movie's size in that space, after the track matrix
    int cardH = 0;
};

/// The same filter and the same dither, sampled on the card's grid.
///
/// Identical to downscaleToTexels in every other respect -- linear-light
/// averaging through the gamma LUT, the 4x4 Bayer threshold -- because a movie
/// and the still it sits on have to be quantised the same way OR THE OVERLAY
/// SHOWS A SEAM, which is the argument VideoPipeline already makes for not
/// letting ffmpeg do the dither. This extends it one step: they have to be
/// RESAMPLED the same way too.
void downscaleOnCardGrid(const std::uint8_t *rgb, int srcW, int srcH, int dstW, int dstH,
                         const CardPlacement &place, std::vector<rivendata::Texel> &out);

/// Encode a finished .rpic file (header + pixels).
///
/// `w`/`h` are the texels being written; `srcW`/`srcH` are how big the picture is
/// in Riven's 608x392 card space, which is what the runtime needs to place it and
/// is not recoverable from the resampled size (RivenImage.hpp).
std::vector<std::uint8_t> encodeRpic(const std::vector<rivendata::Texel> &texels, int w, int h,
                                     int srcW, int srcH);

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

/// One tBMP's pixels, copied out of libvaht.
///
/// The copy is what lets the image stage use more than one core. libvaht is not
/// thread-safe and its Bitmap decodes lazily behind its accessors, so a worker
/// cannot be handed one -- but plain buffers it can. A 608x392 tBMP is 715 KB of
/// RGB and 238 KB of indices, which is nothing against the resampling that
/// follows.
struct BitmapPixels
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb;     ///< width*height*3; empty if unavailable
    std::vector<std::uint8_t> indices; ///< width*height; empty when truecolour
    std::vector<std::uint8_t> palette; ///< 256*3; empty when truecolour

    bool valid() const { return width > 0 && height > 0; }
};

/// Decode tBMP `id` from `set` and copy its pixels out. Call on the thread that
/// owns the ArchiveSet.
BitmapPixels readBitmapPixels(const ArchiveSet &set, std::uint16_t id, std::string &error);

/// Convert already-read pixels and write whichever outputs are requested. Touches
/// nothing shared, so this is the half that runs on a worker.
ImageResult convertBitmapPixels(const BitmapPixels &px, std::uint16_t id,
                                const std::filesystem::path &rpicPath, bool wantRpic,
                                const std::filesystem::path &rpizPath, bool wantRpiz);

/// Both halves together, for callers converting one image.
ImageResult convertBitmap(const ArchiveSet &set, std::uint16_t id,
                          const std::filesystem::path &rpicPath, bool wantRpic,
                          const std::filesystem::path &rpizPath, bool wantRpiz);

} // namespace riven
