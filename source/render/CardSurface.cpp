#include "CardSurface.hpp"

#include <cstdlib>
#include <cstring>

#include "data/ImageFile.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    // ImageFile's bounds are spelled as numbers so that module needs no geometry;
    // this is where they are tied to the real thing.
    static_assert(kMaxRpicW == kViewW, "the .rpic width bound must be the view's");
    static_assert(kMaxRpicH >= kCardH, "a .rpic can be as tall as its source");

    static_assert(CardSurface::kRowBlock == 8,
                  "BgSurface::uploadRows assumes an 8-row dirty block");

    /// One plane. There are two of them -- see the members.
    constexpr std::size_t kPlaneTexels = static_cast<std::size_t>(kViewW) * kViewH;
    constexpr std::size_t kPlaneBytes = kPlaneTexels * sizeof(Texel);
} // namespace

bool CardSurface::create()
{
    if (texels_ != nullptr)
        return true;

    texels_ = static_cast<Texel *>(std::malloc(kPlaneBytes));
    clean_ = static_cast<Texel *>(std::malloc(kPlaneBytes));
    if (texels_ == nullptr || clean_ == nullptr)
    {
        // All or nothing: every path below assumes the two planes exist together.
        destroy();
        return false;
    }
    clear();
    return true;
}

void CardSurface::destroy()
{
    std::free(texels_);
    std::free(clean_);
    texels_ = nullptr;
    clean_ = nullptr;
    overlayRows_ = 0;
    for (std::uint32_t &d : dirty_)
        d = 0;
}

void CardSurface::clear()
{
    if (texels_ == nullptr)
        return;
    // Opaque black, not transparent: a transparent texel is skipped by the
    // blender and would show the 3D clear colour through the card.
    const Texel black = static_cast<Texel>(0x8000);
    for (std::size_t i = 0; i < kPlaneTexels; ++i)
        texels_[i] = clean_[i] = black;
    overlayRows_ = 0;
    markAll();
}

void CardSurface::markAll()
{
    markRowMask(kAllDirty);
}

void CardSurface::markRowMask(std::uint32_t mask)
{
    // Every buffer, not just the one about to be published: a back buffer is two
    // frames stale, so a change that only ever went into the front one would be
    // missing from the other for the rest of the card's life.
    for (std::uint32_t &d : dirty_)
        d |= mask;
}

void CardSurface::markRows(int y, int height)
{
    if (height <= 0)
        return;
    int first = y / kRowBlock;
    int last = (y + height - 1) / kRowBlock;
    if (first < 0)
        first = 0;
    if (last >= kRowBlocks)
        last = kRowBlocks - 1;

    std::uint32_t mask = 0;
    for (int i = first; i <= last; ++i)
        mask |= 1u << i;
    markRowMask(mask);
}

