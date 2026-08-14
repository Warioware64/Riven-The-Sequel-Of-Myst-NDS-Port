#include "global_header.hpp"

#include "BookNotes.hpp"
#include "DebugLog.hpp"
#include "Global.hpp"
#include "RivenData.hpp"
#include "Settings.hpp"
#include "MainMenu.hpp"
#include "SaveGame.hpp"
#include "audio/RivenAudio.hpp"
#include "engine/Engine.hpp"
#include "render/BgSurface.hpp"
#include "render/TextLayer.hpp"
#include "render/TopBg.hpp"

namespace
{
    /// Show why the game cannot start, and stop. There is nothing the player can
    /// do from inside the game -- every screen past this point reads the data
    /// that isn't there -- so this is a dead end on purpose rather than a menu
    /// that black-screens on New Game.
    ///
    /// The top-screen console, not the 3D text renderer: with the card view moved
    /// onto bitmap backgrounds nothing renders through the 3D engine any more, so
    /// NEA_RichTextRender3D would draw to a layer that is switched off. The
    /// console is already up by the time anything can fail (Global::Init).
    [[noreturn]] void haltWith(const char *l1, const char *l2, const char *l3)
    {
        // The picture goes first. Console glyphs are opaque white on whatever is
        // behind them (TopBg.hpp), and this is the one screen in the port that
        // has to be read rather than glanced at -- on black it is legible, over
        // a photograph of Riven it is not. This is also the normal-quit path.
        rivenrt::topBg.setVisible(false);
        // The one print in the port that is never gated. It is the only
        // player-facing error UI there is, and everything it could report has
        // already stopped the game (DebugLog.hpp).
        std::printf("\n-- Riven DS --\n%s\n%s\n%s\n", l1, l2, l3);
        while (true)
            swiWaitForVBlank();
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    global.Init();

    // Before anything reads a setting, and harmless when the card cannot be
    // written: an absent settings.dat is a first boot, not an error.
    settings.load();

    // SaveGame is deliberately free of <nds.h> so the host tests can compile it
    // (SaveGame.hpp), which means it cannot reach the Global singleton and has
    // to be told where the card is. Here, because Global::Init has just created
    // the directory; everything in SaveGame is a no-op until this runs.
    rivenrt::SaveGame::setDirectory(global.savesDir());
    rivenrt::BookNotes::setDirectory(global.notesDir());

    const Global::DataStatus status = global.CheckData();
    if (status != Global::DataStatus::Ok)
    {
        haltWith("Cannot start:", Global::DataStatusTitle(status),
                 Global::DataStatusHint(status));
    }

    // What is missing but not fatal, said once, before anything can be blamed on
    // it. The console is up by now, so this is the first thing the player reads.
    global.ReportOptionalData();

    // Before anything is loaded, so the trace covers the whole run -- and
    // before the picture, which it takes the top screen away from.
    rivenrt::DebugLog::begin();

    // The top screen's picture, behind everything the console has just printed
    // and everything it prints from here on. After ReportOptionalData rather
    // than before it only so that a card with no ui/ says so in the same place
    // as the rest of what is missing.
    rivenrt::topBg.load();

    RivenAudio::initSystem();
    // The master gain, now that there is an audio system to set it on. The two
    // game variables apply() also writes are put in place by startNewGame,
    // which boot() is about to run.
    settings.apply();

    // The port's menu draws on the bottom screen through the card view's own
    // backgrounds, so those have to exist before it and not after. boot() still
    // calls create() and finds it done -- this is a move, not a duplicate.
    rivenrt::bgs.create();
    rivenrt::textLayer.create();
    rivenrt::mainMenu.run();

    // pendingLoad() is null unless the menu ended on Load game with a slot that
    // read back; boot() takes it from there.
    if (!rivenrt::engine.boot(rivenrt::mainMenu.pendingLoad()))
    {
        // The data is there but something in it could not be read. The engine's
        // own message says what; it is a std::string, so it is copied into a
        // buffer the halt screen can hold onto.
        static char detail[96];
        std::snprintf(detail, sizeof(detail), "%s", rivenrt::engine.error().c_str());
        haltWith("Cannot start:", "Riven data could not be loaded", detail);
    }

    // Here and not earlier: the last notices come from inside boot() -- a
    // missing cursor set, a missing inventory strip -- and clearing before that
    // would wipe the picture clean and then print over it again. Dwells only if
    // there was something to read, and does nothing at all with the trace on.
    rivenrt::DebugLog::settleNotices();

    while (!rivenrt::engine.quitRequested())
    {
        // The engine's own frame does the uploads that have to happen in the
        // vblank window, so it is entered from here rather than the other way
        // round.
        NEA_WaitForVBL(static_cast<NEA_UpdateFlags>(0));
        rivenrt::engine.frame();
    }

    rivenrt::DebugLog::end();
    haltWith("Riven DS", "Thank you for playing.", "");
}
