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

    /// Riven's water animation. Honest but inert for now: the effects are
    /// converted and the opcode that starts them is still empty, so this is
    /// wired to the variable the game reads and will start mattering the day
    /// the effects run.
    bool water = true;

    /// Master output gain, 0..255, applied to everything: the ambient layers,
    /// the one-shot effects and the movie soundtrack.
    std::uint8_t masterVolume = 255;

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
