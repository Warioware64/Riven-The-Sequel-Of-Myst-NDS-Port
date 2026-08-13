#include "Settings.hpp"

#include <cstdio>

#include "Global.hpp"
#include "RivenVars.hpp"
#include "audio/RivenAudio.hpp"
#include "engine/Engine.hpp"

Settings settings;

namespace
{
    // settings.dat: [0] = format version, [1] = zipMode, [2] = transitions,
    // [3] = water, [4] = masterVolume (0..255).
    //
    // APPEND ONLY. Every field is gated on the file being long enough AND the
    // version matching, so an older file simply leaves the options it does not
    // carry at their defaults -- which is what lets a new option be added
    // without a version bump and without invalidating a player's card. Never
    // write a default into a byte an old file lacks, and never repurpose a byte.
    constexpr std::uint8_t kVersion = 1;
    constexpr std::size_t kBytes = 5;

    std::string path() { return global.dataDir() + "settings.dat"; }
} // namespace

void Settings::load()
{
    if (!global.hasFat)
        return;

    std::FILE *f = std::fopen(path().c_str(), "rb");
    if (f == nullptr)
        return; // no file yet: the defaults are the first boot

    std::uint8_t buf[kBytes] = {};
    const std::size_t n = std::fread(buf, 1, sizeof(buf), f);
    std::fclose(f);

    if (buf[0] != kVersion)
        return;
    if (n >= 2)
        zipMode = buf[1] != 0;
    if (n >= 3)
        transitions = buf[2] != 0;
    if (n >= 4)
        water = buf[3] != 0;
    if (n >= 5)
        masterVolume = buf[4];
}

void Settings::save() const
{
    if (!global.hasFat)
        return;

    std::FILE *f = std::fopen(path().c_str(), "wb");
    if (f == nullptr)
    {
        std::printf("could not write settings.dat\n");
        return;
    }
    const std::uint8_t buf[kBytes] = {
        kVersion,
        static_cast<std::uint8_t>(zipMode ? 1 : 0),
        static_cast<std::uint8_t>(transitions ? 1 : 0),
        static_cast<std::uint8_t>(water ? 1 : 0),
        masterVolume,
    };
    std::fwrite(buf, 1, sizeof(buf), f);
    std::fclose(f);
}

void Settings::apply() const
{
    RivenAudio::setMasterVolume(masterVolume);

    // Two of the options are Riven's own variables rather than the port's, so
    // the scripts see them the way the original's options screen made them see
    // them (riven.cpp:803, applyGameSettings). Vars::startNewGame writes the
    // same two, which is what covers a game that has not started yet; this is
    // what covers one that has, so a toggle in the in-game options screen is
    // live at the next card rather than at the next boot.
    rivenrt::Vars &v = rivenrt::engine.vars();
    v.set(rivendata::VarId::AZip, zipMode ? 1 : 0);
    v.set(rivendata::VarId::WaterEnabled, water ? 1 : 0);
}
