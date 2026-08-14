#include "riven/VideoPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "RivenData.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/FFmpeg.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/SoundPipeline.hpp"

namespace fs = std::filesystem;
using namespace rivendata;

namespace riven
{
namespace
{
    /// Frame rate used when ffprobe cannot describe one. Riven's own movies are
    /// almost all 15 fps.
    constexpr int kFallbackFpsNum = 15;
    constexpr int kFallbackFpsDen = 1;

    /// Frames past the end of the video track, added so a movie whose audio
    /// outlasts its picture still carries all of its audio. Twelve of a 5-CD
    /// install's 795 movies need this -- ospit's tMOV 0 has 260 seconds of
    /// dialogue over 82 seconds of video -- and they carry no picture at all,
    /// which with raw frames saves 84 KB each rather than a few bytes.
    constexpr int kMaxTailFrames = 20000;

    /// Frames read from ffmpeg between cancellation checks. One: a frame is a
    /// single pipe read and the point of the check is that a 377 MB movie no
    /// longer has to finish before a cancel is noticed.
    constexpr int kCancelCheckFrames = 1;

    // --- the track matrix ---------------------------------------------------
    //
    // A tMOV's CODED size is not the size Riven draws it at. QuickTime's track
    // header carries a 3x3 display matrix, and its `a` and `d` terms scale the
    // track: 79 of the 1054 movies in a 5-CD install are authored at twice or
    // four times the size they are shown at.
    //
    // ScummVM applies it -- QuickTimeParser::readTKHD keeps exactly these two
    // terms as `scaleFactorX/Y = Rational(0x10000, xMod)`
    // (common/formats/quicktime.cpp:480-491) and VideoTrackHandler::getWidth()
    // returns width/scaleFactorX, which is what RivenVideo blits. Ignoring it
    // here is why tspit's lever came out at twice its size and the telescope
    // button at four times.
    //
    // Read from the resource rather than from ffprobe: ffprobe reports the coded
    // size and exposes the matrix only as a rotation. The bytes are already in
    // hand, and the walk is four atoms deep.

    /// A 16.16 fixed-point matrix term as a double, or 1.0 for anything this
    /// cannot read. Defaulting to 1.0 keeps a movie its coded size, which is
    /// what every build before this one did.
    struct TrackScale
    {
        double x = 1.0;
        double y = 1.0;
        bool identity() const { return x == 1.0 && y == 1.0; }
    };

    /// Half-open payload range of an atom.
    struct AtomRange
    {
        std::size_t off = 0;
        std::size_t end = 0;
        bool found = false;
    };

    /// The first child of [off, end) with this type. Atoms are
    /// `uint32 size, char type[4]` with the payload following, and a container's
    /// payload is more atoms.
    AtomRange childAtom(const std::vector<std::uint8_t> &d, const char *type,
                        std::size_t off, std::size_t end)
    {
        while (off + 8 <= end && off + 8 <= d.size())
        {
            const std::uint32_t size = (static_cast<std::uint32_t>(d[off]) << 24)
                                       | (static_cast<std::uint32_t>(d[off + 1]) << 16)
                                       | (static_cast<std::uint32_t>(d[off + 2]) << 8)
                                       | static_cast<std::uint32_t>(d[off + 3]);
            if (size < 8)
                break;
            std::size_t next = off + size;
            if (next > end)
                next = end;
            if (std::memcmp(&d[off + 4], type, 4) == 0)
                return {off + 8, next, true};
            off = next;
        }
        return {};
    }

    /// The same, following a chain of container types.
    AtomRange descend(const std::vector<std::uint8_t> &d, std::initializer_list<const char *> path,
                      std::size_t off, std::size_t end)
    {
        AtomRange at{off, end, true};
        for (const char *type : path)
        {
            at = childAtom(d, type, at.off, at.end);
            if (!at.found)
                return {};
        }
        return at;
    }

    std::int32_t be32(const std::vector<std::uint8_t> &d, std::size_t at)
    {
        return static_cast<std::int32_t>((static_cast<std::uint32_t>(d[at]) << 24)
                                         | (static_cast<std::uint32_t>(d[at + 1]) << 16)
                                         | (static_cast<std::uint32_t>(d[at + 2]) << 8)
                                         | static_cast<std::uint32_t>(d[at + 3]));
    }

