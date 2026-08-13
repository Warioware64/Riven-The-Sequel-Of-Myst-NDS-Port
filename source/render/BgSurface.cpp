#include "BgSurface.hpp"

#include "tonccpy.h"

using namespace rivendata;

namespace rivenrt
{

BgSurface bgs;

namespace
{
    /// Bitmap bases are in 16 KB units and a buffer is 128 KB, so buffer n sits
    /// at base n*8. Bank A is at 0x06000000 (base 0), B at 0x06020000 (base 8)
    /// and D at 0x06040000 (base 16) -- main BG VRAM is one contiguous window
    /// and bank C is not in it, being the sub engine's.
    constexpr int kBaseFor[BgSurface::kBuffers] = {0, 8, 16};

    /// Opaque black for the picture area, transparent for everything below it.
    constexpr Texel kOpaqueBlack = static_cast<Texel>(0x8000);
    constexpr Texel kTransparent = 0;

    /// Priority 1 for the layer on top and 2 for the one underneath, rather than
    /// 0 and 1: OBJ at priority 0 must stay above both (the cursor), and leaving
    /// BG priority 0 unused keeps that unambiguous.
    constexpr unsigned kFrontPriority = 1;
    constexpr unsigned kBackPriority = 2;
} // namespace

bool BgSurface::create()
{
    if (exists())
        return true;

    // bgInitHidden rather than bgInit: the banks hold whatever was in them at
    // power-on, and showing a layer before it is cleared is a screenful of noise.
    bgId_[0] = bgInitHidden(2, BgType_Bmp16, BgSize_B16_256x256, kBaseFor[0], 0);
    bgId_[1] = bgInitHidden(3, BgType_Bmp16, BgSize_B16_256x256, kBaseFor[1], 0);
    if (bgId_[0] < 0 || bgId_[1] < 0)
    {
        bgId_[0] = bgId_[1] = -1;
        return false;
    }

    for (int b = 0; b < kBuffers; ++b)
    {
        Texel *p = pixels(b);
        toncset16(p, kOpaqueBlack, static_cast<std::size_t>(kBufW) * kViewH);
        // The rows below the picture. Written once and never again: a vertical
        // pan scrolls them through the view, and transparent is what lets the
        // outgoing card show through there instead of stale pixels.
        toncset16(p + static_cast<std::size_t>(kBufW) * kViewH, kTransparent,
                  static_cast<std::size_t>(kBufW) * (kBufH - kViewH));
    }

    bound_[0] = 0;
    bound_[1] = 1;
    front_ = 0;
    flipPending_ = false;
    keep_ = -1;

    // Rest: bitmap row 0 lands on screen row kViewOffsetY.
    bgSetScroll(bgId_[0], 0, kRestY);
    bgSetScroll(bgId_[1], 0, kRestY);
    applyPriorities();

    bgShow(bgId_[0]);
    bgShow(bgId_[1]);
    bgUpdate();
    return true;
}

void BgSurface::destroy()
{
    if (!exists())
        return;
    bgHide(bgId_[0]);
    bgHide(bgId_[1]);
    REG_BLDCNT = 0;
    bgUpdate();
    bgId_[0] = bgId_[1] = -1;
}

void BgSurface::applyPriorities()
{
    bgSetPriority(bgId_[front_], kFrontPriority);
    bgSetPriority(bgId_[front_ ^ 1], kBackPriority);
}

void BgSurface::bind(int layer, int buf)
{
    bound_[layer] = buf;
    bgSetMapBase(bgId_[layer], static_cast<unsigned>(kBaseFor[buf]));
}

int BgSurface::spareBuffer() const
{
    for (int b = 0; b < kBuffers; ++b)
        if (b != bound_[0] && b != bound_[1])
            return b;
    return 0; // unreachable: two layers cannot cover three buffers
}

void BgSurface::uploadRows(int buf, const Texel *ram, std::uint32_t mask)
{
    if (!exists() || ram == nullptr || mask == 0)
        return;

    Texel *dst = pixels(buf);
    constexpr int kBlock = 8; ///< must match CardSurface::kRowBlock
    constexpr int kBlocks = (kViewH + kBlock - 1) / kBlock;

    // Adjacent dirty blocks are copied as one run. A whole-card publish is then
    // a single 84 KB tonccpy rather than 21 of 4 KB, which is most of the win:
    // there is no upload window to fit inside any more, because the buffer being
    // written is not the one the 2D engine is scanning.
    int i = 0;
    while (i < kBlocks)
    {
        if ((mask & (1u << i)) == 0)
        {
            ++i;
            continue;
        }
        int j = i;
        while (j < kBlocks && (mask & (1u << j)) != 0)
            ++j;

        const int y0 = i * kBlock;
        int rows = (j - i) * kBlock;
        if (y0 + rows > kViewH)
            rows = kViewH - y0;
        if (rows > 0)
        {
            const std::size_t at = static_cast<std::size_t>(y0) * kViewW;
            tonccpy(dst + at, ram + at,
                    static_cast<std::size_t>(rows) * kViewW * sizeof(Texel));
        }
        i = j;
    }
}

void BgSurface::beginMovieTakeover()
{
    if (!exists() || keep_ >= 0)
        return;
    // Whatever is on screen is the card. Everything else is the movie's; vblank()
    // keeps any layer that flips onto `keep_` off it by rebinding to the spare.
    keep_ = frontBuffer();
}

int BgSurface::endMovieTakeover()
{
    if (!exists() || keep_ < 0)
        return backBuffer();

    const int stale = keep_;
    keep_ = -1;

    if (frontBuffer() == stale)
        return backBuffer(); // never actually flipped; the card is still up

    // Put the parked card back under the movie's last frame and flip to it.
    bind(front_ ^ 1, stale);
    requestFlip();
    // The buffer the movie was writing is now the back one, and holds a movie
    // frame rather than the card.
    return frontBuffer();
}

void BgSurface::beginTransition(Transition t)
{
    if (!exists())
        return;

    if (t == Transition::None || (!isPan(t) && !isBlend(t)))
    {
        // Unknown or explicitly instant: just show the new card.
        requestFlip();
        return;
    }

    type_ = t;
    frame_ = 0;
    frames_ = isBlend(t) ? kBlendSteps : kPanFrames;

    if (isBlend(t))
    {
        // The hardware blends the topmost 1st-target pixel into the 2nd-target
        // pixel beneath it, so the INCOMING card has to be on top before the
        // first step. EVA starts at 0, which outputs the old card unchanged, so
        // swapping now is invisible.
        front_ ^= 1;
        applyPriorities();
        flipAtEnd_ = false;
        REG_BLDCNT = static_cast<std::uint16_t>(
            BLEND_ALPHA | (BLEND_SRC_BG2 << front_) | (BLEND_DST_BG2 << (front_ ^ 1)));
        REG_BLDALPHA = static_cast<std::uint16_t>(BLDALPHA_EVA(0) | BLDALPHA_EVB(16));
    }
    else
    {
        // A pan never overlaps the two images -- each occupies the part of the
        // view the other has left -- so priority does not matter until the end.
        flipAtEnd_ = true;
    }
}

void BgSurface::setLetterbox(bool on)
{
    if (!exists())
        return;
    const int y = on ? kRestY : 0;
    bgSetScroll(bgId_[0], 0, y);
    bgSetScroll(bgId_[1], 0, y);
    bgUpdate();
}

void BgSurface::vblank()
{
    if (!exists())
        return;

    if (frames_ > 0)
    {
        ++frame_;

        if (isBlend(type_))
        {
            const int eva = 16 * frame_ / frames_;
            REG_BLDALPHA = static_cast<std::uint16_t>(BLDALPHA_EVA(eva)
                                                      | BLDALPHA_EVB(16 - eva));
        }
        else
        {
            // front_ is still the OLD card: a pan defers the swap to the end.
            const BgScroll s = transitionScroll(type_, frame_, frames_);
            bgSetScroll(bgId_[front_], s.oldX, s.oldY);
            bgSetScroll(bgId_[front_ ^ 1], s.newX, s.newY);
        }

        if (frame_ >= frames_)
        {
            REG_BLDCNT = 0;
            REG_BLDALPHA = 0;
            bgSetScroll(bgId_[0], 0, kRestY);
            bgSetScroll(bgId_[1], 0, kRestY);
            frames_ = 0;
            frame_ = 0;
            type_ = Transition::None;
            if (flipAtEnd_)
                flipPending_ = true;
        }
    }

    if (flipPending_)
    {
        front_ ^= 1;
        applyPriorities();
        flipPending_ = false;

        // A fullscreen movie must never draw over the parked card. Once a flip
        // has put `keep_` on the back layer, move that layer to the spare; the
        // movie then ping-pongs the two buffers that are not the card's.
        if (keep_ >= 0 && bound_[front_ ^ 1] == keep_)
            bind(front_ ^ 1, spareBuffer());
    }

    // Nothing above reaches BG2X/BG2Y/BGxCNT until this runs -- libnds defers
    // every background write behind a dirty flag.
    bgUpdate();
}

} // namespace rivenrt
