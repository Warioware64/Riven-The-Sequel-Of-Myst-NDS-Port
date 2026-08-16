// Gehn's office on the 233rd Age -- ScummVM's riven_stacks/ospit.cpp.
//
// Not an island and barely a place: five rooms at the end of the game, where
// Gehn asks the player to step into a book. Almost everything here is about
// that one scene:
//
//   * the CAGE, which is xbookclick -- the only external command in Riven that
//     runs INSIDE a movie, watching its clock for the window in which the
//     player may use the trap book, and which ends the game three ways;
//   * GEHN'S JOURNAL, thirteen pages;
//   * the WATCH, which plays the prison combination at the player as three
//     tones, five times; and
//   * two pieces of tidying -- a book that shuts itself and a drawer that does.
//
// Plus xorollcredittime, which is what happens to a player who uses the trap
// book on themselves.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "DebugLog.hpp"
#include "engine/Engine.hpp"
#include "engine/Externals.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// OSpit::xorollcredittime (ospit.cpp:47-67). The player used the trap book,
    /// and the only question left is which ending they have earned.
    void xorollcredittime(Engine &e)
    {
        // Used on Tay rather than here, which ScummVM handles as a special case
        // because its "special change" machinery only carries one destination
        // (ospit.cpp:48-56). 0x3338 is an RMAP GLOBAL id -- byStackId set and
        // byStackCardId clear in the command it builds -- so it is resolved
        // against rspit's own table rather than taken as a local card number.
        if (e.vars().get(VarId::ReturnStackId) == static_cast<std::uint32_t>(StackId::Rspit))
        {
            e.changeToStackAndCard(StackId::Rspit, 0x3338);
            return;
        }

        // The three waits are ScummVM's own (ospit.cpp:62-66) and they differ by
        // four seconds between the endings, because each video holds its last
        // frame for a different length of time before the credits belong over
        // it. The third number is the Polish frame-count override, carried
        // unused -- see runEndGame.
        const std::uint32_t gehn = e.vars().get(VarId::AGehn);
        if (gehn == 0)
            runEndGame(e, 1, 9500, 1225); // Gehn who?
        else if (gehn == 4)
            runEndGame(e, 2, 12000, 558); // you freed him
        else
            runEndGame(e, 3, 8000, 857); // you had already met him
    }

    // --- the cage ------------------------------------------------------------

    /// The trap book actually being used: the four seconds between the player
    /// clicking it and standing outside the cage with Gehn inside it
    /// (ospit.cpp:110-129).
    ///
    /// Split out from xbookclick because it is the only path that ends the
    /// scene, and because everything in it happens in a fixed order that the
    /// watching loop above has no part in.
    void useTrapBook(Engine &e)
    {
        // closeVideos() and not disableAllMovies: Gehn's speech has to stop
        // WITHOUT its last frame being baked into the card that is about to go
        // black (ospit.cpp:110).
        e.closeAllMovies();

        Engine::CursorHide hide{e};

        e.beginScreenUpdate();
        e.scheduleTransition(Transition::Blend);
        e.activatePlst(3); // black
        e.applyScreenUpdate();

        e.playEffect(0, 256); // the link sound
        e.delay(12000);

        e.activateMlst(7, false);
        e.playMovie(codeForMlstIndex(e, 7), true); // Gehn links out

        e.vars().set(VarId::OCage, 1);
        e.vars().set(VarId::AGehn, 4);     // trapped
        e.vars().set(VarId::ATrapBook, 1); // and the book is yours again

        e.playEffect(0, 256);
        e.scheduleTransition(Transition::Blend);

        const std::int32_t local = e.stack().localCardForGlobal(0x2885);
        if (local < 0)
        {
            DebugLog::warn("xbookclick: no card for global id 0x2885");
            return;
        }
        e.changeToCard(static_cast<std::uint16_t>(local));

        // Two seconds of the strip held up, which is how the player is told
        // they have the trap book back. Forced because nothing is pointing at
        // it -- see Inventory::setForcedVisible.
        e.inventory().setForcedVisible(true);
        e.delay(2000);
        e.inventory().setForcedVisible(false);

        // "Stop all running scripts (so we don't remain in the cage)"
        // (ospit.cpp:128). The commands after this one on the cage's hotspot
        // belong to a card the player has just left.
        e.stopScripts();
    }

    /// OSpit::xbookclick (ospit.cpp:69-150). Gehn's speech is already playing
    /// when this is called; the arguments say where in it the trap book is
    /// offered and for how long.
    ///
    /// args are (movie code, start, end, hotspot number). The two times are in
    /// QuickTime's 1/600 second units, which is why they are converted rather
    /// than used.
    void xbookclick(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 4 || args == nullptr)
        {
            DebugLog::warn("xbookclick: %u arguments, not 4",
                           static_cast<unsigned>(argCount));
            return;
        }

        const std::uint16_t code = args[0];
        const std::uint32_t startTime = static_cast<std::uint32_t>(args[1]) * 1000 / 600;
        const std::uint32_t endTime = static_cast<std::uint32_t>(args[2]) * 1000 / 600;

        char hotspotName[16];
        std::snprintf(hotspotName, sizeof(hotspotName), "touchBook%u",
                      static_cast<unsigned>(args[3]));
        const Hotspot *const hotspot = e.hotspotByName(hotspotName);
        if (hotspot == nullptr)
        {
            DebugLog::warn("xbookclick: card %u has no \"%s\"",
                           static_cast<unsigned>(e.cardId()), hotspotName);
            return;
        }
        const Rect book = e.hotspotRect(hotspot);

        const auto pointerOnBook = [&e, &book] {
            return e.pointerCardX() >= book.left && e.pointerCardX() < book.right
                   && e.pointerCardY() >= book.top && e.pointerCardY() < book.bottom;
        };

        // Gehn is still talking; nothing may be clicked yet.
        while (e.movieTimeMs(code) < startTime && !e.quitRequested())
            e.pumpIdleFrame();
        if (e.quitRequested())
            return;

        // He has opened the book and asked. This is the window.
        //
        // pumpInteractiveFrame and not pumpIdleFrame: the idle one deliberately
        // does not read the button or move the pointer, so mouseIsDown() would
        // never become true and the offer could not be taken.
        while (e.movieTimeMs(code) < endTime && !e.quitRequested())
        {
            e.setCursor(pointerOnBook() ? kCursorOpenHand : kCursorMain);
            if (e.mouseIsDown() && pointerOnBook())
            {
                e.setCursor(kCursorMain);
                useTrapBook(e);
                return;
            }
            e.pumpInteractiveFrame();
        }
        e.setCursor(kCursorMain);
        if (e.quitRequested())
            return;

        // No click. The third time he asks is the last time: he shoots, and the
        // rest of the movie already on screen IS the ending -- so it is watched
        // out rather than restarted (ospit.cpp:140-146).
        if (e.vars().get(VarId::AGehn) == 3)
        {
            runEndGame(e, code, 5000, 995, /*alreadyPlaying=*/true);
            return;
        }

        // Otherwise he puts the book away and the scene runs on.
        e.playMovie(code, true);
    }

    // --- the office ----------------------------------------------------------

    /// OSpit::xooffice30_closebook (ospit.cpp:152-176). The blank linking book
    /// on the desk shuts itself when the player looks away from it.
    void xooffice30_closebook(Engine &e)
    {
        std::uint32_t &book = e.vars().at(VarId::ODeskBook);
        if (book != 1)
            return;
        book = 0;

        e.playMovie(1, true);

        const Hotspot *const closeBook = e.hotspotByName("closeBook");
        const Hotspot *const nullSpot = e.hotspotByName("null");
        const Hotspot *const openBook = e.hotspotByName("openBook");
        if (closeBook != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(closeBook), false);
        if (nullSpot != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(nullSpot), false);
        if (openBook != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(openBook), true);

        e.activatePlst(1);
    }

    // --- the watch -----------------------------------------------------------

    /// OSpit::xgwatch (ospit.cpp:231-250). Gehn's pocket watch plays the prison
    /// elevator's combination: five digits, each one of three tones, half a
    /// second apart -- and then the watch itself, so the player can see which
    /// hand went where.
    ///
    /// The one place in the game a puzzle's answer is GIVEN rather than found,
    /// and it is given in a form the player still has to write down.
    void xgwatch(Engine &e)
    {
        Engine::CursorHide hide{e};

        const std::uint32_t combo = e.vars().get(VarId::PCorrectOrder);
        for (std::uint32_t i = 0; i < 5 && !e.quitRequested(); ++i)
        {
            // The digits are 1..3 and the tones are tWAV 14, 15 and 16.
            e.playEffect(static_cast<std::uint16_t>(comboDigit(combo, i) + 13), 256);
            e.delay(500);
        }

        e.activateMlst(1, false);
        e.playMovie(codeForMlstIndex(e, 1), true);
    }

} // namespace

