// External commands -- Riven's opcode 17.
//
// Riven's scripts call out to per-stack native code for anything the opcode table
// cannot express: a book's page turn, a slider's drag maths, a dome's combination
// check. ScummVM implements them per stack in engines/mohawk/riven_stacks/.
//
// ALL 131 OF THEM ARE IMPLEMENTED -- aspit and the shared 26, jspit 29, tspit
// 18, bspit 19, gspit 20, ospit 8, pspit 7, rspit 4. That is every name in every
// stack's NAME 3 resource in the shipped game, plus the handful ScummVM
// registers that this release's data does not call (the demo's boundary
// dialogues, the DVD's two school resets); the set matches ScummVM's
// REGISTER_COMMAND list exactly, in both directions. The fallback below still
// exists and still names anything it does not know, because a different release
// could carry a name this one does not -- but nothing in the 5-CD game reaches
// it.
//
// THIS FILE IS THE DISPATCHER, plus two things that are not one island's:
// aspit's commands, which are the menu and the books rather than a place, and
// the shared vocabulary declared in Externals.hpp -- the port's answer to
// ScummVM's RivenStack base class.
//
// The other seven islands have a file each -- ExternalsJspit, ExternalsTspit,
// ExternalsBspit, ExternalsGspit, ExternalsOspit, ExternalsPspit,
// ExternalsRspit -- and this hands over to them in turn. That is the same split
// ScummVM has, one riven_stacks/<stack>.cpp per island.
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
#include "engine/Externals.hpp"
#include "engine/Script.hpp"
#include "render/Credits.hpp"

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

    /// ASpit::xaatrusbookprevpage / xaatrusbooknextpage (aspit.cpp:176-218).
    /// Ten pages, no transition, and nothing extra to draw on any of them --
    /// which is the whole of the difference between this book and bspit's.
    void atrusBookPage(Engine &e, int delta)
    {
        turnBookPages(e, VarId::AAtrusBook, delta, 1, 10, Transition::None);
    }

    // --- Catherine's journal -------------------------------------------------
    //
    // Forty-nine pages, and unlike Atrus's it has two things to draw on top of
    // the page picture: the white paper edges that show how far through the book
    // you are, and -- on page 28 -- the telescope's combination, which is where
    // the player is meant to read it.

    /// ASpit::cathBookDrawTelescopeCombination (aspit.cpp:254-269). Five D'ni
    /// numerals out of five separate tBMPs, 13 through 17.
    ///
    /// tcorrectorder is a five-digit decimal number whose digits are the buttons
    /// in order, so each numeral is `digit - 1` strips along its own bitmap. The
    /// shape is bspit's lab-journal combination with a different encoding: there
    /// the position came out of a bitmask, here out of a decimal digit.
    void cathBookDrawTelescopeCombination(Engine &e)
    {
        constexpr int kNumberW = 32;
        constexpr int kNumberH = 25;
        constexpr int kDstX = 156;
        constexpr int kDstY = 247;
        constexpr std::uint16_t kFirstBitmap = 13;

        const std::uint32_t combo = e.vars().get(VarId::TCorrectOrder);

        for (int i = 0; i < 5; ++i)
        {
            // The digits are 1..5 and a zero would index a strip to the LEFT of
            // the bitmap. That is what a save written before the combination was
            // rolled looks like, and ScummVM would read off the front of the
            // image; here the numeral is simply left undrawn.
            const std::uint32_t digit = comboDigit(combo, static_cast<std::uint32_t>(i));
            if (digit < 1 || digit > 5)
                continue;

            const int offset = static_cast<int>(digit - 1) * kNumberW;
            CardSurface::Section s;
            s.src = Rect{static_cast<std::int16_t>(offset), 0,
                         static_cast<std::int16_t>(offset + kNumberW), kNumberH};
            s.dst = Rect{static_cast<std::int16_t>(kDstX + i * kNumberW), kDstY,
                         static_cast<std::int16_t>(kDstX + (i + 1) * kNumberW),
                         kDstY + kNumberH};
            e.drawBitmapSections(static_cast<std::uint16_t>(kFirstBitmap + i), &s, 1);
        }
    }

    /// ASpit::cathBookDrawPage (aspit.cpp:240-252), less the page picture --
    /// turnBookPages has already drawn that by the time this runs.
    void cathBookPageDrawn(Engine &e, std::uint32_t page)
    {
        // The white paper edges. Two records, and the gap at page 5 is the
        // original's: neither is drawn on the first page or the fifth.
        if (page > 1 && page < 5)
            e.activatePlst(50);
        else if (page > 5)
            e.activatePlst(51);

        if (page == 28)
            cathBookDrawTelescopeCombination(e);
    }

    /// ASpit::xacathopenbook (aspit.cpp:218-238). The same shape as Atrus's,
    /// with the extra drawing above.
    void xacathopenbook(Engine &e)
    {
        const std::uint32_t page = e.vars().get(VarId::ACathBook);

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

        // Bracketed, because the page and its overlays are one picture as far
        // as the player is concerned -- and because drawBitmapSections needs a
        // screen update around it to be published at all.
        e.beginScreenUpdate();
        e.activatePlst(static_cast<std::uint16_t>(page));
        cathBookPageDrawn(e, page);
        e.applyScreenUpdate();
    }

    // --- the trap book -------------------------------------------------------

    /// ASpit::xatrapbookopen / xatrapbookclose (aspit.cpp:323-345). The trap
    /// book on the shelf, which opens and shuts where the journals turn pages.
    ///
    /// atrap is what the card's own script draws from, so both of these end by
    /// re-entering the card rather than drawing anything themselves.
    void trapBook(Engine &e, bool open)
    {
        e.vars().set(VarId::ATrap, open ? 1u : 0u);

        // RivenStack::pageTurn: the sound and the transition, in that order
        // (riven_stack.cpp:403-413). A book opening is a page turn as far as
        // Riven is concerned, and it is the same pair of recordings.
        playPageTurnSound(e);
        e.scheduleTransition(open ? Transition::WipeLeft : Transition::WipeRight);

        // Closing only: the flyby movie keeps drawing over the shut book
        // otherwise, which ScummVM notes was a glitch in the original engine
        // too (aspit.cpp:329-333). stopMovie and not enableMovie(false): the
        // point is that the overlay stops being composited, and disable() would
        // bake its last frame into the very picture it is in the way of.
        if (!open)
            e.stopMovie(1);

        e.refreshCard();
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
        e.changeToStackAndCard(static_cast<StackId>(stackId), cardId);
    }

    /// ASpit::xaexittomain (aspit.cpp:463-469). Demo-only in the original, and
    /// the card it wants does not exist in the full game; kept pointing at the
    /// menu so the name is not simply unhandled.
    void backToMenu(Engine &e) { e.changeToCard(1); }

} // namespace

