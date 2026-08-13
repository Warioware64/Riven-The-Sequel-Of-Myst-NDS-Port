// The screen-transition arithmetic, checked on the host.
//
// shared/RivenTransition.hpp is renderer code, not converter code, and it is
// tested here for one reason: it is the piece of the bitmap-background renderer
// most likely to be off by one, and it needs no hardware to check. On a DS a
// wrong sign shows up as a card sliding the wrong way past another card, which
// is exactly the sort of thing that is hard to read at 60 Hz and trivial to
// assert.
//
// The convention under test is libnds's bgSetScroll: screen pixel (sx, sy)
// samples background pixel (sx + x, sy + y). So a POSITIVE x moves the picture
// LEFT on screen.

#include <cstdio>
#include <cstdlib>

#include "RivenTransition.hpp"

using namespace rivendata;

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++g_failures;
        }
    }

    /// The screen span, in the axis the transition moves along, that a layer at
    /// this scroll actually covers. Outside the 256x256 background is
    /// transparent, so a layer scrolled entirely off contributes nothing.
    struct Span
    {
        int lo, hi; ///< half open, in screen pixels; empty when hi <= lo
        int size() const { return hi > lo ? hi - lo : 0; }
    };

    /// Where background rows 0..kViewH-1 land on screen for a vertical scroll y.
    Span vertical(int y)
    {
        int lo = -y;
        int hi = kViewH - y;
        if (lo < kViewOffsetY)
            lo = kViewOffsetY;
        if (hi > kViewOffsetY + kViewH)
            hi = kViewOffsetY + kViewH;
        return Span{lo, hi};
    }

    /// Where background columns 0..kViewW-1 land on screen for a horizontal
    /// scroll x.
    Span horizontal(int x)
    {
        int lo = -x;
        int hi = kViewW - x;
        if (lo < 0)
            lo = 0;
        if (hi > kViewW)
            hi = kViewW;
        return Span{lo, hi};
    }

    const Transition kPans[] = {Transition::PanLeft, Transition::PanRight,
                                Transition::PanUp, Transition::PanDown};
    const char *kPanNames[] = {"PanLeft", "PanRight", "PanUp", "PanDown"};

    bool isHorizontal(Transition t)
    {
        return t == Transition::PanLeft || t == Transition::PanRight;
    }
} // namespace

