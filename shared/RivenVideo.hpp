#pragma once

// The RVID container, shared between the converter and the ARM9 runtime.
//
// The codec itself is milestone 8; this header is the part that had to be
// settled first, because the encoder and the player have to agree on it before
// either can be written. docs/video.md is the specification and explains every
// number here -- read it before changing anything in this file.
//
// The short version. Riven ships 1055 movies, 1.93 GB, 10832 seconds at 15 fps
// (measured with `riven-convert --movie-report`; 1038 are Cinepak, 17 are
// QuickTime RLE, and 795 carry an IMA4 audio track at 22050 Hz). They fall in
// two groups that want different decoders:
//
//   FULL -- 181 movies at the full 608x392 card size. Fullscreen cutscenes.
//           Motion compensation runs on the 3D engine and the result is taken
//           by the display-capture unit.
//   LITE -- the other 874, composited onto a card that is already on screen:
//           locked-off shots of animated elements, from 40x40 up. Decoded by
//           the ARM9 into RAM with zero-motion-vector P-frames, because
//           display-capture MC cannot composite onto a card the 3D engine is
//           already drawing.
//
// Everything is little-endian, like the rest of the on-card formats.

#include <cstdint>

namespace rivendata
{

/// Which decoder a file expects.
enum class VideoProfile : std::uint8_t
{
    Full = 0, ///< fullscreen, 3D-engine motion compensation
    Lite = 1, ///< card overlay, ARM9, zero-motion-vector P-frames
};

/// `flags` bits in RvidHeader.
enum : std::uint8_t
{
    /// An audio block is interleaved into every frame.
    kVideoHasAudio = 1 << 0,
};

/// 32-byte header of a .rvid file.
///
/// Followed by `keyframeCount` RvidKeyframe, then the frames back to back.
struct RvidHeader
{
    char magic[4];             ///< "RVID"
    std::uint16_t version;     ///< kVideoVersion
    std::uint8_t profile;      ///< VideoProfile
    std::uint8_t flags;        ///< kVideoHasAudio
    std::uint16_t width;       ///< pixels, already scaled for the DS
    std::uint16_t height;
    std::uint16_t fpsNum;      ///< 15/1 on every Riven movie measured so far,
    std::uint16_t fpsDen;      ///< but stored rather than assumed
    std::uint32_t frameCount;
    std::uint32_t keyframeCount;
    std::uint16_t audioRate;   ///< Hz, 0 when there is no audio
    std::uint16_t audioSamplesPerFrame;
    std::uint32_t largestFrameBytes; ///< so a player can size one buffer up front
};
static_assert(sizeof(RvidHeader) == 32, "RvidHeader must be 32 bytes");

/// Where an I-frame starts, so playback can seek and so a dropped frame can be
/// recovered from. Frames between two entries are P-frames and can only be
/// decoded in order.
struct RvidKeyframe
{
    std::uint32_t frame;
    std::uint32_t offset; ///< bytes from the start of the file
};
static_assert(sizeof(RvidKeyframe) == 8, "RvidKeyframe must be 8 bytes");

/// 8-byte header on every frame. The audio block, when present, follows it and
/// precedes the video payload -- the ARM7 needs the samples before the ARM9
/// needs the picture, and putting them in that order means one read serves
/// both.
struct RvidFrameHeader
{
    std::uint32_t byteCount;  ///< audio block + video payload, excluding this header
    std::uint16_t audioBytes; ///< 0 when the file has no audio
    std::uint8_t type;        ///< RvidFrameType
    std::uint8_t reserved;
};
static_assert(sizeof(RvidFrameHeader) == 8, "RvidFrameHeader must be 8 bytes");

enum class RvidFrameType : std::uint8_t
{
    Intra = 0,     ///< self-contained
    Predicted = 1, ///< depends on the frame before it
};

inline constexpr std::uint16_t kVideoVersion = 1;

/// Luma/chroma-free: RVID works on RGB555 planes, so a block is 8x8 texels in
/// all three planes and motion compensation moves one triangle per block.
inline constexpr int kVideoBlock = 8;

/// Planes are stored as 5-bit samples, 0..31, one byte each. The three planes
/// are always in this order, and R and B predict from G, so G must be decoded
/// first.
enum : int
{
    kPlaneG = 0,
    kPlaneR = 1,
    kPlaneB = 2,
    kPlaneCount = 3,
};

// ---------------------------------------------------------------------------
// The coding tools. Encoder and decoder MUST agree bit for bit -- a P-frame
// predicts from the decoder's reconstruction, so any disagreement compounds
// frame by frame instead of showing up once. They live here, in the shared
// header, for the same reason the IMA tables do: one definition, both sides.
// ---------------------------------------------------------------------------

/// An 8x8 block is four 4x4 quadrants, and each quadrant is four 2x2 groups
/// that sample every OTHER pixel -- a polyphase decomposition, not four
/// adjacent 2x2 squares. Group `i` (0..15, ordered y3, x3, y2, x2) covers
/// (x, y), (x+2, y), (x, y+2), (x+2, y+2) for the origin below.
inline constexpr void subBlockOrigin(int i, int &x, int &y)
{
    const int x2 = i & 1;
    const int y2 = (i >> 1) & 1;
    const int x3 = ((i >> 2) & 1) * 4;
    const int y3 = ((i >> 3) & 1) * 4;
    x = x3 + x2;
    y = y3 + y2;
}

/// Where the four pixels of group `i` sit inside the 8x8 block.
inline constexpr void subBlockPixel(int i, int k, int &x, int &y)
{
    subBlockOrigin(i, x, y);
    x += (k & 1) * 2;
    y += ((k >> 1) & 1) * 2;
}

/// Offset from a pixel to the already-reconstructed pixel that predicts it,
/// within the same 8x8 block. Group 0 has no predictor inside the block and
/// returns false: the G plane starts from zero and R and B start from the
/// reconstructed G pixel underneath, which is what makes a block decodable
/// with no reference to its neighbours.
inline constexpr bool intraPredictorOffset(int i, int &dx, int &dy)
{
    const int x2 = i & 1;
    const int y2 = (i >> 1) & 1;
    const int x3 = (i >> 2) & 1;
    const int y3 = (i >> 3) & 1;

    dx = 0;
    dy = 0;
    if (x2 != 0)
    {
        dx = -1; // the group one column left, already reconstructed
        return true;
    }
    if (y2 != 0)
    {
        dy = -1;
        return true;
    }
    if (x3 != 0)
    {
        dx = -4; // the quadrant to the left
        return true;
    }
    if (y3 != 0)
    {
        dy = -4; // the quadrant above
        return true;
    }
    return false;
}

/// Dequantisation step per Hadamard coefficient, at quality 100.
///
/// The transform has gain 4 and the samples are 5-bit, so a step of 4 on
/// coefficient 0 is exactly one output level -- the group's mean is carried
/// without error, which is what keeps Riven's letterbox black and its flat
/// walls flat. The three AC terms are deliberately much coarser: measured over
/// a fullscreen cutscene, halving them multiplies the bitrate by three for a
/// mean error that improves from 0.15 to 0.10 levels out of 31, which nobody
/// can see on a DS screen. The diagonal term is coarsest of all, being the
/// least visible direction.
inline constexpr int kQuantStep[4] = {4, 8, 8, 20};

/// Forward 2x2 Hadamard. Input is a residual, output has gain 4.
inline constexpr void forwardHadamard(const int in[4], int out[4])
{
    const int t0 = in[0] + in[1];
    const int t1 = in[0] - in[1];
    const int t2 = in[2] + in[3];
    const int t3 = in[2] - in[3];
    out[0] = t0 + t2;
    out[1] = t0 - t2;
    out[2] = t1 + t3;
    out[3] = t1 - t3;
}

inline constexpr int clamp5(int v) { return v < 0 ? 0 : (v > 31 ? 31 : v); }

/// Inverse transform and reconstruction in one step: add the residual to the
/// prediction and clamp back to 5 bits. The +2 before the shift is
/// round-to-nearest across all four outputs at once.
inline constexpr void inverseHadamard(const int coef[4], const int pred[4], int out[4])
{
    const int c0 = coef[0] + 2;
    const int t0 = c0 + coef[1];
    const int t2 = c0 - coef[1];
    const int t1 = coef[2] + coef[3];
    const int t3 = coef[2] - coef[3];
    out[0] = clamp5(pred[0] + ((t0 + t1) >> 2));
    out[1] = clamp5(pred[1] + ((t0 - t1) >> 2));
    out[2] = clamp5(pred[2] + ((t2 + t3) >> 2));
    out[3] = clamp5(pred[3] + ((t2 - t3) >> 2));
}

/// Half-pel sample: the average of two neighbours as the DS 3D engine
/// computes it, which is not (a+b)/2. The GPU expands a 5-bit channel to 6
/// bits as `c ? 2c+1 : 0` and blends at 16/64, so the encoder has to use the
/// same expansion or its prediction will not match what the hardware draws.
inline constexpr int blendHalf(int a, int b)
{
    const int ea = a == 0 ? 0 : 2 * a + 1;
    const int eb = b == 0 ? 0 : 2 * b + 1;
    return (16 * ea + 16 * eb) >> 6;
}

inline bool isRvid(const RvidHeader &h)
{
    return h.magic[0] == 'R' && h.magic[1] == 'V' && h.magic[2] == 'I'
        && h.magic[3] == 'D' && h.version == kVideoVersion;
}

} // namespace rivendata
