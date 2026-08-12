// The video path: bit IO, the transform, the RVID round trip, the QuickTime
// demuxer, and the two source codecs.
//
// The check that matters most is that the reference decoder reproduces the
// encoder's own reconstruction EXACTLY, frame after frame. RVID predicts from
// the decoded picture, so any disagreement between the two sides does not stay
// small -- it compounds until the next keyframe. Everything else here is in
// service of being able to trust that one.
//
// With RIVEN_TEST_DATA set, the same round trip runs over real movies and dumps
// a few frames as PPM. Video is the one thing in this converter where a person
// has to look at the output.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "RivenVideo.hpp"
#include "riven/Archive.hpp"
#include "riven/Bits.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/Layout.hpp"
#include "riven/QuickTime.hpp"
#include "riven/Rvid.hpp"
#include "riven/VideoCodecs.hpp"
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

    /// A synthetic clip with the three things that exercise every path: motion,
    /// a hard cut, and frames where nothing moves at all.
    RvidFrame syntheticFrame(int w, int h, int n)
    {
        std::vector<Texel> texels(static_cast<std::size_t>(w) * h);
        const int shift = n < 20 ? n : 20;      // pans, then holds still
        const bool inverted = n >= 30;          // a hard cut at frame 30
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                int v = ((x + shift) / 3 + y / 5) & 0x1F;
                if (inverted)
                    v = 31 - v;
                const int r = v, g = (v * 2) & 0x1F, b = 31 - v;
                texels[static_cast<std::size_t>(y) * w + x] =
                    static_cast<Texel>(0x8000 | (b << 10) | (g << 5) | r);
            }
        return texelsToFrame(texels, w, h);
    }
} // namespace

