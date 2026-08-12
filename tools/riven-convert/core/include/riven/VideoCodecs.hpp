#pragma once

// The two QuickTime video codecs Riven ships, and the movie audio codec.
//
// Measured with `riven-convert --movie-report` on a 5-CD install: 1038 of the
// 1055 movies are Cinepak ('cvid'), 17 are QuickTime RLE ('rle '), and every
// audio track is IMA4 at 22050 Hz. Nothing else appears, so nothing else is
// implemented -- an unexpected codec is reported, not guessed at.
//
// Both video decoders are stateful: Cinepak carries its codebooks and QuickTime
// RLE its pixels from frame to frame, so a movie must be decoded forwards from
// a keyframe. Both write RGB888, which is what downscaleToTexels takes.
//
// ScummVM's implementations are the reference for all three formats and are
// cited by file and line below, as docs/licensing.md requires. No code is
// copied from them.

#include <cstdint>
#include <string>
#include <vector>

namespace riven
{

/// Common shape: feed samples in decode order, read the frame out.
class VideoDecoder
{
public:
    virtual ~VideoDecoder() = default;

    /// Decode one sample into the internal surface. False on a malformed
    /// sample, with `error` set; the surface then holds whatever survived.
    virtual bool decode(const std::uint8_t *data, std::size_t size) = 0;

    int width() const { return width_; }
    int height() const { return height_; }
    /// width*height*3, row-major. Valid after the first successful decode.
    const std::vector<std::uint8_t> &rgb() const { return rgb_; }
    const std::string &error() const { return error_; }

protected:
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> rgb_;
    std::string error_;
};

/// Cinepak ('cvid'). image/codecs/cinepak.cpp.
class CinepakDecoder final : public VideoDecoder
{
public:
    CinepakDecoder(int width, int height);
    bool decode(const std::uint8_t *data, std::size_t size) override;

private:
    /// One codebook entry: four luma samples and one chroma pair. The colour
    /// space is Cinepak's own, not BT.601 (cinepak.h:38-42).
    struct Codebook
    {
        std::uint8_t y[4] = {0, 0, 0, 0};
        std::int8_t u = 0;
        std::int8_t v = 0;
    };

    /// Per-strip state. The codebooks persist across frames -- a delta frame
    /// may update only a few entries -- which is why this is a member and not
    /// a local.
    struct Strip
    {
        Codebook v1[256];
        Codebook v4[256];
    };

    std::vector<Strip> strips_;
};

/// QuickTime RLE ('rle '). image/codecs/qtrle.cpp.
///
/// `depth` is the sample description's depth field verbatim, flag bits and
/// all: bit 5 (0x20) marks the greyscale variants, so 40 means 8-bit grey.
class QtRleDecoder final : public VideoDecoder
{
public:
    QtRleDecoder(int width, int height, int depth,
                 const std::vector<std::uint8_t> &paletteRgb);
    bool decode(const std::uint8_t *data, std::size_t size) override;

    /// True for a depth this decoder implements.
    static bool supports(int depth);

private:
    int depth_ = 0;
    std::vector<std::uint8_t> palette_; ///< 256*3, only for the indexed depths
};

/// Decode a QuickTime IMA4 stream to interleaved PCM16.
///
/// 34 bytes per packet per channel, 64 samples each, and stereo alternates
/// WHOLE packets rather than nibbles (audio/decoders/adpcm.cpp:247-306). The
/// preamble packs a 16-bit predictor in the top 9 bits and a step index in the
/// low 7, which must be clamped to 0..88 -- real files carry values past it
/// (adpcm.cpp:264-277).
std::vector<std::int16_t> decodeIma4(const std::uint8_t *data, std::size_t size, int channels);

} // namespace riven
