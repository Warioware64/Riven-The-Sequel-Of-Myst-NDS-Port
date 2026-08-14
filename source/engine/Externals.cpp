// External commands -- Riven's opcode 17.
//
// Riven's scripts call out to per-stack native code for anything the opcode table
// cannot express: a book's page turn, a slider's drag maths, a dome's combination
// check. ScummVM implements them per stack in engines/mohawk/riven_stacks/, and
// the full set is milestone 6.
//
// What is here is what the boot path and the intro reach -- aspit's menu and
// books, and the two tspit commands the opening cutscene calls -- plus the
// telescope's cover combination and its up stroke. Anything else is
// reported by name and does nothing, which makes an unported command a control
// that does not respond rather than a crash -- and the log line names exactly
// which one to write next.
//
// Dispatch is by NAME rather than by index, and it has to be: the index is into
// the calling stack's own NAME 2 list, so the same number is a different command
// in a different stack. The name is the only stable identity, which is why the
// converter keeps the lists.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "DebugLog.hpp"
#include "MainMenu.hpp"
#include "engine/Engine.hpp"
#include "engine/Script.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// ASpit::xasetupcomplete (aspit.cpp:142-148). The original also writes an
    /// ini entry to stop offering Setup; there is nothing to write to here.
    /// 0xE2E is the menu card's RMAP global id.
    void xasetupcomplete(Engine &e)
    {
        const std::int32_t local = e.stack().localCardForGlobal(0xE2E);
        if (local >= 0)
            e.changeToCard(static_cast<std::uint16_t>(local));
        else
            DebugLog::warn("xasetupcomplete: no card for global id 0xE2E");
    }

    /// ASpit::xaatrusopenbook (aspit.cpp:150-171). Arms the page hotspots for the
    /// page the book is open at and draws it. The journal itself is a card, so
    /// this is the one piece of book logic the menu path can reach.
    void xaatrusopenbook(Engine &e)
    {
        const std::uint32_t page = e.vars().get(VarId::AAtrusBook);

        const Hotspot *openBook = e.hotspotByName("openBook");
        const Hotspot *nextPage = e.hotspotByName("nextpage");
        const Hotspot *prevPage = e.hotspotByName("prevpage");

        const bool first = page == 1;
        if (prevPage != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(prevPage), !first);
        if (nextPage != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(nextPage), !first);
        if (openBook != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(openBook), first);

        e.activatePlst(static_cast<std::uint16_t>(page));
    }

    /// ASpit::xaatrusbookprevpage / xaatrusbooknextpage (aspit.cpp:176-218),
    /// without the page-turn sound the original plays from the stack's own tWAV
    /// ids -- those are effects, and opcode 4 is the only thing that names one.
    void atrusBookPage(Engine &e, int delta)
    {
        std::uint32_t &page = e.vars().at(VarId::AAtrusBook);
        if (delta < 0 && page <= 1)
            return;
        if (delta > 0 && page >= 10)
            return; // Atrus's journal is ten pages
        page = static_cast<std::uint32_t>(static_cast<int>(page) + delta);
        e.activatePlst(static_cast<std::uint16_t>(page));
    }

    /// RivenInventory::backFromItemScript (riven_inventory.cpp:156-167): out of
    /// a journal and back to the card the player opened it from.
    ///
    /// This used to be "go to the menu card", which was wrong twice over: it
    /// stranded anyone who opened a journal mid-game on the main menu, and it
    /// conflated four different commands. The pair of variables it reads is
    /// written by the inventory strip at the moment of the click, and nothing
    /// else writes them -- which is why the strip had to exist before this could
    /// be right.
    void backFromItem(Engine &e)
    {
        e.stopEffects();

        const std::uint32_t stackId = e.vars().get(VarId::ReturnStackId);
        const std::uint32_t cardId = e.vars().get(VarId::ReturnCardId);
        if (stackId == 0 || cardId == 0)
        {
            // Opened from the menu rather than from the strip -- there is nowhere
            // to go back to, so the menu is the only sensible destination.
            e.changeToCard(1);
            return;
        }
        e.changeToStackAndGlobalCard(static_cast<StackId>(stackId), cardId);
    }

    /// ASpit::xaexittomain (aspit.cpp:463-469). Demo-only in the original, and
    /// the card it wants does not exist in the full game; kept pointing at the
    /// menu so the name is not simply unhandled.
    void backToMenu(Engine &e) { e.changeToCard(1); }

    /// RivenStack::getComboDigit (riven_stack.cpp:197-200). A combination is
    /// stored as a decimal number whose digits are the buttons in order, so
    /// digit 0 of 51234 is 5. Six powers for five digits: the divisor of digit n
    /// is powers[n+1].
    std::uint32_t comboDigit(std::uint32_t combo, std::uint32_t digit)
    {
        static const std::uint32_t powers[] = {100000, 10000, 1000, 100, 10, 1};
        if (digit + 1 >= sizeof(powers) / sizeof(powers[0]))
            return 0;
        return (combo % powers[digit]) / powers[digit + 1];
    }

    /// TSpit::xtisland390_covercombo (tspit.cpp:165-178). The five buttons on the
    /// telescope cover: press them in the order written on the island and the
    /// hatch unlocks, get one wrong and the count goes back to nothing.
    void xtisland390_covercombo(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 1 || args == nullptr)
        {
            DebugLog::warn("xtisland390_covercombo: no button number");
            return;
        }

        std::uint32_t &correctDigits = e.vars().at(VarId::TCoverCombo);
        if (correctDigits < 5
            && args[0] == comboDigit(e.vars().get(VarId::TCorrectOrder), correctDigits))
            ++correctDigits;
        else
            correctDigits = 0;

        const Hotspot *openCover = e.hotspotByName("openCover");
        if (openCover != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(openCover), correctDigits == 5);
    }

    /// TSpit::xtexterior300_telescopeup (tspit.cpp:102-136). One press raises the
    /// tube one of its five positions, and the animation is the matching slice of
    /// one long movie -- which is what Engine::playMovieRange exists for.
    ///
    /// Without the sounds. The original names them ("tTeleMove", and "tTelDnMore"
    /// when the tube will not move) and playCardSound resolves a name to a tWAV
    /// through the archive's resource names (riven_sound.cpp:73-77), which the
    /// converter does not keep -- Engine::playEffect takes an id. Same omission,
    /// and the same reason, as the page turn in atrusBookPage above.
    /// The half that can decide to do nothing. Split out so the caller can put
    /// the card back exactly once, on every path -- see below.
    void telescopeUpMove(Engine &e)
    {
        // No power, no telescope.
        if (e.vars().get(VarId::TTeleValve) == 0)
            return;

        std::uint32_t &pos = e.vars().at(VarId::TTelescope);
        if (pos >= 5)
            return; // already at the top

        // Where each position starts in the travel movie, in milliseconds
        // (tspit.cpp:93). Six entries for five moves: a move runs from its own
        // position to the next one.
        static const std::uint32_t kStops[] = {0, 800, 1680, 2560, 3440, 4320};
        const std::uint32_t from = pos >= 1 ? pos - 1 : 0;

        // Two movies of the same travel, one with the cover on and one without.
        e.playMovieRange(e.vars().get(VarId::TTeleCover) != 0 ? 4 : 5, kStops[from],
                         kStops[from + 1]);

        ++pos;
    }

    void xtexterior300_telescopeup(Engine &e)
    {
        // The button goes down whether or not anything comes of it.
        e.playMovie(3, true);
        telescopeUpMove(e);

        // ALWAYS, and not only when the tube moved, which is where ScummVM
        // stops -- it returns straight out of the two "nothing happens" paths.
        //
        // The button is a blocking play that runs to the movie's end, and both
        // engines BAKE such a frame into the card: ScummVM's playBlocking()
        // ends in disable() (riven_video.cpp:264-268), and this port matches it
        // (Engine::kBakeBlockingMovies). tMOV 38 ends on a frame that is
        // nothing like the still underneath it -- measured against all twenty
        // of card 137's stills -- so once baked it is the card, and pressing
        // the button with the valve shut leaves a 33x40 patch of it on screen
        // until something redraws. ScummVM has the same hole here; the port
        // does not want it, so it asks for the redraw.
        //
        // The card also draws the tube at its new position from the variable,
        // so the moving case needed this anyway (ScummVM: enter(false)).
        e.refreshCard();
    }

    // --- jspit: the sunners -------------------------------------------------
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

    /// ScummVM's getRandomNumberRng: INCLUSIVE at both ends
    /// (common/random.cpp:56 -- getRandomNumber(max - min) + min, and
    /// getRandomNumber(n) itself returns 0..n).
    int randomBetween(int lo, int hi)
    {
        return lo + std::rand() % (hi - lo + 1);
    }

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

    /// The MLST "code" of a record addressed by its INDEX.
    ///
    /// Only the beach timer needs this, and it needs it because ScummVM's
    /// version conflates the two: jspit.cpp:692-694 hands the same number to
    /// RivenCard::playMovie, which takes an index, and to openSlot, which takes
    /// a code. On card 632 they happen to agree (records 1..8 sit on codes 1..8)
    /// and the bug is invisible; it is not a coincidence worth relying on.
    std::uint16_t codeForMlstIndex(Engine &e, std::uint16_t index)
    {
        const Card *const card = e.card();
        if (card != nullptr)
            for (const MovieRec &m : card->mlst)
                if (m.index == index)
                    return m.slot;
        return index;
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
        e.installTimer(sunnersTopStairsTimer, timerTime);
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
        e.installTimer(sunnersMidStairsTimer, timerTime);
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
        e.installTimer(sunnersLowerStairsTimer, timerTime);
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
        e.installTimer(sunnersBeachTimer, timerTime);
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
} // namespace

