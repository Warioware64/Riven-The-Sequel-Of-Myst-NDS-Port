#pragma once

// Turning one Riven tMOV into the video/<stack>/<id>.rvid file the DS reads.
//
// The stages, in order: pull the movie out of the archive with its chunk
// offsets corrected, demux the sample tables, decode each frame with Cinepak
// or QuickTime RLE, downscale it with the SAME filter the stills use, encode it
// as RVID, and interleave the movie's own audio a block per frame.
//
// Shaped like convertBitmap and convertSound: a pure encoder that tests can
// drive, and one convert* that reads, encodes and writes atomically.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenVideo.hpp"
#include "riven/Archive.hpp"

namespace riven
{

/// What convertMovie produced, for progress reporting and size accounting.
struct VideoResult
{
    bool ok = false;
    std::string error;
    std::size_t bytes = 0;
    /// The resource parsed but its codec is one we cannot decode. Reported as
    /// a warning naming the id, not as a failure.
    bool unsupported = false;

    int frames = 0;
    int keyframes = 0;
    rivendata::VideoProfile profile = rivendata::VideoProfile::Lite;
    int width = 0;
    int height = 0;
    bool hasAudio = false;
};

/// Convert tMOV `id` from `set` and write `out` atomically.
///
/// `quality` is the RVID quantiser scale: 100 is the default, lower is smaller.
VideoResult convertMovie(const ArchiveSet &set, std::uint16_t id,
                         const std::filesystem::path &out, int quality);

/// The same, on bytes already pulled out of the archive.
///
/// This split is what lets the video stage use more than one core. libvaht is
/// not thread-safe -- one archive handle, one file cursor -- so reading stays
/// on the thread that owns the ArchiveSet, and only this function, which
/// touches nothing shared, runs on workers.
VideoResult convertMovieBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                              const std::filesystem::path &out, int quality);

} // namespace riven
