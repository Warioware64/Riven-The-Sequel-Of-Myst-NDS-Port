// Prison Island's external commands -- ScummVM's riven_stacks/pspit.cpp.
//
// The smallest island and the one with Catherine on it. Two things need native
// code:
//
//   * the ELEVATOR COMBINATION, five buttons at the top of the shaft whose
//     order is the one Gehn's watch plays at you on ospit -- and which the game
//     refuses to accept at all until Gehn is trapped; and
//   * CATHERINE HERSELF, who paces her cage on a timer for as long as the
//     player stands at the top of the elevator and watches.
//
// Plus the dome, whose five sliders are shared with four other islands.

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
    // --- the elevator combination --------------------------------------------

    /// PSpit::xpisland990_elevcombo (pspit.cpp:82-116). One button press.
    void xpisland990_elevcombo(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 1 || args == nullptr)
        {
            DebugLog::warn("xpisland990_elevcombo: no button number");
            return;
        }

        // tWAV 6..10, one per button.
        e.playEffect(static_cast<std::uint16_t>(args[0] + 5), 256);
        {
            Engine::CursorHide hide{e};
            e.delay(500);
        }

        // ScummVM works around a stuck-looking button here: a mouse released
        // during the half second above never reaches the script handler, so it
        // runs the hotspot's MouseUp itself (pspit.cpp:89-100). The port does
        // not need it -- Engine::delay pumps the IDLE loop, which does not read
        // the button at all, so the release is still waiting to be processed
        // when this returns and the ordinary dispatch picks it up.

        // Impossible to reach without Gehn trapped, and the original refuses it
        // anyway rather than let the ending be brute-forced (pspit.cpp:102-106).
        if (e.vars().get(VarId::AGehn) != 4)
            return;

        std::uint32_t &correctDigits = e.vars().at(VarId::PElevCombo);
        if (correctDigits < 5
            && args[0] == comboDigit(e.vars().get(VarId::PCorrectOrder), correctDigits))
            ++correctDigits;
        else
            correctDigits = 0;
    }

    // --- Catherine in her cage -----------------------------------------------

    /// PSpit::catherineIdleTimer (pspit.cpp:45-80).
    ///
    /// acathstate is which side of the cage she is on, 1 or 2, and every
    /// recording both starts from a side and ends on one -- which is what stops
    /// her crossing the cage between two clips. pcathcheck is only whether this
    /// is the first time the player has looked: her arrival has its own four.
    void catherineIdleTimer(Engine &e)
    {
        std::uint32_t &cathCheck = e.vars().at(VarId::PCathCheck);
        std::uint32_t &cathState = e.vars().at(VarId::ACathState);

        std::uint16_t movie = 0;
        if (cathCheck == 0)
        {
            static const std::uint16_t kMovies[] = {5, 6, 7, 8};
            cathCheck = 1;
            movie = kMovies[randomBetween(0, 3)];
        }
        else if (cathState == 1)
        {
            static const std::uint16_t kMovies[] = {11, 14};
            movie = kMovies[randomBetween(0, 1)];
        }
        else
        {
            static const std::uint16_t kMovies[] = {9, 10, 12, 13};
            movie = kMovies[randomBetween(0, 3)];
        }

        cathState = (movie == 5 || movie == 7 || movie == 11 || movie == 14) ? 2u : 1u;

        // BLOCKING, unlike gspit's imager (pspit.cpp:70-72): the player is
        // looking straight at her rather than at a picture of her, and the card
        // has nothing else to do while she moves.
        e.activateMlst(movie, false);
        e.playMovie(codeForMlstIndex(e, movie), true);

        // getRandomNumber(120), so nought to two minutes -- and it may well be
        // nought, which is how she sometimes moves twice in a row.
        const std::uint32_t next = static_cast<std::uint32_t>(randomBetween(0, 120)) * 1000;
        e.vars().set(VarId::PCathTime, e.clock() + Engine::msToFrames(next));
        e.installTimer(catherineIdleTimer, next);
    }

    // --- the dome ------------------------------------------------------------

    /// pspit's dome, on card 25. The slots are BLST 14..38, the artwork is named
    /// for the card it is on rather than for 190 (PSpit's constructor,
    /// pspit.cpp:33-34), and the strip is cut from two pixels further left than
    /// every other dome's -- see DomeConfig::areaLeft.
    constexpr DomeConfig kPspitDome{14, "psliders.25", "psliderbg.25", 198};

} // namespace

bool runPspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    // --- the elevator combination --------------------------------------------
    if (key == "xpisland990_elevcombo")
    {
        xpisland990_elevcombo(e, args, argCount);
        return true;
    }

    // --- the dome ------------------------------------------------------------
    if (key == "xpisland25_resetsliders")
    {
        resetDomeSliders(e, kPspitDome);
        return true;
    }
    if (key == "xpisland25_slidermd")
    {
        dragDomeSlider(e, kPspitDome);
        return true;
    }
    if (key == "xpisland25_slidermw")
    {
        checkSliderCursorChange(e, kPspitDome);
        return true;
    }
    if (key == "xpisland25_opencard")
    {
        checkDomeSliders(e);
        return true;
    }
    if (key == "xpscpbtn")
    {
        runDomeButtonMovie(e);
        return true;
    }
    if (key == "xpisland290_domecheck")
    {
        runDomeCheck(e);
        return true;
    }

    return false;
}

void installPspitCardTimer(Engine &e)
{
    // 0x3a85 is the top of the prison elevator, and the only card in pspit with
    // a timer (pspit.cpp:142-150).
    //
    // One to thirty-three seconds before she first moves, which is the original's
    // and is deliberately long: the point is that she is not performing for you.
    if (e.globalCardId(e.cardId()) == 0x3a85)
        e.installTimer(catherineIdleTimer,
                       static_cast<std::uint32_t>(randomBetween(1, 33)) * 1000);
}

} // namespace rivenrt