// ---------------------------------------------------------------------------
// The shared vocabulary (Externals.hpp)
// ---------------------------------------------------------------------------

std::uint32_t comboDigit(std::uint32_t combo, std::uint32_t digit)
{
    static const std::uint32_t powers[] = {100000, 10000, 1000, 100, 10, 1};
    if (digit + 1 >= sizeof(powers) / sizeof(powers[0]))
        return 0;
    return (combo % powers[digit]) / powers[digit + 1];
}

int randomBetween(int lo, int hi)
{
    if (hi <= lo)
        return lo;
    return lo + std::rand() % (hi - lo + 1);
}

std::uint16_t codeForMlstIndex(Engine &e, std::uint16_t index)
{
    const Card *const card = e.card();
    if (card != nullptr)
        for (const MovieRec &m : card->mlst)
            if (m.index == index)
                return m.slot;
    return index;
}

void playPageTurnSound(Engine &e)
{
    // 51 of 255, which is RivenStack::pageTurn's own volume
    // (riven_stack.cpp:412) and not a taste decision: a page turn is a small
    // paper noise a foot from your hands, and Riven mixes it that way. At the
    // default 255 it comes out as the loudest thing in either journal, because
    // the port has no gain headroom left to make anything else match it.
    e.playCardSound((std::rand() & 1) != 0 ? "aPage1" : "aPage2", 51);
}

EndingProbe endingProbe;

