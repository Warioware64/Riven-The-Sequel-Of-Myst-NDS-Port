// riven-convert -- turns a Riven install into the files the DS port reads.
//
// Headless driver over the same core the Qt GUI uses, so the two cannot drift.
// Running it with only a source directory probes and reports without writing,
// which is the fastest way to answer "does the converter see my copy of Riven".

#include <atomic>
#include <cstdio>
#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif
#include <algorithm>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "riven/Converter.hpp"
#include "riven/Preflight.hpp"
#include "riven/FFmpeg.hpp"
#include "riven/VideoPipeline.hpp"

namespace
{
    void usage(const char *argv0)
    {
        std::printf(
            "riven-convert -- Riven -> Nintendo DS asset converter\n"
            "\n"
            "usage: %s [options] <riven-source-dir> [dest-card-root]\n"
            "\n"
            "  <riven-source-dir>  a Riven install or mounted disc. The 5-CD and\n"
            "                      DVD/GOG layouts are both detected automatically.\n"
            "  [dest-card-root]    root of the SD card. Data is written to\n"
            "                      <dest>/_nds/riven_nds/data/. Omit to only probe\n"
            "                      the source and report what was found.\n"
            "\n"
            "options:\n"
            "  --no-cards          skip the card graph and scripts\n"
            "  --no-images         skip the downscaled card art\n"
            "  --no-hires          skip the full-resolution zoom art (the largest\n"
            "                      single thing on the card)\n"
            "  --no-water          skip the water effects\n"
            "  --no-audio          skip the sounds\n"
            "  --no-video          skip the movies (by far the longest stage)\n"
            "  --no-cursors        skip Riven's own cursors (read from riven.exe)\n"
            "  --no-extras         skip the inventory art (read from extras.mhk)\n"
            "  --no-compress       leave the audio dynamics alone. The sounds and\n"
            "                      movies are compressed and lifted by default,\n"
            "                      because Riven's peaks are already at full scale\n"
            "                      and no runtime volume can make it louder\n"
            "  --ffmpeg <path>     the ffmpeg binary, or the folder holding it. The\n"
            "                      movie stage decodes through ffmpeg; without this\n"
            "                      it is looked for on PATH. Not needed with\n"
            "                      --no-video\n"
            "  --cards-only        only the card graph -- fast, enough to test navigation\n"
            "  --force             redo assets that are already up to date\n"
            "  --movie-report      list what is inside the movies and exit. Probes\n"
            "                      with ffprobe, writes no output; the numbers in\n"
            "                      docs/video.md come from here\n"
            "  --stack <name>      restrict to one stack (repeatable): aspit, bspit,\n"
            "                      gspit, jspit, ospit, pspit, rspit, tspit\n"
            "  -q, --quiet         only warnings and errors\n"
            "  -h, --help          this text\n",
            argv0);
    }

    /// Progress on stdout: a single carriage-returned line, with log lines
    /// printed above it. Matches what the Myst converter's CLI does, for the
    /// same reason -- a per-file scroll over 20,000 assets is unreadable.
    /// True when stdout is a terminal. A carriage-returned progress line only
    /// overwrites itself on a terminal; piped to a file or a CI log it would
    /// append thousands of lines, so progress is suppressed there entirely.
    bool stdoutIsTerminal()
    {
#if defined(_WIN32)
        return _isatty(_fileno(stdout)) != 0;
#else
        return isatty(fileno(stdout)) != 0;
#endif
    }

    class ConsoleSink final : public riven::ProgressSink
    {
    public:
        explicit ConsoleSink(bool quiet) : quiet_(quiet), interactive_(stdoutIsTerminal()) {}

        void log(riven::Severity severity, std::string_view stage,
                 std::string_view message) override
        {
            if (quiet_ && severity == riven::Severity::Info)
                return;
            clearLine();
            const char *prefix = severity == riven::Severity::Error     ? "ERROR "
                                 : severity == riven::Severity::Warning ? "warn  "
                                                                        : "      ";
            std::printf("%s%.*s: %.*s\n", prefix, static_cast<int>(stage.size()),
                        stage.data(), static_cast<int>(message.size()), message.data());
            std::fflush(stdout);
        }

        void progress(std::uint64_t done, std::uint64_t total, std::string_view stage,
                      std::string_view detail) override
        {
            if (total == 0 || !interactive_)
                return;
            const int pct = static_cast<int>(done * 100 / total);
            // Only redraw when the percentage moves. Writing a line per asset
            // makes the converter measurably slower when stdout is a pipe.
            if (pct == lastPct_ && !stage.empty())
                return;
            lastPct_ = pct;

            char buf[256];
            const int n = std::snprintf(buf, sizeof(buf), "\r[%3d%%] %.*s: %.*s", pct,
                                        static_cast<int>(stage.size()), stage.data(),
                                        static_cast<int>(detail.size()), detail.data());
            lineLen_ = n > 0 ? n : 0;
            std::fputs(buf, stdout);
            std::fflush(stdout);
        }

        void clearLine()
        {
            if (lineLen_ <= 0)
                return;
            std::printf("\r%*s\r", lineLen_, "");
            lineLen_ = 0;
        }

