// The .rvid format, end to end: written by the converter, read back by the code
// the DS reads it with.
//
// This used to compare an ARM9 decoder against a reference decoder frame by frame,
// because a codec that predicts from its own reconstruction can disagree with its
// encoder by one sample and have that compound across a GOP. There is no codec now,
// and the check that replaces it is stronger for being duller: a frame in the file
// must be EXACTLY the texels the downscaler produced. Not close -- equal.
//
// The other half is the container. source/rvid/RvidFile.cpp is compiled here for
// the host, which is why it includes no <nds.h>: on hardware nothing can check that
// a movie was read back the way it was written, and "the frames tile the file" and
// "the index names them in order" are exactly the kind of claim that is true right
// up until it is not.
//
// With RIVEN_TEST_DATA set it runs over a real movie. Without it, over one built
// here, so the format is covered either way.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "RivenVideo.hpp"
#include "riven/Archive.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/Layout.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/FFmpeg.hpp"

#include "riven/VideoPipeline.hpp"

#include "rvid/RvidFile.hpp" // the container reader, built for the host

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

    /// Read a whole .rvid the way the container test wants it: as bytes.
    std::vector<std::uint8_t> slurp(const fs::path &p)
    {
        std::vector<std::uint8_t> out;
        std::FILE *f = std::fopen(p.string().c_str(), "rb");
        if (f == nullptr)
            return out;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (size > 0)
        {
            out.resize(static_cast<std::size_t>(size));
            if (std::fread(out.data(), 1, out.size(), f) != out.size())
                out.clear();
        }
        std::fclose(f);
        return out;
    }

    /// Walk a .rvid as bytes and check the claims its header makes about itself.
    /// Everything here is a property a player depends on and cannot verify.
    void checkContainer(const fs::path &path, const std::string &label)
    {
        const auto file = slurp(path);
        if (file.size() < sizeof(RvidHeader))
        {
            check(false, label + ": the file reads back whole");
            return;
        }

        RvidHeader hdr{};
        std::memcpy(&hdr, file.data(), sizeof(hdr));
        check(isRvid(hdr), label + ": the magic and version validate");
        check(hdr.indexCount == hdr.frameCount,
              label + ": there is one index entry per frame");

        // Frame 0 starts where the index says it does, NOT necessarily where the
        // index ends: the converter sizes the index from an upper bound on the
        // frame count, so a short movie leaves unused entries in between.
        const std::size_t indexEnd =
            sizeof(hdr) + static_cast<std::size_t>(hdr.indexCount) * sizeof(RvidFrameEntry);
        RvidFrameEntry first{};
        if (hdr.indexCount > 0)
            std::memcpy(&first, file.data() + sizeof(hdr), sizeof(first));
        const std::size_t base = hdr.indexCount > 0 ? first.offset : indexEnd;
        check(base >= indexEnd, label + ": frame 0 starts at or after the index");
        const std::uint32_t pictureBytes = rvidFrameBytes(hdr.width, hdr.height);

        std::vector<std::size_t> starts;
        std::size_t pos = base;
        std::uint32_t largest = 0;
        int repeats = 0;
        bool walked = true;
        bool sizesOk = true;

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

            // A frame carries a whole picture or none at all. Anything else would
            // be read as texels and shown as noise.
            const std::uint32_t video = fh.byteCount - fh.audioBytes;
            const bool repeat = (fh.flags & kFrameRepeat) != 0;
            if (fh.audioBytes > fh.byteCount || (video != 0 && video != pictureBytes)
                || (repeat && video != 0) || (!repeat && video != pictureBytes))
                sizesOk = false;
            if (repeat)
                ++repeats;

            largest = std::max<std::uint32_t>(
                largest, static_cast<std::uint32_t>(sizeof(fh)) + fh.byteCount);
            pos += sizeof(fh) + fh.byteCount;
        }

        check(walked && pos == file.size(),
              label + ": the frames tile the file from frame 0 onwards");
        check(sizesOk, label + ": every frame carries a whole picture or none");
        check(largest == hdr.largestFrameBytes,
              label + ": largestFrameBytes is the real maximum");

        bool indexOk = starts.size() == hdr.frameCount;
        for (std::uint32_t n = 0; n < hdr.indexCount && indexOk; ++n)
        {
            RvidFrameEntry fe{};
            std::memcpy(&fe, file.data() + sizeof(hdr) + n * sizeof(fe), sizeof(fe));
            indexOk = fe.frame == n && n < starts.size() && starts[n] == fe.offset;
        }
        check(indexOk, label + ": the index names every frame in order, at its real offset");

        if (repeats > 0)
            std::printf("  %s: %d of %u frames repeat the picture before them\n",
                        label.c_str(), repeats, hdr.frameCount);
    }

    /// Read the file the way the DS does and check what comes out.
    void checkReader(const fs::path &path, const std::string &label,
                     std::uint32_t expectFrames)
    {
        rivenrt::RvidFile f;
        if (!f.open(path.string()))
        {
            check(false, label + ": the reader opens the file (" + f.error() + ")");
            return;
        }
        check(f.frameCount() == expectFrames, label + ": the reader agrees on the frame count");
        check(f.frameBytes() == rvidFrameBytes(f.header().width, f.header().height),
              label + ": the reader agrees on the picture size");

        std::uint32_t read = 0;
        std::uint64_t audio = 0;
        std::uint32_t lastPicture = 0;
        bool ok = true;
        rivenrt::RvidFrameData frame;
        while (f.readNext(frame))
        {
            if (frame.index != read)
            {
                check(false, label + ": frame " + std::to_string(read) + " reads out of order");
                ok = false;
                break;
            }
            if (!frame.repeat && frame.videoBytes != f.frameBytes())
            {
                check(false, label + ": frame " + std::to_string(read) + " is the wrong size");
                ok = false;
                break;
            }
            if (!frame.repeat)
                lastPicture = read + 1;
            audio += frame.audioBytes;
            ++read;
        }
        check(ok && read == expectFrames,
              label + ": every frame reads (" + std::to_string(read) + " of "
                  + std::to_string(expectFrames) + ")");

        // pictureFrames() against the same answer reached the other way round.
        //
        // The reader derives it from the INDEX -- the byte size of each frame,
        // read backwards at open() -- and this counts the frames that actually
        // came out carrying a picture, which is the kFrameRepeat FLAG. Two
        // independent signals for one fact, which is the whole value of checking
        // it here: on hardware nothing can tell you the credits started early.
        if (ok)
            check(f.pictureFrames() == lastPicture,
                  label + ": pictureFrames is the last frame with a picture ("
                      + std::to_string(f.pictureFrames()) + " vs "
                      + std::to_string(lastPicture) + ")");
        if (f.hasAudio())
            check(audio > 0, label + ": the frames carry their audio blocks");

        // Seeking. Every frame is an entry point, so this is exact rather than
        // "the keyframe at or before" -- and Riven restarts movies constantly.
        bool seeksOk = true;
        std::string why;
        for (std::uint32_t step = 0; step < 12 && seeksOk; ++step)
        {
            const std::uint32_t target = expectFrames * step / 12;
            const std::int32_t at = f.seekToFrame(target);
            if (at != static_cast<std::int32_t>(target))
            {
                seeksOk = false;
                why = "seek landed on " + std::to_string(at) + " not " + std::to_string(target);
                break;
            }
            if (!f.readNext(frame) || frame.index != target)
            {
                seeksOk = false;
                why = "the frame there is not the one asked for";
                break;
            }
        }
        check(seeksOk, label + ": seeking lands on the exact frame asked for (" + why + ")");

        check(f.seekToFrame(expectFrames) < 0, label + ": seeking past the end is refused");
        check(f.rewind() && f.position() == 0, label + ": rewind goes to frame 0");

        // And that reading CARRIES ON in order from wherever the seek landed,
        // which is what segment playback is: RvidPlayer::play(loop, start, stop)
        // enters a movie part way through and reads a run of it (the telescope
        // plays a fifth of one long movie per press).
        if (expectFrames > 4)
        {
            const std::uint32_t from = expectFrames / 2;
            const std::uint32_t want = expectFrames - from < 4 ? expectFrames - from : 4;
            bool runOk = f.seekToFrame(from) == static_cast<std::int32_t>(from);
            for (std::uint32_t i = 0; runOk && i < want; ++i)
                runOk = f.readNext(frame) && frame.index == from + i;
            check(runOk, label + ": a run reads in order from the middle of the file");
        }
    }

    /// The check the codec comparison was replaced by: what is in the file is
    /// exactly what the quantiser produced, for every frame.
    ///
    /// The reference now comes from a second ffmpeg run rather than from a
    /// reference decoder -- the pipeline's own decoding is ffmpeg's, so the only
    /// thing left worth asserting is that nothing between ffmpeg's rgb24 and the
    /// bytes on the card alters a pixel.
    void checkFidelity(const fs::path &movie, const FFmpegPaths &ff, const fs::path &path,
                       int dstW, int dstH, const std::string &label, std::uint32_t limit)
    {
        char scale[64];
        std::snprintf(scale, sizeof(scale), "scale=%d:%d:flags=area", dstW, dstH);

        Subprocess pic;
        std::string err;
        if (!pic.start(ff.ffmpeg,
                       {"-v", "error", "-nostdin", "-threads", "1", "-i", movie.string(),
                        "-an", "-vf", scale, "-vsync", "cfr", "-f", "rawvideo",
                        "-pix_fmt", "rgb24", "pipe:1"},
                       err))
        {
            check(false, label + ": ffmpeg runs for the reference decode (" + err + ")");
            return;
        }

        rivenrt::RvidFile f;
        if (!f.open(path.string()))
        {
            check(false, label + ": the reader opens the file (" + f.error() + ")");
            return;
        }

        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(dstW) * dstH * 3);
        std::vector<Texel> want;
        std::vector<Texel> held;
        rivenrt::RvidFrameData frame;
        std::uint32_t checked = 0;
        std::int64_t firstBad = -1;

        for (std::uint32_t n = 0; n < limit; ++n)
        {
            if (!f.readNext(frame))
                break;
            bool eof = false;
            if (!pic.readExact(rgb.data(), rgb.size(), eof))
                break;
            downscaleToTexels(rgb.data(), dstW, dstH, dstW, dstH, want);

            // A frame the pipeline stored as a repeat carries no picture, and the
            // DS keeps showing the one before it -- so that is what to compare
            // ffmpeg's output against.
            if (frame.repeat)
            {
                if (held != want)
                {
                    firstBad = n;
                    break;
                }
            }
            else
            {
                if (frame.videoBytes != want.size() * sizeof(Texel)
                    || std::memcmp(frame.video, want.data(), frame.videoBytes) != 0)
                {
                    firstBad = n;
                    break;
                }
                held = want;
            }
            ++checked;
        }

        pic.kill();
        pic.wait();

        check(firstBad < 0 && checked > 0,
              label + ": every frame in the file is exactly what the quantiser produced"
                  + (firstBad < 0 ? " (" + std::to_string(checked) + " frames)"
                                  : ", first differing at " + std::to_string(firstBad)));
    }

    /// A synthetic movie, so the format is covered without game data. Not a
    /// QuickTime file -- convertMovieBytes needs one of those -- so this exercises
    /// the container and the reader by writing one the same way the pipeline does.
    ///
    /// `tailFrames` of the total carry audio and no picture, which is how the
    /// pipeline stores a movie whose soundtrack outlasts its video (the endings
    /// are all like this, and ospit's tMOV 0 runs 260 seconds of dialogue over 82
    /// seconds of picture). Zero for a movie whose two tracks end together.
    void syntheticContainer(const fs::path &path, int w, int h, std::uint32_t frames,
                            std::uint32_t tailFrames = 0)
    {
        const std::uint32_t pictureBytes = rvidFrameBytes(w, h);

        RvidHeader hdr{};
        std::memcpy(hdr.magic, "RVID", 4);
        hdr.version = kVideoVersion;
        hdr.profile = static_cast<std::uint8_t>(VideoProfile::Lite);
        hdr.flags = 0;
        hdr.width = static_cast<std::uint16_t>(w);
        hdr.height = static_cast<std::uint16_t>(h);
        hdr.fpsNum = 15;
        hdr.fpsDen = 1;
        hdr.flags = tailFrames > 0 ? kVideoHasAudio : 0;
        hdr.frameCount = frames;
        hdr.indexCount = frames;
        hdr.largestFrameBytes =
            static_cast<std::uint32_t>(sizeof(RvidFrameHeader)) + pictureBytes;

        // A tail frame's audio block, sized the way the pipeline sizes one: the
        // samples a frame's worth of the clock is worth, as IMA nibbles behind
        // their state word. The number does not matter to anything being tested
        // here; that it is FAR SMALLER than a picture does, because that is the
        // difference RvidFile::pictureFrames reads.
        const std::uint32_t audioBytes = tailFrames > 0 ? 4u + 22050u / 15u / 2u : 0u;
        hdr.audioRate = tailFrames > 0 ? 22050 : 0;
        hdr.audioSamplesPerFrame = tailFrames > 0 ? 22050 / 15 : 0;

        std::vector<RvidFrameEntry> index(frames);
        std::vector<Texel> picture(pictureBytes / sizeof(Texel));

        std::string err;
        AtomicFileWriter out;
        if (!out.open(path, err))
        {
            check(false, std::string("the synthetic movie opens for writing: ") + err);
            return;
        }
        out.write(&hdr, sizeof(hdr));
        out.write(index.data(), index.size() * sizeof(RvidFrameEntry));

        const std::uint32_t pictureCount = tailFrames < frames ? frames - tailFrames : frames;
        std::vector<std::uint8_t> audio(audioBytes, 0x42);

        std::uint64_t offset = sizeof(hdr) + index.size() * sizeof(RvidFrameEntry);
        for (std::uint32_t n = 0; n < frames; ++n)
        {
            const bool repeat = n >= pictureCount;
            for (std::size_t i = 0; i < picture.size(); ++i)
                picture[i] = static_cast<Texel>(0x8000 | ((n + i) & 0x7FFF));

            RvidFrameHeader fh{};
            fh.audioBytes = static_cast<std::uint16_t>(audioBytes);
            fh.flags = repeat ? kFrameRepeat : 0;
            fh.byteCount = audioBytes + (repeat ? 0u : pictureBytes);

            index[n].frame = n;
            index[n].offset = static_cast<std::uint32_t>(offset);
            out.write(&fh, sizeof(fh));
            if (audioBytes > 0)
                out.write(audio.data(), audioBytes);
            if (!repeat)
                out.write(picture.data(), pictureBytes);
            offset += sizeof(fh) + fh.byteCount;
        }
        // Only a picture frame can be the largest, and there is always one.
        hdr.largestFrameBytes =
            static_cast<std::uint32_t>(sizeof(RvidFrameHeader)) + audioBytes + pictureBytes;

        if (!out.rewriteHeader(&hdr, sizeof(hdr), index.data(),
                               index.size() * sizeof(RvidFrameEntry), err)
            || !out.commit(err))
            check(false, std::string("the synthetic movie writes: ") + err);
    }

    /// A stand-in for the ARM9's tonccpy sink: records every chunk it is handed,
    /// in order, and reassembles the picture from the offsets it was given.
    struct SinkRecorder
    {
        std::vector<std::uint8_t> assembled;
        std::vector<std::size_t> chunkSizes;
        std::size_t highWater = 0;
        bool outOfOrder = false;

        void reset(std::size_t frameBytes)
        {
            assembled.assign(frameBytes, 0);
            chunkSizes.clear();
            highWater = 0;
            outOfOrder = false;
        }

        static void thunk(void *ctx, std::size_t off, const void *src, std::size_t n)
        {
            auto &r = *static_cast<SinkRecorder *>(ctx);
            // The ARM9 sink writes VRAM at dstOffset with no bookkeeping of its
            // own, so the reader owes it strictly forward, non-overlapping,
            // gap-free offsets.
            if (off != r.highWater)
                r.outOfOrder = true;
            r.highWater = off + n;
            if (off + n <= r.assembled.size())
                std::memcpy(r.assembled.data() + off, src, n);
            else
                r.outOfOrder = true;
            r.chunkSizes.push_back(n);
        }
    };

    /// The sink delivers exactly the bytes the plain read does, in order.
    ///
    /// This is the one place that can be checked at all: on hardware the sink's
    /// destination is VRAM, and a dropped or misplaced chunk shows up as a band
    /// of noise across a cutscene rather than as an error.
    void checkVideoSink(const fs::path &path, const std::string &label,
                        std::uint32_t frames)
    {
        rivenrt::RvidFile plain;
        rivenrt::RvidFile sunk;
        if (!plain.open(path.string()) || !sunk.open(path.string()))
        {
            check(false, label + ": both readers open the file");
            return;
        }

        const std::size_t frameBytes = plain.frameBytes();
        std::vector<std::uint8_t> direct(frameBytes);
        SinkRecorder rec;
        sunk.setVideoSink(&SinkRecorder::thunk, &rec);

        bool same = true;
        bool sizesOk = true;
        bool ordered = true;
        bool videoCleared = true;
        std::uint32_t seen = 0;
        std::size_t chunksPerFrame = 0;

        for (;; ++seen)
        {
            rivenrt::RvidFrameData a{};
            rivenrt::RvidFrameData b{};
            const bool ga = plain.readNextInto(a, direct.data(), direct.size());
            rec.reset(frameBytes);
            // The destination pointer is still required (it is what selects the
            // caller-buffer path) even though the sink is what places the bytes.
            const bool gb = sunk.readNextInto(b, rec.assembled.data(), frameBytes);
            if (ga != gb)
            {
                check(false, label + ": both readers end at the same frame");
                return;
            }
            if (!ga)
                break;

            if (a.videoBytes != b.videoBytes || a.repeat != b.repeat
                || a.index != b.index || a.audioBytes != b.audioBytes)
                sizesOk = false;
            if (a.videoBytes > 0)
            {
                if (std::memcmp(direct.data(), rec.assembled.data(), a.videoBytes) != 0)
                    same = false;
                if (rec.outOfOrder || rec.highWater != a.videoBytes)
                    ordered = false;
                // With a sink installed the picture is NOT in the reader's
                // buffer, and out.video must say so rather than pointing at the
                // staging area, which is about to be overwritten.
                if (b.video != nullptr)
                    videoCleared = false;
                chunksPerFrame = rec.chunkSizes.size();
            }
        }

        check(seen == frames, label + ": the sink path reads every frame");
        check(sizesOk, label + ": the sink path reports the same frame metadata");
        check(same, label + ": the sink delivers exactly the bytes a plain read does");
        check(ordered, label + ": chunks arrive in order, gap-free, and stop at the end");
        check(videoCleared, label + ": a sunk frame reports no readable picture pointer");

        const bool split = frameBytes > rivenrt::RvidFile::kSinkChunkBytes;
        check(!split || chunksPerFrame > 1,
              label + ": a frame larger than one chunk is delivered in several");
    }
} // namespace

