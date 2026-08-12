#pragma once

// Reading the headers of a Riven movie.
//
// Riven's movies are QuickTime files stored as tMOV resources inside the Mohawk
// archives (riven_video.cpp:64-66). libvaht will hand back a byte stream that
// plays standalone -- it rewrites the stco chunk offsets and stops there
// (vaht_mov.h:8-14) -- but it decodes nothing, so the RVID encoder in milestone
// 8 needs its own demuxer.
//
// This is the first half of that: the atom walk and the sample tables, far
// enough to say what every movie IS. No frame data is touched and no codec is
// implemented. It exists because the RVID design has to be sized against real
// numbers -- which codecs are actually in there, how big the movies are, what
// frame rates they run at, and whether the audio tracks are something we can
// decode -- and guessing at those would be designing a format against a
// hypothesis. `riven-convert --movie-report <source>` prints the answers.
//
// QuickTime is big-endian, so ResourceReader reads it directly.

#include <cstdint>
#include <string>
#include <vector>

#include "riven/Archive.hpp"
#include "riven/ResourceReader.hpp"

namespace riven
{

/// QuickTime IMA4: 34 bytes hold 64 samples for one channel -- a 2-byte
/// preamble carrying the initial predictor and step index, then 32 bytes of
/// nibbles. ScummVM hardcodes these rather than trusting the sample
/// description, because a version-0 stsd does not carry them
/// (audio/decoders/quicktime.cpp:151-154).
inline constexpr std::uint32_t kIma4SamplesPerPacket = 64;
inline constexpr std::uint32_t kIma4PacketBytes = 34;

/// Where one piece of a track's data lives inside the resource.
///
/// For a video track this is one frame. For an audio track it is one CHUNK --
/// QuickTime's audio sample tables count PCM sample frames rather than
/// compressed packets, so the useful unit is the chunk, and the decoder
/// concatenates them (common/formats/quicktime.cpp:504-512 and
/// audio/decoders/quicktime.cpp:339-350 describe both halves of that).
struct MovieSample
{
    std::size_t offset = 0;      ///< bytes from the start of the resource
    std::uint32_t size = 0;
    std::uint32_t duration = 0;  ///< media timescale units; 0 for audio chunks
    std::uint32_t startTime = 0; ///< cumulative, media timescale
    bool keyframe = true;
};

/// One track of a movie.
struct MovieTrack
{
    enum class Kind
    {
        Video,
        Audio,
        Other,
    };

    Kind kind = Kind::Other;
    std::string codec;          ///< four-character sample-description format
    std::uint32_t timescale = 0;///< units per second
    std::uint64_t duration = 0; ///< in timescale units
    std::uint32_t sampleCount = 0;
    std::uint64_t dataBytes = 0;

    /// Frames (video) or chunks (audio), in decode order. Empty when the movie
    /// was probed for its headers only.
    std::vector<MovieSample> samples;
    /// The duration most samples have. Riven's movies are constant-rate, so
    /// this and `timescale` give an exact frame rate; `rateIsConstant` says
    /// whether that was actually true of this file.
    std::uint32_t modalDuration = 0;
    bool rateIsConstant = true;

    // Video
    int width = 0;
    int height = 0;
    int depth = 0;

    // Audio
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;

    double seconds() const
    {
        return timescale > 0 ? static_cast<double>(duration) / timescale : 0.0;
    }
    /// Frames per second, from the track's own duration and sample count.
    double rate() const
    {
        const double s = seconds();
        return s > 0.0 ? sampleCount / s : 0.0;
    }
};

/// What one tMOV turned out to be.
struct MovieInfo
{
    bool ok = false;
    std::string error;
    std::uint16_t id = 0;
    std::size_t resourceBytes = 0;
    std::vector<MovieTrack> tracks;

    const MovieTrack *video() const;
    const MovieTrack *audio() const;
};

/// Walk a tMOV resource's atoms and read its track headers. Never throws; a
/// resource that is not a QuickTime movie comes back with ok == false.
///
/// `withSamples` also builds the per-track sample tables, which is what a
/// decoder needs and what `--movie-report` does not.
MovieInfo probeMovie(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                     bool withSamples = false);

/// Probe every tMOV in `set`, in id order. Damaged resources are reported
/// through the result rather than skipped silently.
std::vector<MovieInfo> probeMovies(const ArchiveSet &set);

} // namespace riven