    TrackScale readTrackScale(const std::vector<std::uint8_t> &bytes)
    {
        TrackScale s;
        const AtomRange moov = childAtom(bytes, "moov", 0, bytes.size());
        if (!moov.found)
            return s;

        // The VIDEO track, not the first one. jspit's tMOV 190 and 335 put their
        // sound track first, and a sound track's matrix is the identity -- so
        // taking whichever trak came first read 1:1 off the audio and left those
        // two movies at four times their size, which is the whole bug again in
        // the two files least likely to be looked at.
        std::size_t off = moov.off;
        while (off + 8 <= moov.end)
        {
            const AtomRange trak = childAtom(bytes, "trak", off, moov.end);
            if (!trak.found)
                break;
            off = trak.end;

            const AtomRange hdlr = descend(bytes, {"mdia", "hdlr"}, trak.off, trak.end);
            // hdlr: 4 bytes of version/flags, then the component type and its
            // subtype. The subtype is what says 'vide'.
            if (!hdlr.found || hdlr.end < hdlr.off + 12
                || std::memcmp(&bytes[hdlr.off + 8], "vide", 4) != 0)
                continue;

            const AtomRange tkhd = childAtom(bytes, "tkhd", trak.off, trak.end);
            // tkhd's payload ends with the 36-byte matrix and then width and
            // height, in both version 0 and version 1. `a` is the matrix's first
            // term and `d` its fifth, so they sit 44 and 28 bytes from the end.
            if (!tkhd.found || tkhd.end < tkhd.off + 44 || tkhd.end > bytes.size())
                return s;
            const std::int32_t a = be32(bytes, tkhd.end - 44);
            const std::int32_t d = be32(bytes, tkhd.end - 28);
            if (a > 0)
                s.x = static_cast<double>(a) / 65536.0;
            if (d > 0)
                s.y = static_cast<double>(d) / 65536.0;
            return s;
        }
        return s;
    }

    /// Deletes the temporary .mov on the way out, however that happens.
    struct ScopedFile
    {
        fs::path path;
        ~ScopedFile()
        {
            if (path.empty())
                return;
            std::error_code ec;
            fs::remove(path, ec);
        }
    };

    std::vector<std::string> split(const std::string &s, char sep)
    {
        std::vector<std::string> out;
        std::size_t at = 0;
        for (;;)
        {
            const std::size_t end = s.find(sep, at);
            out.push_back(s.substr(at, end == std::string::npos ? std::string::npos
                                                                : end - at));
            if (end == std::string::npos)
                break;
            at = end + 1;
        }
        while (!out.empty() && out.back().empty())
            out.pop_back();
        return out;
    }

    std::string firstLine(const std::string &s)
    {
        std::string line = s.substr(0, s.find('\n'));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        return line;
    }

    int toInt(const std::string &s, int fallback = 0)
    {
        if (s.empty() || s == "N/A")
            return fallback;
        char *end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        return end == s.c_str() ? fallback : static_cast<int>(v);
    }

    /// "30000/1001" or "15/1". ffprobe writes r_frame_rate this way and it is
    /// exactly the pair the container wants, so it is kept as a ratio rather
    /// than rounded to a double and back.
    void parseRate(const std::string &s, int &num, int &den)
    {
        num = kFallbackFpsNum;
        den = kFallbackFpsDen;
        const std::size_t slash = s.find('/');
        if (slash == std::string::npos)
            return;
        const int n = toInt(s.substr(0, slash));
        const int d = toInt(s.substr(slash + 1));
        if (n > 0 && d > 0)
        {
            num = n;
            den = d;
        }
    }

    /// One ffprobe call's worth of CSV, split into fields. Empty on any failure,
    /// which the callers read as "no such stream".
    std::vector<std::string> probeFields(const FFmpegPaths &ff, const fs::path &movie,
                                         const char *streams, const char *entries,
                                         std::string &error, bool countPackets = false)
    {
        std::vector<std::string> argv = {"-v", "error"};
        if (countPackets)
            argv.push_back("-count_packets");
        argv.insert(argv.end(), {"-select_streams", streams, "-show_entries", entries,
                                 "-of", "csv=p=0", movie.string()});

        std::string text;
        if (!runCapture(ff.ffprobe, argv, text, error))
            return {};
        const std::string line = firstLine(text);
        if (line.empty())
            return {};
        return split(line, ',');
    }

