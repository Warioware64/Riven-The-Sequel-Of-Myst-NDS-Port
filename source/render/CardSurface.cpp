#include "CardSurface.hpp"

#include <cstdlib>
#include <cstring>

#include "data/ImageFile.hpp"
#include "render/Resample.hpp"
#include "tonccpy.h" // reading a background buffer back out of VRAM

using namespace rivendata;

namespace rivenrt
{
namespace
{
    // ImageFile's bounds are spelled as numbers so that module needs no geometry;
    // this is where they are tied to the real thing.
    // Not == kViewW: the converter leaves art wider than the card at 1:1, so a
    // .rpic can be wider than the view it is drawn into (bspit's 800x25 numeral
    // strips). It can never be narrower than the view is wide, or a full-card
    // still would not fit.
    static_assert(kMaxRpicW >= kViewW, "a .rpic must at least hold a full-card still");
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
    ++generation_;
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
                              std::string &error, Placed *placed)
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

    // THE RECTANGLE'S TOP-LEFT CORNER, AND THE PICTURE'S OWN SIZE. Not the
    // rectangle's other two edges: copyImageToScreen ignores them and blits the
    // image at its own size (riven_graphics.cpp:367-381), and Riven ships 96 PLST
    // records whose rectangle disagrees with the bitmap it names -- gspit card
    // 266's viewer among them, where a 188x196 picture of Catherine is filed
    // under a 207x242 rectangle. Stretching to the rectangle made those wrong.
    //
    // The size comes from the file because only the converter saw it: the .rpic
    // holds a picture resampled to min(kViewW, sourceWidth) up to the card's own
    // width and left alone above it, so a 188-wide overlay is still 188 texels,
    // a 608-wide still is 256, and an 800-wide strip is still 800
    // (RivenImage.hpp).
    //
    // Clipping the right edge to the card is ScummVM's "clip the width to fit on
    // the screen. Fixes some images."
    int right = cardRect.left + img.srcWidth;
    if (right > kCardW)
        right = kCardW;
    const int bottom = cardRect.top + img.srcHeight;

    // Before the DS rectangle is derived, and in card coordinates: this is where
    // the picture went as far as Riven is concerned, which is the space the zoom
    // viewer draws in. See Placed.
    if (placed != nullptr)
    {
        placed->card = Rect{cardRect.left, cardRect.top, static_cast<std::int16_t>(right),
                            static_cast<std::int16_t>(bottom)};
        placed->fileW = img.width;
        placed->fileH = img.height;
        placed->sourceW = img.srcWidth;
        placed->sourceH = img.srcHeight;
    }

    // The rectangle is in Riven's original coordinates; scaling it here rather
    // than in the converter is what keeps one scale constant in the whole port
    // (RivenData.hpp:23-29).
    int x0 = toDsX(cardRect.left);
    int y0 = toDsY(cardRect.top);
    int x1 = toDsX(right);
    int y1 = toDsY(bottom);
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

    // Three cases. A full-card still is already exactly 256x165 and is copied;
    // a picture the converter left at its source size (convertBitmapPixels in
    // ImagePipeline.cpp: a small overlay, or a strip wider than the card) still
    // owes the card-to-DS reduction and gets it box-filtered; and a picture
    // being magnified falls back to the point sample, which is what averaging
    // would do anyway.
    //
    // BOTH PLANES. This is a script drawing, so it belongs to the card and has
    // to reach `clean_`; it also has to be seen now, so it has to reach
    // `texels_`. Mirroring afterwards instead would mean re-deriving this
    // clipped rect a second time, one clamp away from disagreeing with the write
    // it is meant to copy.
    const bool reducing = img.width > dw || img.height > dh;
    Span cols[kViewW];
    if (reducing)
        buildColumnSpans(0, img.width, dw, cols);

    for (int y = 0; y < dh; ++y)
    {
        const std::size_t at = static_cast<std::size_t>(y0 + y) * kViewW + x0;
        Texel *dst = texels_ + at;
        Texel *keep = clean_ + at;
        const std::size_t bytes = static_cast<std::size_t>(dw) * sizeof(Texel);

        if (reducing)
        {
            boxFilterRow(img.texels.data(), img.width, rowSpan(0, img.height, dh, y),
                         cols, dw, dst);
            std::memcpy(keep, dst, bytes);
            continue;
        }

        const int sy = img.height == dh ? y : y * img.height / dh;
        const rivendata::Texel *src =
            img.texels.data() + static_cast<std::size_t>(sy) * img.width;
        if (img.width == dw)
        {
            std::memcpy(dst, src, bytes);
            std::memcpy(keep, src, bytes);
        }
        else
        {
            for (int x = 0; x < dw; ++x)
                dst[x] = keep[x] = src[x * img.width / dw];
        }
    }

    ++generation_;
    markRows(y0, dh);
    return true;
}

