#include "ZoomView.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "DebugLog.hpp"
#include "RivenData.hpp"
#include "data/ImageFile.hpp"
#include "render/BgSurface.hpp"

namespace rivenrt
{

ZoomView zoomView;

namespace
{
    /// The whole screen, letterbox off. A zoom is a 1:1 crop of a bigger
    /// picture and there is nothing to frame it for.
    ///
    /// This was the card view for a while, and the reason was real: rows
    /// kViewH..255 of every buffer are reserved. BgSurface fills them
    /// transparent ONCE and never again (BgSurface.hpp:32-40) and CardSurface's
    /// republish is bounded by kViewH, so anything written there used to be
    /// permanent -- the band under the card kept the zoom's pixels for the rest
    /// of the run. What was missing was a way to put them back;
    /// BgSurface::resetBuffer is it, and Engine::toggleZoom calls it on the way
    /// out. The 27 rows are worth the call: they are 16% more picture.
    constexpr int kWindowW = rivendata::kScreenW;
    constexpr int kWindowH = rivendata::kScreenH;

    static_assert(kMaxRpizW == rivendata::kCardW && kMaxRpizH == rivendata::kCardH,
                  "the .rpiz cap and the card size have drifted apart");

    /// The display alternates between two buffers, so a moved window has to be
    /// drawn twice before it is on screen whichever one is showing.
    constexpr int kRedrawsPerMove = 2;

    /// What the patches may hold between them, in bytes. Every overlay Riven
    /// draws on a card fits many times over -- the widest is the dome's slider
    /// strip at 12 KB -- so this is a backstop against a card that draws
    /// something unforeseen, not a budget anything is expected to spend.
    constexpr std::size_t kPatchBudget = 96 * 1024;

    /// And what the decoded overlay sources may hold. Same argument: the two
    /// dome strips together are under 20 KB, and this exists so that a cache
    /// cannot become the thing that runs the machine out of memory.
    constexpr std::size_t kSourceBudget = 64 * 1024;

    /// A source bigger than this is not cached at all. A full-card twin is
    /// 238 KB and would evict everything to hold something that is only ever
    /// asked for once -- if a script ever draws a whole still as an overlay,
    /// that is the case this refuses.
    constexpr std::size_t kSourceMaxBytes = 48 * 1024;

