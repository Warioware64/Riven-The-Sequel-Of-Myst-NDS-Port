// Tay's external commands -- ScummVM's riven_stacks/rspit.cpp.
//
// The rebel age, which the player visits once and cannot explore: four
// commands, of which two are empty and one is an ending.
//
// The fourth is the WINDOW of the rebel prison, and it is the only thing on Tay
// that moves. Look out of it and something is happening in the middle distance
// -- a rebel crossing a bridge, a boat, birds -- every forty seconds or so, for
// as long as you stand there. Riven has three of these ambient-life timers
// (jspit's sunners, gspit's imager and this) and this is the one that is armed
// by a command rather than by arriving somewhere.

#include <cstdlib>
#include <string>

#include "engine/Engine.hpp"
#include "engine/Externals.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    void rebelPrisonWindowTimer(Engine &e);

    /// How long until the next thing happens outside the window, in
    /// milliseconds. Thirty-eight to fifty-eight seconds (rspit.cpp:72).
    std::uint32_t nextWindowDelayMs()
    {
        return static_cast<std::uint32_t>(randomBetween(38, 58)) * 1000;
    }

    /// RSpit::rebelPrisonWindowTimer (rspit.cpp:64-79). One of twelve
    /// recordings, blocking, and then wait again.
    void rebelPrisonWindowTimer(Engine &e)
    {
        const std::uint16_t movie = static_cast<std::uint16_t>(randomBetween(2, 13));
        e.activateMlst(movie, false);
        e.playMovie(codeForMlstIndex(e, movie), true);

        const std::uint32_t next = nextWindowDelayMs();

        // Saved as a stamp so that leaving the window and coming back does not
        // start the wait over -- which is what xrwindowsetup reads below. Engine
        // frames rather than ScummVM's wall clock, for the reason bspit's Ytram
        // trap uses them: the frame counter is in the save file and a wall clock
        // is not.
        e.vars().set(VarId::RVillageTime, e.clock() + Engine::msToFrames(next));
        e.installTimer(rebelPrisonWindowTimer, next);
    }

    /// RSpit::xrwindowsetup (rspit.cpp:81-112). Run from the window card's load
    /// script: decide what the player is about to see and arm the timer.
    void xrwindowsetup(Engine &e)
    {
        // Time left over from a previous visit: pick the wait back up where it
        // was rather than starting a new one.
        const std::uint32_t villageTime = e.vars().get(VarId::RVillageTime);
        if (e.clock() < villageTime)
        {
            const std::uint32_t remaining = (villageTime - e.clock()) * 1000 / 60;
            e.installTimer(rebelPrisonWindowTimer, remaining);
            return;
        }

        std::uint32_t next = 0;
        if (randomBetween(0, 2) == 0 && e.vars().get(VarId::RRichard) == 0)
        {
            // A rebel is put on the bridge. The card's own scripts play that
            // one; all this does is say so and stay out of the way for the next
            // forty seconds.
            e.vars().set(VarId::RRebelView, 0);
            next = nextWindowDelayMs();
        }
        else
        {
            // Otherwise the timer picks something at random, and soon.
            e.vars().set(VarId::RRebelView, 1);
            next = static_cast<std::uint32_t>(randomBetween(0, 20)) * 1000;
        }

        // rvillagetime is deliberately NOT stamped here: the card's scripts
        // reset it to 0 a moment later, so a stamp would be overwritten anyway
        // (rspit.cpp:106-108, which notes the consequence -- returning to the
        // window twice does not reinstall the timer from the leftover).
        e.installTimer(rebelPrisonWindowTimer, next);
    }

} // namespace

bool runRspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    (void)args;
    (void)argCount;

    if (key == "xrwindowsetup")
    {
        xrwindowsetup(e);
        return true;
    }

    if (key == "xrcredittime")
    {
        // The trap book, used on Tay. Both endings are the same movie: with
        // Gehn trapped he thanks you for showing him the rebel age and leaves
        // you to die, and without him the rebels burn the book. ScummVM tells
        // them apart only to pass a different frame-count override for the
        // Polish release's credits (rspit.cpp:42-56), which is the one thing
        // this port's runEndGame does not do.
        runEndGame(e, 1);
        return true;
    }

    // --- empty, and empty in ScummVM too -------------------------------------
    // rspit.cpp:58-62 has both as bare bodies. The strip is already hidden here
    // by the cutscene test and shown again by the ordinary rules, so there is
    // nothing for either to do -- the same argument as tspit's xthideinventory.
    if (key == "xrhideinventory" || key == "xrshowinventory")
        return true;

    return false;
}

} // namespace rivenrt