int main()
{
    // --- the reader's refusals ----------------------------------------------
    {
        rivenrt::RvidFile f;
        check(!f.open("/nonexistent/nothing.rvid"), "a missing file is refused");

        const fs::path junk = fs::temp_directory_path() / "riven-test-not-a-movie.rvid";
        std::string err;
        const std::uint8_t garbage[64] = {'N', 'O', 'P', 'E'};
        writeFileAtomic(junk, garbage, sizeof(garbage), err);
        check(!f.open(junk.string()), "a file that is not RVID is refused");
        std::error_code ec;
        fs::remove(junk, ec);
    }

    // --- a synthetic movie ---------------------------------------------------
    {
        const fs::path path = fs::temp_directory_path() / "riven-test-synthetic.rvid";
        syntheticContainer(path, 40, 72, 37);
        checkContainer(path, "synthetic 40x72");
        checkReader(path, "synthetic 40x72", 37);
        checkVideoSink(path, "synthetic 40x72", 37);
        std::error_code ec;
        fs::remove(path, ec);
    }

    // --- a movie whose soundtrack outlasts its picture -------------------------
    //
    // The shape every ending has, and the reason RvidFile::pictureFrames exists:
    // twelve of the game's movies carry frames past the end of their video that
    // hold audio and no picture at all. Before this, the credits waited for the
    // whole container and so arrived minutes after the screen stopped moving.
    //
    // 12 of the 40 frames are tail, so the answer wanted is 28 -- and it has to
    // come out of the INDEX, which is all the reader has at open() time.
    {
        const fs::path path = fs::temp_directory_path() / "riven-test-audio-tail.rvid";
        syntheticContainer(path, 40, 72, 40, 12);
        checkContainer(path, "tailed 40x72");
        checkReader(path, "tailed 40x72", 40);
        {
            rivenrt::RvidFile f;
            if (f.open(path.string()))
                check(f.pictureFrames() == 28 && f.frameCount() == 40,
                      "the audio tail is not counted as picture ("
                          + std::to_string(f.pictureFrames()) + " of "
                          + std::to_string(f.frameCount()) + ")");
            else
                check(false, "the tailed movie opens");
        }
        std::error_code ec;
        fs::remove(path, ec);
    }

    // --- the sink does not outlive the movie that installed it ----------------
    //
    // The bug this pins down showed on hardware as a black rectangle exactly the
    // size and position of an overlay animation. A movie slot is reused for movie
    // after movie, and the two profiles want opposite things: a fullscreen movie
    // installs a sink because its destination is VRAM, an overlay reads into its
    // own scratch buffer. A sink left behind by the first swallows the second's
    // picture whole -- the read succeeds, the scratch buffer keeps the zeros it was
    // allocated with, and zero is a transparent texel, so the composite punches a
    // hole through to the black backdrop.
    {
        const fs::path path = fs::temp_directory_path() / "riven-test-sink-reuse.rvid";
        syntheticContainer(path, 40, 72, 6);

        rivenrt::RvidFile f;
        SinkRecorder rec;
        check(f.open(path.string()), "reuse: the file opens");
        f.setVideoSink(&SinkRecorder::thunk, &rec);
        rec.reset(f.frameBytes());
        rivenrt::RvidFrameData sunkFrame{};
        check(f.readNextInto(sunkFrame, rec.assembled.data(), f.frameBytes())
                  && rec.highWater > 0,
              "reuse: the sink takes the picture while it is installed");

        // Reopening is what a slot does between movies.
        f.close();
        check(f.open(path.string()), "reuse: the file opens again");

        std::vector<std::uint8_t> caller(f.frameBytes(), 0);
        rec.reset(f.frameBytes());
        rivenrt::RvidFrameData plainFrame{};
        check(f.readNextInto(plainFrame, caller.data(), caller.size()),
              "reuse: the reopened file reads");
        check(rec.highWater == 0, "reuse: close() takes the sink back off");
        check(plainFrame.video == caller.data(),
              "reuse: the picture goes to the caller's buffer, not the old sink");
        bool wrote = false;
        for (const std::uint8_t b : caller)
            wrote = wrote || b != 0;
        check(wrote, "reuse: the caller's buffer is actually written");

        std::error_code ec;
        fs::remove(path, ec);
    }

    // --- a movie whose frame does not divide into whole chunks ---------------
    //
    // 256x165 is what every fullscreen movie is, and 84480 bytes is ten whole
    // 8192-byte chunks plus 2560 -- so the sink's last chunk is a short one and
    // its row-splitting has to cope. The odd width below makes the chunks land
    // mid-row as well, which a 256-wide movie never does.
    {
        const fs::path path = fs::temp_directory_path() / "riven-test-chunky.rvid";
        syntheticContainer(path, 253, 165, 5);
        checkContainer(path, "synthetic 253x165");
        checkVideoSink(path, "synthetic 253x165", 5);
        std::error_code ec;
        fs::remove(path, ec);
    }

    // --- a real movie --------------------------------------------------------
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || *dataEnv == '\0')
    {
        std::printf("RIVEN_TEST_DATA not set: skipping the real-movie checks\n");
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    const Source src = detectSource(dataEnv);
    const StackSource *aspit = src.valid() ? src.find(StackId::Aspit) : nullptr;
    if (aspit == nullptr)
    {
        std::printf("no aspit in %s: skipping the real-movie checks\n", dataEnv);
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    ArchiveSet set;
    std::vector<std::string> failures;
    set.openAll(aspit->dataArchives, failures);
    if (set.empty())
    {
        std::printf("could not open aspit's archives: skipping\n");
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    // ffmpeg is what decodes now, so a machine without it can still check the
    // container and the reader above but not a real movie.
    const FFmpegPaths ff = findFFmpeg();
    std::string version;
    std::string ffError;
    if (!probeFFmpeg(ff, version, ffError))
    {
        std::printf("skipping the real-movie checks: %s\n", ffError.c_str());
        std::printf("%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    // aspit tMOV 0 is the trap book animation: 608x392 and therefore FULL, 121
    // frames at 15 fps, with an IMA4 track. Small enough to check every frame of.
    const auto bytes = set.readMovie(0);
    const MovieProbe probe = probeMovieBytes(bytes, 0, ff);
    if (!probe.ok)
    {
        check(false, "aspit tMOV 0 has a video track: " + probe.error);
    }
    else
    {
        const int dstW = probe.width * kScaleNum / kScaleDen;
        const int dstH = probe.height * kScaleNum / kScaleDen;
        check(probe.fullscreen(), "aspit tMOV 0 is a FULL-profile movie");
        check(probe.hasAudio(), "aspit tMOV 0 has a soundtrack");

        const fs::path out = fs::temp_directory_path() / "riven-test-rvid-arm9.rvid";
        const VideoResult vr = convertMovieBytes(bytes, 0, out, ff);
        if (!vr.ok)
        {
            check(false, "aspit tMOV 0 converts: " + vr.error);
        }
        else
        {
            check(vr.profile == VideoProfile::Full, "the result is FULL");
            check(vr.hasAudio, "the result has audio");
            check(vr.width == dstW && vr.height == dstH,
                  "the result is the card view scaled by the one ratio");

            // The fidelity check needs the movie as a file, the same way the
            // pipeline staged it.
            const fs::path staged = fs::temp_directory_path() / "riven-test-tmov0.mov";
            std::FILE *sf = std::fopen(staged.string().c_str(), "wb");
            if (sf != nullptr)
            {
                std::fwrite(bytes.data(), 1, bytes.size(), sf);
                std::fclose(sf);
            }

            checkContainer(out, "aspit tMOV 0");
            checkReader(out, "aspit tMOV 0", static_cast<std::uint32_t>(vr.frames));
            checkFidelity(staged, ff, out, dstW, dstH, "aspit tMOV 0",
                          static_cast<std::uint32_t>(vr.frames));
            checkVideoSink(out, "aspit tMOV 0", static_cast<std::uint32_t>(vr.frames));

            std::printf("  aspit tMOV 0: %s, %d frames, %llu bytes (%.1f KB/frame), "
                        "%d repeated\n",
                        probe.videoCodec.c_str(), vr.frames,
                        static_cast<unsigned long long>(vr.bytes),
                        vr.frames > 0 ? vr.bytes / 1024.0 / vr.frames : 0.0,
                        vr.repeatedFrames);

            std::error_code ec;
            fs::remove(out, ec);
            fs::remove(staged, ec);
        }
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
