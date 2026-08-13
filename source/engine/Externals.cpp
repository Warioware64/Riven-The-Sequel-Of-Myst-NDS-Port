// External commands -- Riven's opcode 17.
//
// Riven's scripts call out to per-stack native code for anything the opcode table
// cannot express: a book's page turn, a slider's drag maths, a dome's combination
// check. ScummVM implements them per stack in engines/mohawk/riven_stacks/, and
// the full set is milestone 6.
//
// What is here is what the boot path and the intro reach: aspit's menu and books,
// and the two tspit commands the opening cutscene calls. Anything else is
// reported by name and does nothing, which makes an unported command a control
// that does not respond rather than a crash -- and the log line names exactly
// which one to write next.
//
// Dispatch is by NAME rather than by index, and it has to be: the index is into
// the calling stack's own NAME 2 list, so the same number is a different command
// in a different stack. The name is the only stable identity, which is why the
// converter keeps the lists.

#include <cstdio>
#include <cstring>
#include <string>

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
            std::printf("xasetupcomplete: no card for global id 0xE2E\n");
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
} // namespace

void runExternalCommand(Engine &e, std::uint16_t nameIndex, const std::uint16_t *args,
                        std::size_t argCount)
{
    (void)args;
    (void)argCount;

    const std::string name = e.nameFromList(kExternalCommandNames, nameIndex);
    if (name.empty())
    {
        std::printf("external command: no NAME 2 entry %u\n",
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
    // tspit hides the strip around the opening cutscene (tspit.cpp).
    if (key == "xthideinventory")
    {
        e.inventory().setForcedHidden(true);
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
    if (key == "xarestoregame" || key == "xasavegame" || key == "xaresumegame")
    {
        // Saves are the other half of milestone 9. Reported rather than
        // silently ignored so a dead menu button is explained.
        std::printf("external command: %s needs saves (milestone 9)\n", name.c_str());
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

    std::printf("external command: %s is not implemented\n", name.c_str());
}

} // namespace rivenrt
