// The video path: what ffmpeg reports about the game's movies, and the .rvid
// files the pipeline builds out of them.
//
// There is no codec to test here any more, in either direction. Frames are stored
// raw, so what used to be a bit-exactness argument between an encoder and a
// decoder is a plain equality that lives in test_rvid_arm9.cpp; and the decoding
// is ffmpeg's, which is not this project's to verify. What is left is the three
// things that ARE ours:
//
//   * the census -- every movie in the install has a video stream ffprobe can
//     describe, which is what makes "unsupported codec" an impossible outcome
//     rather than a hoped-for one;
//   * the container -- the frames tile the file, largestFrameBytes is true, and
//     the index names every frame in order;
//   * the QUANTISER -- the stored texels are what our own dither produces from
//     ffmpeg's rgb24, and they are close to what the stills path would have
//     produced from the same source. 874 of Riven's movies are overlays
//     composited into a card still, so a difference here is a visible seam.
//
// Everything real-data needs RIVEN_TEST_DATA, and everything at all needs ffmpeg;
// both are skipped cleanly when absent. A few frames are dumped as PPM because
// video is the one thing in this converter where a person has to look at it.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "RivenVideo.hpp"
#include "riven/Archive.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/Layout.hpp"
#include "riven/FFmpeg.hpp"
#include "riven/VideoPipeline.hpp"

namespace fs = std::filesystem;
using namespace riven;
using namespace rivendata;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const std::string &what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++g_failures;
        }
    }

    void writePpm(const fs::path &path, const std::vector<Texel> &texels, int w, int h)
    {
        FILE *f = std::fopen(path.string().c_str(), "wb");
        if (f == nullptr)
            return;
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (const Texel t : texels)
        {
            const int r = t & 0x1F, g = (t >> 5) & 0x1F, b = (t >> 10) & 0x1F;
            const std::uint8_t rgb[3] = {static_cast<std::uint8_t>((r << 3) | (r >> 2)),
                                         static_cast<std::uint8_t>((g << 3) | (g >> 2)),
                                         static_cast<std::uint8_t>((b << 3) | (b >> 2))};
            std::fwrite(rgb, 1, 3, f);
        }
        std::fclose(f);
    }

    /// Decode `count` frames and return the last, as rgb24. When `w`/`h` are
    /// non-zero ffmpeg scales; when they are zero the frame comes back at the
    /// movie's own size, which is what the stills path would have been handed.
    std::vector<std::uint8_t> lastFrameOf(const FFmpegPaths &ff, const fs::path &movie,
                                          int count, int srcW, int srcH, int w, int h)
    {
        const int outW = w > 0 ? w : srcW;
        const int outH = h > 0 ? h : srcH;
        const std::size_t bytes = static_cast<std::size_t>(outW) * outH * 3;

        std::vector<std::string> argv = {"-v",  "error", "-nostdin", "-threads",
                                         "1",   "-i",    movie.string(), "-an"};
        char scale[64];
        if (w > 0 && h > 0)
        {
            std::snprintf(scale, sizeof(scale), "scale=%d:%d:flags=area", w, h);
            argv.push_back("-vf");
            argv.push_back(scale);
        }
        argv.insert(argv.end(), {"-frames:v", std::to_string(count), "-f", "rawvideo",
                                 "-pix_fmt", "rgb24", "pipe:1"});

        Subprocess p;
        std::string err;
        if (!p.start(ff.ffmpeg, argv, err))
            return {};

        std::vector<std::uint8_t> frame(bytes);
        std::vector<std::uint8_t> got;
        for (int n = 0; n < count; ++n)
        {
            bool eof = false;
            if (!p.readExact(frame.data(), frame.size(), eof))
                break;
            got = frame;
        }
        p.kill();
        p.wait();
        return got;
    }

    /// Mean and maximum absolute per-channel difference between two texel
    /// images, in 5-bit units (so 31 is the full range).
    void texelDelta(const std::vector<Texel> &a, const std::vector<Texel> &b,
                    double &mean, int &worst)
    {
        mean = 0.0;
        worst = 0;
        if (a.size() != b.size() || a.empty())
        {
            worst = 32;
            return;
        }
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            for (int shift = 0; shift <= 10; shift += 5)
            {
                const int d = std::abs(((a[i] >> shift) & 0x1F) - ((b[i] >> shift) & 0x1F));
                sum += static_cast<std::uint64_t>(d);
                worst = std::max(worst, d);
            }
        }
        mean = static_cast<double>(sum) / (a.size() * 3);
    }
} // namespace