bool CardSurface::drawPictureSections(const std::string &path, const Section *sections,
                                      std::size_t count, std::string &error,
                                      Placed *placed)
{
    if (texels_ == nullptr)
    {
        error = "no card surface to draw on";
        return false;
    }
    if (sections == nullptr || count == 0)
        return true;

    // ONCE, for the whole batch -- the reason this function exists.
    RpicImage img;
    if (!loadRpicImage(path, img, error))
        return false;
    if (!img.valid())
    {
        error = "picture decoded to nothing";
        return false;
    }

    // Per file, not per section: the two widths are the file's, and the card
    // rectangle a section landed on is the one its caller already has.
    if (placed != nullptr)
    {
        placed->fileW = img.width;
        placed->fileH = img.height;
        placed->sourceW = img.srcWidth;
        placed->sourceH = img.srcHeight;
    }

    bool drewAnything = false;
    for (std::size_t i = 0; i < count; ++i)
    {
        const Rect &src = sections[i].src;
        const Rect &dstCard = sections[i].dst;

        // The source is in the file's own pixels and is clamped to it; the
        // destination is in card coordinates and is scaled, exactly as
        // drawPicture does.
        int sx0 = src.left < 0 ? 0 : src.left;
        int sy0 = src.top < 0 ? 0 : src.top;
        int sx1 = src.right > img.width ? img.width : src.right;
        int sy1 = src.bottom > img.height ? img.height : src.bottom;
        if (sx1 <= sx0 || sy1 <= sy0)
            continue;

        int x0 = toDsX(dstCard.left);
        int y0 = toDsY(dstCard.top);
        int x1 = toDsX(dstCard.right);
        int y1 = toDsY(dstCard.bottom);
        if (x0 < 0)
            x0 = 0;
        if (y0 < 0)
            y0 = 0;
        if (x1 > kViewW)
            x1 = kViewW;
        if (y1 > kViewH)
            y1 = kViewH;
        if (x1 <= x0 || y1 <= y0)
            continue; // a slot that scaled away to nothing

        const int dw = x1 - x0;
        const int dh = y1 - y0;
        const int sw = sx1 - sx0;
        const int sh = sy1 - sy0;

        // Box-filtered where the section is being reduced, point-sampled where
        // it is not, and both planes either way -- the same three cases and the
        // same reasons as drawPicture. This is the path the dome combination's
        // numerals take: 32x24 of strip into 13x10 of card, where a point sample
        // steps over the strokes that tell one numeral from another.
        const bool reducing = sw > dw || sh > dh;
        Span cols[kViewW];
        if (reducing)
            buildColumnSpans(sx0, sw, dw, cols);

        for (int y = 0; y < dh; ++y)
        {
            const std::size_t at = static_cast<std::size_t>(y0 + y) * kViewW + x0;
            Texel *dst = texels_ + at;
            Texel *keep = clean_ + at;

            if (reducing)
            {
                boxFilterRow(img.texels.data(), img.width, rowSpan(sy0, sh, dh, y), cols,
                             dw, dst);
                std::memcpy(keep, dst, static_cast<std::size_t>(dw) * sizeof(Texel));
                continue;
            }

            const int sy = sy0 + (sh == dh ? y : y * sh / dh);
            const rivendata::Texel *srcRow =
                img.texels.data() + static_cast<std::size_t>(sy) * img.width;
            for (int x = 0; x < dw; ++x)
                dst[x] = keep[x] = srcRow[sx0 + (sw == dw ? x : x * sw / dw)];
        }

        markRows(y0, dh);
        drewAnything = true;
    }

    if (drewAnything)
        ++generation_;
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
    ++generation_;
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
    ++generation_;
}

void CardSurface::adoptBuffer(int buf)
{
    if (texels_ == nullptr || buf < 0 || buf >= BgSurface::kBuffers)
        return;

    // A buffer row is a card row -- both are kViewW wide and the layers rest at
    // the letterbox offset so that bitmap row r IS card row r (BgSurface.hpp) --
    // so the picture area copies as one run with no per-row stride to walk.
    static_assert(BgSurface::kBufW == kViewW,
                  "a buffer row must be a card row for this to be one copy");
    const Texel *src = BgSurface::pixels(buf);
    tonccpy(texels_, src, kPlaneBytes);
    // BOTH planes: this frame is the card now, not something laid over it, so it
    // has to survive refreshFromClean the way a drawing does.
    std::memcpy(clean_, texels_, kPlaneBytes);

    // Nothing is on top any more. The fullscreen movie covered the whole screen,
    // and whatever an overlay had written into texels_ before it went away with
    // the pixels just overwritten -- leaving those rows marked would have the
    // next refresh try to take back a frame that no longer exists.
    overlayRows_ = 0;
    ++generation_;

    for (int b = 0; b < BgSurface::kBuffers; ++b)
        dirty_[b] = kAllDirty;
    dirty_[buf] = 0; // this one IS the card; sending it back would be a copy to itself
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
