#pragma once

// Turning one Riven tMOV into the video/<stack>/<id>.rvid file the DS reads.
//
// The stages, in order: pull the movie out of the archive with its chunk offsets
// corrected, stage it as a file, let ffmpeg demux, decode, scale and pace the
// picture and decode, downmix and resample the sound, quantise each frame with
// the SAME dither the stills use, and write it out raw with the audio
// interleaved a block per frame.
//
// There is no encoder in that list. What the picture loses to the 608 -> 256
// downscale is all it loses; the file holds exactly the texels the DS samples.
// docs/video.md has the reasoning and the eight-gigabyte price.
//
// ffmpeg is a CHILD PROCESS, not a library -- riven/FFmpeg.hpp says why, and
// what the caller owes in return (finding it, and reporting its absence before a
// run starts rather than a thousand movies later).
//
// Shaped like convertBitmap and convertSound: one convert* that reads, converts
// and writes atomically. Unlike them it STREAMS its output -- a fullscreen movie
// is 84480 bytes a frame and the longest is 4684 frames, so ospit's tMOV 21 is
// 377 MB and assembling it in memory first is not an option.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenVideo.hpp"
#include "riven/Archive.hpp"
#include "riven/FFmpeg.hpp"
#include "riven/MovieList.hpp"
#include "riven/Progress.hpp"

namespace riven
{

/// What ffprobe says is inside a movie. Everything the conversion decides --
/// FULL or LITE, the frame rate, whether there is a soundtrack -- comes from
/// here, and so does the --movie-report census.
struct MovieProbe
{
    std::uint16_t id = 0;
    bool ok = false;
    std::string error;

    std::string videoCodec;
    int width = 0;
    int height = 0;
    int fpsNum = 15;
    int fpsDen = 1;
    double seconds = 0.0;
    /// Video packets, counted by demuxing. Riven's codecs are intra-only, so
    /// this is the frame count -- exactly, for all but one movie in the game,
    /// and an over-count there. NOT nb_frames or duration: both are wrong on a
    /// movie extracted from a Mohawk archive. See probeMovieFile.
    int packets = 0;
    /// The frame count to plan around. Same as `packets`; named separately
    /// because it is a BOUND, and the conversion's real count comes from how
    /// many frames ffmpeg actually hands over.
    int frames = 0;

    std::string audioCodec;
    int audioRate = 0;
    int audioChannels = 0;

    /// Size of the tMOV resource, for the size census. Zero unless the caller
    /// filled it in.
    std::uint64_t resourceBytes = 0;

    bool hasVideo() const { return !videoCodec.empty() && width > 0 && height > 0; }
    bool hasAudio() const { return !audioCodec.empty(); }
    /// Whether the CODED size covers the card. The conversion decides the
    /// profile from the size after the track matrix instead (convertMovieBytes),
    /// because that is what the movie is drawn at; the two differ for the 79
    /// movies that carry a scale, none of which is anywhere near this big. This
    /// is the census's answer, for --movie-report and the tests.
    bool fullscreen() const;
    double rate() const
    {
        return fpsDen > 0 ? static_cast<double>(fpsNum) / fpsDen : 0.0;
    }
};

/// Probe a movie already staged as a file. Two ffprobe calls, no decoding.
MovieProbe probeMovieFile(const std::filesystem::path &movie, const FFmpegPaths &ff);

/// Probe movie bytes, staging them beside `scratchDir` for ffprobe to seek in.
/// ffprobe cannot read a Mohawk resource, and a .mov is not pipe-safe -- its
/// sample tables can sit after the media -- so there is no way around the file.
MovieProbe probeMovieBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                           const FFmpegPaths &ff,
                           const std::filesystem::path &scratchDir = {});

/// What convertMovie produced, for progress reporting and size accounting.
struct VideoResult
{
    bool ok = false;
    std::string error;
    std::uint64_t bytes = 0;
    /// ffprobe found no video stream to work with. Reported as a warning naming
    /// the id, not as a failure.
    bool unsupported = false;
    /// The picture converted but the soundtrack did not. Worth saying and not
    /// worth failing over: a silent cutscene still plays.
    std::string audioError;

    int frames = 0;
    /// Frames that carried no picture of their own -- either because the movie's
    /// audio outlasted its video, or because the frame was identical to the one
    /// before it. The DS then keeps showing the frame it already holds.
    int repeatedFrames = 0;
    rivendata::VideoProfile profile = rivendata::VideoProfile::Lite;
    int width = 0;
    int height = 0;
    /// The size Riven draws the movie at, in card coordinates: the coded size
    /// with the track matrix applied, before the 608 -> 256 scale. `width` is
    /// the DS columns this covers at the movie's MLST position, which is a
    /// pixel more than scaling this length for 583 of the 1055 movies -- so the
    /// two are worth being able to compare.
    int cardWidth = 0;
    int cardHeight = 0;
    bool hasAudio = false;
    /// What ffprobe called the source codec, for --movie-report's census.
    std::string codec;
    /// The movie's QuickTime track matrix was not 1:1, so `width`/`height` are
    /// smaller than the coded size. 79 of a 5-CD install's 1054 movies. Worth
    /// reporting because a run's count of these is the check that the scale
    /// reached everything it should.
    bool trackScaled = false;
};

/// Convert tMOV `id` from `set` and write `out` atomically.
///
/// `place` is the movie's MLST position (riven/MovieList.hpp), and it decides
/// the converted size: an overlay of card-width W at left L covers DS columns
/// [toDsX(L), toDsX(L+W)), which is one wider than floor(W*s) for 583 of the
/// game's 1055 movies. Null falls back to the length scale, which is right only
/// when the two agree -- the synthetic movies in the tests, and any movie no
/// MLST names.
///
/// `cancel` may be null. When it is not, it is checked once per frame and the
/// ffmpeg child is killed before the cancellation propagates -- which is the one
/// place a long movie used to be uninterruptible.
VideoResult convertMovie(const ArchiveSet &set, std::uint16_t id,
                         const std::filesystem::path &out, const FFmpegPaths &ff,
                         const MoviePlacement *place = nullptr,
                         const CancelToken *cancel = nullptr);

/// The same, on bytes already pulled out of the archive.
///
/// This split is what lets the video stage use more than one core. libvaht is
/// not thread-safe -- one archive handle, one file cursor -- so reading stays
/// on the thread that owns the ArchiveSet, and only this function, which
/// touches nothing shared, runs on workers.
VideoResult convertMovieBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                              const std::filesystem::path &out, const FFmpegPaths &ff,
                              const MoviePlacement *place = nullptr,
                              const CancelToken *cancel = nullptr);

} // namespace riven
