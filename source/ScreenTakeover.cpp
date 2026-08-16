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

void screenHandBack()
{
    // Queued, not committed: vblank()'s own bgUpdate() at the end takes this and
    // the flip in one write. See the header.
    bgs.setScrollY(rivendata::kRestY);
    (void)bgs.endMovieTakeover();

    if (bgs.flipPending())
        screenFrame();
    else
        // endMovieTakeover's no-flip branch: the screen never actually flipped,
        // so the card is still the one being scanned out and there is nothing to
        // wait for. Only the scroll is left to commit, and setLetterbox is how
        // you commit it without spending a frame.
        bgs.setLetterbox(true);

    for (int b = 0; b < BgSurface::kBuffers; ++b)
        if (b != bgs.frontBuffer())
            bgs.resetBuffer(b);
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
    if (g_screenInGame)
    {
        // The rebind, the flip and the clears, in the order that never shows
        // black -- and it used to be written out here in the wrong one.
        screenHandBack();
        engine.surface().invalidateAll();
        engine.applyScreenUpdate(true);
        screenShowPointer(true);
    }
    else
    {
        // No game, so no takeover to end and no buffers to hand back -- the
        // letterbox is the only thing the ctor turned off, and it still has to
        // go back on. screenHandBack does this half itself for the other branch.
        bgs.setLetterbox(true);
    }
    // Outside both: the strip is suppressed on the way in whether or not a
    // game is running, so it has to be released either way.
    screenShowInventory(true);
}

} // namespace rivenrt