    /// Overlap of two half-open ranges, into [lo, hi).
    inline bool overlap(int aLo, int aHi, int bLo, int bHi, int &lo, int &hi)
    {
        lo = aLo > bLo ? aLo : bLo;
        hi = aHi < bHi ? aHi : bHi;
        return hi > lo;
    }
} // namespace

bool ZoomView::load(const std::string &path, bool keepOrigin)
{
    RpizImage img;
    std::string error;
    if (!loadRpizImage(path, img, error))
    {
        DebugLog::warn("no zoom art: %s", error.c_str());
        return false;
    }
    if (img.width < kWindowW || img.height < kWindowH)
    {
        // A picture smaller than the window has nothing to pan and nothing to
        // show that the card view is not already showing.
        DebugLog::warn("zoom: %dx%d is not bigger than the view", img.width, img.height);
        return false;
    }

    // Committed only from here: everything above can fail, and reopen() promises
    // that a page which will not load leaves the one on screen alone.
    const int wasX = originX_;
    const int wasY = originY_;
    const bool had = active();

    clearPatches();
    indices_ = std::move(img.indices);
    for (int i = 0; i < rivendata::kPaletteEntries; ++i)
        palette_[i] = img.palette[i];
    width_ = img.width;
    height_ = img.height;

    if (keepOrigin && had)
    {
        originX_ = std::clamp(wasX, 0, width_ - kWindowW);
        originY_ = std::clamp(wasY, 0, height_ - kWindowH);
    }
    else
    {
        // Open on the middle of the picture, which is where the card view's
        // centre was: the player pressed the button while looking at something,
        // and the middle is the least surprising guess at what.
        originX_ = (width_ - kWindowW) / 2;
        originY_ = (height_ - kWindowH) / 2;
    }
    redrawsLeft_ = kRedrawsPerMove;

    DebugLog::log("ZOOM %dx%d, window at %d,%d", width_, height_, originX_, originY_);
    return true;
}

bool ZoomView::open(const std::string &path)
{
    close();
    if (!load(path, false))
        return false;

    // Both halves of taking the screen. The letterbox goes back on in
    // Engine::leaveZoom and not in close(), because open() calls close() first
    // and would turn it on between the two.
    bgs.beginMovieTakeover();
    bgs.setLetterbox(false);
    return true;
}

bool ZoomView::reopen(const std::string &path)
{
    // The screen is already taken, so this is the load and nothing else.
    return active() && load(path, true);
}

void ZoomView::close()
{
    if (!active())
        return;

    // Freed rather than kept: 238 KB is a sixteenth of the machine, and the
    // next card will want it for its own art.
    indices_.clear();
    indices_.shrink_to_fit();
    width_ = height_ = 0;
    redrawsLeft_ = 0;
    clearPatches();
    sources_.clear();
    sources_.shrink_to_fit();
    sourceBytes_ = 0;
}

void ZoomView::clearPatches()
{
    patches_.clear();
    patches_.shrink_to_fit();
    patchBytes_ = 0;
}

void ZoomView::pan(int dx, int dy)
{
    if (!active())
        return;
    const int x = std::clamp(originX_ + dx, 0, width_ - kWindowW);
    const int y = std::clamp(originY_ + dy, 0, height_ - kWindowH);
    if (x == originX_ && y == originY_)
        return;
    originX_ = x;
    originY_ = y;
    redrawsLeft_ = kRedrawsPerMove;
}

const RpizImage *ZoomView::source(const std::string &path)
{
    for (Source &s : sources_)
        if (s.path == path)
        {
            s.used = ++sourceClock_;
            return &s.img;
        }

    RpizImage img;
    std::string error;
    if (!loadRpizImage(path, img, error))
    {
        // Not a warning worth repeating on every tick of a drag: the engine
        // falls back to the card's own pixels and says so once.
        DebugLog::log("zoom: no twin (%s)", error.c_str());
        return nullptr;
    }

    const std::size_t bytes = img.indices.size();
    if (bytes > kSourceMaxBytes)
    {
        // Used once and thrown away. The caller has already copied what it
        // wanted out of it by the time this matters, so returning a pointer into
        // a temporary would be wrong -- keep it, and evict it next time round.
        sources_.push_back(Source{path, std::move(img), 0});
        const RpizImage *const kept = &sources_.back().img;
        sourceBytes_ += bytes;
        return kept;
    }

    while (!sources_.empty() && sourceBytes_ + bytes > kSourceBudget)
    {
        auto oldest = sources_.begin();
        for (auto it = sources_.begin(); it != sources_.end(); ++it)
            if (it->used < oldest->used)
                oldest = it;
        sourceBytes_ -= oldest->img.indices.size();
        sources_.erase(oldest);
    }

    sources_.push_back(Source{path, std::move(img), ++sourceClock_});
    sourceBytes_ += bytes;
    return &sources_.back().img;
}

namespace
{
    /// Cut `dst` down to the picture. The caller keeps the rectangle it asked
    /// for as well, because a destination clipped at the far edge is a CROP of
    /// the source and not a squash of it: the scale is decided by the rectangle
    /// the drawing meant, and only fewer of its pixels are written.
    ///
    /// Only the far edges are cut. A negative corner would have to move the
    /// source too, and no caller has one -- these destinations are card
    /// rectangles, and the card starts at zero.
    bool clipDst(int pictureW, int pictureH, rivendata::Rect &dst)
    {
        if (dst.width() <= 0 || dst.height() <= 0 || dst.left < 0 || dst.top < 0)
            return false;
        if (dst.right > pictureW)
            dst.right = static_cast<std::int16_t>(pictureW);
        if (dst.bottom > pictureH)
            dst.bottom = static_cast<std::int16_t>(pictureH);
        return dst.width() > 0 && dst.height() > 0;
    }
} // namespace

bool ZoomView::stamp(const std::string &rpizPath, const rivendata::Rect &srcIn,
                     const rivendata::Rect &dstIn)
{
    if (!active())
        return false;

    // THE SIZE FIRST, before a byte is read or allocated. What a patch costs is
    // decided by its destination alone, and refusing here is the difference
    // between saying no to a 476 KB overlay and finding out by asking a 4 MB
    // machine for it -- twice, once for the decoded source and once for the
    // patch. Only the destination is needed to know that, and it comes from the
    // caller.
    rivendata::Rect dst = dstIn;
    if (!clipDst(width_, height_, dst) || !affordable(dst))
        return false;

    const RpizImage *const img = source(rpizPath);
    if (img == nullptr || !img->valid())
        return false;

    rivendata::Rect src = srcIn;
    if (src.left < 0)
        src.left = 0;
    if (src.top < 0)
        src.top = 0;
    if (src.right > img->width)
        src.right = static_cast<std::int16_t>(img->width);
    if (src.bottom > img->height)
        src.bottom = static_cast<std::int16_t>(img->height);
    // The extent the DRAWING meant, which is what the scale is against -- see
    // clipDst: a destination cut at the picture's edge is a crop, not a squash.
    const int wantW = dstIn.width();
    const int wantH = dstIn.height();
    if (src.width() <= 0 || src.height() <= 0)
        return false;

    const int dw = dst.width();
    const int dh = dst.height();
    const int sw = src.width();
    const int sh = src.height();

    std::vector<rivendata::Texel> texels(static_cast<std::size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y)
    {
        // Nearest neighbour, like CardSurface's own section draw: the common
        // case is 1:1 -- the twin is the tBMP and the destination is in the same
        // space it was drawn in -- and where it is not, the caller asked for a
        // scale the card is showing too. Against the UNCLIPPED extent, so that
        // clipping crops.
        const int sy = src.top + (sh == wantH ? y : y * sh / wantH);
        const std::uint8_t *const in =
            img->indices.data() + static_cast<std::size_t>(sy) * img->width;
        rivendata::Texel *const out = texels.data() + static_cast<std::size_t>(y) * dw;
        for (int x = 0; x < dw; ++x)
            out[x] = img->palette[in[src.left + (sw == wantW ? x : x * sw / wantW)]];
    }

    commit(dst, std::move(texels));
    return true;
}

bool ZoomView::affordable(const rivendata::Rect &dst) const
{
    const std::size_t bytes = static_cast<std::size_t>(dst.width()) * dst.height()
                              * sizeof(rivendata::Texel);
    for (const Patch &p : patches_)
        if (p.dst.left == dst.left && p.dst.top == dst.top && p.dst.right == dst.right
            && p.dst.bottom == dst.bottom)
            return true; // the same rectangle again: it costs what it already costs

    if (patchBytes_ + bytes <= kPatchBudget)
        return true;

    DebugLog::warn("zoom: %zu KB of overlays is the limit; one is not drawn",
                   kPatchBudget / 1024);
    return false;
}

void ZoomView::commit(const rivendata::Rect &dst, std::vector<rivendata::Texel> &&texels)
{
    const std::size_t bytes = texels.size() * sizeof(rivendata::Texel);
    for (Patch &p : patches_)
    {
        // The same rectangle again: the dome's slider strip, every tick of a
        // drag. Replaced rather than appended, which is what keeps a drag from
        // growing the list at all -- and what keeps the ORDER stable, so a
        // script that paints over an earlier draw still replays that way round.
        if (p.dst.left == dst.left && p.dst.top == dst.top && p.dst.right == dst.right
            && p.dst.bottom == dst.bottom)
        {
            patchBytes_ += bytes - p.texels.size() * sizeof(rivendata::Texel);
            p.texels = std::move(texels);
            redrawsLeft_ = kRedrawsPerMove;
            return;
        }
    }

    patches_.push_back(Patch{dst, std::move(texels)});
    patchBytes_ += bytes;
    redrawsLeft_ = kRedrawsPerMove;
}

void ZoomView::stampFromCard(const rivendata::Texel *cardPlane, const rivendata::Rect &dstIn)
{
    if (!active() || cardPlane == nullptr)
        return;

    // No source rectangle: the card view holds this same rectangle, 0.42x down,
    // so the source is the destination through toDsX/toDsY.
    rivendata::Rect dst = dstIn;
    if (!clipDst(width_, height_, dst) || !affordable(dst))
        return;

    const int dw = dst.width();
    const int dh = dst.height();
    std::vector<rivendata::Texel> texels(static_cast<std::size_t>(dw) * dh);
    for (int y = 0; y < dh; ++y)
    {
        int sy = rivendata::toDsY(dst.top + y);
        if (sy >= rivendata::kViewH)
            sy = rivendata::kViewH - 1;
        const rivendata::Texel *const in =
            cardPlane + static_cast<std::size_t>(sy) * rivendata::kViewW;
        rivendata::Texel *const out = texels.data() + static_cast<std::size_t>(y) * dw;
        for (int x = 0; x < dw; ++x)
        {
            int sx = rivendata::toDsX(dst.left + x);
            if (sx >= rivendata::kViewW)
                sx = rivendata::kViewW - 1;
            out[x] = in[sx];
        }
    }

    commit(dst, std::move(texels));
}

void ZoomView::redraw(int buffer)
{
    rivendata::Texel *const dst = BgSurface::pixels(buffer);
    const std::uint8_t *src = indices_.data() + static_cast<std::size_t>(originY_) * width_
                              + originX_;

    // 49 152 pixels through a 256-entry table, once per moved frame. Written as
    // a row at a time with both pointers advanced by their own strides so the
    // compiler has no addressing to recompute: the source rows are 608 apart and
    // the destination's are 256, and neither depends on the loop variable.
    for (int y = 0; y < kWindowH; ++y)
    {
        rivendata::Texel *out = dst + static_cast<std::size_t>(y) * BgSurface::kBufW;
        const std::uint8_t *in = src + static_cast<std::size_t>(y) * width_;
        for (int x = 0; x < kWindowW; ++x)
            out[x] = palette_[in[x]];
    }

    // The overlays, on top and in the order they were drawn on the card -- a
    // script that paints over its own earlier draw has to come out the same way
    // round here. Only the part of each inside the window, so a patch off the
    // edge of it costs the two comparisons and nothing else.
    for (const Patch &p : patches_)
    {
        int x0 = 0;
        int x1 = 0;
        int y0 = 0;
        int y1 = 0;
        if (!overlap(p.dst.left, p.dst.right, originX_, originX_ + kWindowW, x0, x1))
            continue;
        if (!overlap(p.dst.top, p.dst.bottom, originY_, originY_ + kWindowH, y0, y1))
            continue;

        const int stride = p.dst.width();
        const std::size_t bytes = static_cast<std::size_t>(x1 - x0) * sizeof(rivendata::Texel);
        for (int y = y0; y < y1; ++y)
        {
            const rivendata::Texel *const in =
                p.texels.data() + static_cast<std::size_t>(y - p.dst.top) * stride
                + (x0 - p.dst.left);
            rivendata::Texel *const out =
                dst + static_cast<std::size_t>(y - originY_) * BgSurface::kBufW
                + (x0 - originX_);
            std::memcpy(out, in, bytes);
        }
    }
}

void ZoomView::publish()
{
    if (!active() || redrawsLeft_ == 0)
        return;

    // One buffer per frame, and only the one that is off screen. The window is
    // 96 KB of halfword writes into VRAM; drawing both in a frame would spend
    // more of it on a pan than the pan is worth, and the flip means the player
    // sees the new window on the very next frame either way.
    //
    // BgSurface::vblank keeps the back layer off the parked card, so the buffer
    // this asks for is never the one holding it.
    redraw(bgs.backBuffer());
    --redrawsLeft_;
    bgs.requestFlip();
}

} // namespace rivenrt
