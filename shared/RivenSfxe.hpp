#pragma once

// Riven water effects (SFXE), shared between the converter and the ARM9 runtime.
//
// An SFXE is a short looping animation that shifts bands of already-drawn
// pixels around to make water ripple. It is not image data: each frame is a
// tiny script of "copy this run of pixels from row R of the untouched card to
// row R' of the screen" (riven_graphics.cpp:414-483). That means it costs
// almost nothing to store and it composites onto whatever the card is showing.
//
// The on-disc script has three opcodes, all uint16 big-endian:
//   1 -- advance the destination row by one
//   3 -- copy: dstLeft, srcLeft, srcTop, rowWidth
//   4 -- end of frame
// Anything else is an error.
//
// The converter flattens that into the arrays below: one CopyOp per opcode-3,
// with the destination row already resolved from the opcode-1 run, so the DS
// never interprets opcodes at draw time. Coordinates stay in Riven's original
// 608x392 space, like everything else in the schema (see RivenData.hpp).
//
// Effects run only while the "waterenabled" variable is non-zero
// (riven_graphics.cpp:773).

#include <cstdint>
#include <vector>

#include <yas/serialize.hpp>
#include <yas/std_types.hpp>

namespace rivendata
{

/// One resolved pixel-run copy. `srcTop`/`dstRow` are absolute rows in the
/// card's 608x392 space.
struct SfxeCopy
{
    std::uint16_t dstLeft = 0;
    std::uint16_t srcLeft = 0;
    std::uint16_t srcTop = 0;
    std::uint16_t dstRow = 0;
    std::uint16_t rowWidth = 0;

    template <class Ar> void serialize(Ar &ar)
    {
        ar & dstLeft & srcLeft & srcTop & dstRow & rowWidth;
    }
};

/// One animation frame: a slice of the shared copy list.
struct SfxeFrame
{
    std::uint32_t firstCopy = 0;
    std::uint32_t copyCount = 0;

    template <class Ar> void serialize(Ar &ar) { ar & firstCopy & copyCount; }
};

/// A whole effect. Written to sfxe/<stack>/<id>.rsfx.
struct SfxeEffect
{
    std::uint16_t id = 0;
    /// Rect of the card the effect disturbs; nothing outside it is touched.
    std::int16_t left = 0, top = 0, right = 0, bottom = 0;
    /// Frames per second. ScummVM derives the frame delay as 1000/speed
    /// (riven_graphics.cpp:453); 0 means the resource did not say, and the
    /// runtime should fall back rather than divide by zero.
    std::uint16_t speed = 0;

    std::vector<SfxeFrame> frames;
    std::vector<SfxeCopy> copies;

    template <class Ar> void serialize(Ar &ar)
    {
        ar & id & left & top & right & bottom & speed & frames & copies;
    }
};

/// Magic word at the head of an SFXE resource: 'SL'.
inline constexpr std::uint16_t kSfxeMagic = 0x534C;

} // namespace rivendata
