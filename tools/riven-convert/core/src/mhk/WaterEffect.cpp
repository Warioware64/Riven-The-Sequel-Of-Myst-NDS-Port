#include "riven/WaterEffect.hpp"

namespace riven
{
namespace
{
    // Frame-script opcodes (riven_graphics.cpp:466-483).
    constexpr std::uint16_t kOpNextRow = 1;
    constexpr std::uint16_t kOpCopy = 3;
    constexpr std::uint16_t kOpEnd = 4;

    // A frame script is a few hundred copies at most (one per row of the
    // effect rect, and Riven's tallest is under 400 rows). Anything past this
    // is a runaway read through unrelated resource bytes rather than a real
    // frame.
    constexpr std::size_t kMaxCopiesPerFrame = 2048;

    /// Read one frame's script. Returns false without disturbing `copies`
    /// beyond `firstCopy` if the script is unreadable.
    bool readFrame(ResourceReader &r, std::uint32_t offset, std::uint16_t topRow,
                   std::vector<rivendata::SfxeCopy> &copies,
                   rivendata::SfxeFrame &frame)
    {
        frame.firstCopy = static_cast<std::uint32_t>(copies.size());

        r.seek(offset);
        if (!r.ok())
            return false;

        // The destination row walks down from the top of the effect rect, one
        // row per opcode 1. Resolving it here is the point of this parser: the
        // DS then replays copies with no interpreter at all.
        std::uint16_t dstRow = topRow;
        std::size_t copiesThisFrame = 0;

        while (!r.atEnd() && copiesThisFrame <= kMaxCopiesPerFrame)
        {
            const std::uint16_t op = r.u16();
            if (!r.ok())
                break;

            if (op == kOpEnd)
            {
                frame.copyCount =
                    static_cast<std::uint32_t>(copies.size()) - frame.firstCopy;
                return true;
            }
            if (op == kOpNextRow)
            {
                ++dstRow;
                continue;
            }
            if (op != kOpCopy)
                break; // corrupt or truncated: give up on this frame

            rivendata::SfxeCopy c;
            c.dstLeft = r.u16();
            c.srcLeft = r.u16();
            c.srcTop = r.u16();
            c.rowWidth = r.u16();
            c.dstRow = dstRow;
            if (!r.ok())
                break;

            copies.push_back(c);
            ++copiesThisFrame;
        }

        // Ran out of data, hit a bad opcode, or overran the copy guard. Roll
        // back this frame's partial copies so the effect holds only whole
        // frames -- a half-drawn ripple frame looks like a graphics bug.
        copies.resize(frame.firstCopy);
        return false;
    }
} // namespace

SfxeParse parseSfxe(ResourceReader &r, std::uint16_t id)
{
    SfxeParse result;

    rivendata::SfxeEffect fx;
    fx.id = id;

    if (r.u16() != rivendata::kSfxeMagic)
        return result;

    const std::uint16_t frameCount = r.u16();
    const std::uint32_t offsetTablePos = r.u32();
    fx.left = r.i16();
    fx.top = r.i16();
    fx.right = r.i16();
    fx.bottom = r.i16();
    fx.speed = r.u16();
    // The rest of the header is fields ScummVM skips; the offset table is
    // reached by seeking, so there is nothing to read past here.

    if (!r.ok() || frameCount == 0)
        return result;

    result.framesClaimed = frameCount;

    r.seek(offsetTablePos);
    if (!r.ok() || r.remaining() < static_cast<std::size_t>(frameCount) * 4)
        return result;

    std::vector<std::uint32_t> frameOffsets(frameCount);
    for (std::uint16_t i = 0; i < frameCount; ++i)
        frameOffsets[i] = r.u32();
    if (!r.ok())
        return result;

    fx.frames.reserve(frameCount);
    const auto topRow = static_cast<std::uint16_t>(fx.top);

    for (std::uint16_t f = 0; f < frameCount; ++f)
    {
        rivendata::SfxeFrame frame;
        if (readFrame(r, frameOffsets[f], topRow, fx.copies, frame))
            fx.frames.push_back(frame);
        else
            ++result.framesDropped;
    }

    if (fx.frames.empty())
        return result;

    result.effect = std::move(fx);
    return result;
}

} // namespace riven
