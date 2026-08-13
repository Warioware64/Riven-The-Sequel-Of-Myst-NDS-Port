#include "global_header.hpp"

#include "Global.hpp"
#include "RivenData.hpp"
#include "audio/RivenAudio.hpp"
#include "engine/Engine.hpp"

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

    const Global::DataStatus status = global.CheckData();
    if (status != Global::DataStatus::Ok)
    {
        haltWith("Cannot start:", Global::DataStatusTitle(status),
                 Global::DataStatusHint(status));
    }

    // What is missing but not fatal, said once, before anything can be blamed on
    // it. The console is up by now, so this is the first thing the player reads.
    global.ReportOptionalData();

    RivenAudio::initSystem();

    if (!rivenrt::engine.boot())
    {
        // The data is there but something in it could not be read. The engine's
        // own message says what; it is a std::string, so it is copied into a
        // buffer the halt screen can hold onto.
        static char detail[96];
        std::snprintf(detail, sizeof(detail), "%s", rivenrt::engine.error().c_str());
        haltWith("Cannot start:", "Riven data could not be loaded", detail);
    }

    while (!rivenrt::engine.quitRequested())
    {
        // The engine's own frame does the uploads that have to happen in the
        // vblank window, so it is entered from here rather than the other way
        // round.
        NEA_WaitForVBL(static_cast<NEA_UpdateFlags>(0));
        rivenrt::engine.frame();
    }

    haltWith("Riven DS", "Thank you for playing.", "");
}
