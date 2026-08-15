#pragma once

// The dome sliders, shared by five islands.
//
// Every dome in Riven is the same puzzle: twenty-five slots, five sliders in
// them, and a combination the player has to read off the island and reproduce.
// ScummVM makes it a base class the five stacks inherit (riven_stacks/domespit.
// cpp); jspit, bspit, gspit, pspit and tspit each pass in their own slider
// artwork and their own first hotspot, and nothing else differs.
//
// The port keeps that shape -- these are free functions taking the per-dome
// parameters -- so that the four remaining islands are five lines each rather
// than another copy of this file.
//
// THE STATE LIVES IN A VARIABLE. ScummVM holds it on the stack object, which
// means it resets every time the player leaves the island and comes back
// (domespit.cpp:40, and a RivenStack is constructed on each changeToStack).
// Riding in the variable map instead costs nothing, is saved with the game for
// free, and is what the player would expect: sliders they set stay set.

#include <cstdint>

namespace rivenrt
{

class Engine;

/// One dome's fixed facts. ScummVM passes the two names to the DomeSpit
/// constructor and the start hotspot to every call (domespit.cpp:36-41).
struct DomeConfig
{
    /// BLST id of slot 1. The twenty-five slots are consecutive from here --
    /// jspit's are 10..34.
    std::uint16_t startBlstId;
    /// The card-resource names of the slider strip and its empty background,
    /// e.g. "jsliders.190" and "jsliderbg.190". Resolved through
    /// Engine::bitmapIdForName, which prefixes the card id.
    const char *sliderBmpName;
    const char *sliderBgBmpName;
    /// Left edge of the area the strip is cut from, in card coordinates. 200 on
    /// four of the five domes and 198 on pspit's, which ScummVM writes as a
    /// stack test in the middle of the draw (domespit.cpp:200-206). A field
    /// instead, because it is a fact about one dome and not about the code.
    std::int16_t areaLeft = 200;
};

/// The five entry points, one per external command shape.

/// DomeSpit::resetDomeSliders (domespit.cpp:66-97). Slide them all back to the
/// right, one tick at a time so the player sees it happen.
void resetDomeSliders(Engine &engine, const DomeConfig &dome);

/// DomeSpit::dragDomeSlider (domespit.cpp:157-198). The whole drag, from the
/// press that started it to the release that ends it.
void dragDomeSlider(Engine &engine, const DomeConfig &dome);

/// DomeSpit::checkSliderCursorChange (domespit.cpp:115-124). An open hand over
/// a slider, the ordinary pointer over a gap.
void checkSliderCursorChange(Engine &engine, const DomeConfig &dome);

/// DomeSpit::runDomeButtonMovie (domespit.cpp:43-48).
void runDomeButtonMovie(Engine &engine);

/// DomeSpit::runDomeCheck (domespit.cpp:50-64). Did the player press the button
/// while the golden symbol was showing?
void runDomeCheck(Engine &engine);

/// DomeSpit::checkDomeSliders (domespit.cpp:99-113). Reset and Open share a
/// rectangle and which of them is live is the puzzle's only answer to "have I
/// got it right?".
///
/// Called from inside the drag and the reset, and separately by the
/// `..._opencard` command that tspit, bspit, gspit and pspit each have and
/// jspit does not -- which is why this only became public with them.
void checkDomeSliders(Engine &engine);

} // namespace rivenrt