int main()
{
    // --- bit IO -------------------------------------------------------------
    {
        // Exp-Golomb at its boundaries: every power of two changes the code
        // length, and the signed folding has to survive the sign of zero.
        std::vector<std::int32_t> values = {0,  1,  -1,  2,   -2,  3,   -3, 7,
                                            8,  -8, 15, 16,  -16, 63, 64, 255,
                                            256, -256, 4095, -4095};
        BitWriter w;
        for (const auto v : values)
            w.putSE(v);
        for (const auto v : values)
            w.putUE(static_cast<std::uint32_t>(std::abs(v)));
        w.flush();

        BitReader r(w.bytes().data(), w.bytes().size());
        bool same = true;
        for (const auto v : values)
            same = same && r.getSE() == v;
        for (const auto v : values)
            same = same && r.getUE() == static_cast<std::uint32_t>(std::abs(v));
        check(same && r.ok(), "Exp-Golomb round-trips at its boundary values");

        // The cost functions drive every rate-distortion decision, so they have
        // to agree with what the writer actually emits.
        bool costsMatch = true;
        for (const auto v : values)
        {
            BitWriter one;
            one.putSE(v);
            costsMatch = costsMatch && static_cast<int>(one.sizeBits()) == BitWriter::costSE(v);
        }
        check(costsMatch, "the bit-cost estimate matches what is written");
    }

    // --- the transform ------------------------------------------------------
    {
        std::mt19937 rng(12345);
        std::uniform_int_distribution<int> pixel(0, 31);
        bool exact = true;
        for (int trial = 0; trial < 2000 && exact; ++trial)
        {
            int src[4], pred[4];
            for (int i = 0; i < 4; ++i)
            {
                src[i] = pixel(rng);
                pred[i] = pixel(rng);
            }
            int resid[4];
            for (int i = 0; i < 4; ++i)
                resid[i] = src[i] - pred[i];

            int coef[4];
            forwardHadamard(resid, coef);
            int out[4];
            inverseHadamard(coef, pred, out);
            for (int i = 0; i < 4; ++i)
                exact = exact && out[i] == src[i];
        }
        check(exact, "the transform is lossless when nothing is quantised");

        // The half-pel blend is the DS's, not an average: it must stay in range
        // and be symmetric.
        bool blendOk = true;
        for (int a = 0; a <= 31; ++a)
            for (int b = 0; b <= 31; ++b)
            {
                const int v = blendHalf(a, b);
                blendOk = blendOk && v >= 0 && v <= 31 && v == blendHalf(b, a);
            }
        check(blendOk, "the half-pel blend is symmetric and stays in 5 bits");
    }

    // --- the RVID round trip ------------------------------------------------
    for (const auto profile : {VideoProfile::Lite, VideoProfile::Full})
    {
        const int w = 64, h = 48;
        RvidSettings settings;
        settings.profile = profile;

        RvidEncoder encoder(w, h, settings);
        RvidDecoder decoder(w, h, settings);

        bool identical = true;
        bool decoded = true;
        long worstError = 0;
        std::vector<std::vector<std::uint8_t>> frames;
        std::vector<bool> intraFlags;

        for (int n = 0; n < 40; ++n)
        {
            const RvidFrame src = syntheticFrame(w, h, n);
            const RvidEncodedFrame e = encoder.encode(src, n == 0);
            frames.push_back(e.bytes);
            intraFlags.push_back(e.intra);

            if (!decoder.decode(e.bytes.data(), e.bytes.size(), e.intra))
            {
                decoded = false;
                break;
            }
            for (int p = 0; p < kPlaneCount; ++p)
                identical = identical
                            && decoder.frame().plane[p] == encoder.reference().plane[p];

            for (int p = 0; p < kPlaneCount; ++p)
                for (std::size_t i = 0; i < src.plane[p].size(); ++i)
                    worstError = std::max<long>(
                        worstError, std::abs(static_cast<int>(src.plane[p][i])
                                             - static_cast<int>(decoder.frame().plane[p][i])));
        }

        const std::string what = profile == VideoProfile::Full ? "FULL" : "LITE";
        check(decoded, what + ": every frame decodes");
        check(identical, what + ": the decoder reproduces the encoder's reconstruction exactly");
        check(worstError <= 8, what + ": no sample drifts more than 8 levels from the source");

        // A hard cut has to produce an I-frame of its own accord, or the
        // encoder is spending P-frame bits on a picture it cannot predict.
        check(intraFlags.size() > 30 && intraFlags[30], what + ": a hard cut forces a keyframe");

        // Decoding from a keyframe must give the same picture as decoding from
        // the start -- this is what the keyframe index promises.
        std::size_t k = 0;
        for (std::size_t i = 1; i < intraFlags.size(); ++i)
            if (intraFlags[i])
                k = i;
        if (k > 0)
        {
            RvidDecoder fresh(w, h, settings);
            bool ok = true;
            for (std::size_t i = k; i < frames.size(); ++i)
                ok = ok && fresh.decode(frames[i].data(), frames[i].size(), intraFlags[i]);
            check(ok && fresh.frame().plane[0] == decoder.frame().plane[0],
                  what + ": decoding from a keyframe matches decoding from the start");
        }
    }

    // --- plane conversion ---------------------------------------------------
    {
        // Padding to a multiple of 8 must not disturb the real pixels.
        std::vector<Texel> texels(21 * 13);
        for (std::size_t i = 0; i < texels.size(); ++i)
            texels[i] = static_cast<Texel>(0x8000 | (i * 7919));
        const RvidFrame f = texelsToFrame(texels, 21, 13);
        check(f.width == 24 && f.height == 16, "frames pad up to a multiple of 8");
        const auto back = frameToTexels(f, 21, 13);
        check(back == texels, "texels survive the trip through the codec's planes");
    }

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

    int movies = 0, cvid = 0, rle = 0, other = 0, withAudio = 0;
    int badSamples = 0, badRanges = 0;
    int dumped = 0;
    std::uint64_t outBytes = 0;
    int converted = 0;

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
            const MovieInfo info = probeMovie(bytes, id, true);
            if (!info.ok)
                continue;
            const MovieTrack *v = info.video();
            if (v == nullptr)
                continue;

            ++movies;
            if (v->codec == "cvid")
                ++cvid;
            else if (v->codec == "rle ")
                ++rle;
            else
                ++other;
            if (info.audio() != nullptr)
                ++withAudio;

            // The demuxer's two promises: every sample the tables describe is
            // present, and none of them points outside the resource.
            if (v->samples.size() != v->sampleCount)
                ++badSamples;
            for (const auto &s : v->samples)
                if (s.offset + s.size > bytes.size())
                {
                    ++badRanges;
                    break;
                }

            // Decoding and converting every movie would be a full conversion
            // run; two per stack is enough to catch a codec that has stopped
            // working.
            if (i % std::max<std::size_t>(1, ids.size() / 2) != 0 || converted >= 16)
                continue;

            const fs::path out =
                outDir / (std::string(rivendata::stackName(stack.id)) + "-"
                          + std::to_string(id) + ".rvid");
            const VideoResult vr = convertMovie(set, id, out, 100);
            if (!vr.ok)
            {
                if (!vr.unsupported)
                    std::fprintf(stderr, "  %s\n", vr.error.c_str());
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
                || hdr.keyframeCount == 0)
            {
                check(false, "tMOV " + std::to_string(id) + " wrote a coherent .rvid");
                continue;
            }

            // Walk the frames the way a player would, and check the keyframe
            // index actually points at frame starts.
            std::size_t pos = sizeof(hdr)
                            + static_cast<std::size_t>(hdr.keyframeCount) * sizeof(RvidKeyframe);
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
                  "tMOV " + std::to_string(id) + ": the frames tile the file exactly");
            check(largest == hdr.largestFrameBytes,
                  "tMOV " + std::to_string(id) + ": largestFrameBytes is the real maximum");

            bool indexOk = true;
            for (std::uint32_t n = 0; n < hdr.keyframeCount; ++n)
            {
                RvidKeyframe kf{};
                std::memcpy(&kf, file.data() + sizeof(hdr) + n * sizeof(kf), sizeof(kf));
                indexOk = indexOk && kf.frame < hdr.frameCount
                          && kf.frame < starts.size() && starts[kf.frame] == kf.offset;
            }
            check(indexOk, "tMOV " + std::to_string(id) + ": the keyframe index points at frames");

            // Two frames out as PPM, so the result can be looked at.
            if (dumped < 3 && v->sampleCount > 20)
            {
                std::unique_ptr<VideoDecoder> dec;
                if (v->codec == "cvid")
                    dec.reset(new CinepakDecoder(v->width, v->height));
                else
                    dec.reset(new QtRleDecoder(v->width, v->height, v->depth, {}));

                RvidSettings settings;
                settings.profile = vr.profile;
                const int padW = (vr.width + 7) / 8 * 8;
                const int padH = (vr.height + 7) / 8 * 8;
                RvidDecoder rd(padW, padH, settings);

                bool ok = true;
                for (std::uint32_t n = 0; n < 20 && n < hdr.frameCount; ++n)
                {
                    RvidFrameHeader fh{};
                    std::memcpy(&fh, file.data() + starts[n], sizeof(fh));
                    const std::uint8_t *payload =
                        file.data() + starts[n] + sizeof(fh) + fh.audioBytes;
                    ok = ok
                         && rd.decode(payload, fh.byteCount - fh.audioBytes,
                                      fh.type
                                          == static_cast<std::uint8_t>(RvidFrameType::Intra));

                    if (n < v->samples.size())
                        dec->decode(bytes.data() + v->samples[n].offset,
                                    v->samples[n].size);
                }
                check(ok, "tMOV " + std::to_string(id) + ": 20 frames decode from the file");

                const auto srcTexels = downscaleToTexels(dec->rgb().data(), v->width,
                                                         v->height, vr.width, vr.height);
                const std::string base =
                    (outDir / (std::string(rivendata::stackName(stack.id)) + "-"
                               + std::to_string(id)))
                        .string();
                writePpm(base + "-source.ppm", srcTexels, vr.width, vr.height);
                writePpm(base + "-rvid.ppm", frameToTexels(rd.frame(), vr.width, vr.height),
                         vr.width, vr.height);
                ++dumped;
            }
        }
    }

    std::printf("video: %d movies -- %d cvid, %d rle, %d other, %d with audio\n", movies,
                cvid, rle, other, withAudio);
    check(movies > 0, "the install has movies");
    check(other == 0, "every movie uses a codec the converter decodes");
    check(badSamples == 0, "every video sample the tables describe was located");
    check(badRanges == 0, "no sample points outside its resource");

    if (converted > 0)
        std::printf("  converted %d movies, %llu bytes; frames in %s\n", converted,
                    static_cast<unsigned long long>(outBytes), outDir.string().c_str());

    std::printf("video: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
