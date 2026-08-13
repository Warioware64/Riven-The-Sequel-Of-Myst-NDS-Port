#include "ZoomView.hpp"

#include <algorithm>
#include <cstdio>

#include "DebugLog.hpp"
#include "RivenData.hpp"
#include "data/ImageFile.hpp"
#include "render/BgSurface.hpp"

namespace rivenrt
{

ZoomView zoomView;

namespace
{
    /// The window is the CARD VIEW, not the whole screen, and the letterbox
    /// stays on while the viewer is up.
    ///
    /// The obvious thing is to use all 192 rows -- it is a 1:1 crop of a bigger
    /// picture, so why frame it -- and it is wrong, because rows kViewH..255 of
    /// every buffer are reserved. BgSurface fills them transparent ONCE and
    /// never again (BgSurface.hpp:32-40), and CardSurface's republish is bounded
    /// by kViewH, so anything written there is permanent: the band under the
    /// card kept the zoom's pixels for the rest of the run.
    ///
    /// Framing the window as the card view means those rows are never written
    /// and there is nothing to put back. The 27 rows are not lost, only moved --
    /// the pannable range grows by exactly as much.
    constexpr int kWindowW = rivendata::kViewW;
    constexpr int kWindowH = rivendata::kViewH;

    static_assert(kMaxRpizW == rivendata::kCardW && kMaxRpizH == rivendata::kCardH,
                  "the .rpiz cap and the card size have drifted apart");

    /// The display alternates between two buffers, so a moved window has to be
    /// drawn twice before it is on screen whichever one is showing.
    constexpr int kRedrawsPerMove = 2;
} // namespace

bool ZoomView::open(const std::string &path)
{
    close();

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

    indices_ = std::move(img.indices);
    for (int i = 0; i < rivendata::kPaletteEntries; ++i)
        palette_[i] = img.palette[i];
    width_ = img.width;
    height_ = img.height;

    // Open on the middle of the picture, which is where the card view's centre
    // was: the player pressed the button while looking at something, and the
    // middle is the least surprising guess at what.
    originX_ = (width_ - kWindowW) / 2;
    originY_ = (height_ - kWindowH) / 2;
    redrawsLeft_ = kRedrawsPerMove;

    DebugLog::log("ZOOM %dx%d, window at %d,%d", width_, height_, originX_, originY_);

    bgs.beginMovieTakeover();
    return true;
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