bool runOspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    // --- the cage ------------------------------------------------------------
    if (key == "xbookclick")
    {
        xbookclick(e, args, argCount);
        return true;
    }
    if (key == "xorollcredittime")
    {
        xorollcredittime(e);
        return true;
    }

    // --- the office ----------------------------------------------------------
    if (key == "xooffice30_closebook")
    {
        xooffice30_closebook(e);
        return true;
    }
    if (key == "xobedroom5_closedrawer")
    {
        // Reaching for the journal shuts the drawer under it
        // (ospit.cpp:178-183).
        e.playMovie(2, true);
        e.vars().set(VarId::OStandDrawer, 0);
        return true;
    }

    // --- Gehn's journal ------------------------------------------------------
    if (key == "xogehnopenbook")
    {
        e.activatePlst(static_cast<std::uint16_t>(e.vars().get(VarId::OGehnPage)));
        return true;
    }
    if (key == "xogehnbookprevpage")
    {
        turnBookPages(e, VarId::OGehnPage, -1, 1, 13, Transition::WipeRight);
        return true;
    }
    if (key == "xogehnbooknextpage")
    {
        turnBookPages(e, VarId::OGehnPage, +1, 1, 13, Transition::WipeLeft);
        return true;
    }

    // --- the watch -----------------------------------------------------------
    // Named for Garden Island and living here, which is the game's own filing
    // and not a mistake: the watch is an ospit prop and its NAME 3 entry is in
    // ospit's list (ScummVM registers it on OSpit too, ospit.cpp:44).
    if (key == "xgwatch")
    {
        xgwatch(e);
        return true;
    }

    return false;
}

} // namespace rivenrt
