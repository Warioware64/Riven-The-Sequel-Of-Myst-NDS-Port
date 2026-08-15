// Boiler Island's external commands -- ScummVM's riven_stacks/bspit.cpp.
//
// The island of machinery, and it needs native code for all of it:
//
//   * GEHN'S LAB JOURNAL, twenty-two pages, one of which has the dome's
//     combination printed on it in D'ni numerals the engine has to assemble;
//   * the BOILER, whose water, heat and platform are three independent states
//     and whose animation is a sixteen-way table over them;
//   * the YTRAM TRAP, the only thing in Riven that happens on a clock the
//     player cannot see -- bait it, walk away, come back and it has gone off;
//   * the WATER VALVE, a wheel dragged rather than clicked, and the one control
//     that reaches into the boiler's state from outside; and
//   * the DOME, whose five sliders are shared with four other islands.
//
// Plus the wood chipper, which is a lever, and the sound plug, which is the
// only command in the game that remaps a card's ambient.

#include <cstdlib>
#include <string>

#include "DebugLog.hpp"
#include "engine/DomeSpit.hpp"
#include "engine/Engine.hpp"
#include "engine/Externals.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    // --- Gehn's lab journal --------------------------------------------------

    /// The dome's combination, drawn onto page 14 as five D'ni numerals.
    ///
    /// BSpit::labBookDrawDomeCombination (bspit.cpp:69-91). adomecombo is a
    /// 25-bit mask of which slots the sliders belong in, bit 24 being slot 0;
    /// each numeral is one horizontal strip of 25 numbers 32x24, and the nth
    /// set bit picks the nth strip's (24 - bit)th number.
    ///
    /// So five tBMPs, five sections, one call each -- the strips are separate
    /// files, which is the only reason this is not a single batch.
    void labBookDrawDomeCombination(Engine &e)
    {
        constexpr int kNumberW = 32;
        constexpr int kNumberH = 24;
        constexpr int kDstX = 240;
        constexpr int kDstY = 82;
        constexpr std::uint16_t kFirstBitmap = 364;

        const std::uint32_t combo = e.vars().get(VarId::ADomeCombo);

        int drawn = 0;
        e.beginScreenUpdate();
        for (int bit = 24; bit >= 0 && drawn < 5; --bit)
        {
            if ((combo & (1u << bit)) == 0)
                continue;

            const int offset = (24 - bit) * kNumberW;
            CardSurface::Section s;
            s.src = Rect{static_cast<std::int16_t>(offset), 0,
                         static_cast<std::int16_t>(offset + kNumberW), kNumberH};
            s.dst = Rect{static_cast<std::int16_t>(kDstX + drawn * kNumberW), kDstY,
                         static_cast<std::int16_t>(kDstX + (drawn + 1) * kNumberW),
                         kDstY + kNumberH};
            e.drawBitmapSections(static_cast<std::uint16_t>(kFirstBitmap + drawn), &s, 1);
            ++drawn;
        }
        e.applyScreenUpdate();

        // ScummVM asserts five here (bspit.cpp:90). A count that is not five
        // means adomecombo is not a five-slider state, which a corrupt save can
        // manage and an assert would answer by killing the game -- so it is a
        // line instead, and the page is drawn with whatever numerals there were.
        if (drawn != 5)
            DebugLog::warn("lab book: adomecombo has %d sliders, not 5", drawn);
    }

    /// Both page-turn commands and the open, which differ only in whether page
    /// 14 has to have the combination put back on it.
    void labBookPageDrawn(Engine &e, std::uint32_t page)
    {
        if (page == 14)
            labBookDrawDomeCombination(e);
    }

    /// BSpit::xblabopenbook (bspit.cpp:57-67).
    void xblabopenbook(Engine &e)
    {
        const std::uint32_t page = e.vars().get(VarId::BLabPage);
        e.activatePlst(static_cast<std::uint16_t>(page));
        labBookPageDrawn(e, page);
    }

    // --- the boiler ----------------------------------------------------------

    /// BSpit::xsoundplug (bspit.cpp:145-155). Which ambient the crater's FIRST
    /// SLST record names depends on the grating and the water, and the card's
    /// script has no way to say so.
    ///
    /// The numbers are ScummVM's verbatim, and they are list positions rather
    /// than record indices -- slot 0 takes slot 1's, 2's or 3's sounds. See
    /// Engine::overrideCardSound, which is where the off-by-one lives.
    void xsoundplug(Engine &e)
    {
        if (e.vars().get(VarId::BCraterGg) != 0)
            e.overrideCardSound(0, 1);
        else if (e.vars().get(VarId::BBlrWtr) == 0)
            e.overrideCardSound(0, 2);
        else
            e.overrideCardSound(0, 3);
    }

    /// BSpit::xbchangeboiler (bspit.cpp:157-228). Sixteen movies for three
    /// two-state machines and which of them the player just touched.
    ///
    /// The variables are read BEFORE the change: the card's script sets them
    /// after this returns, so `water == 0` here means "water is about to start
    /// coming in", not "there is none".
    void xbchangeboiler(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 1 || args == nullptr)
        {
            DebugLog::warn("xbchangeboiler: no action");
            return;
        }

        const std::uint32_t heat = e.vars().get(VarId::BHeat);
        const std::uint32_t water = e.vars().get(VarId::BBlrWtr);
        const bool platformUp = e.vars().get(VarId::BBlrGrt) == 1;

        // closeVideos() and NOT opcode 29 (bspit.cpp:163). The two are
        // different calls in ScummVM and the difference lands here: the boiling
        // loop xbupdateboiler left running has to stop before the transition
        // plays over the top of it, and it must NOT leave its last frame baked
        // into the card underneath.
        e.closeAllMovies();

        std::uint16_t index = 0;
        switch (args[0])
        {
        case 1: // the water is filling or draining
            if (water == 0)
                index = platformUp ? 12 : 10;
            else if (heat == 1)
                index = platformUp ? 22 : 19;
            else
                index = platformUp ? 16 : 13;
            break;
        case 2: // the heat is going on or off, and only matters with water in
            if (water != 0)
                index = heat == 1 ? (platformUp ? 23 : 20) : (platformUp ? 18 : 15);
            break;
        case 3: // the platform is being raised or lowered
            if (platformUp)
                index = water == 1 ? (heat == 1 ? 24 : 17) : 11;
            else
                index = water == 1 ? (heat == 1 ? 21 : 14) : 9;
            break;
        default:
            break;
        }

        if (index != 0)
            e.activateMlst(index, false);

        // The sound the script names, or -- for the heat, which does not name
        // one -- the card's first (bspit.cpp:221-224).
        if (argCount > 1)
            e.activateSlst(args[1]);
        else if (args[0] == 2)
            e.activateSlst(1);

        // Code 11, and always: every one of the sixteen records plays there.
        e.playMovie(11, true);
    }

    /// BSpit::xbupdateboiler (bspit.cpp:230-249). The boiling loop, on or off.
    void xbupdateboiler(Engine &e)
    {
        if (e.vars().get(VarId::BHeat) != 0)
        {
            const std::uint16_t index = e.vars().get(VarId::BBlrGrt) == 0 ? 8 : 7;
            e.activateMlst(index, true);
            return;
        }

        // disable() rather than a plain stop, on both, because ScummVM's
        // disable+stop pair is what bakes the last frame into the card -- the
        // water has to stay where it stopped rather than vanish.
        e.enableMovie(7, false);
        e.enableMovie(8, false);
    }

    // --- the Ytram trap ------------------------------------------------------
    //
    // Bait the plate, lower the trap, and somewhere between ten seconds and
    // three minutes later a Ytram walks into it. The player is expected to leave
    // and come back, so the deadline is a STAMP rather than a running timer:
    // bytramtime is a clock reading, the timer is only how the engine notices,
    // and returning to the card re-arms it from the stamp.
    //
    // Which is why Engine::clock() is in the save file. The stamp is meaningless
    // against a clock that restarts at zero, and a trap set before a save would
    // otherwise either fire the instant the game loaded or never fire at all.

    void checkYtramCatch(Engine &e, bool playSound);

    /// BSpit::ytramTrapTimer (bspit.cpp:251-257).
    void ytramTrapTimer(Engine &e)
    {
        e.removeTimer();
        checkYtramCatch(e, true);
    }

    /// BSpit::checkYtramCatch (bspit.cpp:270-303).
    void checkYtramCatch(Engine &e, bool playSound)
    {
        std::uint32_t &deadline = e.vars().at(VarId::BYTramTime);

        // The trap has been moved back up. You cannot catch them that way.
        if (deadline == 0)
            return;

        // Not yet -- re-arm and wait. This is the branch that makes setting the
        // trap and walking away work, and the branch a load has to survive.
        if (e.clock() < deadline)
        {
            e.installTimer(ytramTrapTimer, (deadline - e.clock()) * 1000 / 60);
            return;
        }

        std::uint32_t &caught = e.vars().at(VarId::BYTram);
        ++caught;
        if (caught > 3)
            caught = 3;

        e.vars().set(VarId::BYTrapped, 1);
        e.vars().set(VarId::BBait, 0);
        e.vars().set(VarId::BYTrap, 0);
        deadline = 0;

        if (playSound)
            e.playEffect(33, 256); // by raw id, the way ScummVM plays it
    }

    /// BSpit::xbsettrap (bspit.cpp:259-268).
    void xbsettrap(Engine &e)
    {
        const int seconds = randomBetween(10, 60 * 3);
        e.vars().set(VarId::BYTramTime,
                     e.clock() + Engine::msToFrames(static_cast<std::uint32_t>(seconds)
                                                    * 1000));
        e.installTimer(ytramTrapTimer, static_cast<std::uint32_t>(seconds) * 1000);
    }

    /// The bait and the plate share their two hotspots and their two pictures;
    /// only where the pellet starts differs (bspit.cpp:310-333 and :365-392).
    ///
    /// `fromPlate` is what makes them different: picking the pellet back OFF the
    /// plate has to be able to end with no bait at all, while putting it down
    /// for the first time can only ever succeed or change nothing.
    void baitDrag(Engine &e, bool fromPlate)
    {
        constexpr std::uint16_t kBaitHotspot = 9;
        constexpr std::uint16_t kPlateHotspot = 16;

        e.setCursor(kCursorPellet);
        if (fromPlate)
            e.activatePlst(3); // the plate, empty, because it is in hand now

        while (e.mouseIsDown() && !e.quitRequested())
            e.pumpInteractiveFrame();

        e.setCursor(kCursorMain);

        const Hotspot *const bait = e.hotspotByBlstId(kBaitHotspot);
        const Hotspot *const plate = e.hotspotByBlstId(kPlateHotspot);
        if (bait == nullptr || plate == nullptr)
        {
            DebugLog::warn("bait: card %u has no hotspot 9/16",
                           static_cast<unsigned>(e.cardId()));
            return;
        }

        const Rect &r = e.hotspotRect(plate);
        const bool onPlate = e.pointerCardX() >= r.left && e.pointerCardX() < r.right
                             && e.pointerCardY() >= r.top && e.pointerCardY() < r.bottom;

        if (onPlate)
        {
            e.vars().set(VarId::BBait, 1);
            e.activatePlst(4);
            e.enableHotspot(kBaitHotspot, false);
            e.enableHotspot(kPlateHotspot, true);
            return;
        }

        // Dropped somewhere else. From the pellet's own dish that is simply a
        // press that went nowhere -- ScummVM does not even redraw. From the
        // plate it means the player has taken the bait back.
        if (!fromPlate)
            return;
        e.vars().set(VarId::BBait, 0);
        e.enableHotspot(kBaitHotspot, true);
        e.enableHotspot(kPlateHotspot, false);
    }

    /// BSpit::xbfreeytram (bspit.cpp:335-363). Two movies back to back, and
    /// which pair depends on how many have been caught.
    void xbfreeytram(Engine &e)
    {
        std::uint16_t index = 0;
        switch (e.vars().get(VarId::BYTram))
        {
        case 1:
            index = 11;
            break;
        case 2:
            index = 12;
            break;
        default:
            // ScummVM notes the original did rand(13, 14) and uses 13..15
            // (bspit.cpp:346-348). Kept as ScummVM has it: three recordings
            // exist and there is no reason to leave one of them unplayed.
            index = static_cast<std::uint16_t>(randomBetween(13, 15));
            break;
        }

        e.activateMlst(index, false);
        e.playMovie(11, true);

        e.activateMlst(static_cast<std::uint16_t>(index + 5), false);
        e.playMovie(12, true);

        e.activatePlst(4);
    }

    // --- the water valve -----------------------------------------------------

    /// BSpit::valveChangePosition (bspit.cpp:449-481). One notch of the wheel,
    /// and the boiler's state if the water is now pointed at it.
    void valveChangePosition(Engine &e, std::uint32_t position, std::uint16_t movieCode,
                             std::uint16_t pictureIndex)
    {
        e.playMovie(movieCode, true);
        e.activatePlst(pictureIndex);

        if (position == 1)
        {
            if (e.vars().get(VarId::BIdVlv) == 1)
            {
                // The pipe is open: whatever is in the boiler drains out.
                if (e.vars().get(VarId::BBlrArm) == 1 && e.vars().get(VarId::BBlrWtr) == 1)
                {
                    e.vars().set(VarId::BHeat, 0);
                    e.vars().set(VarId::BBlrWtr, 0);
                    e.playCardSound("bBlrFar");
                }
                // The pipe is closed: it fills again, and it comes up hot if the
                // boiler's own valve was left on.
                if (e.vars().get(VarId::BBlrArm) == 0 && e.vars().get(VarId::BBlrWtr) == 0)
                {
                    e.vars().set(VarId::BHeat, e.vars().get(VarId::BBlrValve));
                    e.vars().set(VarId::BBlrWtr, 1);
                    e.playCardSound("bBlrFar");
                }
            }
            else
            {
                // Water is going the other way; the grating inside matches the
                // switch outside.
                e.vars().set(VarId::BBlrGrt, e.vars().get(VarId::BBlrSw) == 1 ? 0 : 1);
            }
        }

        e.vars().set(VarId::BValve, position);
    }

    /// BSpit::xvalvecontrol (bspit.cpp:418-447). Drag the wheel; it clicks round
    /// one position at a time and keeps going while the button is held.
    void xvalvecontrol(Engine &e)
    {
        // Ten of the ORIGINAL's pixels, and so measured in them -- the same ten
        // on a 256-wide screen would be a quarter of the wheel's travel, and the
        // same four DS pixels jspit's lever comment warns is inside a resistive
        // touchscreen's jitter.
        constexpr int kNotch = 10;

        const int startX = e.dragStartCardX();
        const int startY = e.dragStartCardY();

        e.setCursor(kCursorClosedHand);
        while (e.mouseIsDown() && !e.quitRequested())
        {
            const int changeX = e.pointerCardX() - startX;
            // Inverted, as in the original: screen y grows downward and this is
            // "how far UP has the player pulled" (bspit.cpp:427).
            const int changeY = startY - e.pointerCardY();

            switch (e.vars().get(VarId::BValve))
            {
            case 0:
                if (changeY <= -kNotch)
                    valveChangePosition(e, 1, 2, 2);
                break;
            case 1:
                if (changeX >= 0 && changeY >= kNotch)
                    valveChangePosition(e, 0, 3, 1);
                else if (changeX <= -kNotch && changeY <= kNotch)
                    valveChangePosition(e, 2, 1, 3);
                break;
            case 2:
                if (changeX >= kNotch)
                    valveChangePosition(e, 1, 4, 2);
                break;
            default:
                break;
            }

            e.pumpInteractiveFrame();
        }
        e.setCursor(kCursorMain);
    }

    /// BSpit::xbchipper (bspit.cpp:483-502). Pull the lever down and the chipper
    /// runs; anything else and it does not.
    void xbchipper(Engine &e)
    {
        const int startY = e.dragStartCardY();

        bool pulled = false;
        while (e.mouseIsDown() && !e.quitRequested())
        {
            if (e.pointerCardY() > startY)
            {
                pulled = true;
                break;
            }
            e.pumpInteractiveFrame();
        }

        if (pulled)
            e.playMovie(2, true);
    }

    // --- the dome ------------------------------------------------------------

    /// bspit's dome, on card 190. The slots are BLST 9..33 -- the same id 9 the
    /// bait hotspot uses, on a different card, which is fine and worth knowing.
    constexpr DomeConfig kBspitDome{9, "bSliders.190", "bSliderBG.190"};

} // namespace

