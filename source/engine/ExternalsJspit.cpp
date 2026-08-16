// Jungle Island's external commands -- ScummVM's riven_stacks/jspit.cpp.
//
// Five things live on this island that the opcode table cannot express, and each
// of them is one of the game's set pieces:
//
//   * the REBEL TUNNEL, twenty-five stones whose press order is a five-symbol
//     combination, drawn from a bitmask that no card script can compute;
//   * the SUNNERS, who react to being approached and fidget on a timer;
//   * the GALLOWS CARRIAGE, which gives the player five seconds to climb in;
//   * the WHARK ELEVATOR, a lever you drag rather than click; and
//   * the DOME, whose five sliders are shared with four other islands.
//
// Plus the beetles, which are one line each, and the whark number game, which
// teaches Riven's counting system by feeding a villager to something.

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
    // --- the sunners ---------------------------------------------------------
    //
    // Four cards down the lagoon steps (RMAP 0x77d6, 0x79bd, 0x7beb, 0xb6ca --
    // jspit 621, 627, 629, 632) share one piece of state: jsunners, which is 0
    // while the creatures are basking, 1 once they have been startled and 2 once
    // they have left. Two halves make them alive:
    //
    //   * the ALERT, run from each card's CardEnter script, which plays the
    //     reaction to the player arriving and lets a click cut it short; and
    //   * the TIMER, armed on the way in, which plays a random idle movie every
    //     few seconds for as long as the player stands there.
    //
    // The timers are why Engine has a timer at all. jsunnertime is their clock,
    // and the cards zero it themselves on entry (every one of the four has
    // `SetVar jsunnertime 0` in its CardEnter), so nothing here has to worry
    // about a stamp from a previous session -- and the re-anchor below rewrites
    // a stale one within a single poll anyway.

    /// JSpit::sunnersPlayVideo (jspit.cpp:542-567). Play a sunner movie and
    /// watch for a click; a click startles them, sends the player on to
    /// `destGlobalId`, and -- this is what the return value is for -- means the
    /// caller is now standing on a different card.
    bool sunnersPlayVideo(Engine &e, std::uint16_t code, std::uint32_t destGlobalId,
                          bool sunnersShouldFlee)
    {
        if (!e.playMovieUntilClick(code))
            return false;

        if (sunnersShouldFlee)
            e.vars().set(VarId::JSunners, 1);

        // ScummVM builds a one-command ChangeCard script here; the port can just
        // ask, and does it the same way as the destination is named -- by RMAP
        // global id, because a local card number means nothing across stacks.
        const std::int32_t local = e.stack().localCardForGlobal(destGlobalId);
        if (local < 0)
        {
            DebugLog::warn("sunners: no card for global id %lu",
                           static_cast<unsigned long>(destGlobalId));
            return false;
        }
        e.changeToCard(static_cast<std::uint16_t>(local));
        return true;
    }

    void sunnersTopStairsTimer(Engine &e);
    void sunnersMidStairsTimer(Engine &e);
    void sunnersLowerStairsTimer(Engine &e);
    void sunnersBeachTimer(Engine &e);

    /// JSpit::sunnersTopStairsTimer (jspit.cpp:569-598).
    ///
    /// The shape all four share, and every part of it is load-bearing:
    ///
    ///   * gone means gone -- once jsunners is not 0 the timer removes itself
    ///     rather than re-arming, because there is no movie left to play;
    ///   * nothing is started on top of a movie that is still running, which is
    ///     what the movieEnded() test is for -- the poll comes round every half
    ///     second and the movies are seconds long;
    ///   * the re-anchor of jsunnertime sits INSIDE that test but OUTSIDE the
    ///     if/else, so it runs on the "not due yet" pass as well. Move it into
    ///     either arm and the first wait either never expires or expires at once.
    ///
    /// The one deliberate divergence is the early return after a click. The
    /// original falls through to installTimer (jspit.cpp:597) even though
    /// sunnersPlayVideo has just changed the card -- which overwrites the timer
    /// the destination installed on the way in, and leaves a proc addressing
    /// movie codes that belong to the card the player has left.
    void sunnersTopStairsTimer(Engine &e)
    {
        if (e.vars().get(VarId::JSunners) != 0)
        {
            e.removeTimer();
            return;
        }

        std::uint32_t timerTime = 500;
        if (e.movieEnded(1))
        {
            const std::uint32_t sunnerTime = e.vars().get(VarId::JSunnerTime);
            if (sunnerTime == 0)
            {
                timerTime = static_cast<std::uint32_t>(randomBetween(2, 15)) * 1000;
            }
            else if (sunnerTime < e.clock())
            {
                const std::uint16_t code = static_cast<std::uint16_t>(randomBetween(1, 3));
                if (sunnersPlayVideo(e, code, 0x79BD, false))
                    return; // gone to another card; its own timer is armed
                timerTime = e.movieDurationMs(code)
                            + static_cast<std::uint32_t>(randomBetween(2, 15)) * 1000;
            }
            e.vars().set(VarId::JSunnerTime, e.clock() + Engine::msToFrames(timerTime));
        }
        e.installTimer(sunnersTopStairsTimer, timerTime, "jspit sunners top");
    }

    /// JSpit::sunnersMidStairsTimer (jspit.cpp:600-637). Codes 2, 3 and 4 with
    /// 4 four times as likely as the other two, and a click flees to the bottom
    /// of the steps.
    void sunnersMidStairsTimer(Engine &e)
    {
        if (e.vars().get(VarId::JSunners) != 0)
        {
            e.removeTimer();
            return;
        }

        std::uint32_t timerTime = 500;
        if (e.movieEnded(1))
        {
            const std::uint32_t sunnerTime = e.vars().get(VarId::JSunnerTime);
            if (sunnerTime == 0)
            {
                timerTime = static_cast<std::uint32_t>(randomBetween(1, 10)) * 1000;
            }
            else if (sunnerTime < e.clock())
            {
                const int r = randomBetween(0, 5); // getRandomNumber(5)
                const std::uint16_t code = r == 4 ? 2 : (r == 5 ? 3 : 4);
                if (sunnersPlayVideo(e, code, 0x7BEB, true))
                    return;
                timerTime = static_cast<std::uint32_t>(randomBetween(1, 10)) * 1000;
            }
            e.vars().set(VarId::JSunnerTime, e.clock() + Engine::msToFrames(timerTime));
        }
        e.installTimer(sunnersMidStairsTimer, timerTime, "jspit sunners mid");
    }

    /// JSpit::sunnersLowerStairsTimer (jspit.cpp:639-668).
    void sunnersLowerStairsTimer(Engine &e)
    {
        if (e.vars().get(VarId::JSunners) != 0)
        {
            e.removeTimer();
            return;
        }

        std::uint32_t timerTime = 500;
        if (e.movieEnded(1))
        {
            const std::uint32_t sunnerTime = e.vars().get(VarId::JSunnerTime);
            if (sunnerTime == 0)
            {
                timerTime = static_cast<std::uint32_t>(randomBetween(1, 30)) * 1000;
            }
            else if (sunnerTime < e.clock())
            {
                const std::uint16_t code = static_cast<std::uint16_t>(randomBetween(3, 5));
                if (sunnersPlayVideo(e, code, 0xB6CA, true))
                    return;
                timerTime = static_cast<std::uint32_t>(randomBetween(1, 30)) * 1000;
            }
            e.vars().set(VarId::JSunnerTime, e.clock() + Engine::msToFrames(timerTime));
        }
        e.installTimer(sunnersLowerStairsTimer, timerTime, "jspit sunners lower");
    }

    /// JSpit::sunnersBeachTimer (jspit.cpp:670-703). The odd one out: the beach
    /// card's script does not activate these six records, so the timer has to do
    /// it itself before it can play one -- and there is nothing to click through
    /// here, because the player is already as close as they can get.
    void sunnersBeachTimer(Engine &e)
    {
        if (e.vars().get(VarId::JSunners) != 0)
        {
            e.removeTimer();
            return;
        }

        std::uint32_t timerTime = 500;
        if (e.movieEnded(3))
        {
            const std::uint32_t sunnerTime = e.vars().get(VarId::JSunnerTime);
            if (sunnerTime == 0)
            {
                timerTime = static_cast<std::uint32_t>(randomBetween(1, 30)) * 1000;
            }
            else if (sunnerTime < e.clock())
            {
                const std::uint16_t index = static_cast<std::uint16_t>(randomBetween(3, 8));
                e.activateMlst(index, false);
                e.playMovie(codeForMlstIndex(e, index), true);
                timerTime = static_cast<std::uint32_t>(randomBetween(1, 30)) * 1000;
            }
            e.vars().set(VarId::JSunnerTime, e.clock() + Engine::msToFrames(timerTime));
        }
        e.installTimer(sunnersBeachTimer, timerTime, "jspit sunners beach");
    }

    /// JSpit::xjlagoon700_alert (jspit.cpp:487-500). Mid-staircase: the sunners
    /// look up, and moving on startles them.
    void xjlagoon700_alert(Engine &e)
    {
        if (e.vars().get(VarId::JSunners) != 0)
            return; // gone; nothing to react
        sunnersPlayVideo(e, 1, 0x7BEB, true);
    }

    /// JSpit::xjlagoon800_alert (jspit.cpp:502-522). Lower staircase, and the
    /// first card that can show them actually leaving -- two movies back to back,
    /// because the pair was shot as two.
    void xjlagoon800_alert(Engine &e)
    {
        const std::uint32_t sunners = e.vars().get(VarId::JSunners);
        if (sunners == 0)
        {
            sunnersPlayVideo(e, 1, 0xB6CA, true);
            return;
        }
        if (sunners != 1)
            return; // already 2: they left the last time the player came down

        e.playMovie(2, true);
        e.playMovie(6, true);
        e.vars().set(VarId::JSunners, 2);
        // enter(false): the card draws its own "they are gone" still from
        // jsunners, which has just changed under it.
        e.refreshCard();
    }

    /// JSpit::xjlagoon1500_alert (jspit.cpp:524-540). The beach.
    void xjlagoon1500_alert(Engine &e)
    {
        const std::uint32_t sunners = e.vars().get(VarId::JSunners);
        if (sunners == 0)
        {
            // No click to watch for and nowhere to be sent: there is no card
            // beyond this one, so the original plays it blocking and so does this.
            e.playMovie(3, true);
            return;
        }
        if (sunners != 1)
            return;

        e.playMovie(2, true);
        e.vars().set(VarId::JSunners, 2);
        e.refreshCard();
    }

    // --- the rebel tunnel ----------------------------------------------------
    //
    // Twenty-five stones in a ring of tunnels, each a card of its own (217-248).
    // Two variables hold the puzzle, and NEITHER is named by any script in the
    // game -- they exist only here, which is why RivenVars.hpp carries them as
    // hand-added entries:
    //
    //   * jicons is a bitmask, one bit per stone, of which are pressed;
    //   * jiconorder is the ORDER they were pressed in, five bits per stone,
    //     newest at the bottom -- a base-32 word, and the thing compared against
    //     jiconcorrectorder.
    //
    // The two have to agree, which is what makes the "unpress" case fiddly: a
    // stone can only be lifted if it was the last one pressed, so lifting is a
    // shift right by five rather than a search.

    /// jspit.cpp:76-90. jiconorder holds five bits per pressed stone, so its
    /// magnitude says how many there are.
    int countDepressedIcons(std::uint32_t iconOrder)
    {
        if (iconOrder >= (1u << 20))
            return 5;
        if (iconOrder >= (1u << 15))
            return 4;
        if (iconOrder >= (1u << 10))
            return 3;
        if (iconOrder >= (1u << 5))
            return 2;
        if (iconOrder >= (1u << 1))
            return 1;
        return 0;
    }

    /// JSpit::xreseticons (jspit.cpp:66-71). Called on the way to Tay: the
    /// puzzle is not carried across.
    void xreseticons(Engine &e)
    {
        e.vars().set(VarId::JIcons, 0);
        e.vars().set(VarId::JIconOrder, 0);
        e.vars().set(VarId::JRBook, 0);
    }

    /// JSpit::xicon (jspit.cpp:92-99). Not an action -- a QUESTION, asked from
    /// each stone card's CardLoad script, whose answer lands in atemp and is
    /// switched on immediately after. 0 = up, 1 = down and liftable, 2 = down
    /// and stuck (something was pressed after it).
    void xicon(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 1 || args == nullptr)
        {
            DebugLog::warn("xicon: no stone number");
            return;
        }
        const std::uint32_t icons = e.vars().get(VarId::JIcons);
        const std::uint32_t order = e.vars().get(VarId::JIconOrder);

        std::uint32_t atemp = 0;
        if ((icons & (1u << (args[0] - 1))) != 0)
            atemp = (order & 0x1f) == args[0] ? 1u : 2u;
        e.vars().set(VarId::ATemp, atemp);
    }

    /// JSpit::xtoggleicon (jspit.cpp:116-134). Press or lift one stone, and
    /// declare the puzzle solved if the order now matches.
    void xtoggleicon(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 1 || args == nullptr)
        {
            DebugLog::warn("xtoggleicon: no stone number");
            return;
        }
        std::uint32_t icons = e.vars().get(VarId::JIcons);
        std::uint32_t order = e.vars().get(VarId::JIconOrder);
        const std::uint32_t bit = 1u << (args[0] - 1);

        if ((icons & bit) != 0)
        {
            icons &= ~bit;
            order >>= 5; // it was the last one pressed; xicon guaranteed that
        }
        else
        {
            icons |= bit;
            order = (order << 5) + args[0];
        }

        e.vars().set(VarId::JIcons, icons);
        e.vars().set(VarId::JIconOrder, order);

        if (order == e.vars().get(VarId::JIconCorrectOrder))
            e.vars().set(VarId::JRBook, 1);
    }

    /// JSpit::xcheckicons (jspit.cpp:101-114). A sixth stone is one too many:
    /// the whole ring rises again and the player starts over.
    void xcheckicons(Engine &e)
    {
        if (countDepressedIcons(e.vars().get(VarId::JIconOrder)) != 5)
            return;

        e.vars().set(VarId::JIconOrder, 0);
        e.vars().set(VarId::JIcons, 0);

        // By raw id, which is the one case ScummVM does not go through
        // playCardSound for -- tWAV 46 is jspit's, and it is the stones.
        e.playEffect(46, 256);

        // Blocking, because the stones are still moving. The original spins
        // doFrame(); the port's inner loop is the same thing.
        while (e.isEffectPlaying() && !e.quitRequested())
            e.pumpIdleFrame();
    }

    /// The four "pictfix" commands (jspit.cpp:136-236) all do the same thing
    /// with a different table: draw the overlay for each stone that is currently
    /// down. They exist because the tunnel cards show several stones at once and
    /// a script cannot loop over a bitmask.
    ///
    /// Each entry is a bit of jicons; the PLST record is the entry's position
    /// plus two, because record 1 is the tunnel itself.
    void tunnelPictfix(Engine &e, const int *bits, int count)
    {
        const std::uint32_t icons = e.vars().get(VarId::JIcons);
        e.beginScreenUpdate();
        for (int i = 0; i < count; ++i)
            if ((icons & (1u << bits[i])) != 0)
                e.activatePlst(static_cast<std::uint16_t>(i + 2));
        e.applyScreenUpdate();
    }

    // --- the beetles ---------------------------------------------------------

    /// jspit.cpp:462-485. The card plays a beetle if the variable says so, and
    /// these five decide it -- one time in four, and on card 372 only when the
    /// villager girl is not there to have scared it off.
    void playBeetle(Engine &e, bool needGirlAway)
    {
        const bool play = randomBetween(0, 3) == 0
                          && (!needGirlAway || e.vars().get(VarId::JGirl) != 1);
        e.vars().set(VarId::JPlayBeetle, play ? 1u : 0u);
    }

    // --- the whark elevator --------------------------------------------------

    /// How far the stylus must travel before a press counts as a pull.
    ///
    /// ScummVM's threshold is ten pixels of a 608-wide screen (jspit.cpp:365-369),
    /// which is FOUR on the DS -- inside the jitter of a resistive touchscreen,
    /// so a plain tap on the lever would read as a deliberate drag and move the
    /// elevator on its own. Eight DS pixels is about nineteen of the original's:
    /// still a small deliberate flick, and not reachable by accident.
    constexpr int kDragThresholdDs = 8;

    /// JSpit::jspitElevatorLoop (jspit.cpp:355-373). Hold the lever, move up or
    /// down, let go: -1 for down, +1 for up, 0 for a press that went nowhere.
    int elevatorLoop(Engine &e)
    {
        const int startY = e.dragStartY();
        e.setCursor(kCursorClosedHand);

        while (e.mouseIsDown() && !e.quitRequested())
        {
            e.pumpInteractiveFrame();

            // Screen y grows DOWNWARD here and in the original alike, so
            // ScummVM's "pos.y > startPos.y + 10" is the player pulling the
            // lever DOWN, which sends the car down.
            if (e.pointerY() > startY + kDragThresholdDs)
                return -1;
            if (e.pointerY() < startY - kDragThresholdDs)
                return 1;
        }
        return 0;
    }

    /// 0x1e374 is the middle level, 0x1e597 the top, 0x1e29c the bottom.
    void elevatorGoTo(Engine &e, std::uint32_t globalId)
    {
        const std::int32_t local = e.stack().localCardForGlobal(globalId);
        if (local >= 0)
            e.changeToCard(static_cast<std::uint16_t>(local));
        else
            DebugLog::warn("elevator: no card for global id %lu",
                           static_cast<unsigned long>(globalId));
    }

    /// JSpit::xhandlecontrolup (jspit.cpp:375-405). At the top of the shaft the
    /// only way is down.
    void xhandlecontrolup(Engine &e)
    {
        if (elevatorLoop(e) != -1)
            return;

        e.playMovie(1, true);

        Engine::CursorHide hide{e};
        e.playMovie(2, false);

        // Off the MOVIE's clock, not the engine's. ScummVM tests
        // `secondVideo->getTime() > 3333` (jspit.cpp:393) -- and note that its
        // own comment right above says it would rather use the stored movie
        // opcode, so this loop stays hand-rolled: no script stores anything for
        // this card, and opcode 38 would have nothing to run here.
        //
        // The two clocks agree only while nothing costs a frame. A dropped frame
        // puts the engine's ahead of the picture and so the sound in front of the
        // moment it belongs to; movieTimeMs is derived from the frame actually on
        // screen and cannot drift away from it.
        bool played = false;
        while (!e.movieEnded(2) && !e.quitRequested())
        {
            e.pumpIdleFrame();
            if (!played && e.movieTimeMs(2) > 3333)
            {
                // getCard()->playSound(1, false) is an SLST activate, not an
                // effect (riven_card.cpp:785-790).
                e.activateSlst(1);
                played = true;
            }
        }
        e.enableMovie(2, false); // disable(): bake the last frame

        elevatorGoTo(e, 0x1e374);
    }

    /// JSpit::xhandlecontroldown (jspit.cpp:407-420). At the bottom, only up.
    void xhandlecontroldown(Engine &e)
    {
        if (elevatorLoop(e) != 1)
            return;

        e.playMovie(1, true);
        e.playMovie(2, true);
        elevatorGoTo(e, 0x1e374);
    }

    /// JSpit::xhandlecontrolmid (jspit.cpp:422-460). The middle level, where the
    /// lever goes either way -- and where the whark's mouth has to be shut
    /// before the car can pass it.
    void xhandlecontrolmid(Engine &e)
    {
        const int changeLevel = elevatorLoop(e);
        if (changeLevel == 0)
            return;

        e.playMovie(changeLevel == 1 ? 7 : 6, true); // the lever itself

        if (e.vars().get(VarId::JWMouth) == 1)
        {
            e.playMovie(3, true);
            e.playMovie(8, true);
            e.vars().set(VarId::JWMouth, 0);
        }

        e.playMovie(changeLevel == 1 ? 5 : 4, true); // the ride
        elevatorGoTo(e, changeLevel == 1 ? 0x1e597 : 0x1e29c);
    }

    // --- the dome ------------------------------------------------------------

    /// jspit's dome, on card 339. The slots are BLST 10..34 ("s1".."s25") and
    /// the artwork is named for card 190, where it was first used
    /// (JSpit's constructor, jspit.cpp:32-33).
    constexpr DomeConfig kJspitDome{10, "jsliders.190", "jsliderbg.190"};

    // --- the gallows carriage ------------------------------------------------

    /// JSpit::xvga1300_carriage (jspit.cpp:238-333). Pull the handle and the
    /// carriage drops; you then have five seconds to climb in before it goes
    /// back up without you.
    void xvga1300_carriage(Engine &e)
    {
        e.playMovie(1, true); // the handle

        // The pan down to the carriage. enableCardUpdateScript(false) brackets
        // the publish because this card's own update script would redraw the
        // view it has just left -- the one command in the game that needs it.
        e.beginScreenUpdate();
        e.scheduleTransition(Transition::PanDown);
        e.activatePlst(7);
        e.enableCardUpdateScript(false);
        e.applyScreenUpdate();
        e.enableCardUpdateScript(true);

        e.playMovie(4, true); // the carriage starts to fall

        e.beginScreenUpdate();
        e.scheduleTransition(Transition::PanUp);
        e.activatePlst(1);
        e.applyScreenUpdate();

        e.setCursor(kCursorMain);
        // The handle is still held from the press that got here; without this
        // the "did they click?" test below is satisfied before the player has
        // seen anything (jspit.cpp:261).
        e.mouseForceUp();

        if (e.vars().get(VarId::JGallows) == 1)
        {
            // The gallows floor is open, so the carriage cannot be boarded: it
            // drops, waits, and comes back on its own.
            e.playMovie(2, true);
            e.delay(5000);
            e.playMovie(3, true);
            e.refreshCard();
            return;
        }

        bool gotClick = false;

        // Watch for a click all the way through the drop...
        e.playMovie(2, false);
        while (!e.movieEnded(2) && !e.quitRequested())
        {
            e.pumpInteractiveFrame();
            if (e.mouseIsDown())
                gotClick = true;
        }
        e.enableMovie(2, false); // disable(), as the original does

        // ...and then for five seconds more. The clock is engine frames, which
        // is close enough for a "quick, get in" window and errs generous: a
        // frame that overruns its vblank makes the window slightly longer, never
        // shorter.
        const std::uint32_t deadline = e.clock() + Engine::msToFrames(5000);
        while (!gotClick && e.clock() < deadline && !e.quitRequested())
        {
            e.pumpInteractiveFrame();
            if (e.mouseIsDown())
                gotClick = true;
        }

        if (!gotClick)
        {
            // Too slow. The carriage goes back up empty.
            e.playMovie(3, true);
            e.refreshCard();
            return;
        }

        // In. The original hides the pointer for the whole ride and shows it
        // again at the top (jspit.cpp:291, :326); the RAII guard does the same
        // and cannot leak it on the way out.
        Engine::CursorHide hide{e};

        const std::int32_t boarded = e.stack().localCardForGlobal(0x18D4D);
        const std::int32_t riding = e.stack().localCardForGlobal(0x18AB5);
        const std::int32_t arrived = e.stack().localCardForGlobal(0x17167);
        if (boarded < 0 || riding < 0 || arrived < 0)
        {
            DebugLog::warn("carriage: a destination card is missing");
            return;
        }

        // The original builds one three-command script (jspit.cpp:305-309); the
        // port asks directly. changeToCard and NOT changeToStackAndCard:
        // all three are jspit's own, and the stack-changing form is deferred to
        // the end of the script, which is far too late -- the movie below
        // belongs to the card it lands on.
        e.changeToCard(static_cast<std::uint16_t>(boarded));
        e.scheduleTransition(Transition::PanLeft);
        e.changeToCard(static_cast<std::uint16_t>(riding));

        e.playMovie(1, true); // the ride up

        e.changeToCard(static_cast<std::uint16_t>(arrived));
    }

    // --- the whark number game -----------------------------------------------

    /// JSpit::redrawWharkNumberPuzzle (jspit.cpp:715-721). Card 769's PLST is
    /// 1 = the room, 2..11 = the ten numerals, 12/13 = the two spinner overlays.
    void redrawWharkNumberPuzzle(Engine &e, std::uint16_t overlay, int number)
    {
        e.beginScreenUpdate();
        e.activatePlst(overlay);
        e.activatePlst(static_cast<std::uint16_t>(number + 1));
        e.applyScreenUpdate();
    }

    /// JSpit::xschool280_playwhark (jspit.cpp:723-779). Riven's counting lesson:
    /// spin the wheel, read the numeral it stops on, and watch the villager walk
    /// that many steps closer to the whark.
    void xschool280_playwhark(Engine &e)
    {
        // Which of the two spinners the scripts have selected.
        const bool left = e.vars().get(VarId::JWharkPos) == 1;
        const VarId posVar = left ? VarId::JLeftPos : VarId::JRightPos;
        const std::uint16_t spinCode = left ? 1 : 2;
        const std::uint16_t overlayPlst = left ? 12 : 13;
        const std::uint16_t doomCode = left ? 3 : 5;
        const std::uint16_t snackCode = left ? 4 : 6;

        e.playMovie(spinCode, true); // seek(0) + playBlocking

        const int number = randomBetween(1, 10);
        redrawWharkNumberPuzzle(e, overlayPlst, number);

        // The walk is a slice of one long movie. Both films run 11560 ms and are
        // cut into 19 parts, one per position the villager can stand in, so the
        // slice IS the number the wheel gave (jspit.cpp:753-763).
        const std::uint32_t from = e.vars().get(posVar);
        const std::uint32_t to = from + static_cast<std::uint32_t>(number);
        e.playMovieRange(doomCode, (11560 / 19) * from, (11560 / 19) * to);
        // ScummVM's disable() after the ranged play, which bakes the villager's
        // new position into the card -- a ranged play does not bake by itself.
        e.enableMovie(doomCode, false);
        e.vars().set(posVar, to);

        if (to > 19)
        {
            // Nineteen steps is as far as the walkway goes.
            e.playMovie(snackCode, true);
            redrawWharkNumberPuzzle(e, overlayPlst, number);
            e.vars().set(posVar, 0);
        }

        // Exactly one of the two turn buttons is live at a time, and which one
        // alternates with every spin (jspit.cpp:774-778).
        const Hotspot *const rotateLeft = e.hotspotByName("rotateLeft");
        const Hotspot *const rotateRight = e.hotspotByName("rotateRight");
        if (rotateLeft != nullptr)
        {
            const std::size_t i = e.hotspotIndexOf(rotateLeft);
            e.enableHotspotByIndex(i, !e.hotspotEnabled(i));
        }
        if (rotateRight != nullptr)
        {
            const std::size_t i = e.hotspotIndexOf(rotateRight);
            e.enableHotspotByIndex(i, !e.hotspotEnabled(i));
        }
    }
} // namespace