void runEndGame(Engine &e, std::uint16_t movieCode, std::uint32_t delayMs,
                std::uint32_t frameCountOverride, bool alreadyPlaying)
{
    if (endingProbe.armed)
    {
        // The console is asking which ending this is, not to watch it. Record
        // and fall through to the tail -- see EndingProbe.
        endingProbe.fired = true;
        endingProbe.movieCode = movieCode;
        endingProbe.delayMs = delayMs;
        endingProbe.frameCountOverride = frameCountOverride;
        endingProbe.alreadyPlaying = alreadyPlaying;
    }

    // The ambient stops before the movie starts: an ending is silent under its
    // own soundtrack, and Riven's last five minutes have their own
    // (riven_stack.cpp:209).
    e.stopAllAmbient();
    e.stopEffects();

    if (endingProbe.armed)
    {
        // Neither the video nor the roll, and the movie that may already be
        // running is stopped rather than left decoding under the menu.
        if (alreadyPlaying)
            e.stopMovie(movieCode);
    }
    else if (alreadyPlaying)
    {
        // Watch it out rather than start it. This is the loop runCredits spins
        // (riven_stack.cpp:239-257) with the credits taken out of it -- they
        // are rolled below instead, once, for all three call sites.
        while (!e.movieEnded(movieCode) && !e.quitRequested())
            e.pumpIdleFrame();
    }
    else
    {
        e.playMovie(movieCode, true);
    }

    // The roll. ScummVM interleaves it with the video's own loop because the
    // video's AUDIO keeps playing under the credits after its picture ends
    // (riven_stack.cpp:227-232 says so in as many words). Here the movie has
    // been played to its end by the line above and there is nothing left of it
    // to run under, so the roll follows rather than overlaps -- and `delayMs`,
    // which ScummVM measures from the last picture frame, is the pause before
    // it starts either way.
    if (!endingProbe.armed && !e.quitRequested())
        runCredits(e, delayMs);

    // riven_stack.cpp:262-270: the game state is thrown away and the player is
    // put back on the menu. startNewGame cannot clear the zip destinations --
    // they are visited-card history rather than a variable -- so this has to,
    // exactly as xanewgame does.
    e.vars().startNewGame();
    e.clearZipDests();

    // Aspit card 1, by its LOCAL id, which is what ScummVM asks for
    // (riven_stack.cpp:268-269 builds RivenStackChangeCommand with byStackCardId
    // set). Not a global id, and there is no telling the two apart by value --
    // see changeToStackAndCard.
    //
    // Deferred, because this is running inside a hotspot script and the
    // interpreter is still holding the stack it would replace.
    e.changeToStackAndCard(StackId::Aspit, 1, true);

    if (endingProbe.armed)
    {
        // OBSERVED, not asserted. A flag set on the line after the call it is
        // meant to be about proves only that control reached that line, which is
        // the difference between a check and a decoration -- so both of these
        // read the engine back.
        //
        // `reset` looks at a variable the harness had just set to something
        // else: startNewGame's defaults have agehn and pcage at zero, so either
        // one still carrying a case's value means the state was not thrown away.
        // `returned` is only meaningful because the console runs outside the
        // interpreter, where the change above is immediate rather than deferred.
        endingProbe.reset = e.vars().get(VarId::AGehn) == 0
                            && e.vars().get(VarId::PCage) == 0
                            && e.vars().get(VarId::ATrapBook) == 0;
        endingProbe.returned = e.stack().id == StackId::Aspit && e.cardId() == 1;
    }
}