    private:
        bool quiet_ = false;
        bool interactive_ = false;
        int lastPct_ = -1;
        int lineLen_ = 0;
    };

    void printChecks(const std::vector<riven::Check> &checks)
    {
        for (const auto &c : checks)
        {
            const char *mark = c.level == riven::Check::Level::Fail   ? "X"
                               : c.level == riven::Check::Level::Warn ? "!"
                                                                     : "OK";
            std::printf("  %-2s %s: %s\n", mark, c.title.c_str(), c.detail.c_str());
        }
    }

    /// What is actually inside this copy of the game's movies. Nothing is
    /// written and nothing is decoded: each movie is staged and handed to
    /// ffprobe, so the RVID design in docs/video.md can be argued from
    /// measurements rather than assumptions.
    ///
    /// This is slower than it was when the converter carried its own QuickTime
    /// header parser -- it stages every tMOV and runs two ffprobe calls over it,
    /// which for 1055 movies is a couple of gigabytes of temporary writes and a
    /// minute or so. It is a diagnostic command, run by hand, and the parser it
    /// replaced was 543 lines that existed for nothing else.
    int movieReport(const riven::SourceInfo &info, const riven::Options &opts)
    {
        const riven::FFmpegPaths ff = riven::findFFmpeg(opts.ffmpegPath);
        std::string version;
        std::string ffError;
        if (!riven::probeFFmpeg(ff, version, ffError))
        {
            std::fprintf(stderr, "%s\n", ffError.c_str());
            return 2;
        }
        std::printf("%s\n", version.c_str());

        struct Tally
        {
            int movies = 0;
            std::uint64_t bytes = 0;
        };
        std::map<std::string, Tally> videoCodecs;
        std::map<std::string, int> sizes, audioFormats;
        int total = 0, unreadable = 0, silent = 0;
        std::uint64_t totalBytes = 0;
        double totalSeconds = 0.0;

        // The FULL/LITE split the RVID design turns on: a movie that covers the
        // whole card versus one composited onto it.
        struct Class
        {
            int movies = 0;
            std::uint64_t frames = 0;
            std::uint64_t bytes = 0;
            double seconds = 0.0;
        };
        Class full, lite;
        int widest = 0, tallest = 0;

        for (const auto &si : info.stacks)
        {
            const riven::StackSource *ss = info.source.find(si.id);
            if (ss == nullptr)
                continue;

            riven::ArchiveSet set;
            std::vector<std::string> failures;
            set.openAll(ss->dataArchives, failures);
            if (set.empty())
                continue;

            std::printf("\n%s\n", rivendata::stackName(si.id));
            for (const std::uint16_t id : set.resourceIds("tMOV"))
            {
                const auto bytes = set.readMovie(id);
                const riven::MovieProbe m =
                    bytes.empty()
                        ? riven::MovieProbe{}
                        : riven::probeMovieBytes(bytes, id, ff);

                ++total;
                totalBytes += m.resourceBytes;
                if (!m.ok)
                {
                    ++unreadable;
                    std::printf("  %5u  %s\n", id,
                                m.error.empty() ? "could not be read" : m.error.c_str());
                    continue;
                }

                char box[16];
                std::snprintf(box, sizeof(box), "%dx%d", m.width, m.height);
                ++sizes[box];
                auto &t = videoCodecs[m.videoCodec];
                ++t.movies;
                t.bytes += m.resourceBytes;
                totalSeconds += m.seconds;

                Class &c = m.fullscreen() ? full : lite;
                ++c.movies;
                c.frames += static_cast<std::uint64_t>(m.frames);
                c.bytes += m.resourceBytes;
                c.seconds += m.seconds;
                if (!m.fullscreen())
                {
                    widest = std::max(widest, m.width);
                    tallest = std::max(tallest, m.height);
                }

                std::printf("  %5u  %-6s %9s %6d frames %5.1f fps %6.1fs %8s", id,
                            m.videoCodec.c_str(), box, m.frames, m.rate(), m.seconds,
                            riven::humanBytes(m.resourceBytes).c_str());

                if (m.hasAudio())
                {
                    char fmt[48];
                    std::snprintf(fmt, sizeof(fmt), "%s %d Hz %dch",
                                  m.audioCodec.c_str(), m.audioRate, m.audioChannels);
                    ++audioFormats[fmt];
                    std::printf("  audio %-6s %5d Hz %dch", m.audioCodec.c_str(),
                                m.audioRate, m.audioChannels);
                }
                else
                {
                    ++silent;
                }
                std::printf("\n");
            }
        }

        std::printf("\n%d movies, %s, %.0f seconds of video\n", total,
                    riven::humanBytes(totalBytes).c_str(), totalSeconds);
        if (unreadable > 0)
            std::printf("%d could not be read\n", unreadable);
        std::printf("%d have no audio track\n", silent);

        std::printf("\nvideo codecs (as ffprobe names them):\n");
        for (const auto &[codec, t] : videoCodecs)
            std::printf("  %-8s %5d movies, %s of resource\n", codec.c_str(), t.movies,
                        riven::humanBytes(t.bytes).c_str());
        std::printf("audio tracks:\n");
        for (const auto &[fmt, n] : audioFormats)
            std::printf("  %-20s %5d movies\n", fmt.c_str(), n);

        std::printf("\nprofiles (RVID: fullscreen cutscene versus card overlay):\n");
        std::printf("  FULL  %4d movies %8llu frames %8.0fs %10s\n", full.movies,
                    static_cast<unsigned long long>(full.frames), full.seconds,
                    riven::humanBytes(full.bytes).c_str());
        std::printf("  LITE  %4d movies %8llu frames %8.0fs %10s   widest %d, tallest %d\n",
                    lite.movies, static_cast<unsigned long long>(lite.frames), lite.seconds,
                    riven::humanBytes(lite.bytes).c_str(), widest, tallest);
        std::printf("  %zu distinct frame sizes\n", sizes.size());

        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    riven::Options opts;
    bool quiet = false;
    bool wantMovieReport = false;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help")
        {
            usage(argv[0]);
            return 0;
        }
        else if (a == "--no-cards")
            opts.cards = false;
        else if (a == "--no-images")
            opts.images = false;
        else if (a == "--no-hires")
            opts.hires = false;
        else if (a == "--no-water")
            opts.water = false;
        else if (a == "--no-audio")
            opts.audio = false;
        else if (a == "--no-video")
            opts.video = false;
        else if (a == "--no-cursors")
            opts.cursors = false;
        else if (a == "--no-compress")
        {
            opts.compressAudio = false;
        }
        else if (a == "--no-extras")
            opts.extras = false;
        else if (a == "--ffmpeg")
        {
            if (++i >= argc)
            {
                std::fprintf(stderr, "--ffmpeg needs a path\n");
                return 2;
            }
            opts.ffmpegPath = argv[i];
        }
        else if (a == "--cards-only")
        {
            opts.cards = true;
            opts.images = opts.hires = opts.water = opts.audio = opts.video = false;
            opts.cursors = opts.extras = false;
        }
        else if (a == "--force")
            opts.force = true;
        else if (a == "--movie-report")
            wantMovieReport = true;
        else if (a == "-q" || a == "--quiet")
            quiet = true;
        else if (a == "--stack")
        {
            if (++i >= argc)
            {
                std::fprintf(stderr, "--stack needs a name\n");
                return 2;
            }
            const auto id = rivendata::parseStackName(argv[i]);
            if (id == rivendata::StackId::None)
            {
                std::fprintf(stderr, "unknown stack '%s'\n", argv[i]);
                return 2;
            }
            opts.stacks.insert(id);
        }
        else if (!a.empty() && a[0] == '-')
        {
            std::fprintf(stderr, "unknown option '%s'\n", a.c_str());
            return 2;
        }
        else
            positional.push_back(a);
    }

