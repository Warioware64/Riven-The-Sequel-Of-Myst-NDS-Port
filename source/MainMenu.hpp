#pragma once

// The port's own menu, and the settings screen behind it.
//
// Shaped after the Myst port's InitialMenu: a blocking loop that owns its own
// frames, because there is no engine running yet and nothing else to drive
// them. The one departure is that the rows are touchable as well as
// D-pad-driven -- Myst's menu was keys only, and this port has a touch screen
// and a persistent pointer already.
//
// Riven's own aspit card 1 is ALSO a main menu, and the two do not compete:
// that one is the game's (new game, load, options, quit as painted hotspots)
// and this one is the port's. They meet at the settings screen, which the
// xaoptions external command opens too, so the Options button on Riven's menu
// leads here instead of printing that it needs milestone 9.

#include "SaveGame.hpp"

namespace rivenrt
{

class MainMenu
{
public:
    /// Show the menu and block until the player starts a game.
    ///
    /// Needs BgSurface up (the menu draws on the bottom screen) and the font in
    /// NitroFS. If either is missing this returns straight away, so a ROM whose
    /// NitroFS did not build still boots into Riven rather than into nothing.
    void run();

    /// The slot the boot menu loaded, or null for a new game.
    ///
    /// The FILE is read here rather than in main(), while this screen is still
    /// up: a slot that will not read can then be reported on the menu the player
    /// is looking at and they can pick another one, instead of being dropped
    /// into a new game to work out for themselves what happened.
    const SaveGame::SaveState *pendingLoad() const
    {
        return haveLoad_ ? &load_ : nullptr;
    }

    /// Just the settings screen, for xaoptions. Blocks until the player leaves
    /// it, saves if anything changed, and puts the card back on screen.
    void runSettings();

    /// The port's in-game menu, opened by START: Save, Load, Notebook,
    /// Settings, Resume.
    ///
    /// Riven's own aspit menu has three of these buttons too, and they land here
    /// through runSavePicker/runLoadPicker rather than through this -- pressing
    /// Save on the game's own menu should open the save list, not another menu.
    void runInGameMenu();

    /// Save to, or load from, a slot the player picks. Both take the screen the
    /// same way runSettings does, so they can be called from Riven's own menu
    /// card (Externals: xasavegame, xarestoregame) as well as from ours.
    void runSavePicker();

    /// True when a slot was read and handed to the engine -- which is not the
    /// same as "the card changed": a load asked for from inside a script is
    /// deferred (Engine::restoreFrom), and a load can legitimately land on the
    /// card the player was already standing on. The in-game menu closes on
    /// this, because comparing card ids gets both of those wrong.
    bool runLoadPicker();

private:
    bool haveLoad_ = false;
    SaveGame::SaveState load_;
};

extern MainMenu mainMenu;

} // namespace rivenrt