bool runJspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    // --- the sunners ---------------------------------------------------------
    if (key == "xjlagoon700_alert")
    {
        xjlagoon700_alert(e);
        return true;
    }
    if (key == "xjlagoon800_alert")
    {
        xjlagoon800_alert(e);
        return true;
    }
    if (key == "xjlagoon1500_alert")
    {
        xjlagoon1500_alert(e);
        return true;
    }

    // --- the rebel tunnel ----------------------------------------------------
    if (key == "xreseticons")
    {
        xreseticons(e);
        return true;
    }
    if (key == "xicon")
    {
        xicon(e, args, argCount);
        return true;
    }
    if (key == "xtoggleicon")
    {
        xtoggleicon(e, args, argCount);
        return true;
    }
    if (key == "xcheckicons")
    {
        xcheckicons(e);
        return true;
    }
    // The four tables, verbatim from jspit.cpp:136-236. They are not a pattern
    // with an offset -- 105 and 106 overlap 103's and 104's ranges, because the
    // tunnels themselves overlap -- so they are written out.
    if (key == "xjtunnel103_pictfix")
    {
        static const int kBits[] = {0, 1, 2, 3, 22, 23, 24};
        tunnelPictfix(e, kBits, 7);
        return true;
    }
    if (key == "xjtunnel104_pictfix")
    {
        static const int kBits[] = {9, 10, 11, 12, 13, 14, 15, 16};
        tunnelPictfix(e, kBits, 8);
        return true;
    }
    if (key == "xjtunnel105_pictfix")
    {
        static const int kBits[] = {3, 4, 5, 6, 7, 8, 9};
        tunnelPictfix(e, kBits, 7);
        return true;
    }
    if (key == "xjtunnel106_pictfix")
    {
        static const int kBits[] = {16, 17, 18, 19, 20, 21, 22};
        tunnelPictfix(e, kBits, 7);
        return true;
    }

    // --- the beetles ---------------------------------------------------------
    if (key == "xjplaybeetle_550" || key == "xjplaybeetle_600"
        || key == "xjplaybeetle_950" || key == "xjplaybeetle_1050")
    {
        playBeetle(e, false);
        return true;
    }
    if (key == "xjplaybeetle_1450")
    {
        playBeetle(e, true); // only when the girl is not on the card
        return true;
    }

    // --- the whark elevator --------------------------------------------------
    if (key == "xhandlecontrolup")
    {
        xhandlecontrolup(e);
        return true;
    }
    if (key == "xhandlecontroldown")
    {
        xhandlecontroldown(e);
        return true;
    }
    if (key == "xhandlecontrolmid")
    {
        xhandlecontrolmid(e);
        return true;
    }

    // --- the gallows carriage ------------------------------------------------
    if (key == "xvga1300_carriage")
    {
        xvga1300_carriage(e);
        return true;
    }

    // --- the dome ------------------------------------------------------------
    // Five names, and every one of them is a one-line call into the shared
    // implementation -- which is the whole point of it being shared. bspit,
    // gspit, pspit and tspit will each add exactly this much.
    if (key == "xjdome25_resetsliders")
    {
        resetDomeSliders(e, kJspitDome);
        return true;
    }
    if (key == "xjdome25_slidermd")
    {
        dragDomeSlider(e, kJspitDome);
        return true;
    }
    if (key == "xjdome25_slidermw")
    {
        checkSliderCursorChange(e, kJspitDome);
        return true;
    }
    if (key == "xjscpbtn")
    {
        runDomeButtonMovie(e);
        return true;
    }
    if (key == "xjisland3500_domecheck")
    {
        runDomeCheck(e);
        return true;
    }

    // --- the whark number game -----------------------------------------------
    if (key == "xschool280_playwhark")
    {
        xschool280_playwhark(e);
        return true;
    }

    // --- registered by ScummVM, absent from this release's data ---------------
    // Listed rather than left to the fallback so that a release which DOES call
    // them does not print a line the player can do nothing about.
    if (key == "xjschool280_resetleft" || key == "xjschool280_resetright")
        return true; // DVD-only, and dummies there too (jspit.cpp:705-713)
    if (key == "xjatboundary")
        return true; // the demo's "buy the game" dialogue (jspit.cpp:781-783)

    return false;
}

void installJspitCardTimer(Engine &e)
{
    switch (e.globalCardId(e.cardId()))
    {
    case 0x77d6: // Sunners, top of stairs
        e.installTimer(sunnersTopStairsTimer, 500, "jspit sunners top");
        break;
    case 0x79bd: // Sunners, middle of stairs
        e.installTimer(sunnersMidStairsTimer, 500, "jspit sunners mid");
        break;
    case 0x7beb: // Sunners, bottom of stairs
        e.installTimer(sunnersLowerStairsTimer, 500, "jspit sunners lower");
        break;
    case 0xb6ca: // Sunners, shoreline
        e.installTimer(sunnersBeachTimer, 500, "jspit sunners beach");
        break;
    default:
        break;
    }
}

} // namespace rivenrt