    if (positional.empty())
    {
        usage(argv[0]);
        return 2;
    }

    opts.source = positional[0];
    if (positional.size() > 1)
        opts.dest = positional[1];
    opts.normalise();

    const riven::SourceInfo info = riven::scanSource(opts.source);
    if (!info.ok())
    {
        std::fprintf(stderr,
                     "No Riven data found in '%s'.\n"
                     "Point this at the folder holding Data/ and All/ (a 5-CD\n"
                     "install), or at the DVD/GOG install root.\n",
                     opts.source.string().c_str());
        return 1;
    }

    std::printf("Source: %s\n", info.source.root.string().c_str());
    std::printf("Layout: %s\n\n", riven::toString(info.source.layout));
    for (const auto &s : info.stacks)
        std::printf("  %-6s %4d cards  %4d images  %3d movies  %3d sounds  %3d effects\n",
                    rivendata::stackName(s.id), s.cards, s.images, s.movies, s.sounds,
                    s.effects);
    if (!info.source.missingStacks.empty())
    {
        std::printf("\nMissing or unreadable:");
        for (const auto id : info.source.missingStacks)
            std::printf(" %s", rivendata::stackName(id));
        std::printf("\n");
    }

    if (wantMovieReport)
        return movieReport(info, opts);

    if (opts.dest.empty())
    {
        std::printf("\nEstimated output: about %s\n",
                    riven::humanBytes(riven::estimateOutput(info, opts)).c_str());
        std::printf("No destination given: nothing was written.\n");
        return 0;
    }

    std::printf("\nChecks:\n");
    const auto checks = riven::runChecks(info, opts);
    printChecks(checks);
    if (!riven::checksPass(checks))
    {
        std::fprintf(stderr, "\nRefusing to start: fix the failures above.\n");
        return 1;
    }

    std::printf("\n");
    ConsoleSink sink(quiet);
    riven::CancelToken cancel;
    riven::Converter converter;
    const riven::ConversionResult result = converter.run(opts, sink, cancel);
    sink.clearLine();

    std::printf("\n%s\n%s\n", result.message.c_str(), result.summary().c_str());
    std::printf("Wrote %s to %s\n", riven::humanBytes(result.bytesWritten).c_str(),
                riven::Converter::dataDir(opts.dest).string().c_str());

    return result.usable() ? 0 : 1;
}
