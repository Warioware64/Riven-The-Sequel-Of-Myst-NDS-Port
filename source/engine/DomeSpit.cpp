#include "engine/DomeSpit.hpp"

#include <string>
#include <vector>

#include "DebugLog.hpp"
#include "engine/Engine.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Five sliders in the rightmost five of twenty-five slots. Bit 24 is slot
    /// 0, so this is slots 20..24 (domespit.cpp:33).
    constexpr std::uint32_t kDefaultSliderState = 0x01F00000;
    constexpr int kSlotCount = 25;

    /// The top of the area the strip is drawn into, in card coordinates
    /// (domespit.cpp:201). The same on all five domes; only the left edge
    /// differs, and that is DomeConfig::areaLeft.
    constexpr int kAreaTop = 250;

    /// The x range the pointer is clamped into before a slot is looked up
    /// (domespit.cpp:131-132, CLIP to [211, 406]). CARD coordinates, and they
    /// stay card coordinates: clamping in DS pixels first would round the two
    /// ends onto slots the player cannot otherwise reach.
    constexpr int kMinX = 211;
    constexpr int kMaxX = 407;

    int popcount(std::uint32_t v)
    {
        int n = 0;
        for (; v != 0; v &= v - 1)
            ++n;
        return n;
    }

    /// The state, out of the variable map.
    ///
    /// Zero means "never set", because a real state always has exactly five bits
    /// -- and so does anything else with the wrong number of them, which is a
    /// corrupted save rather than a puzzle position. ScummVM asserts on that
    /// (domespit.cpp:94-96); a handheld cannot afford to, and a dome that
    /// quietly starts over is much better than one that cannot be used.
    std::uint32_t sliderState(Engine &e)
    {
        const std::uint32_t v = e.vars().get(VarId::DomeSliders);
        return popcount(v) == 5 ? v : kDefaultSliderState;
    }

    void setSliderState(Engine &e, std::uint32_t v)
    {
        e.vars().set(VarId::DomeSliders, v);
    }

    /// domespit.cpp:153-155. Bit 24 is the leftmost slot.
    bool isSliderAtSlot(std::uint32_t state, int slot)
    {
        return slot >= 0 && slot < kSlotCount && (state & (1u << (24 - slot))) != 0;
    }

    /// DomeSpit::getSliderSlotClosestToPos (domespit.cpp:126-151). Which of the
    /// twenty-five the pointer is over, or -1.
    ///
    /// Only x matters: the original substitutes the hotspot's own top for the
    /// pointer's y (domespit.cpp:138-140) so that a drag survives the stylus
    /// wandering above or below the strip, which on a touchscreen it will.
    int slotClosestToPos(Engine &e, const DomeConfig &dome, int cardX)
    {
        if (cardX < kMinX)
            cardX = kMinX;
        if (cardX > kMaxX - 1)
            cardX = kMaxX - 1;

        for (int i = 0; i < kSlotCount; ++i)
        {
            const Hotspot *const h =
                e.hotspotByBlstId(static_cast<std::uint16_t>(dome.startBlstId + i));
            if (h == nullptr)
                continue;
            const Rect &r = e.hotspotRect(h);
            if (cardX >= r.left && cardX < r.right)
                return i;
        }
        return -1;
    }

    /// DomeSpit::drawDomeSliders (domespit.cpp:200-229). Each slot is a piece of
    /// one of two strips -- the one with a slider in it, or the empty one -- cut
    /// from the same place it is drawn to.
    ///
    /// One batched call per strip rather than twenty-five separate draws: this
    /// runs on every tick of a drag and about twenty times over during a reset,
    /// and each draw would otherwise open and decode the file again from the SD
    /// card.
    void drawDomeSliders(Engine &e, const DomeConfig &dome)
    {
        const std::int32_t sliders = e.bitmapIdForName(dome.sliderBmpName);
        const std::int32_t background = e.bitmapIdForName(dome.sliderBgBmpName);
        if (sliders < 0 || background < 0)
        {
            DebugLog::warn("dome: no slider art (\"%s\" / \"%s\")", dome.sliderBmpName,
                           dome.sliderBgBmpName);
            return;
        }

        const std::uint32_t state = sliderState(e);
        std::vector<CardSurface::Section> full;
        std::vector<CardSurface::Section> empty;
        full.reserve(kSlotCount);
        empty.reserve(kSlotCount);

        for (int i = 0; i < kSlotCount; ++i)
        {
            const Hotspot *const h =
                e.hotspotByBlstId(static_cast<std::uint16_t>(dome.startBlstId + i));
            if (h == nullptr)
                continue;

            const Rect &r = e.hotspotRect(h);
            CardSurface::Section s;
            s.dst = r;
            // The strip is the size of the whole slider area, so a slot's pixels
            // sit at its own position minus the area's corner
            // (domespit.cpp:216-218).
            s.src.left = static_cast<std::int16_t>(r.left - dome.areaLeft);
            s.src.top = static_cast<std::int16_t>(r.top - kAreaTop);
            s.src.right = static_cast<std::int16_t>(r.right - dome.areaLeft);
            s.src.bottom = static_cast<std::int16_t>(r.bottom - kAreaTop);

            (isSliderAtSlot(state, i) ? full : empty).push_back(s);
        }

        e.beginScreenUpdate();
        if (!empty.empty())
            e.drawBitmapSections(static_cast<std::uint16_t>(background), empty.data(),
                                 empty.size());
        if (!full.empty())
            e.drawBitmapSections(static_cast<std::uint16_t>(sliders), full.data(),
                                 full.size());
        e.applyScreenUpdate();
    }

} // namespace

