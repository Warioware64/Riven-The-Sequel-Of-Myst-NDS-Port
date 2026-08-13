#pragma once

// Everything worth knowing BEFORE a conversion starts.
//
// A Riven conversion is a multi-gigabyte job that can run for hours. The Myst
// converter's preflight module put the case well: "Everything that can be known
// before it starts is worth knowing before it starts, not 18 minutes in." This
// is the same idea, and it can be sharper here -- Riven's stills are all the
// same size, so the output estimate is arithmetic rather than a measured fudge
// factor.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenData.hpp"
#include "riven/Layout.hpp"
#include "riven/Options.hpp"

namespace riven
{

/// Per-stack resource census, read from archive headers only.
struct StackInfo
{
    rivendata::StackId id = rivendata::StackId::None;
    int cards = 0;
    int images = 0;  ///< tBMP
    int movies = 0;  ///< tMOV
    int sounds = 0;  ///< tWAV
    int effects = 0; ///< SFXE
    std::uint64_t archiveBytes = 0;
};

/// What a source directory turned out to hold.
struct SourceInfo
{
    Source source;
    std::vector<StackInfo> stacks;
    /// Archives that exist but could not be opened -- the signature of an
    /// interrupted copy. Reported rather than treated as fatal.
    std::vector<std::string> unreadable;

    int totalCards = 0;
    int totalImages = 0;
    int totalMovies = 0;
    int totalSounds = 0;
    int totalEffects = 0;

    bool ok() const { return source.valid() && !stacks.empty(); }

    /// One line for the UI: layout, stacks found, headline counts.
    std::string summary() const;
};

/// Read the archive headers of every stack. Cheap enough (a few hundred ms on a
/// full install) to re-run whenever the user edits the source path.
SourceInfo scanSource(const std::filesystem::path &root);

/// Estimated bytes the conversion will write, for the size line and the
/// free-space check.
///
/// Riven makes this nearly exact rather than a guess: every card still is
/// 608x392, so a .rpic is a known 256x165x2 and a .rpiz is a known 608x392
/// index plane. Only the LZ77 ratio is estimated, and it was measured at ~55%
/// across real archives.
std::uint64_t estimateOutput(const SourceInfo &info, const Options &opts);

/// Human-readable byte count: "1.4 GB", "812 MB", "4 KB".
std::string humanBytes(std::uint64_t bytes);

/// One pre-run condition, shown as a list in the UI.
struct Check
{
    enum class Level
    {
        Ok,
        Warn,
        Fail
    };
    Level level = Level::Ok;
    std::string title;
    std::string detail;
};

/// Conditions that must hold before Start is allowed. Any Fail blocks the run.
///
/// The checks that earned their place come from the Myst converter's
/// experience, most importantly "the destination is inside the source", which
/// would otherwise feed the converter its own output.
std::vector<Check> runChecks(const SourceInfo &info, const Options &opts);

/// Is ffmpeg where the options say it is?
///
/// Its own check because it is the one condition a user can fix without touching
/// anything about the conversion, and because finding out at movie 1 of 1055 is
/// two hours too late. A Fail when the video stage is on, an Ok note when it is
/// off -- a run with --no-video needs no ffmpeg at all.
Check checkFFmpeg(const Options &opts);

/// True when no check failed.
bool checksPass(const std::vector<Check> &checks);

} // namespace riven
