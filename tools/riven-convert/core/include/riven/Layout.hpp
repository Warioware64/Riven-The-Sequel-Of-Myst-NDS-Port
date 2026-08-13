#pragma once

// Source-tree detection and per-stack archive search order.
//
// Riven ships in layouts that differ enough to matter: the 5-CD release keeps
// data under All/ + Data/ + Assets1/ + program/, while the DVD/GOG release uses
// lowercase all/ data/ exe/ assets1/ program/ (the directories ScummVM searches,
// riven.cpp:94-99). Some installs are a flat directory of .mhk files. This
// module normalises all of them into one thing: for each stack, the list of
// archives to search, in priority order.
//
// Priority is load-bearing and easy to get wrong. Riven's 1.02 patch ships
// b_Data1.mhk and j_Data3.mhk which OVERRIDE resources in the base archives,
// and ScummVM opens them first precisely so they win its linear resource scan
// (riven.cpp:441-443). Getting this backwards produces a game that looks
// converted and is subtly the wrong version.

#include <filesystem>
#include <string>
#include <vector>

#include "RivenData.hpp"

namespace riven
{

/// Which shape of install a source directory turned out to be.
enum class SourceLayout
{
    Unknown,
    FiveCd,   ///< All/ Data/ Assets1/ Assets2/ program/ (mixed case)
    Dvd,      ///< all/ data/ exe/ assets1/ program/
    Flat,     ///< every .mhk in one directory
};

const char *toString(SourceLayout l);

/// Everything the converter needs to know about one stack's source data.
struct StackSource
{
    rivendata::StackId id = rivendata::StackId::None;
    /// Archives holding cards, images and movies, highest priority FIRST.
    std::vector<std::filesystem::path> dataArchives;
    /// Archives holding the ambient sound tracks.
    std::vector<std::filesystem::path> soundArchives;

    bool usable() const { return !dataArchives.empty(); }
};

/// A detected source tree.
struct Source
{
    std::filesystem::path root;
    SourceLayout layout = SourceLayout::Unknown;
    std::vector<StackSource> stacks;   ///< only stacks with at least one archive
    std::vector<rivendata::StackId> missingStacks;
    /// program/arcriven.z when present -- the DCL installer archive holding
    /// extras.MHK (inventory, marbles, credits) and riven.exe (cursors).
    std::filesystem::path installerArchive;
    /// extras.mhk, when the install has it unpacked already.
    std::filesystem::path extrasArchive;
    /// riven.exe / rivendmo.exe, when the install has it unpacked. Riven's
    /// cursors are PE resources in it and are nowhere else.
    std::filesystem::path executable;

    bool valid() const { return layout != SourceLayout::Unknown && !stacks.empty(); }
    const StackSource *find(rivendata::StackId id) const;
};

/// Probe `root` and work out what is there. Never throws; an unreadable or
/// unrecognised directory comes back with layout == Unknown.
///
/// Archives that exist but are not Mohawk containers are treated as missing
/// rather than as errors. That is not hypothetical: a partial download can
/// leave a full-size, entirely zero-filled o_Data.MHK in place, and the useful
/// behaviour is to convert the other seven stacks and say so.
Source detectSource(const std::filesystem::path &root);

/// True if `p` starts with a valid MHWK/RSRC header. Cheap: reads 12 bytes.
bool looksLikeMohawk(const std::filesystem::path &p);

} // namespace riven
