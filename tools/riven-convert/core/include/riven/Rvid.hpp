#pragma once

// The RVID codec: encoder, and the reference decoder the tests check it with.
//
// The format is specified in docs/video.md and its structures live in
// shared/RivenVideo.hpp, along with the transform, the quantiser and the
// prediction rules -- everything both ends must agree on bit for bit. This
// header is only the machinery around them.
//
// The decoder exists for the same reason decompressLz77 does: the real decoder
// is on the other side of a DS, and an encoder that nothing can verify is an
// encoder nobody should trust. It is also what makes the encoder correct at
// all -- a P-frame predicts from the DECODED picture, so the encoder runs the
// decoder's reconstruction path on every block and keeps its output.

#include <cstdint>
#include <string>
#include <vector>

#include "RivenImage.hpp" // Texel: the codec's input and output are ARGB1555
#include "RivenVideo.hpp"

namespace riven
{

/// One frame as the codec sees it: three 5-bit planes, dimensions already
/// padded up to a multiple of 8.
struct RvidFrame
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> plane[rivendata::kPlaneCount];

    void resize(int w, int h)
    {
        width = w;
        height = h;
        for (auto &p : plane)
            p.assign(static_cast<std::size_t>(w) * h, 0);
    }
    bool empty() const { return width <= 0 || height <= 0; }
};

struct RvidSettings
{
    rivendata::VideoProfile profile = rivendata::VideoProfile::Lite;

    /// Percentage applied to the quantiser: 100 is the default, higher is
    /// finer and larger, lower is coarser and smaller.
    int quality = 100;

    /// Longest run of P-frames before an I-frame is forced. Also the seek
    /// granularity, since only I-frames start a decode.
    int gopLength = 60;

    /// Re-encode a P-frame as an I-frame when more than this fraction of its
    /// blocks chose intra anyway -- the cheap scene-change detector.
    double intraRatio = 0.6;
};

/// What one encoded frame came out as.
struct RvidEncodedFrame
{
    std::vector<std::uint8_t> bytes;
    bool intra = false;
    int intraBlocks = 0;
};

class RvidEncoder
{
public:
    RvidEncoder(int width, int height, const RvidSettings &settings);

    /// Encode `src` (which must match the encoder's dimensions). The first
    /// frame is always intra whatever `forceIntra` says.
    RvidEncodedFrame encode(const RvidFrame &src, bool forceIntra);

    /// The reconstruction the decoder will hold after the last encoded frame.
    const RvidFrame &reference() const { return ref_; }

private:
    struct Vector
    {
        int x = 0; ///< half-pel
        int y = 0;
    };

    RvidEncodedFrame encodeIntra(const RvidFrame &src);
    RvidEncodedFrame encodeInter(const RvidFrame &src);

    Vector predictVector(int bx, int by) const;
    Vector searchVector(const RvidFrame &src, int bx, int by, const Vector &predictor) const;
    long blockSad(const RvidFrame &src, int bx, int by, const Vector &v) const;
    bool vectorLegal(int bx, int by, const Vector &v) const;

    int width_ = 0;
    int height_ = 0;
    int blocksX_ = 0;
    int blocksY_ = 0;
    RvidSettings settings_;
    int step_[4] = {0, 0, 0, 0};

    RvidFrame ref_;      ///< the decoder's reconstruction of the last frame
    RvidFrame working_;  ///< reconstruction being built for this frame
    bool haveRef_ = false;
    std::vector<Vector> vectors_; ///< one per block, this frame
};

class RvidDecoder
{
public:
    RvidDecoder(int width, int height, const RvidSettings &settings);

    /// Decode one frame payload. False on a malformed frame, with `error` set.
    bool decode(const std::uint8_t *data, std::size_t size, bool intra);

    const RvidFrame &frame() const { return frame_; }
    const std::string &error() const { return error_; }

private:
    int width_ = 0;
    int height_ = 0;
    int blocksX_ = 0;
    int blocksY_ = 0;
    rivendata::VideoProfile profile_ = rivendata::VideoProfile::Lite;
    int step_[4] = {0, 0, 0, 0};

    RvidFrame frame_;
    RvidFrame prev_;
    std::string error_;
};

/// Fill `step` from the quality percentage. Shared so the two ends cannot
/// disagree about what a quality setting means.
void rvidQuantSteps(int quality, int step[4]);

/// Turn a downscaled ARGB1555 image into codec planes, padding the dimensions
/// up to a multiple of 8 by repeating the edge pixels -- padding with black
/// would put a hard edge inside the last block and cost bits to code.
RvidFrame texelsToFrame(const std::vector<rivendata::Texel> &texels, int w, int h);

/// The inverse, cropping back to `w` x `h`.
std::vector<rivendata::Texel> frameToTexels(const RvidFrame &f, int w, int h);

} // namespace riven