bool CardSurface::drawPicture(const std::string &path, const Rect &cardRect,
                              std::string &error)
{
    if (texels_ == nullptr)
    {
        error = "no card surface to draw on";
        return false;
    }

    RpicImage img;
    if (!loadRpicImage(path, img, error))
        return false;
    if (!img.valid())
    {
        error = "picture decoded to nothing";
        return false;
    }

    // The rectangle is in Riven's original coordinates; scaling it here rather
    // than in the converter is what keeps one scale constant in the whole port
    // (RivenData.hpp:23-29).
    int x0 = toDsX(cardRect.left);
    int y0 = toDsY(cardRect.top);
    int x1 = toDsX(cardRect.right);
    int y1 = toDsY(cardRect.bottom);
    if (x1 <= x0 || y1 <= y0)
    {
        error = "picture's destination rectangle is empty";
        return false;
    }
    if (x1 > kViewW)
        x1 = kViewW;
    if (y1 > kViewH)
        y1 = kViewH;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;

    const int dw = x1 - x0;
    const int dh = y1 - y0;

    // Nearest neighbour. The common case is a 1:1 copy -- a full-card still is
    // already exactly 256x165 -- and where it is not, the picture is a small
    // overlay whose converted size does not match the card scale
    // (ImagePipeline.cpp:247-255). Resampling it properly would mean a second
    // filter on the DS for a case the eye cannot resolve at this size.
    //
    // BOTH PLANES, in the one loop. This is a script drawing, so it belongs to
    // the card and has to reach `clean_`; it also has to be seen now, so it has
    // to reach `texels_`. Mirroring afterwards instead would mean re-deriving
    // this clipped rect a second time, one clamp away from disagreeing with the
    // write it is meant to copy.
    for (int y = 0; y < dh; ++y)
    {
        const int sy = img.height == dh ? y : y * img.height / dh;
        const rivendata::Texel *src =
            img.texels.data() + static_cast<std::size_t>(sy) * img.width;
        const std::size_t at = static_cast<std::size_t>(y0 + y) * kViewW + x0;
        Texel *dst = texels_ + at;
        Texel *keep = clean_ + at;
        if (img.width == dw)
        {
            const std::size_t bytes = static_cast<std::size_t>(dw) * sizeof(Texel);
            std::memcpy(dst, src, bytes);
            std::memcpy(keep, src, bytes);
        }
        else
        {
            for (int x = 0; x < dw; ++x)
                dst[x] = keep[x] = src[x * img.width / dw];
        }
    }

    markRows(y0, dh);
    return true;
}

void CardSurface::noteOverlayRows(std::uint32_t mask)
{
    overlayRows_ |= mask;
    markRowMask(mask);
}

void CardSurface::refreshFromClean()
{
    if (texels_ == nullptr || overlayRows_ == 0)
        return;

    // Whole rows, not the overlay's rect. A dirty band is card-wide and `clean_`
    // holds the right content for all of it, so restoring the band is both
    // simpler and more correct than tracking rects -- and a band shared with a
    // second, still-running overlay is put back by the recomposite that follows
    // this (Engine::recompositeOverlays).
    for (int b = 0; b < kRowBlocks; ++b)
    {
        if ((overlayRows_ & (1u << b)) == 0)
            continue;
        const int y0 = b * kRowBlock;
        int rows = kRowBlock;
        if (y0 + rows > kViewH)
            rows = kViewH - y0;
        if (rows <= 0)
            continue;
        const std::size_t at = static_cast<std::size_t>(y0) * kViewW;
        std::memcpy(texels_ + at, clean_ + at,
                    static_cast<std::size_t>(rows) * kViewW * sizeof(Texel));
    }

    markRowMask(overlayRows_);
    overlayRows_ = 0;
}

void CardSurface::bakeRect(int x, int y, int w, int h)
{
    if (texels_ == nullptr || w <= 0 || h <= 0)
        return;

    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > kViewW ? kViewW : x + w;
    int y1 = y + h > kViewH ? kViewH : y + h;
    if (x1 <= x0 || y1 <= y0)
        return;

    // No dirty marking: these texels are already in `texels_` and have already
    // been published. All this decides is that they SURVIVE the next refresh.
    const std::size_t bytes = static_cast<std::size_t>(x1 - x0) * sizeof(Texel);
    for (int row = y0; row < y1; ++row)
    {
        const std::size_t at = static_cast<std::size_t>(row) * kViewW + x0;
        std::memcpy(clean_ + at, texels_ + at, bytes);
    }
}

void CardSurface::publish(BgSurface &bg, int buf)
{
    if (texels_ == nullptr || !bg.exists() || dirty_[buf] == 0)
        return;

    // No upload window, no partial write, no early break. `buf` is not the
    // buffer the 2D engine is scanning out, so the whole card can go at once and
    // the change appears in one flip -- which is what makes opcodes 20/21 ("show
    // this update whole") actually true rather than merely intended.
    bg.uploadRows(buf, texels_, dirty_[buf]);
    dirty_[buf] = 0;
}

} // namespace rivenrt
