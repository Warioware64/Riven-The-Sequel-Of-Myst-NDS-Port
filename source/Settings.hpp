#pragma once

// The player's options, and the only thing this port writes to the card apart
// from saves.
//
// Modelled on the Myst port's Settings, including the file format, because the
// format is the part that has to survive: options get added and a player's
// card outlives the build that wrote it.

#include <cstdint>

class Settings
{
public:
    /// Riven's own Zip Mode: a hotspot that leads somewhere already visited
    /// becomes a one-click jump. Off is the original's default (ScummVM
    /// registers zip_mode false, riven_metaengine.cpp:26), and it is off here
    /// for the same reason -- it skips walking, which is most of the game.
    bool zipMode = false;

    /// Slides and dissolves between cards. On is the game; off makes every card
    /// change instant, which is worth having on hardware where the pan costs 18
    /// frames the player did not ask for.
    bool transitions = true;

    /// Riven's water animation -- the SFXE ripples the game runs over a still
    /// (WaterEffect.hpp). Reaches the engine as Riven's own "waterenabled"
    /// variable, which is what the original tests before drawing a frame
    /// (riven_graphics.cpp:773), so this takes effect the moment it is changed
    /// rather than at the next card.
    bool water = true;

    /// Master output gain, 0..255, applied to everything: the ambient layers,
    /// the one-shot effects and the movie soundtrack.
    ///
    /// The byte spans 0..TWICE unity, so 127 is normal and 255 is a 2x boost --
    /// the default is therefore the middle of the range and not the top. It was
    /// a plain 0..unity attenuator, which meant the control started at its
    /// maximum and could only ever make things quieter; a cutscene that was too
    /// quiet had nowhere to go. Above unity only the movie soundtrack rises,
    /// because it is the only thing mixed in software (RivenAudio.hpp).
    ///
    /// 128 and not the 127 the settings row's tenth step writes: 128 is the one
    /// byte that lands on EXACTLY unity on both paths, so a first boot sounds
    /// bit-for-bit like the old build. The row reads it as 100% either way.
    std::uint8_t masterVolume = 128;

    /// Trace every card change and every asset load to the top screen and to
    /// <data>/debug.log, turn on the screenshot and VRAM-dump chords, and let
    /// SELECT+START open the command console (engine/DebugConsole.cpp -- which
    /// needs this off in normal play for a hardware reason as well as a taste
    /// one: its keyboard writes over the palette entries the top-screen picture
    /// is using). Off,
    /// and not a preference: it is loud enough to push the failures the console
    /// exists to show off the screen. See DebugLog.hpp.
    ///
    /// Read once, at startup, by DebugLog::begin -- so unlike the others this
    /// one takes effect at the next boot, and the settings screen says so.
    bool debugMode = false;

    /// Read settings.dat. Missing or unreadable leaves every default in place,
    /// which is the correct first boot. No-op without FAT.
    void load();

    /// Write settings.dat. No-op without FAT.
    void save() const;

    /// Push the settings that something else owns a copy of into it: the audio
    /// system's master gain, and the game variables Riven's own scripts read.
    ///
    /// Called after load() and again whenever the settings screen is left, so
    /// that changing an option in the middle of a game takes effect there and
    /// then rather than at the next boot.
    void apply() const;
};

extern Settings settings;