int main()
{
    constexpr int kFrames = 18; // BgSurface::kPanFrames

    // --- classification ----------------------------------------------------
    check(!isPan(Transition::None) && !isBlend(Transition::None),
          "None is neither a pan nor a blend");
    check(isBlend(Transition::Blend) && isBlend(Transition::Blend2),
          "both blend ids are blends");
    check(!isPan(Transition::Blend), "a blend is not a pan");
    for (int i = 0; i < 4; ++i)
        check(isPan(kPans[i]), "every pan classifies as one");
    // The wipes are aliased onto the pans; that aliasing is deliberate and is
    // what the renderer relies on, so it is asserted rather than assumed.
    check(panForm(Transition::WipeLeft) == Transition::PanLeft, "WipeLeft aliases PanLeft");
    check(panForm(Transition::WipeRight) == Transition::PanRight, "WipeRight aliases PanRight");
    check(panForm(Transition::WipeUp) == Transition::PanUp, "WipeUp aliases PanUp");
    check(panForm(Transition::WipeDown) == Transition::PanDown, "WipeDown aliases PanDown");
    check(panForm(Transition::Blend) == Transition::Blend, "panForm leaves a blend alone");

    for (int p = 0; p < 4; ++p)
    {
        const Transition t = kPans[p];
        const char *name = kPanNames[p];
        const bool horiz = isHorizontal(t);
        const int travel = horiz ? kViewW : kViewH;

        // --- the endpoints, which are the whole contract -------------------
        {
            const BgScroll a = transitionScroll(t, 0, kFrames);
            check(a.oldX == 0 && a.oldY == kRestY, "a pan starts with the old card at rest");
            const Span in = horiz ? horizontal(a.newX) : vertical(a.newY);
            check(in.size() == 0, "a pan starts with the new card entirely out of view");

            const BgScroll z = transitionScroll(t, kFrames, kFrames);
            check(z.newX == 0 && z.newY == kRestY,
                  "a pan ends with the new card exactly at rest");
            const Span out = horiz ? horizontal(z.oldX) : vertical(z.oldY);
            check(out.size() == 0, "a pan ends with the old card entirely out of view");
            if (g_failures != 0)
                std::printf("  (while checking %s)\n", name);
        }

        // --- monotonic, and the two never overlap --------------------------
        //
        // The non-overlap matters on hardware: the incoming card is on the layer
        // with the LOWER priority during a pan, so if the two ever covered the
        // same pixel the new card would be hidden behind the old one.
        int prevNew = -1;
        int prevOld = -1;
        for (int f = 0; f <= kFrames; ++f)
        {
            const BgScroll s = transitionScroll(t, f, kFrames);
            const Span oldSpan = horiz ? horizontal(s.oldX) : vertical(s.oldY);
            const Span newSpan = horiz ? horizontal(s.newX) : vertical(s.newY);

            check(newSpan.size() >= prevNew, "the incoming card never shrinks");
            check(prevOld < 0 || oldSpan.size() <= prevOld,
                  "the outgoing card never grows");
            prevNew = newSpan.size();
            prevOld = oldSpan.size();

            const int overlapLo = oldSpan.lo > newSpan.lo ? oldSpan.lo : newSpan.lo;
            const int overlapHi = oldSpan.hi < newSpan.hi ? oldSpan.hi : newSpan.hi;
            check(overlapHi <= overlapLo, "the two cards never cover the same pixel");

            // Together they always cover the whole view: any gap would show the
            // black backdrop through the middle of a slide.
            check(oldSpan.size() + newSpan.size() == (horiz ? kViewW : kViewH),
                  "the two cards together always fill the view");

            if (g_failures != 0)
            {
                std::printf("  (while checking %s at frame %d/%d: "
                            "old %d..%d, new %d..%d)\n",
                            name, f, kFrames, oldSpan.lo, oldSpan.hi, newSpan.lo,
                            newSpan.hi);
                break;
            }
        }
        check(prevNew == travel, "the incoming card ends up covering the whole view");
    }

    // --- direction: which edge the new card comes in from -------------------
    //
    // ScummVM's names are the direction the CAMERA moves, so the new image
    // enters from the opposite side (riven_graphics.cpp:61-85).
    {
        const int mid = 9; // half way through an 18-frame pan
        check(horizontal(transitionScroll(Transition::PanLeft, mid, kFrames).newX).lo
                  > kViewW / 4,
              "PanLeft brings the new card in from the right");
        check(horizontal(transitionScroll(Transition::PanRight, mid, kFrames).newX).lo == 0,
              "PanRight brings the new card in from the left");
        check(vertical(transitionScroll(Transition::PanUp, mid, kFrames).newY).hi
                  == kViewOffsetY + kViewH,
              "PanUp brings the new card in from the bottom");
        check(vertical(transitionScroll(Transition::PanDown, mid, kFrames).newY).lo
                  == kViewOffsetY,
              "PanDown brings the new card in from the top");
    }

    // --- degenerate inputs --------------------------------------------------
    {
        const BgScroll none = transitionScroll(Transition::None, 3, kFrames);
        check(none.oldX == 0 && none.oldY == kRestY && none.newX == 0
                  && none.newY == kRestY,
              "a non-pan leaves both layers at rest");
        const BgScroll blend = transitionScroll(Transition::Blend, 3, kFrames);
        check(blend.oldY == kRestY && blend.newY == kRestY,
              "a blend leaves both layers at rest -- the effect is BLDALPHA");

        const BgScroll zero = transitionScroll(Transition::PanLeft, 1, 0);
        check(zero.oldY == kRestY && zero.newY == kRestY, "zero frames does not divide by zero");
        const BgScroll over = transitionScroll(Transition::PanLeft, 99, kFrames);
        const BgScroll end = transitionScroll(Transition::PanLeft, kFrames, kFrames);
        check(over.newX == end.newX && over.oldX == end.oldX,
              "a frame past the end clamps to the end");
        const BgScroll under = transitionScroll(Transition::PanLeft, -5, kFrames);
        const BgScroll begin = transitionScroll(Transition::PanLeft, 0, kFrames);
        check(under.newX == begin.newX, "a negative frame clamps to the start");
    }

    // --- geometry the renderer depends on -----------------------------------
    check(kRestY == -kViewOffsetY, "rest puts background row 0 at the view's top");
    check(kViewH <= 256 && kViewW <= 256,
          "the card view fits a 256x256 background");

    if (g_failures != 0)
    {
        std::printf("%d transition checks failed\n", g_failures);
        return 1;
    }
    std::printf("transition: all checks passed\n");
    return 0;
}
