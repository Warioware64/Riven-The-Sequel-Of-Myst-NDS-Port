#pragma once

// The conversion pipeline.
//
// One entry point, driven by Options, reporting through ProgressSink and
// interruptible through CancelToken. The CLI and the Qt GUI are both thin
// drivers over this and share every behaviour as a result.
//
// Two policies run through the whole thing and are worth stating once:
//
//   * A failure on one asset costs that asset, never the run. Riven copies in
//     the wild are routinely damaged -- zero-filled archives, resources that
//     turn to zeroes partway through -- and a converter that stops on the first
//     bad byte converts nothing. Failures are logged at Warning or Error and
//     counted; the run continues.
//
//   * Work already done is skipped unless Options::force. Combined with atomic
//     writes, that makes a cancelled run resumable and a re-run nearly free.

#include <cstdint>
#include <filesystem>
#include <string>

#include "riven/Options.hpp"
#include "riven/Preflight.hpp"
#include "riven/Progress.hpp"

namespace riven
{

struct ConversionResult
{
    enum class Outcome
    {
        Ok,
        Cancelled,
        Failed, ///< something structural: nothing useful was produced
    };

    Outcome outcome = Outcome::Failed;
    std::string message;

    int stacksConverted = 0;
    int cardsWritten = 0;
    int imagesWritten = 0;
    int hiresWritten = 0;
    int effectsWritten = 0;
    int soundsWritten = 0;
    int moviesWritten = 0;
    /// Frames across every movie converted, which is the number that says how
    /// much work the video stage actually did.
    std::uint64_t videoFrames = 0;
    /// Of `soundsWritten`, how many came from the DVD release's MPEG-2 Layer II
    /// tracks. Worth saying out loud: it is the one path that re-encodes.
    int mpegSounds = 0;
    int skipped = 0;   ///< already up to date
    int warnings = 0;
    int errors = 0;
    std::uint64_t bytesWritten = 0;

    bool usable() const { return outcome != Outcome::Failed; }
    std::string summary() const;
};

class Converter
{
public:
    /// Run a conversion. Never throws: cancellation and failure both come back
    /// through the result.
    ConversionResult run(Options opts, ProgressSink &sink, CancelToken &cancel);

    /// Where converted data lands under a card root. The DS looks here.
    static std::filesystem::path dataDir(const std::filesystem::path &dest);
};

} // namespace riven
