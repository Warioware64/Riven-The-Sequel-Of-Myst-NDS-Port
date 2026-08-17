#include "WaterEffect.hpp"

#include <cstdio>
#include <cstring>

#include "DebugLog.hpp"
#include "RivenSfxe.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Must match the converter's kYasFlags exactly (Converter.cpp:31), the same
    /// way StackFile.cpp does. An .rsfx is the bare payload -- unlike a stack
    /// file it carries no header of its own (Converter.cpp:697-704).
    constexpr std::size_t kYasFlags = yas::mem | yas::binary | yas::no_header;

    /// The largest effect in a retail copy of Riven is around 45 KB in this
    /// form. The bound is not a schema check -- an .rsfx has nothing to check
    /// against -- it is here so that a file that is not an effect at all is
    /// refused before yas is handed it, because yas reports a bad archive by
    /// throwing and the ARM9 is built -fno-exceptions.
    constexpr std::size_t kMaxRsfxBytes = 256 * 1024;

    /// What the DS runs its frames at, for turning an effect's fps into a count
    /// of engine frames.
    constexpr int kEngineFps = 60;

    bool readWhole(const std::string &path, std::vector<std::uint8_t> &out,
                   std::string &error)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
        {
            error = "cannot open " + path;
            return false;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size <= 0 || static_cast<std::size_t>(size) > kMaxRsfxBytes)
        {
            std::fclose(f);
            error = path + " is not the size of a water effect";
            return false;
        }
        out.resize(static_cast<std::size_t>(size));
        const std::size_t got = std::fread(out.data(), 1, out.size(), f);
        std::fclose(f);
        if (got != out.size())
        {
            error = path + " is truncated";
            return false;
        }
        return true;
    }
} // namespace

void WaterEffect::clear()
{
    copies_.clear();
    frames_.clear();
    copies_.shrink_to_fit();
    frames_.shrink_to_fit();
    cur_ = 0;
    ticks_ = 0;
    period_ = 4;
}

bool WaterEffect::load(const std::string &path)
{
    clear();

    std::vector<std::uint8_t> payload;
    std::string error;
    if (!readWhole(path, payload, error))
    {
        DebugLog::warn("SFXE %s", error.c_str());
        return false;
    }

    SfxeEffect fx;
    yas::mem_istream is(payload.data(), payload.size());
    yas::binary_iarchive<yas::mem_istream, kYasFlags> ia(is);
    ia &fx;

    if (fx.frames.empty() || fx.copies.empty())
    {
        DebugLog::warn("SFXE %s holds no frames", path.c_str());
        return false;
    }

    // 1000/speed is the original's frame delay (riven_graphics.cpp:453); here
    // the vblank is the clock, so it is a count of engine frames instead. Every
    // effect measured in a retail copy is 15 fps, which makes this 4. A speed of
    // zero means the resource did not say (RivenSfxe.hpp:65-68) -- fall back
    // rather than divide by it.
    const int fps = fx.speed != 0 ? fx.speed : 15;
    period_ = kEngineFps / fps;
    if (period_ < 1)
        period_ = 1;

    frames_.reserve(fx.frames.size());
    copies_.reserve(fx.copies.size());

    for (const SfxeFrame &sf : fx.frames)
    {
        Frame frame;
        frame.first = static_cast<std::uint32_t>(copies_.size());

        // The converter's slices index its own copy list, and nothing has
        // checked them against it: an .rsfx carries no header, so this is where
        // a file that is not this build's schema has to be caught rather than
        // followed off the end of the vector.
        const std::size_t end = static_cast<std::size_t>(sf.firstCopy) + sf.copyCount;
        if (end > fx.copies.size())
        {
            DebugLog::warn("SFXE %s: frame runs past its copy list", path.c_str());
            clear();
            return false;
        }

        for (std::size_t i = sf.firstCopy; i < end; ++i)
        {
            const SfxeCopy &c = fx.copies[i];

            // Both ends of the run through the same map, so the width is the
            // distance the destination actually covers rather than a scaled
            // length that could disagree with it by a pixel. The narrower of the
            // two ends keeps the source read inside the view as well.
            const int dstX = toDsX(c.dstLeft);
            const int srcX = toDsX(c.srcLeft);
            const int dstY = toDsY(c.dstRow);
            const int srcY = toDsY(c.srcTop);
            int width = toDsX(c.dstLeft + c.rowWidth) - dstX;
            const int srcWidth = toDsX(c.srcLeft + c.rowWidth) - srcX;
            if (srcWidth < width)
                width = srcWidth;
            if (width > kViewW - dstX)
                width = kViewW - dstX;
            if (width > kViewW - srcX)
                width = kViewW - srcX;

            // A run narrow enough to scale away, or a row off the bottom of the
            // view. Riven's effect rects go to row 392, which is the first row
            // the DS view does not have.
            if (width <= 0 || dstX < 0 || srcX < 0 || dstY < 0 || srcY < 0
                || dstY >= kViewH || srcY >= kViewH)
                continue;

            Copy out;
            out.dstX = static_cast<std::uint8_t>(dstX);
            out.dstY = static_cast<std::uint8_t>(dstY);
            out.srcX = static_cast<std::uint8_t>(srcX);
            out.srcY = static_cast<std::uint8_t>(srcY);
            out.width = static_cast<std::uint16_t>(width);
            copies_.push_back(out);

            frame.rowMask |= 1u << (dstY / CardSurface::kRowBlock);
        }

        // Kept even when it came out empty. Thirteen frames in a retail copy
        // scale down to no copies at all, and dropping them would shorten the
        // loop -- the effect would skip a beat rather than hold for one, which
        // is what the original does with a frame whose script draws nothing.
        frame.count = static_cast<std::uint32_t>(copies_.size()) - frame.first;
        frames_.push_back(frame);
    }

    if (copies_.empty())
    {
        DebugLog::warn("SFXE %s: nothing left of it at this size", path.c_str());
        clear();
        return false;
    }

    // The copy count is the number that decides whether a card can afford its
    // water, so it is the one worth having in the log.
    DebugLog::log("SFXE %s: %zu frames, %zu copies, every %d frames", path.c_str(),
                  frames_.size(), copies_.size(), period_);
    return true;
}

std::uint32_t WaterEffect::update(CardSurface &surface)
{
    if (frames_.empty() || !surface.exists())
        return 0;

    if (++ticks_ < period_)
        return 0;

    // After the period gate, so the number is the cost of a tick that DREW and
    // not an average over the three that did nothing.
    DebugLog::Perf::Scope waterScope{DebugLog::Perf::Water};
    ticks_ = 0;

    const Frame &frame = frames_[cur_];
    Texel *const dst = surface.texels();
    const Texel *const src = surface.clean();

    // The whole replay. Every coordinate was resolved and clipped at load time,
    // so there is nothing here but the copies -- which is the point: this runs
    // up to 4500 times in one frame on the heaviest cards.
    for (std::uint32_t i = 0; i < frame.count; ++i)
    {
        const Copy &c = copies_[frame.first + i];
        std::memcpy(dst + static_cast<std::size_t>(c.dstY) * kViewW + c.dstX,
                    src + static_cast<std::size_t>(c.srcY) * kViewW + c.srcX,
                    static_cast<std::size_t>(c.width) * sizeof(Texel));
    }

    if (++cur_ >= frames_.size())
        cur_ = 0;
    return frame.rowMask;
}

} // namespace rivenrt
