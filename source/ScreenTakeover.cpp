#include "ScreenTakeover.hpp"

#include "engine/Engine.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "render/Cursor.hpp"
#include "render/Inventory.hpp"
#include "render/TextLayer.hpp"

namespace rivenrt
{
namespace
{
    /// How many port screens are stacked on the display, and whether the
    /// outermost one found a game running. File-static because the nesting is
    /// between two separate calls, not inside one.
    int g_screenDepth = 0;
    bool g_screenInGame = false;
} // namespace

void screenFrame()
{
    NEA_WaitForVBL(static_cast<NEA_UpdateFlags>(0));
    bgs.vblank();
}

bool screenUsable() { return bgs.exists() && textLayer.ready(); }

void screenShowPointer(bool on)
{
    Cursor &c = engine.cursor();
    if (!c.exists())
        return;
    c.setVisible(on);
    c.flush();
}

void screenShowInventory(bool on)
{
    Inventory &inv = engine.inventory();
    if (!inv.exists())
        return;
    inv.setSuppressed(!on);
    inv.flush();
}

ScreenTakeover::ScreenTakeover()
{
    if (g_screenDepth++ > 0)
        return;
    // The card view, if there is one, is on the front buffer and must survive:
    // these screens are reachable mid-game, and the card has to come back
    // afterwards without being reloaded. Same trick a fullscreen movie uses
    // (BgSurface.hpp:94-100).
    g_screenInGame = engine.booted();
    if (g_screenInGame)
        bgs.beginMovieTakeover();
    bgs.setLetterbox(false);
    screenShowPointer(false);
    screenShowInventory(false);
}

ScreenTakeover::~ScreenTakeover()
{
    if (--g_screenDepth > 0)
        return;
    bgs.setLetterbox(true);
    if (g_screenInGame)
    {
        // Every buffer but the one holding the parked card. The text is opaque
        // and was drawn on all 192 rows, so the rows below the card view have to
        // be handed back transparent or the next vertical pan would slide black
        // through the view.
        for (int b = 0; b < BgSurface::kBuffers; ++b)
            if (b != bgs.parkedBuffer())
                bgs.resetBuffer(b);

        (void)bgs.endMovieTakeover();
        engine.surface().invalidateAll();
        engine.applyScreenUpdate(true);
        screenShowPointer(true);
    }
    // Outside the branch: the strip is suppressed on the way in whether or not a
    // game is running, so it has to be released either way.
    screenShowInventory(true);
}

} // namespace rivenrt
