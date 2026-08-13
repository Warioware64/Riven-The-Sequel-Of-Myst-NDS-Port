#pragma once

// Riven's screen transitions: the ids opcode 18 carries, and the scroll offsets
// that animate one on the DS's two bitmap backgrounds.
//
// This lives in shared/ for one reason: the arithmetic below is the part of the
// renderer most likely to be off by one, and it needs no hardware to check. The
// host test (tools/riven-convert/tests/test_transition.cpp) compiles this header
// and nothing else. Keep it free of <nds.h>.
//
// The ids are ScummVM's RivenTransition (riven_graphics.h:41-52) verbatim, and
// the direction algebra is derived from makeDirectionalInitalArea and
// TransitionEffectPan::drawFrame (riven_graphics.cpp:61-85, 221-236).

#include <cstdint>

#include "RivenData.hpp"

namespace rivendata
{

enum class Transition : int
{
    None = -1,

    // "Wipes": the new image is revealed in place and neither image moves.
    // ScummVM notes that none of 0..11 are used in the shipped data
    // (riven_graphics.cpp:41), so they are aliased onto the matching pan rather
    // than given their own window-clipped implementation.
    WipeLeft = 0,
    WipeRight = 1,
    WipeUp = 2,
    WipeDown = 3,

    // The pans. Both images move; the new one enters from the opposite side to
    // the name, which is the direction the CAMERA moves.
    PanLeft = 12,   ///< new enters from the right
    PanRight = 13,  ///< new enters from the left
    PanUp = 14,     ///< new enters from the bottom
    PanDown = 15,   ///< new enters from the top

    Blend = 16,
    Blend2 = 17,    ///< identical to Blend; the data uses it on tspit card 155
};

/// Whether `t` moves the two images past each other (as opposed to blending
/// them in place, or doing nothing).
inline constexpr bool isPan(Transition t)
{
    switch (t)
    {
    case Transition::WipeLeft:
    case Transition::WipeRight:
    case Transition::WipeUp:
    case Transition::WipeDown:
    case Transition::PanLeft:
    case Transition::PanRight:
    case Transition::PanUp:
    case Transition::PanDown:
        return true;
    default:
        return false;
    }
}

inline constexpr bool isBlend(Transition t)
{
    return t == Transition::Blend || t == Transition::Blend2;
}

/// The wipes reuse the pan of the same direction.
inline constexpr Transition panForm(Transition t)
{
    switch (t)
    {
    case Transition::WipeLeft:  return Transition::PanLeft;
    case Transition::WipeRight: return Transition::PanRight;
    case Transition::WipeUp:    return Transition::PanUp;
    case Transition::WipeDown:  return Transition::PanDown;
    default:                    return t;
    }
}

/// Where each of the two backgrounds must be scrolled to, part-way through a pan.
///
/// The convention is libnds's bgSetScroll: screen pixel (sx, sy) samples bitmap
/// pixel (sx + x, sy + y). A background is 256x256 with only rows 0..kViewH-1
/// holding picture, and its display-area-overflow bit is clear, so anything
/// sampled outside it is transparent -- which is what makes the incoming image
/// slide in from off-view with no clipping work.
struct BgScroll
{
    int oldX, oldY;
    int newX, newY;
};

/// The resting transform: the card view sits kViewOffsetY rows down the screen,
/// so bitmap row 0 must appear at screen row kViewOffsetY.
inline constexpr int kRestY = -kViewOffsetY;

/// `frame` counts 1..frames; frame == frames must land exactly on rest for both
/// layers, which is what lets the caller stop without a correcting write.
inline constexpr BgScroll transitionScroll(Transition t, int frame, int frames)
{
    if (frames <= 0)
        return BgScroll{0, kRestY, 0, kRestY};
    if (frame < 0)
        frame = 0;
    if (frame > frames)
        frame = frames;

    // Travel is the full width or the full picture height -- the incoming image
    // starts exactly one view off and arrives exactly at rest.
    const int px = kViewW * frame / frames;
    const int py = kViewH * frame / frames;

    switch (panForm(t))
    {
    case Transition::PanLeft:
        return BgScroll{px, kRestY, px - kViewW, kRestY};
    case Transition::PanRight:
        return BgScroll{-px, kRestY, kViewW - px, kRestY};
    case Transition::PanUp:
        return BgScroll{0, kRestY + py, 0, -(kViewH + kViewOffsetY) + py};
    case Transition::PanDown:
        return BgScroll{0, kRestY - py, 0, (kViewH - kViewOffsetY) - py};
    default:
        // Not a pan: both layers stay at rest and the effect is elsewhere
        // (an alpha blend, or nothing at all).
        return BgScroll{0, kRestY, 0, kRestY};
    }
}

} // namespace rivendata