    std::string field(const std::vector<std::string> &f, std::size_t i)
    {
        if (i >= f.size() || f[i] == "N/A")
            return {};
        return f[i];
    }

    /// Write the movie somewhere ffmpeg can seek in it.
    ///
    /// Unavoidable: ffprobe and ffmpeg cannot read a Mohawk resource, and a .mov
    /// is not pipe-safe because its sample tables may sit after the media. The
    /// bytes are already in RAM -- readMovie rewrote the chunk offsets on the way
    /// out -- so this is one write, beside the output where it is on the same
    /// volume as the file being produced.
    bool stageMovie(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                    const fs::path &dir, fs::path &staged, std::string &error)
    {
        std::error_code ec;
        const fs::path where = dir.empty() ? fs::temp_directory_path(ec) : dir;
        fs::create_directories(where, ec);
        staged = where / ("riven-tmov-" + std::to_string(id) + ".tmp.mov");

        std::FILE *f = std::fopen(staged.string().c_str(), "wb");
        if (f == nullptr)
        {
            staged.clear();
            error = "could not stage the movie for ffmpeg";
            return false;
        }
        const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        const bool closed = std::fclose(f) == 0;
        if (!wrote || !closed)
        {
            error = "could not stage the movie for ffmpeg";
            return false;
        }
        return true;
    }

    /// The movie's whole soundtrack as mono PCM16 at the DS's rate.
    ///
    /// ffmpeg does the decode and the resample in one pass, which is what replaces
    /// decodeIma4 + the sample-table trimming the old pipeline needed. `-vn` keeps
    /// it from decoding the picture twice.
    ///
    /// The downmix is NOT left to ffmpeg's `-ac 1`, which averages the channels and
    /// hands back whatever level that leaves. Asking for stereo and folding it here
    /// costs one more pipe -- nothing beside the picture -- and gets the sounds and
    /// the movies onto the same rule: see downmixToMonoWithMakeup. A soundtrack that
    /// really is mono comes back as two identical channels and is unchanged by it.
    std::vector<std::int16_t> readAudio(const FFmpegPaths &ff, const fs::path &movie,
                                        std::string &error)
    {
        std::vector<std::int16_t> stereo;

        Subprocess p;
        if (!p.start(ff.ffmpeg,
                     {"-v", "error", "-nostdin", "-i", movie.string(),
                      "-vn", "-ac", "2", "-ar", std::to_string(kTargetRate), "-f",
                      "s16le", "pipe:1"},
                     error))
            return stereo;

        std::vector<std::uint8_t> buf(64 * 1024);
        std::vector<std::uint8_t> pending;
        for (;;)
        {
            const std::size_t got = p.read(buf.data(), buf.size());
            if (got == 0)
                break;
            pending.insert(pending.end(), buf.begin(), buf.begin() + got);
            // Whole samples only; a pipe can split one across two reads.
            const std::size_t whole = pending.size() & ~std::size_t(1);
            const std::size_t samples = whole / 2;
            const std::size_t at = stereo.size();
            stereo.resize(at + samples);
            std::memcpy(stereo.data() + at, pending.data(), whole);
            pending.erase(pending.begin(),
                          pending.begin() + static_cast<std::ptrdiff_t>(whole));
        }

        if (p.wait() != 0)
        {
            error = p.stderrText();
            return {};
        }
        // A truncated final frame would leave half a stereo pair behind and shift
        // every channel after it.
        if (stereo.size() % 2 != 0)
            stereo.pop_back();
        return downmixToMonoWithMakeup(stereo, 2);
    }
} // namespace

bool MovieProbe::fullscreen() const
{
    return width >= kCardW && height >= kCardH;
}

MovieProbe probeMovieFile(const fs::path &movie, const FFmpegPaths &ff)
{
    MovieProbe p;
    if (!ff.usable())
    {
        p.error = "ffmpeg is not available";
        return p;
    }

    // -count_packets, and NOT nb_frames or duration.
    //
    // Both of those are wrong on these files, badly and silently. A movie
    // extracted from a Mohawk archive comes out with its header rewritten
    // (vaht_mov), and measured across the game: nb_frames reads 23 for a
    // 350-frame movie and 195 for a 13-frame one, while duration-in-seconds
    // comes back numerically equal to the FRAME count, so duration * rate
    // overshoots by exactly the frame rate. Neither is off by a rounding error;
    // both are unusable.
    //
    // Counting packets costs a demux pass with no decoding, and Riven's codecs
    // are intra-only -- one packet is one frame -- so it is exact. Where it is
    // not exact it is an over-count (one movie in the game has 916 packets and
    // 625 frames), which is the safe direction: it is used to SIZE the index,
    // and the real count comes from the read loop.
    std::string err;
    const auto v = probeFields(ff, movie, "v:0",
                               "stream=codec_name,width,height,r_frame_rate,"
                               "nb_read_packets",
                               err, true);
    if (v.empty() || field(v, 0).empty())
    {
        p.error = err.empty() ? "no video stream" : err;
        return p;
    }

    p.videoCodec = field(v, 0);
    p.width = toInt(field(v, 1));
    p.height = toInt(field(v, 2));
    parseRate(field(v, 3), p.fpsNum, p.fpsDen);
    p.packets = toInt(field(v, 4));
    p.frames = p.packets;
    if (p.frames > 0 && p.rate() > 0.0)
        p.seconds = p.frames / p.rate();

    const auto a =
        probeFields(ff, movie, "a:0", "stream=codec_name,sample_rate,channels", err);
    if (!a.empty() && !field(a, 0).empty())
    {
        p.audioCodec = field(a, 0);
        p.audioRate = toInt(field(a, 1));
        p.audioChannels = toInt(field(a, 2));
    }

    p.ok = p.hasVideo();
    if (!p.ok)
        p.error = "the video stream has no frame size";
    return p;
}

MovieProbe probeMovieBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                           const FFmpegPaths &ff, const fs::path &scratchDir)
{
    MovieProbe p;
    p.id = id;
    p.resourceBytes = bytes.size();

    if (looksDamaged(bytes))
    {
        p.error = "zero-filled in this copy of the game";
        return p;
    }

    ScopedFile temp;
    if (!stageMovie(bytes, id, scratchDir, temp.path, p.error))
        return p;

    MovieProbe out = probeMovieFile(temp.path, ff);
    out.id = id;
    out.resourceBytes = bytes.size();
    return out;
}