bool runBspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    // --- Gehn's lab journal --------------------------------------------------
    if (key == "xblabopenbook")
    {
        xblabopenbook(e);
        return true;
    }
    if (key == "xblabbooknextpage")
    {
        turnBookPages(e, VarId::BLabPage, +1, 1, 22, Transition::WipeLeft,
                      labBookPageDrawn);
        return true;
    }
    if (key == "xblabbookprevpage")
    {
        turnBookPages(e, VarId::BLabPage, -1, 1, 22, Transition::WipeRight,
                      labBookPageDrawn);
        return true;
    }

    // --- the boiler ----------------------------------------------------------
    if (key == "xsoundplug")
    {
        xsoundplug(e);
        return true;
    }
    if (key == "xbchangeboiler")
    {
        xbchangeboiler(e, args, argCount);
        return true;
    }
    if (key == "xbupdateboiler")
    {
        xbupdateboiler(e);
        return true;
    }

    // --- the Ytram trap ------------------------------------------------------
    if (key == "xbsettrap")
    {
        xbsettrap(e);
        return true;
    }
    if (key == "xbcheckcatch")
    {
        checkYtramCatch(e, argCount >= 1 && args != nullptr && args[0] != 0);
        return true;
    }
    if (key == "xbait")
    {
        baitDrag(e, false);
        return true;
    }
    if (key == "xbaitplate")
    {
        baitDrag(e, true);
        return true;
    }
    if (key == "xbfreeytram")
    {
        xbfreeytram(e);
        return true;
    }

    // --- the water valve and the chipper -------------------------------------
    if (key == "xvalvecontrol")
    {
        xvalvecontrol(e);
        return true;
    }
    if (key == "xbchipper")
    {
        xbchipper(e);
        return true;
    }

    // --- the dome ------------------------------------------------------------
    if (key == "xbisland190_resetsliders")
    {
        resetDomeSliders(e, kBspitDome);
        return true;
    }
    if (key == "xbisland190_slidermd")
    {
        dragDomeSlider(e, kBspitDome);
        return true;
    }
    if (key == "xbisland190_slidermw")
    {
        checkSliderCursorChange(e, kBspitDome);
        return true;
    }
    if (key == "xbisland190_opencard")
    {
        checkDomeSliders(e);
        return true;
    }
    if (key == "xbscpbtn")
    {
        runDomeButtonMovie(e);
        return true;
    }
    if (key == "xbisland_domecheck")
    {
        runDomeCheck(e);
        return true;
    }

    return false;
}

} // namespace rivenrt
