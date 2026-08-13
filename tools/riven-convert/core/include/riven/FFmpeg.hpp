#pragma once

// Running ffmpeg and ffprobe as child processes.
//
// The converter used to carry its own QuickTime demuxer and its own Cinepak and
// QuickTime RLE decoders -- about 1100 lines that existed so the build would need
// no codec library. ffmpeg does all of it, correctly, for every codec, and it
// does the scaling and the audio resampling too.
//
// A SUBPROCESS rather than linking libavcodec, deliberately:
//
//   * The build stays as it was. No find_package, no pkg-config, no vcpkg
//     manifest, no per-platform hunt for shared libraries -- the CLI and the GUI
//     still compile with nothing but a C++20 toolchain and Qt.
//   * The licence stays where it is. ffmpeg is LGPL or GPL depending on how it
//     was built; invoking a separate binary is not linking, so nothing about its
//     licence reaches this program. See docs/licensing.md.
//   * Cancellation gets BETTER, not worse. The old decoder ran a 377 MB movie to
//     completion before it noticed a cancel; the read loop here checks between
//     frames and kills the child.
//
// The cost is that ffmpeg has to be on the machine. Preflight.hpp reports that
// before a run starts rather than after a thousand movies have failed.
//
// STDERR GOES TO A FILE, not to a pipe. A child that fills a stderr pipe nobody
// is draining blocks forever, and the parent here is blocked reading stdout --
// that is a deadlock waiting for a movie with an unusually chatty warning. The
// file is read after the child exits and is what error messages quote.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace riven
{

struct FFmpegPaths
{
    std::filesystem::path ffmpeg;
    std::filesystem::path ffprobe;

    /// An explicit path was given and nothing runnable was there. Kept separate
    /// from "not found" so the error can name what was actually looked at.
    bool overrideFailed = false;
    std::filesystem::path tried;

    bool usable() const { return !ffmpeg.empty() && !ffprobe.empty(); }
};

/// Locate both binaries. `override` may be the ffmpeg binary itself or the
/// directory holding it; when it is empty PATH is searched. ffprobe is looked for
/// beside ffmpeg first, then on PATH -- some builds ship them separately.
///
/// An explicit `override` that does not resolve does NOT fall back to PATH: a
/// user who points at the wrong ffmpeg has to be told, not quietly given a
/// different one.
FFmpegPaths findFFmpeg(const std::filesystem::path &override = {});

/// Run `ffmpeg -version` and `ffprobe -version`. `version` gets ffmpeg's first
/// line, which is what the log and the GUI show.
bool probeFFmpeg(const FFmpegPaths &paths, std::string &version, std::string &error);

/// One child process, with its stdout on a pipe.
class Subprocess
{
public:
    Subprocess() = default;
    ~Subprocess();
    Subprocess(const Subprocess &) = delete;
    Subprocess &operator=(const Subprocess &) = delete;

    bool start(const std::filesystem::path &exe, const std::vector<std::string> &argv,
               std::string &error);

    /// Up to `n` bytes. 0 means end of output (or a read error, which wait()
    /// will then explain through the exit status).
    std::size_t read(void *dst, std::size_t n);

    /// Exactly `n` bytes, looping over short reads -- a pipe hands over whatever
    /// is in it, which for a 126 KB frame is never the whole thing at once.
    /// `eof` distinguishes "the movie ended" from "the movie was truncated".
    bool readExact(void *dst, std::size_t n, bool &eof);

    /// Stop the child now. Safe to call more than once, and safe on a child that
    /// already exited.
    void kill();

    /// Reap the child and return its exit status; -1 if it was killed or never
    /// started. Reads the captured stderr.
    int wait();

    bool running() const { return running_; }
    const std::string &stderrText() const { return stderr_; }

private:
    void cleanup();

    bool running_ = false;
    int exitCode_ = -1;
    std::string stderr_;
    std::filesystem::path errPath_;

    // Opaque so this header stays free of <windows.h> and <unistd.h>.
    void *handle_ = nullptr; ///< pid_t on POSIX, PROCESS_INFORMATION* on Windows
    int outFd_ = -1;         ///< POSIX read end
    void *outHandle_ = nullptr; ///< Windows read end
};

/// Start a process, read all of its stdout, and wait. For ffprobe, whose output
/// is a few dozen bytes of CSV. False on a non-zero exit; `error` then carries
/// the child's stderr.
bool runCapture(const std::filesystem::path &exe, const std::vector<std::string> &argv,
                std::string &out, std::string &error);

} // namespace riven
