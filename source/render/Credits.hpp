#pragma once

// RivenGraphics::beginCredits / updateCredits (riven_graphics.cpp:671-726) and
// the loop in RivenStack::runCredits that drives them (riven_stack.cpp:217-270).
//
// The last thing Riven shows. Two title cards fade up and hold for four seconds
// each, and then seventeen images of names scroll past a row at a time until
// they run out.
//
// THE SCROLL IS THE HARDWARE'S. ScummVM moves the whole screen up one row every
// frame and writes a new row at the bottom -- a 608x392 memmove sixty times a
// second, which is nothing on a PC and would be 470 KB/frame here. A BgSurface
// buffer is 256x256 behind a 192-row window and the 2D engine wraps it, so the
// same effect is a scroll register and ONE row of pixels: 512 bytes a step
// (BgSurface::setScrollY).
//
// THE SPEED IS ScummVM'S, MEASURED IN SOURCE ROWS. ScummVM advances one row of a
// 392-row image per 1/60 s, so the roll takes 17 * 392 / 60 = 111 seconds. The
// converted images are 192 rows (riven/Credits.hpp says why), so advancing one
// of OUR rows per frame would run the credits at twice speed and finish in
// under a minute. The accumulator below advances 192/392 of a row per frame
// instead, which is the same wall clock and the same apparent speed.
//
// SKIPPABLE ON ANY BUTTON, which ScummVM's loop is not. Its `while
// (!hasGameEnded() && !endOfVideo())` runs on a machine you can alt-tab away
// from; two silent minutes with no way out of them is a different object on a
// handheld, and the ending has already been watched by the time this starts.

#include <cstdint>

namespace rivenrt
{

class Engine;

/// Roll the credits, then return with the screen handed back to the caller.
///
/// `delayMs` is the pause between the ending's video running out and the first
/// title card -- ScummVM's `delay` argument, which is per-ending and measured
/// against those videos (riven_stack.cpp:244).
///
/// Does nothing, quickly, when the converted art is missing: an install whose
/// extras stage did not run still has to be able to finish the game.
void runCredits(Engine &engine, std::uint32_t delayMs);

} // namespace rivenrt
