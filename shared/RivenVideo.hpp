#pragma once

// The RVID container, shared between the converter and the ARM9 runtime.
//
// docs/video.md is the specification and explains every number here -- read it
// before changing anything in this file.
//
// Frames are stored RAW: each one is width*height little-endian ARGB1555 texels,
// exactly what the DS samples, with an IMA ADPCM audio block interleaved ahead of
// it. There is no codec. That is a deliberate reversal -- there was one, and
// docs/video.md says what it was and why it went -- and the short version is that
// this port has a card with room to spare and a 67 MHz ARM9 that does not. Raw
// frames cost eight gigabytes and buy back perfect fidelity, no decode at all, and
// a player that cannot fall behind for any reason except how fast the card reads.
//
// What Riven ships, measured with `riven-convert --movie-report`: 1055 movies,
// 1.93 GB, 10832 seconds at 15 fps; 1038 Cinepak, 17 QuickTime RLE, 795 with an
// IMA4 track at 22050 Hz. They fall in two groups, which is what VideoProfile is
// for -- not two decoders any more, but two ways of reaching the screen:
//
//   FULL -- 181 movies at the full 608x392 card size, so 256x165 on the DS.
//           Fullscreen cutscenes. Read straight into a texture and drawn as a
//           quad.
//   LITE -- the other 874, composited onto a card that is already on screen:
//           locked-off shots of animated elements, from 40x40 up. Composited into
//           the card's own picture, because nothing can put a 39x76 movie on top
//           of a card the 3D engine is drawing.
//
// Everything is little-endian, like the rest of the on-card formats.

#include <cstdint>

namespace rivendata
{

/// How a movie reaches the screen.
enum class VideoProfile : std::uint8_t
{
    Full = 0, ///< fullscreen: its own texture, drawn as a quad
    Lite = 1, ///< card overlay: composited into the card picture
};

/// `flags` bits in RvidHeader.
enum : std::uint8_t
{
    /// An audio block is interleaved into every frame.
    kVideoHasAudio = 1 << 0,
};

/// 36-byte header of a .rvid file.
///
/// Followed by `frameCount` RvidFrameEntry, then the frames back to back.
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
    std::uint32_t indexCount;  ///< RvidFrameEntry count; equals frameCount
    std::uint16_t audioRate;   ///< Hz, 0 when there is no audio
    std::uint16_t audioSamplesPerFrame;
    std::uint32_t largestFrameBytes; ///< so a player can size one buffer up front
    std::uint32_t reserved;
};
static_assert(sizeof(RvidHeader) == 36, "RvidHeader must be 36 bytes");

/// Where a frame starts, in bytes from the start of the file.
///
/// One per frame, because every frame is independent -- raw pixels predict from
/// nothing. That is what makes this an index rather than the keyframe list it
/// used to be: seeking to any frame is O(1) instead of "the last I-frame at or
/// before", which matters because Riven restarts movies constantly. A lever
/// animation plays from the top every time the lever is touched.
///
/// It cannot simply be a multiply: the video payload is a fixed size per movie,
/// but the audio block that precedes it is not -- the sample clock does not
/// divide evenly into the frame rate, so blocks alternate between two lengths.
/// Eight bytes a frame is 37 KB on the longest movie in the game.
struct RvidFrameEntry
{
    std::uint32_t frame;
    std::uint32_t offset;
};
static_assert(sizeof(RvidFrameEntry) == 8, "RvidFrameEntry must be 8 bytes");

/// 8-byte header on every frame. The audio block, when present, follows it and
/// precedes the video payload -- the ARM7 needs the samples before the ARM9
/// needs the picture, and putting them in that order means one read serves
/// both.
struct RvidFrameHeader
{
    std::uint32_t byteCount;  ///< audio block + video payload, excluding this header
    std::uint16_t audioBytes; ///< 0 when the file has no audio
    std::uint8_t flags;       ///< kFrameRepeat
    std::uint8_t reserved;
};
static_assert(sizeof(RvidFrameHeader) == 8, "RvidFrameHeader must be 8 bytes");

/// `flags` bits in RvidFrameHeader.
enum : std::uint8_t
{
    /// This frame has no video payload and repeats the picture before it.
    ///
    /// How a movie whose audio outlasts its video carries the rest of its
    /// soundtrack. Twelve of the 795 need it -- ospit's tMOV 0 has 260 seconds of
    /// dialogue over 82 seconds of picture -- and with raw frames the saving is no
    /// longer a few bytes but the whole 84 KB.
    kFrameRepeat = 1 << 0,
};

/// 5 sizes an overlay from the DS columns it covers rather than from its length
/// scaled: same layout, different geometry, so old files are current-looking and
/// a pixel short -- 583 of the 1055. 4 is the soundtrack's makeup gain
/// (shared/RivenSound.hpp) reaching movies too: same layout, different samples,
/// so old files are current-looking and quiet. 3 dropped the codec: frames are
/// raw texels, and the keyframe index became a per-frame index. 2 added a
/// quantiser setting, 1 had none. Nothing that reads this has ever shipped, so
/// there is no migration.
inline constexpr std::uint16_t kVideoVersion = 5;

/// Bytes one frame's picture takes at these dimensions. No padding and no
/// alignment: the DS samples ARGB1555 directly, and for a FULL movie the width is
/// already 256 -- the texture width -- so a frame is one contiguous copy into the
/// top of a 256x256 texture.
inline constexpr std::uint32_t rvidFrameBytes(int width, int height)
{
    return static_cast<std::uint32_t>(width) * static_cast<std::uint32_t>(height) * 2u;
}

inline bool isRvid(const RvidHeader &h)
{
    return h.magic[0] == 'R' && h.magic[1] == 'V' && h.magic[2] == 'I'
        && h.magic[3] == 'D' && h.version == kVideoVersion;
}

} // namespace rivendata