VideoResult convertMovie(const ArchiveSet &set, std::uint16_t id, const fs::path &out,
                         const FFmpegPaths &ff, const CancelToken *cancel)
{
    const auto bytes = set.readMovie(id);
    if (bytes.empty())
    {
        // Either the resource is missing or libvaht could not follow its atom
        // tree. Ask for the raw bytes so the failure is reported against real
        // content rather than an empty buffer.
        VideoResult res;
        const auto raw = set.read("tMOV", id);
        res.error = "tMOV " + std::to_string(id)
                  + (raw.empty() ? " could not be read"
                                 : " is not a readable QuickTime movie");
        return res;
    }
    return convertMovieBytes(bytes, id, out, ff, cancel);
}

VideoResult convertMovieBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                              const fs::path &out, const FFmpegPaths &ff,
                              const CancelToken *cancel)
{
    VideoResult res;
    const std::string what = "tMOV " + std::to_string(id);

    if (looksDamaged(bytes))
    {
        res.error = what + " is zero-filled in this copy of the game";
        return res;
    }
    if (!ff.usable())
    {
        res.error = what + ": ffmpeg is not available";
        return res;
    }

    ScopedFile temp;
    if (!stageMovie(bytes, id, out.parent_path(), temp.path, res.error))
    {
        res.error = what + ": " + res.error;
        return res;
    }

    // --- what is in it -----------------------------------------------------
    const MovieProbe video = probeMovieFile(temp.path, ff);
    if (!video.ok)
    {
        res.error = what + ": " + (video.error.empty() ? "no readable video stream"
                                                       : video.error);
        res.unsupported = true;
        return res;
    }

    // --- geometry ----------------------------------------------------------
    // Two scales, in this order. First the TRACK MATRIX, which turns the coded
    // size into the size Riven actually draws the movie at -- see readTrackScale
    // above; 79 of the game's movies are authored bigger than they are shown.
    // Then the card ratio, because everything on the DS is the card scaled by
    // the same amount and an overlay has to line up with the still it sits on.
    // No padding: raw frames are the texels themselves.
    const TrackScale trackScale = readTrackScale(bytes);
    const int cardW = std::max(1, static_cast<int>(video.width * trackScale.x));
    const int cardH = std::max(1, static_cast<int>(video.height * trackScale.y));

    // Truncating, as before. A movie of card-width W drawn at left L covers DS
    // columns [toDsX(L), toDsX(L+W)), which is floor(W*s) wide or one more
    // depending on L -- and L is a property of the MLST record, not of the
    // movie, so it cannot be known here. floor() is the one that is never wide.
    const int dstW = std::max(1, cardW * kScaleNum / kScaleDen);
    const int dstH = std::max(1, cardH * kScaleNum / kScaleDen);
    const std::uint32_t frameBytes = rvidFrameBytes(dstW, dstH);

    if (!trackScale.identity())
        res.trackScaled = true;

    // The SCALED size, not the coded one: fullscreen is about what covers the
    // card. No shipped movie changes profile because of this -- nothing scaled
    // is anywhere near 608x392 -- but the two sizes are no longer the same
    // thing and this one has to say which it means.
    const VideoProfile profile =
        (cardW >= kCardW && cardH >= kCardH) ? VideoProfile::Full : VideoProfile::Lite;
    res.profile = profile;
    res.width = dstW;
    res.height = dstH;
    res.codec = video.videoCodec;

    const int fpsNum = video.fpsNum;
    const int fpsDen = video.fpsDen;
    const double fps = video.rate() > 0.0 ? video.rate() : 15.0;

    // --- audio -------------------------------------------------------------
    std::vector<std::int16_t> mono;
    if (video.hasAudio())
    {
        std::string err;
        mono = readAudio(ff, temp.path, err);
        if (mono.empty() && !err.empty())
        {
            // A movie whose picture is fine and whose sound is not is still
            // worth having, so this is a warning carried on the result rather
            // than a failure.
            res.audioError = err;
        }
        // The same loudness stage the sounds get (SoundPipeline.hpp), and the
        // one the opening cutscene needs most: its four movies average -32, -20,
        // -18 and -14 dB with their peaks already on the rail, so there is
        // nothing to be gained by a multiplier and everything by spending that
        // crest factor. Before the IMA encoder below, so the quantiser adapts.
        if (!mono.empty())
            mono = compressMono(mono, kTargetRate);
    }
    const int audioRate = kTargetRate;
    res.hasAudio = !mono.empty();

    // How many picture frames there are. A bound, not a promise: ffmpeg is asked
    // for a constant rate below and the real count comes from the read loop.
    int videoFrames = video.frames > 0 ? video.frames : 1;

    // A movie whose audio outlasts its picture gets extra frames that repeat the
    // last one, so the whole soundtrack fits in the container.
    int frameCount = videoFrames;
    if (!mono.empty() && fps > 0.0)
    {
        const int needed =
            static_cast<int>((static_cast<double>(mono.size()) / audioRate) * fps + 0.999);
        frameCount = std::clamp(std::max(frameCount, needed), frameCount,
                                frameCount + kMaxTailFrames);
    }

    // --- the picture -------------------------------------------------------
    //
    // ffmpeg decodes, scales and paces; this side quantises. The dither is NOT
    // handed to ffmpeg (-pix_fmt rgb555le would apply its own): 874 of Riven's
    // movies are overlays composited into a card still, and the two have to be
    // quantised the same way or the overlay shows a seam against the picture it
    // sits on. downscaleToTexels at 1:1 is exactly that quantiser -- the same
    // 4x4 Bayer threshold in linear light the stills go through.
    //
    // -vsync cfr with an explicit -r is what makes frame N of the output frame N
    // of the container. It replaces the modal-duration arithmetic the old
    // demuxer needed, and duplicate frames are then found by comparing texels,
    // which is both simpler and more accurate than trusting sample timings.
    //
    // ffmpeg is left to thread itself rather than pinned to one core, which is what
    // it was while the converter ran a movie per core. Measured, this buys nothing:
    // 28 movies of rspit take 32.3 s pinned and 32.7 s unpinned, both at ~185% of
    // an eight-core machine. ffmpeg's Cinepak decoder is not threaded, and 1038 of
    // Riven's 1055 movies are Cinepak, so there is no second core for it to find.
    // What that 185% is, is this process and ffmpeg's running at once through the
    // pipe: it decodes the next frame while we quantise the last. The flag is gone
    // anyway because there is no longer a reason to hold ffmpeg back.
    //
    // So the honest figure for one movie at a time is under two cores of eight. See
    // Converter.cpp on what that costs.
    char scale[64];
    std::snprintf(scale, sizeof(scale), "scale=%d:%d:flags=area", dstW, dstH);
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%d/%d", fpsNum, fpsDen);

    Subprocess pic;
    if (!pic.start(ff.ffmpeg,
                   {"-v", "error", "-nostdin", "-i", temp.path.string(),
                    "-an", "-vf", scale, "-vsync", "cfr", "-r", rate, "-f", "rawvideo",
                    "-pix_fmt", "rgb24", "pipe:1"},
                   res.error))
        return res;

    // --- the file ----------------------------------------------------------
    // Written in one pass, streaming: the header and the index go down first with
    // the index's offsets still blank, the frames follow, and the index is patched
    // in place at the end. The alternative is holding 377 MB in a vector.
    const std::size_t indexBytes =
        static_cast<std::size_t>(frameCount) * sizeof(RvidFrameEntry);
    const std::size_t base = sizeof(RvidHeader) + indexBytes;

    RvidHeader hdr{};
    std::memcpy(hdr.magic, "RVID", 4);
    hdr.version = kVideoVersion;
    hdr.profile = static_cast<std::uint8_t>(profile);
    hdr.flags = mono.empty() ? 0 : kVideoHasAudio;
    hdr.width = static_cast<std::uint16_t>(dstW);
    hdr.height = static_cast<std::uint16_t>(dstH);
    hdr.fpsNum = static_cast<std::uint16_t>(fpsNum);
    hdr.fpsDen = static_cast<std::uint16_t>(fpsDen);
    hdr.frameCount = static_cast<std::uint32_t>(frameCount);
    hdr.indexCount = static_cast<std::uint32_t>(frameCount);
    hdr.audioRate = static_cast<std::uint16_t>(mono.empty() ? 0 : audioRate);
    hdr.audioSamplesPerFrame = static_cast<std::uint16_t>(
        mono.empty() ? 0 : std::lround(audioRate * fpsDen / double(fpsNum)));
    hdr.largestFrameBytes = 0; // patched below

    AtomicFileWriter file;
    if (!file.open(out, res.error))
        return res;

    std::vector<RvidFrameEntry> index(static_cast<std::size_t>(frameCount));
    file.write(&hdr, sizeof(hdr));
    if (indexBytes > 0)
        file.write(index.data(), indexBytes);

    ImaStream audioStream;
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(dstW) * dstH * 3);
    std::vector<Texel> texels;
    std::vector<Texel> previous;
    std::vector<std::uint8_t> audioBlock;
    std::size_t audioPos = 0;
    std::uint32_t largestFrame = 0;
    std::uint64_t offset = base;
    bool havePicture = false;
    bool pictureEnded = false;
    int written = 0;

    for (int i = 0; i < frameCount; ++i)
    {
        if (cancel != nullptr && (i % kCancelCheckFrames) == 0 && cancel->cancelled())
        {
            // Kill the child before unwinding: without this the read end closes,
            // ffmpeg takes a SIGPIPE at its own pace, and a cancelled run leaves
            // one process per worker still decoding.
            pic.kill();
            pic.wait();
            cancel->throwIfCancelled();
        }

        // Past the end of the picture the frame simply repeats, and a repeated
        // raw frame is stored as no picture at all.
        bool newPicture = false;
        if (!pictureEnded)
        {
            bool eof = false;
            if (pic.readExact(rgb.data(), rgb.size(), eof))
            {
                downscaleToTexels(rgb.data(), dstW, dstH, dstW, dstH, texels);
                if (texels.size() != frameBytes / 2)
                {
                    res.error = what + ": frame " + std::to_string(i) + " did not quantise";
                    return res;
                }
                // A frame identical to the one before it costs 8 bytes instead
                // of 84 KB, and the DS shows the frame it is already holding.
                newPicture = previous.empty() || texels != previous;
                if (newPicture)
                    previous = texels;
                havePicture = true;
            }
            else if (eof)
            {
                pictureEnded = true;
            }
            else
            {
                res.error = what + ": ffmpeg stopped mid-frame";
                if (!pic.stderrText().empty())
                    res.error += ": " + pic.stderrText();
                return res;
            }
        }
        if (!havePicture)
        {
            res.error = what + ": ffmpeg produced no frames";
            if (!pic.stderrText().empty())
                res.error += ": " + pic.stderrText();
            return res;
        }
        // Nothing left to say once both streams have run out.
        if (pictureEnded && audioPos >= mono.size())
        {
            frameCount = i;
            break;
        }

        // Audio for this frame: however many samples the clock says should have
        // been played by the end of it, minus what has gone already.
        audioBlock.clear();
        if (!mono.empty())
        {
            const std::size_t upTo = static_cast<std::size_t>(
                (static_cast<double>(i + 1) * fpsDen / fpsNum) * audioRate);
            std::size_t take = upTo > audioPos ? upTo - audioPos : 0;
            take = std::min(take, mono.size() - std::min(audioPos, mono.size()));
            take &= ~std::size_t(1); // whole bytes, so blocks stay byte-aligned
            if (take > 0)
            {
                const ImaState start = audioStream.state();
                const auto nibbles = audioStream.encode(mono.data() + audioPos, take);
                const std::uint32_t word = makeAdpcmState(start.predictor, start.index);
                audioBlock.resize(kAdpcmStateBytes + nibbles.size());
                std::memcpy(audioBlock.data(), &word, sizeof(word));
                std::memcpy(audioBlock.data() + kAdpcmStateBytes, nibbles.data(),
                            nibbles.size());
                audioPos += take;
            }
        }

        RvidFrameHeader fh{};
        fh.audioBytes = static_cast<std::uint16_t>(audioBlock.size());
        fh.flags = newPicture ? 0 : kFrameRepeat;
        fh.byteCount = static_cast<std::uint32_t>(audioBlock.size())
                     + (newPicture ? frameBytes : 0u);

        index[static_cast<std::size_t>(i)].frame = static_cast<std::uint32_t>(i);
        index[static_cast<std::size_t>(i)].offset = static_cast<std::uint32_t>(offset);

        file.write(&fh, sizeof(fh));
        if (!audioBlock.empty())
            file.write(audioBlock.data(), audioBlock.size());
        if (newPicture)
            file.write(texels.data(), frameBytes);

        const std::uint32_t total = static_cast<std::uint32_t>(sizeof(fh)) + fh.byteCount;
        largestFrame = std::max(largestFrame, total);
        offset += total;
        ++written;
        if (!newPicture)
            ++res.repeatedFrames;

        if (!file.ok())
            break; // poisoned; commit() reports why
    }

    // The child is drained either way: a movie whose duration over-counted by a
    // frame leaves bytes in the pipe, and closing on them is what turns a clean
    // exit into a SIGPIPE the next check would report as a failure.
    pic.kill();
    const int picStatus = pic.wait();
    if (!havePicture)
    {
        res.error = what + ": ffmpeg produced no frames";
        if (!pic.stderrText().empty())
            res.error += ": " + pic.stderrText();
        return res;
    }
    (void)picStatus; // killed on purpose above, so the status says nothing

    frameCount = written;
    hdr.frameCount = static_cast<std::uint32_t>(frameCount);
    hdr.indexCount = static_cast<std::uint32_t>(frameCount);
    index.resize(static_cast<std::size_t>(frameCount));

    // The offsets are uint32, so a movie past 4 GB would silently wrap. Nothing
    // in Riven comes near -- the largest is 377 MB -- but the check costs nothing
    // and the failure would be invisible.
    if (offset > 0xFFFFFFFFull)
    {
        res.error = what + " is too large for a 32-bit index";
        return res;
    }

    hdr.largestFrameBytes = largestFrame;
    // The index was sized for the frame count the duration predicted and the
    // real count can be lower, so the tail is left as written-once padding
    // between the index and the first frame rather than moving every frame.
    if (!file.rewriteHeader(&hdr, sizeof(hdr), index.data(),
                            index.size() * sizeof(RvidFrameEntry), res.error))
        return res;
    if (!file.commit(res.error))
        return res;

    res.bytes = file.bytesWritten();
    res.frames = frameCount;
    res.ok = true;
    return res;
}

} // namespace riven
