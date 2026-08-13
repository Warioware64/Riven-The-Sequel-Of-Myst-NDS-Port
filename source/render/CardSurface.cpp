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
} // namespace

bool CardSurface::create()
{
    if (texels_ != nullptr)
        return true;

    texels_ = static_cast<Texel *>(
        std::malloc(static_cast<std::size_t>(kViewW) * kViewH * sizeof(Texel)));
    if (texels_ == nullptr)
        return false;
    clear();
    return true;
}

void CardSurface::destroy()
{
    if (texels_ != nullptr)
    {
        std::free(texels_);
        texels_ = nullptr;
    }
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
    for (std::size_t i = 0, n = static_cast<std::size_t>(kViewW) * kViewH; i < n; ++i)
        texels_[i] = black;
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
    for (int y = 0; y < dh; ++y)
    {
        const int sy = img.height == dh ? y : y * img.height / dh;
        const rivendata::Texel *src =
            img.texels.data() + static_cast<std::size_t>(sy) * img.width;
        Texel *dst = texels_ + static_cast<std::size_t>(y0 + y) * kViewW + x0;
        if (img.width == dw)
        {
            std::memcpy(dst, src, static_cast<std::size_t>(dw) * sizeof(Texel));
        }
        else
        {
            for (int x = 0; x < dw; ++x)
                dst[x] = src[x * img.width / dw];
        }
    }

    markRows(y0, dh);
    return true;
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
