#include "global_header.hpp"

#include "Global.hpp"
#include "RivenData.hpp"

namespace
{
    // NEA_Process takes a capture-less callback, so the screen's text lives at
    // file scope. Same pattern the Myst port uses for its menu and error screens.
    const char *g_line1 = "";
    const char *g_line2 = "";
    const char *g_line3 = "";

    void drawStatus()
    {
        NEA_2DViewInit();
        NEA_RichTextRender3D(0, "Riven DS", 16, 30);
        NEA_RichTextRender3D(0, g_line1, 16, 60);
        NEA_RichTextRender3D(0, g_line2, 16, 80);
        NEA_RichTextRender3D(0, g_line3, 16, 100);
    }

    /// Show why the game cannot start, and stop. There is nothing the player can
    /// do from inside the game -- every screen past this point reads the data
    /// that isn't there -- so this is a dead end on purpose rather than a menu
    /// that black-screens on New Game.
    [[noreturn]] void haltWith(const char *l1, const char *l2, const char *l3)
    {
        g_line1 = l1;
        g_line2 = l2;
        g_line3 = l3;
        while (true)
        {
            NEA_WaitForVBL(static_cast<NEA_UpdateFlags>(0));
            NEA_Process(drawStatus);
        }
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    global.Init();

    // The UI font is the only thing in NitroFS besides the menu art. Without it
    // nothing below can report anything, so it is loaded before the data check.
    NEA_RichTextResetSystem();
    NEA_RichTextInit(0);
    NEA_RichTextMetadataLoadFAT(0, "font/DejaVuSans-Bold.fnt");
    NEA_RichTextMaterialLoadGRF(0, "font/DejaVuSans-Bold_0_png.grf");

    const Global::DataStatus status = global.CheckData();
    if (status != Global::DataStatus::Ok)
    {
        haltWith("Cannot start:", Global::DataStatusTitle(status),
                 Global::DataStatusHint(status));
    }

    // Milestone 1 stops here: the ROM boots, mounts the card and confirms a
    // conversion is present. Card loading and the engine run loop land in
    // milestone 4.
    static char l2[64];
    std::snprintf(l2, sizeof(l2), "Schema v%u  view %dx%d",
                  static_cast<unsigned>(rivendata::kSchemaVersion),
                  rivendata::kViewW, rivendata::kViewH);
    haltWith("Data found.", l2, "Engine not built yet.");
}
