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