int main()
{
    // --- ffmpeg -------------------------------------------------------------
    const FFmpegPaths ff = findFFmpeg();
    std::string version;
    std::string ffError;
    if (!probeFFmpeg(ff, version, ffError))
    {
        std::printf("video: skipped -- %s\n", ffError.c_str());
        return 0;
    }
    std::printf("video: %s\n", version.c_str());

    // --- real data ----------------------------------------------------------
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("video: %d checks, %d failed "
                    "(RIVEN_TEST_DATA unset: skipped the real-data checks)\n",
                    g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    const Source source = detectSource(dataEnv);
    if (!source.valid())
    {
        std::printf("video: skipped (no Riven data under '%s')\n", dataEnv);
        return g_failures == 0 ? 0 : 1;
    }

    const fs::path outDir = fs::temp_directory_path() / "riven-test-video";
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);

    std::map<std::string, int> codecs;
    int movies = 0, noVideo = 0, withAudio = 0;
    int dumped = 0;
    std::uint64_t outBytes = 0;
    int converted = 0;
    double worstMean = 0.0;
    int worstMax = 0;
    int parityChecked = 0;

    for (const auto &stack : source.stacks)
    {
        ArchiveSet set;
        std::vector<std::string> failures;
        set.openAll(stack.dataArchives, failures);
        const auto ids = set.resourceIds("tMOV");

        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const std::uint16_t id = ids[i];
            const auto bytes = set.readMovie(id);
            if (bytes.empty())
                continue;

            const MovieProbe m = probeMovieBytes(bytes, id, ff);
            ++movies;
            if (!m.ok)
            {
                ++noVideo;
                std::fprintf(stderr, "  tMOV %u: %s\n", id, m.error.c_str());
                continue;
            }
            ++codecs[m.videoCodec];
            if (m.hasAudio())
                ++withAudio;

            // Converting every movie would be a full conversion run; two per
            // stack is enough to catch the pipeline having stopped working.
            if (i % std::max<std::size_t>(1, ids.size() / 2) != 0 || converted >= 16)
                continue;

            const fs::path out =
                outDir / (std::string(rivendata::stackName(stack.id)) + "-"
                          + std::to_string(id) + ".rvid");
            const VideoResult vr = convertMovie(set, id, out, ff);
            if (!vr.ok)
            {
                check(false, "tMOV " + std::to_string(id) + ": " + vr.error);
                continue;
            }
            ++converted;
            outBytes += vr.bytes;

            // Re-read the header exactly as the DS will.
            std::vector<std::uint8_t> file(vr.bytes);
            FILE *f = std::fopen(out.string().c_str(), "rb");
            const std::size_t got =
                f != nullptr ? std::fread(file.data(), 1, file.size(), f) : 0;
            if (f != nullptr)
                std::fclose(f);

            RvidHeader hdr{};
            if (got < sizeof(hdr))
            {
                check(false, "the written .rvid reads back whole");
                continue;
            }
            std::memcpy(&hdr, file.data(), sizeof(hdr));
            if (!isRvid(hdr) || hdr.width == 0 || hdr.height == 0 || hdr.frameCount == 0
                || hdr.indexCount != hdr.frameCount)
            {
                check(false, "tMOV " + std::to_string(id) + " wrote a coherent .rvid");
                continue;
            }

            // Walk the frames the way a player would, and check the index
            // actually points at frame starts.
            RvidFrameEntry firstEntry{};
            std::memcpy(&firstEntry, file.data() + sizeof(hdr), sizeof(firstEntry));
            check(firstEntry.offset
                      >= sizeof(hdr)
                             + static_cast<std::size_t>(hdr.indexCount)
                                   * sizeof(RvidFrameEntry),
                  "tMOV " + std::to_string(id) + ": frame 0 starts at or after the index");
            std::size_t pos = firstEntry.offset;
            std::vector<std::size_t> starts;
            std::uint32_t largest = 0;
            bool walked = true;
            for (std::uint32_t n = 0; n < hdr.frameCount; ++n)
            {
                if (pos + sizeof(RvidFrameHeader) > file.size())
                {
                    walked = false;
                    break;
                }
                RvidFrameHeader fh{};
                std::memcpy(&fh, file.data() + pos, sizeof(fh));
                starts.push_back(pos);
                largest = std::max<std::uint32_t>(
                    largest, static_cast<std::uint32_t>(sizeof(fh) + fh.byteCount));
                pos += sizeof(fh) + fh.byteCount;
            }
            check(walked && pos == file.size(),
                  "tMOV " + std::to_string(id) + ": the frames tile the file from frame 0 onwards");
            check(largest == hdr.largestFrameBytes,
                  "tMOV " + std::to_string(id) + ": largestFrameBytes is the real maximum");

            // One entry per frame, in order, each pointing at that frame's start.
            // Raw frames predict from nothing, so every frame is an entry point and
            // the index is what makes seeking to one O(1).
            bool indexOk = starts.size() == hdr.frameCount;
            for (std::uint32_t n = 0; n < hdr.indexCount && indexOk; ++n)
            {
                RvidFrameEntry fe{};
                std::memcpy(&fe, file.data() + sizeof(hdr) + n * sizeof(fe), sizeof(fe));
                indexOk = fe.frame == n && n < starts.size() && starts[n] == fe.offset;
            }
            check(indexOk,
                  "tMOV " + std::to_string(id) + ": the index names every frame in order");

            // --- the quantiser ---------------------------------------------
            //
            // Frame 20 out of the container, the same frame out of ffmpeg, and
            // the same frame downscaled the way a still would have been. The
            // first pair must be IDENTICAL -- that is the whole claim of storing
            // raw texels. The second is the seam measurement: ffmpeg scales in
            // gamma space and downscaleToTexels averages in linear light, and if
            // that gap were large an overlay would not match the card under it.
            if (dumped < 3 && m.frames > 25 && starts.size() > 20)
            {
                RvidFrameHeader fh{};
                std::memcpy(&fh, file.data() + starts[20], sizeof(fh));
                const bool repeat = (fh.flags & kFrameRepeat) != 0;
                const std::size_t pixels =
                    static_cast<std::size_t>(vr.width) * vr.height;

                fs::path staged;
                std::string stageErr;
                {
                    // The probe deleted its own copy; stage one to read frames from.
                    staged = outDir / (std::to_string(id) + ".mov");
                    FILE *sf = std::fopen(staged.string().c_str(), "wb");
                    if (sf != nullptr)
                    {
                        std::fwrite(bytes.data(), 1, bytes.size(), sf);
                        std::fclose(sf);
                    }
                }

                const auto scaled = lastFrameOf(ff, staged, 21, m.width, m.height,
                                                vr.width, vr.height);
                const auto native =
                    lastFrameOf(ff, staged, 21, m.width, m.height, 0, 0);

                if (!repeat && scaled.size() == pixels * 3)
                {
                    const Texel *stored = reinterpret_cast<const Texel *>(
                        file.data() + starts[20] + sizeof(fh) + fh.audioBytes);
                    const auto ours = downscaleToTexels(scaled.data(), vr.width,
                                                        vr.height, vr.width, vr.height);
                    check(std::vector<Texel>(stored, stored + pixels) == ours,
                          "tMOV " + std::to_string(id)
                              + ": the stored frame is exactly our quantiser's output");

                    if (native.size()
                        == static_cast<std::size_t>(m.width) * m.height * 3)
                    {
                        const auto viaStills = downscaleToTexels(
                            native.data(), m.width, m.height, vr.width, vr.height);
                        double mean = 0.0;
                        int worst = 0;
                        texelDelta(ours, viaStills, mean, worst);
                        worstMean = std::max(worstMean, mean);
                        worstMax = std::max(worstMax, worst);
                        ++parityChecked;

                        const std::string base =
                            (outDir / (std::string(rivendata::stackName(stack.id)) + "-"
                                       + std::to_string(id)))
                                .string();
                        writePpm(base + "-ffmpeg-scaled.ppm", ours, vr.width, vr.height);
                        writePpm(base + "-stills-scaled.ppm", viaStills, vr.width,
                                 vr.height);
                    }
                    ++dumped;
                }
                fs::remove(staged, ec);
            }
        }
    }

    // --- the track matrix ---------------------------------------------------
    //
    // A tMOV's coded size is not the size Riven draws it at: QuickTime's track
    // matrix scales it, and 79 of a 5-CD install's 1054 movies carry one. The
    // converter ignored it for a long time, so those movies went onto the card
    // at twice or four times their size -- tspit's lever at 2x, the telescope
    // button at 4x, both plainly wrong on the screen.
    //
    // Two named movies rather than a survey: one that carries a scale and one
    // that does not, so a regression in either direction is caught. tspit 19 is
    // the lever (128x80 coded, matrix 0.5) and tspit 43 the telescope's travel
    // movie (80x112, matrix 1.0), which has always been right and must stay so.
    for (const auto &stack : source.stacks)
    {
        if (stack.id != rivendata::StackId::Tspit)
            continue;

        ArchiveSet set;
        std::vector<std::string> failures;
        set.openAll(stack.dataArchives, failures);

        struct Expect
        {
            std::uint16_t id;
            int w, h;
            const char *what;
        };
        // 128*0.5*256/608 = 26, 80*0.5*256/608 = 16; and 80*256/608 = 33,
        // 112*256/608 = 47 for the unscaled one.
        const Expect expects[] = {
            {19, 26, 16, "carries a 0.5 track matrix"},
            {43, 33, 47, "carries no track matrix"},
        };
        for (const Expect &e : expects)
        {
            const fs::path out = outDir / ("matrix-" + std::to_string(e.id) + ".rvid");
            const VideoResult vr = convertMovie(set, e.id, out, ff);
            if (!vr.ok)
            {
                check(false, "tMOV " + std::to_string(e.id) + ": " + vr.error);
                continue;
            }
            check(vr.width == e.w && vr.height == e.h,
                  "tspit tMOV " + std::to_string(e.id) + " " + e.what + ", so it converts to "
                      + std::to_string(e.w) + "x" + std::to_string(e.h) + " (got "
                      + std::to_string(vr.width) + "x" + std::to_string(vr.height) + ")");
            check(vr.trackScaled == (e.id == 19),
                  "tspit tMOV " + std::to_string(e.id) + " reports whether it was scaled");
        }
        break;
    }

    std::printf("video: %d movies, %d with audio, %d with no video stream\n", movies,
                withAudio, noVideo);
    for (const auto &[codec, n] : codecs)
        std::printf("  %-8s %d\n", codec.c_str(), n);
    check(movies > 0, "the install has movies");
    check(noVideo == 0, "ffprobe describes a video stream in every movie");

    if (parityChecked > 0)
    {
        // The threshold is a REGRESSION guard, not a quality bar: the measured
        // numbers on a 5-CD install are well inside it, and the point is to
        // notice if a change to either scaler moves them.
        std::printf("  scaler parity over %d frames: mean %.3f, worst %d "
                    "(of 31 levels)\n",
                    parityChecked, worstMean, worstMax);
        check(worstMean < 1.5,
              "ffmpeg's scaling and the stills' scaling agree on average");
        check(worstMax <= 12, "ffmpeg's scaling and the stills' scaling never diverge wildly");
    }

    if (converted > 0)
        std::printf("  converted %d movies, %llu bytes; frames in %s\n", converted,
                    static_cast<unsigned long long>(outBytes), outDir.string().c_str());

    std::printf("video: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