void turnBookPages(Engine &e, VarId pageVar, int delta, std::uint32_t firstPage,
                   std::uint32_t lastPage, Transition transition,
                   void (*onPage)(Engine &, std::uint32_t))
{
    if (delta == 0)
        return;

    // ONE PAGE PER PRESS, and this is the port's one deliberate divergence from
    // RivenStack's book loop.
    //
    // ScummVM turns pages `while (keepTurningPages())` -- while the mouse is
    // still held (riven_stack.cpp:414-416, and the while in
    // BSpit::xblabbooknextpage) -- so that holding the corner riffles through
    // the book. On a mouse that is a deliberate press-and-hold. On a resistive
    // touchscreen a press that outlasts a page turn AND its sound is simply how
    // a tap feels, so the same loop would shut a twenty-two-page journal on one
    // stylus poke. There is no threshold that separates the two, because there
    // is no second button to hold.
    std::uint32_t &page = e.vars().at(pageVar);
    if (delta < 0 && page <= firstPage)
        return;
    if (delta > 0 && page >= lastPage)
        return;
    page = static_cast<std::uint32_t>(static_cast<int>(page) + delta);

    playPageTurnSound(e);

    // BRACKETED, and it has to be. scheduleTransition only records a request;
    // the thing that runs it is a completed screen update, and there is no
    // other one coming -- the lab journal's hotspots are a bare opcode 17 with
    // no 20/21 around it, and nothing in the frame loop publishes on its own.
    // Without the pair the wipe never plays, and worse, the request sits in the
    // engine until the next card entry publishes it over a card it has nothing
    // to do with. This is the shape the gallows carriage already uses
    // (ExternalsJspit.cpp).
    //
    // The transition goes BEFORE the picture: Riven schedules the transition it
    // wants and then draws what it applies to.
    e.beginScreenUpdate();
    if (transition != Transition::None)
        e.scheduleTransition(transition);
    e.activatePlst(static_cast<std::uint16_t>(page));
    if (onPage != nullptr)
        onPage(e, page);
    e.applyScreenUpdate();

    // Waiting for the sound out is what makes a page turn feel like one thing
    // rather than a sound over a picture -- and letting go ends the wait, which
    // is RivenStack::waitForPageTurnSound's `&& keepTurningPages()`
    // (riven_stack.cpp:418-422). Without that test a second tap arriving during
    // the first turn's sound is swallowed and the book stops a page short.
    //
    // pumpInteractiveFrame and not pumpIdleFrame: the idle one deliberately
    // does not read the button, so mouseIsDown() would never change and the
    // test would be decoration.
    while (e.isEffectPlaying() && e.mouseIsDown() && !e.quitRequested())
        e.pumpInteractiveFrame();
}

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
    if (key == "xacathopenbook")
    {
        xacathopenbook(e);
        return;
    }
    // Forty-nine pages, and the wipes run the other way round to Atrus's
    // journal: Catherine's book is read top to bottom (aspit.cpp:288, :309).
    if (key == "xacathbookprevpage")
    {
        turnBookPages(e, VarId::ACathBook, -1, 1, 49, Transition::WipeDown,
                      cathBookPageDrawn);
        return;
    }
    if (key == "xacathbooknextpage")
    {
        turnBookPages(e, VarId::ACathBook, +1, 1, 49, Transition::WipeUp,
                      cathBookPageDrawn);
        return;
    }
    if (key == "xatrapbookopen")
    {
        trapBook(e, true);
        return;
    }
    if (key == "xatrapbookclose")
    {
        trapBook(e, false);
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

    // --- the islands ---------------------------------------------------------
    // A file per island, and this is the whole of them here. ScummVM splits the
    // same way, one riven_stacks/<stack>.cpp per island; the difference is only
    // that its registration is a table built at construction and this is a name
    // test, for the reason at the top of this file.
    //
    // Order does not matter -- every name in the game carries its island's
    // prefix, and the handful that do not (xtakeit, xdrawmarbles, xflies,
    // xicon, xbait, xbookclick, xgwatch) are still unique -- so they are in the
    // order the islands were written.
    if (runJspitExternal(e, key, args, argCount))
        return;
    if (runTspitExternal(e, key, args, argCount))
        return;
    if (runBspitExternal(e, key, args, argCount))
        return;
    if (runGspitExternal(e, key, args, argCount))
        return;
    if (runOspitExternal(e, key, args, argCount))
        return;
    if (runPspitExternal(e, key, args, argCount))
        return;
    if (runRspitExternal(e, key, args, argCount))
        return;

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
    // jspit's and pspit's are the only two overrides of an otherwise empty base
    // (riven_stack.cpp:272), so every other stack falls through to nothing. No
    // removeTimer in the default case: changeToCard has already cleared it
    // before this is reached.
    if (e.stack().id == StackId::Jspit)
        installJspitCardTimer(e);
    else if (e.stack().id == StackId::Pspit)
        installPspitCardTimer(e);
}

} // namespace rivenrt