void runExternalCommand(Engine &e, std::uint16_t nameIndex, const std::uint16_t *args,
                        std::size_t argCount)
{
    const std::string name = e.nameFromList(kExternalCommandNames, nameIndex);
    if (name.empty())
    {
        DebugLog::warn("external command: no NAME 2 entry %u",
                    static_cast<unsigned>(nameIndex));
        return;
    }

    // Case-insensitive, because the NAME lists are mixed case and ScummVM's
    // registrations are not.
    const std::string key = Vars::normalise(name);

    if (key == "xastartupbtnhide")
    {
        // The original hides Start/Setup based on an ini entry, and ScummVM
        // returns immediately for everything but the 25th-anniversary release
        // (aspit.cpp:88-93). Nothing to do.
        return;
    }
    if (key == "xasetupcomplete")
    {
        xasetupcomplete(e);
        return;
    }
    if (key == "xaatrusopenbook")
    {
        xaatrusopenbook(e);
        return;
    }
    if (key == "xaatrusbookprevpage")
    {
        atrusBookPage(e, -1);
        return;
    }
    if (key == "xaatrusbooknextpage")
    {
        atrusBookPage(e, +1);
        return;
    }
    // The three book-back commands all link home (aspit.cpp:172-174, :271-273,
    // :317-321); only the trap book also puts itself away first.
    if (key == "xaatrusbookback" || key == "xacathbookback")
    {
        backFromItem(e);
        return;
    }
    if (key == "xtrapbookback")
    {
        e.vars().at(VarId::ATrap) = 0;
        backFromItem(e);
        return;
    }
    if (key == "xaexittomain")
    {
        backToMenu(e);
        return;
    }
    if (key == "xademoquit")
    {
        e.requestQuit();
        return;
    }
    if (key == "xadisablemenureturn" || key == "xaenablemenureturn")
    {
        // These toggle whether the menu offers Return. It is always offered
        // here, which is the same as the original with a started game.
        return;
    }
    // aspit.cpp:441 and :450: the intro hides the strip and putting it back is
    // what ends the cutscene as far as the inventory is concerned.
    if (key == "xadisablemenuintro")
    {
        e.inventory().setForcedHidden(true);
        return;
    }
    if (key == "xaenablemenuintro")
    {
        e.inventory().setForcedHidden(false);
        return;
    }
    if (key == "xthideinventory")
    {
        // EMPTY, and matching ScummVM's, which is also empty (tspit.cpp:190).
        //
        // The name invites forceHidden(true) and this port had it, with no
        // counterpart anywhere -- so from the opening cutscene onwards the strip
        // was hidden for the rest of the game and Atrus's journal, which nothing
        // else reaches, was unreachable. Nothing needs it: the strip is already
        // hidden during the cutscene by fullscreenMoviePlaying() and on aspit by
        // the stack test (Inventory::update), which is why ScummVM can afford to
        // leave it empty too.
        return;
    }
    if (key == "xaoptions")
    {
        // Riven's own menu has an Options button, and this is what it is wired
        // to. The screen it opens is the port's, not the original's -- the
        // original's is a card whose controls are for a mouse and a CD -- but
        // it is reached from the button the player expects.
        mainMenu.runSettings();
        return;
    }
    if (key == "xasavegame")
    {
        // Riven's own menu card has Save and Restore buttons, and they open the
        // port's slot list rather than the original's file dialogue -- which was
        // built for a mouse, a keyboard and a hard disc. Same argument as
        // xaoptions above: the screen is ours, the button is the one the player
        // expects.
        mainMenu.runSavePicker();
        return;
    }
    if (key == "xarestoregame")
    {
        mainMenu.runLoadPicker();
        return;
    }
    if (key == "xaresumegame")
    {
        // NOT a save at all, and it never was -- it only shared a line with the
        // other two because all three sit on the same menu card. Resume means
        // "put me back where I was", which is the pair of variables the
        // inventory writes before it links away, so this is the journal's back
        // button under another name (backFromItem, above).
        backFromItem(e);
        return;
    }
    if (key == "xanewgame")
    {
        e.vars().startNewGame();
        // Zip destinations are visited-card history, not a variable, so
        // startNewGame cannot clear them and this has to (riven.cpp:679).
        // Without it a new game would start with the last one's shortcuts.
        e.clearZipDests();
        return;
    }
    if (key == "xalaunchbrowser")
        return; // there is no browser to launch

    // --- tspit: the telescope ------------------------------------------------
    if (key == "xtisland390_covercombo")
    {
        xtisland390_covercombo(e, args, argCount);
        return;
    }
    if (key == "xtexterior300_telescopeup")
    {
        xtexterior300_telescopeup(e);
        return;
    }

    // --- tspit: the opening cutscene ----------------------------------------
    // Both of these are EMPTY in ScummVM too -- tspit.cpp has them as a comment
    // and nothing else. The inventory they describe is granted by the card's own
    // variables; the commands exist so the original's script had somewhere to
    // call. Listed rather than left to the fallback so the intro does not print
    // two "not implemented" lines every time it plays.
    if (key == "xtatrusgivesbooks")
        return; // "Give the player Atrus' Journal and the Trap book"
    if (key == "xtchotakesbook")
        return; // "And now Cho takes the trap book"

    // --- jspit: the sunners --------------------------------------------------
    if (key == "xjlagoon700_alert")
    {
        xjlagoon700_alert(e);
        return;
    }
    if (key == "xjlagoon800_alert")
    {
        xjlagoon800_alert(e);
        return;
    }
    if (key == "xjlagoon1500_alert")
    {
        xjlagoon1500_alert(e);
        return;
    }

    // --- everywhere: the insects --------------------------------------------
    // RivenStack::xflies (riven_stack.cpp:193-195), registered on the base class
    // rather than per stack because two islands use it: jspit's jungle by night
    // (fireflies) and by day, and gspit's. args[0] is the fireflies flag and
    // args[1] the count, which the shipped data keeps between 1 and 5.
    if (key == "xflies")
    {
        if (argCount >= 2)
            e.setFliesEffect(args[1], args[0] == 1);
        return;
    }

    DebugLog::warn("external command: %s is not implemented", name.c_str());
}

void installCardTimer(Engine &e)
{
    // JSpit::installCardTimer (jspit.cpp:785-802) is the only override of an
    // otherwise empty base (riven_stack.cpp:272), so every other stack falls
    // through to nothing. No removeTimer in the default case: changeToCard has
    // already cleared it before this is reached.
    if (e.stack().id != StackId::Jspit)
        return;

    switch (e.globalCardId(e.cardId()))
    {
    case 0x77d6: // Sunners, top of stairs
        e.installTimer(sunnersTopStairsTimer, 500);
        break;
    case 0x79bd: // Sunners, middle of stairs
        e.installTimer(sunnersMidStairsTimer, 500);
        break;
    case 0x7beb: // Sunners, bottom of stairs
        e.installTimer(sunnersLowerStairsTimer, 500);
        break;
    case 0xb6ca: // Sunners, shoreline
        e.installTimer(sunnersBeachTimer, 500);
        break;
    default:
        break;
    }
}

} // namespace rivenrt
