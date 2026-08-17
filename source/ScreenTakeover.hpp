#pragma once

// Taking the bottom screen away from the card view, and giving it back.
//
// Every screen the PORT draws rather than Riven -- the menu, the save picker,
// the settings, and now the credits roll -- does the same eight things in the
// same order, and two of them were bugs before commit 1f4daf8: a buffer written
// past kViewH has to be handed back with resetBuffer, and the inventory strip
// has to be put away with setSuppressed rather than setForcedHidden, which
// belongs to Riven's own scripts (Inventory.hpp).
//
// COUNTED, and that is the point of making it a guard rather than a pair of
// functions. The in-game menu opens the settings screen, which is a port screen
// opening a port screen; beginMovieTakeover is idempotent but endMovieTakeover
// is not, so the inner screen closing would hand the card back while the outer
// one was still drawing over it. Only the outermost takes and returns the
// display.
//
// Scope-bound for the reason Engine::CursorHide is: these screens have early
// returns, and a hand-written take/release pair leaks the display on every one
// of them.
//
// LIVED IN MainMenu.cpp until the credits wanted it too. That is the bar this
// project uses for moving something to a header of its own -- a second caller,
// not a guess that there will be one (Externals.hpp says the same thing about
// its own contents).

namespace rivenrt
{

/// One frame of a port screen: wait for the vblank window and spend it the way
/// the engine does -- the flip the last redraw asked for is committed FIRST,
/// before any of this frame's drawing (Engine::flushUploads).
///
/// It used to be the other way round -- draw, then requestFlip and vblank in one
/// breath -- which put the priority-swap register write after a 128 KB clear and
/// a screenful of glyphs, well into active display. The swap then took effect
/// part way down the screen.
void screenFrame();

/// Give the display back to the card parked under a screen that took it, and do
/// it without a black frame in the middle.
///
/// ORDER, and it is the whole of this function. resetBuffer fills the picture
/// rows with OPAQUE BLACK (BgSurface::resetBuffer) and has to be run on every
/// buffer the screen wrote, because those writes cover all 192 rows and the ones
/// below the card view must go back transparent or the next vertical pan slides
/// black through the view. But one of the buffers the screen wrote is the one
/// being scanned out -- so clearing first and rebinding after put a screenful of
/// black on the hardware for the frame between them. Worse, it left
/// endMovieTakeover's flip PENDING: a transition beginning in that frame took
/// the flip inside its own first vblank and swapped the old and new cards half
/// way down the slide. Catherine's journal, page-turned inside the zoom viewer,
/// was where that showed.
///
/// So: rebind and ask for the flip, let ONE vblank commit it, and only then
/// clear -- at which point frontBuffer() names the card. That is a better guard
/// than parkedBuffer() was, because it is the buffer the hardware is reading
/// rather than the one somebody meant to keep, and endMovieTakeover has already
/// forgotten the latter by then.
///
/// The letterbox goes back on UNCOMMITTED, because committing it with its own
/// bgUpdate() there and then would slide the OUTGOING image thirteen rows down
/// for the rest of the frame. This way the scroll and the flip reach the
/// registers together, in the one bgUpdate at the tail of vblank().
///
/// Caller-agnostic on purpose: it is the display half of handing back and
/// nothing else. Repainting the card -- invalidateAll, applyScreenUpdate -- is
/// each caller's own tail, because they do not agree on it.
void screenHandBack();

/// True when the port's screens can be drawn at all: a background surface, and
/// the menu font that every one of them but the credits needs.
bool screenUsable();

/// Put the pointer away, or bring it back. setVisible only raises a flag -- it
/// is Cursor::flush that writes OAM, and nothing is calling flush while a screen
/// owns the frame, so this has to.
void screenShowPointer(bool on);

/// The same for the inventory strip, which is sprites too and would otherwise
/// float over a settings screen opened from Riven's own Options button.
/// setSuppressed and not setForcedHidden: that flag belongs to the scripts.
void screenShowInventory(bool on);

struct ScreenTakeover
{
    ScreenTakeover();
    ~ScreenTakeover();

    ScreenTakeover(const ScreenTakeover &) = delete;
    ScreenTakeover &operator=(const ScreenTakeover &) = delete;
};

} // namespace rivenrt
