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

    /// Just the settings screen, for xaoptions. Blocks until the player leaves
    /// it, saves if anything changed, and puts the card back on screen.
    void runSettings();
};

extern MainMenu mainMenu;

} // namespace rivenrt