// Public because four of the five domes have an `..._opencard` command that is
// nothing but this call; the drag and the reset below use it as well.
void checkDomeSliders(Engine &e)
{
    const Hotspot *const reset = e.hotspotByName("ResetSliders");
    const Hotspot *const open = e.hotspotByName("OpenDome");
    const bool solved = e.vars().get(VarId::ADomeCombo) == sliderState(e);

    if (reset != nullptr)
        e.enableHotspotByIndex(e.hotspotIndexOf(reset), !solved);
    if (open != nullptr)
        e.enableHotspotByIndex(e.hotspotIndexOf(open), solved);
}

void resetDomeSliders(Engine &e, const DomeConfig &dome)
{
    // The rightmost slider moves left until it meets the next one, then the pair
    // move together until they meet the third, and so on -- so the five end up
    // back in the five rightmost slots, which is the default state
    // (domespit.cpp:66-97).
    std::uint32_t state = sliderState(e);
    int slidersFound = 0;

    for (int i = 0; i < kSlotCount; ++i)
    {
        if ((state & (1u << i)) != 0)
        {
            ++slidersFound;
            continue;
        }

        for (int j = 0; j < slidersFound; ++j)
        {
            state &= ~(1u << (i - j - 1));
            state |= 1u << (i - j);
        }

        if (slidersFound != 0)
        {
            setSliderState(e, state);
            e.playCardSound("aBigTic");
            drawDomeSliders(e, dome);
            e.delay(20);
        }
    }

    // ScummVM asserts the result here. Reaching a different one means the state
    // was not a legal position to begin with, which sliderState() has already
    // ruled out -- so this cannot fail, and if it ever did, a dome the player
    // can still use beats a crash.
    setSliderState(e, state);
}

void dragDomeSlider(Engine &e, const DomeConfig &dome)
{
    std::uint32_t state = sliderState(e);
    int dragged = slotClosestToPos(e, dome, e.pointerCardX());

    // Not on a slider: the press means nothing.
    if (dragged < 0 || !isSliderAtSlot(state, dragged))
        return;

    e.setCursor(kCursorClosedHand);

    while (e.mouseIsDown() && !e.quitRequested())
    {
        const int over = slotClosestToPos(e, dome, e.pointerCardX());
        if (over >= 0)
        {
            // One slot at a time, and only into an empty one: sliders cannot
            // pass through each other, which is what makes the puzzle a puzzle.
            if (over > dragged && dragged < 24 && !isSliderAtSlot(state, dragged + 1))
            {
                state &= ~(1u << (24 - dragged));
                ++dragged;
                state |= 1u << (24 - dragged);
                setSliderState(e, state);
                e.playCardSound("aBigTic");
                drawDomeSliders(e, dome);
            }
            if (over < dragged && dragged > 0 && !isSliderAtSlot(state, dragged - 1))
            {
                state &= ~(1u << (24 - dragged));
                --dragged;
                state |= 1u << (24 - dragged);
                setSliderState(e, state);
                e.playCardSound("aBigTic");
                drawDomeSliders(e, dome);
            }
        }

        e.pumpInteractiveFrame();
    }

    checkDomeSliders(e);
}

void checkSliderCursorChange(Engine &e, const DomeConfig &dome)
{
    const int slot = slotClosestToPos(e, dome, e.pointerCardX());
    e.setCursor(isSliderAtSlot(sliderState(e), slot) ? kCursorOpenHand : kCursorMain);
}

void runDomeButtonMovie(Engine &e)
{
    // The original engine drew the button going down as a series of stills; both
    // ScummVM and this play the movie of it instead (domespit.cpp:43-48).
    e.playMovie(2, true);
}

void runDomeCheck(Engine &e)
{
    // The dome's marker turns gold for a moment once per revolution, and the
    // button only works while it is showing. The last frame of the looping movie
    // IS the golden one, with three frames of leeway either side because a
    // player cannot be expected to hit a single frame (domespit.cpp:50-64).
    const std::int32_t cur = e.movieCurrentFrame(1);
    const std::uint32_t count = e.movieFrameCount(1);
    if (cur < 0 || count == 0)
        return;

    if (static_cast<std::int32_t>(count) - cur < 3 || cur < 3)
        e.vars().set(VarId::DomeCheck, 1);
}

} // namespace rivenrt
