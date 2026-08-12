// Sound pipeline: the IMA codec, the seek table, the .rsnd container, and --
// with RIVEN_TEST_DATA -- the two questions only real game data can answer.
//
// The first is which nibble of a byte holds the earlier sample. ScummVM and
// libvaht disagree (see SoundPipeline.hpp), and for a mono continuous stream
// the wrong choice is not a subtle degradation, it is noise. The measurement
// below is the tiebreak: correctly decoded speech and ambience move smoothly
// from sample to sample, and a stream decoded with the nibbles swapped does
// not. It is checked per sound, not on average, so one lucky file cannot
// carry the result.
//
// The second is whether our IMA arithmetic matches an independent
// implementation. libvaht's decoder is that oracle: given the same nibble
// order it must agree sample for sample, or one of us has the tables, the
// clamping or the update order wrong.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
#include <vaht/vaht.h>
}

#include "RivenSound.hpp"
#include "riven/Archive.hpp"
#include "riven/Layout.hpp"
#include "riven/SoundPipeline.hpp"

namespace fs = std::filesystem;
using namespace riven;

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

    /// A swept sine with an envelope: covers quiet passages (where the step
    /// index has to walk down) and loud ones (where it has to walk up), which
    /// is where an ADPCM codec with a wrong index update goes audibly wrong.
    std::vector<std::int16_t> sweep(std::size_t count)
    {
        std::vector<std::int16_t> pcm(count);
        double phase = 0.0;
        for (std::size_t i = 0; i < count; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(count);
            phase += 0.01 + 0.2 * t;
            const double env = 0.05 + 0.9 * t;
            pcm[i] = static_cast<std::int16_t>(std::lround(std::sin(phase) * env * 30000.0));
        }
        return pcm;
    }

    double rms(const std::vector<std::int16_t> &v)
    {
        if (v.empty())
            return 0.0;
        double sum = 0.0;
        for (const auto s : v)
            sum += static_cast<double>(s) * s;
        return std::sqrt(sum / static_cast<double>(v.size()));
    }

    double errorRms(const std::vector<std::int16_t> &a, const std::vector<std::int16_t> &b)
    {
        const std::size_t n = std::min(a.size(), b.size());
        if (n == 0)
            return 0.0;
        double sum = 0.0;
        for (std::size_t i = 0; i < n; ++i)
        {
            const double d = static_cast<double>(a[i]) - b[i];
            sum += d * d;
        }
        return std::sqrt(sum / static_cast<double>(n));
    }

    /// |mean| / RMS. Audio has no DC component, so this is ~0 for a correct
    /// decode. An IMA stream decoded with its nibbles transposed applies each
    /// delta to a predictor whose step size was chosen for the other sample,
    /// and the error integrates into an offset of thousands -- which is what
    /// this measures, and what makes it a decisive test rather than a
    /// suggestive one.
    double dcRatio(const std::vector<std::int16_t> &pcm)
    {
        if (pcm.empty())
            return 0.0;
        double sum = 0.0;
        for (const auto s : pcm)
            sum += s;
        const double level = rms(pcm);
        return level > 1.0 ? std::fabs(sum / static_cast<double>(pcm.size())) / level : 0.0;
    }

    void writeWav(const fs::path &path, const std::vector<std::int16_t> &pcm, int rate)
    {
        FILE *f = std::fopen(path.string().c_str(), "wb");
        if (f == nullptr)
            return;
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(pcm.size() * 2);
        const std::uint32_t riffSize = 36 + dataBytes;
        const std::uint32_t fmtSize = 16;
        const std::uint16_t one = 1;
        const std::uint16_t bits = 16;
        const std::uint32_t rate32 = static_cast<std::uint32_t>(rate);
        const std::uint32_t byteRate = rate32 * 2;
        const std::uint16_t blockAlign = 2;

        std::fwrite("RIFF", 1, 4, f);
        std::fwrite(&riffSize, 4, 1, f);
        std::fwrite("WAVEfmt ", 1, 8, f);
        std::fwrite(&fmtSize, 4, 1, f);
        std::fwrite(&one, 2, 1, f);       // PCM
        std::fwrite(&one, 2, 1, f);       // mono
        std::fwrite(&rate32, 4, 1, f);
        std::fwrite(&byteRate, 4, 1, f);
        std::fwrite(&blockAlign, 2, 1, f);
        std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 1, 4, f);
        std::fwrite(&dataBytes, 4, 1, f);
        std::fwrite(pcm.data(), 1, dataBytes, f);
        std::fclose(f);
    }

    /// Decode a whole tWAV through libvaht, for the cross-check.
    std::vector<std::int16_t> decodeThroughVaht(const Archive &archive, std::uint16_t id,
                                                int &channels)
    {
        std::vector<std::int16_t> pcm;
        channels = 0;

        vaht_resource *res = vaht_resource_open(archive.raw(), "tWAV", id);
        if (res == nullptr)
            return pcm;
        vaht_wav *wav = vaht_wav_open(res);
        vaht_resource_close(res);
        if (wav == nullptr)
            return pcm;

        channels = vaht_wav_channels(wav);
        const std::uint32_t frames = vaht_wav_samplecount(wav);
        // vaht_wav_read wants a multiple of 4 and treats `size` as the buffer
        // capacity, writing half of it (vaht_wav.h:131-134).
        std::size_t cap = static_cast<std::size_t>(frames) * channels * 2;
        cap = (cap + 3) & ~std::size_t(3);
        pcm.resize(cap / 2 + 4);
        const std::uint32_t got = vaht_wav_read(wav, static_cast<std::uint32_t>(cap * 2),
                                                pcm.data());
        pcm.resize(got / 2);
        vaht_wav_close(wav);
        return pcm;
    }
} // namespace

int main()
{
    // --- IMA codec, synthetic ----------------------------------------------
    {
        const auto pcm = sweep(20000);
        const auto nibbles = encodeIma(pcm.data(), pcm.size());
        check(nibbles.size() == (pcm.size() + 1) / 2, "encodeIma packs two samples per byte");

        const auto back = decodeIma(nibbles.data(), nibbles.size(), pcm.size(), 1, false);
        check(back.size() == pcm.size(), "decodeIma returns the frames asked for");

        // 4-bit ADPCM on a signal this dynamic sits well under a tenth of the
        // signal's own level; anything near it means the quantiser or the step
        // update is wrong.
        const double ratio = errorRms(pcm, back) / rms(pcm);
        check(ratio < 0.10, "IMA round-trip error is under 10% RMS");
        if (ratio >= 0.10)
            std::fprintf(stderr, "  (error was %.1f%%)\n", ratio * 100.0);

        // Silence must survive exactly: the predictor starts at 0, so an
        // all-zero input has to come back all zero or the codec has a bias.
        const std::vector<std::int16_t> quiet(4096, 0);
        const auto quietBack = decodeIma(encodeIma(quiet.data(), quiet.size()).data(),
                                         quiet.size() / 2, quiet.size(), 1, false);
        bool allZero = true;
        for (const auto s : quietBack)
            allZero = allZero && s == 0;
        check(allZero, "silence encodes and decodes to silence");
    }

    // --- nibble order -------------------------------------------------------
    {
        auto data = encodeIma(sweep(4096).data(), 4096);
        const auto original = data;

        swapNibbles(data.data(), data.size());
        check(data != original, "swapNibbles changes the bytes");
        swapNibbles(data.data(), data.size());
        check(data == original, "swapNibbles is its own inverse");

        auto swapped = original;
        swapNibbles(swapped.data(), swapped.size());
        const auto lowFirst = decodeIma(original.data(), original.size(), 4096, 1, false);
        const auto highFirstOfSwapped =
            decodeIma(swapped.data(), swapped.size(), 4096, 1, true);
        check(lowFirst == highFirstOfSwapped,
              "swapping the nibbles is the same as swapping the decode order");
    }

    // --- seek table ---------------------------------------------------------
    {
        const auto pcm = sweep(10000);
        const auto nibbles = encodeIma(pcm.data(), pcm.size());

        std::vector<rivendata::RsndSeekPoint> seek;
        const unsigned interval = 512;
        const auto full = decodeIma(nibbles.data(), nibbles.size(), pcm.size(), 1, false, {},
                                    &seek, interval);
        check(seek.size() == (pcm.size() + interval - 1) / interval,
              "one seek point per interval");
        check(!seek.empty() && seek.front().predictor == 0 && seek.front().index == 0,
              "the first seek point is the stream's starting state");

        bool allMatch = true;
        for (std::size_t k = 1; k < seek.size(); ++k)
        {
            const std::size_t frame = k * interval;
            const std::size_t byteOffset = frame / 2;
            const std::size_t want = std::min<std::size_t>(1000, pcm.size() - frame);
            const ImaState state{seek[k].predictor, seek[k].index};
            const auto tail = decodeIma(nibbles.data() + byteOffset,
                                        nibbles.size() - byteOffset, want, 1, false, state);
            for (std::size_t i = 0; i < want; ++i)
                allMatch = allMatch && tail[i] == full[frame + i];
        }
        check(allMatch, "decoding from a seek point matches decoding from the start");
    }

    // --- the container ------------------------------------------------------
    {
        RsndSource src;
        src.codec = rivendata::SoundCodec::ImaAdpcm;
        src.sampleRate = 22050;
        src.sampleCount = 8;
        src.nibbles = {0x11, 0x22, 0x33, 0x44};
        src.seek = {{123, 7, 0}};
        src.loops = true;
        src.loopStart = 2;
        src.loopEnd = 6;

        const auto file = encodeRsnd(src);
        check(file.size() == sizeof(rivendata::RsndHeader)
                                 + sizeof(rivendata::RsndSeekPoint) + 4 + src.nibbles.size(),
              "an .rsnd is header + seek table + state word + nibbles");

        rivendata::RsndHeader hdr{};
        std::memcpy(&hdr, file.data(), sizeof(hdr));
        check(rivendata::isRsnd(hdr), "the header identifies itself");
        check(hdr.sampleRate == 22050 && hdr.sampleCount == 8 && hdr.channels == 1,
              "the header round-trips its fields");
        check(hdr.flags == rivendata::kSoundLoops && hdr.loopStart == 2 && hdr.loopEnd == 6,
              "loop information round-trips");
        check(hdr.seekEntries == 1 && hdr.seekInterval == rivendata::kSeekInterval,
              "the seek table is described");
        check(hdr.dataBytes == 4 + src.nibbles.size(),
              "dataBytes counts the state word with the payload");

        // The DS hardware reads predictor and step index out of this word.
        std::uint32_t state = 0;
        std::memcpy(&state, file.data() + sizeof(hdr) + sizeof(rivendata::RsndSeekPoint), 4);
        check(state == rivendata::makeAdpcmState(123, 7),
              "the payload starts with the DS ADPCM state word");
    }

    // --- real data ----------------------------------------------------------
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("sound: %d checks, %d failed "
                    "(RIVEN_TEST_DATA unset: skipped the real-data checks)\n",
                    g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }

    const Source source = detectSource(dataEnv);
    if (!source.valid())
    {
        std::printf("sound: skipped (no Riven data under '%s')\n", dataEnv);
        return g_failures == 0 ? 0 : 1;
    }

    int adpcm = 0, pcm = 0, mp2 = 0, unparsed = 0;
    int monoSounds = 0, stereoSounds = 0;
    int orderAgreed = 0, orderTotal = 0;
    int adpcTemplate = 0, adpcTotal = 0, adpcExtra = 0;
    int vahtChecked = 0, vahtAgreed = 0;
    std::uint64_t totalOut = 0;
    int converted = 0;
    int dumped = 0;

    const fs::path outDir = fs::temp_directory_path() / "riven-test-sound";
    std::error_code ec;
    fs::remove_all(outDir, ec);
    fs::create_directories(outDir, ec);

    // Every stack, every sound: the header work is cheap, and the mono sounds
    // that settle the nibble order are only a seventh of the game and are not
    // evenly spread across the stacks. The expensive parts -- decoding and
    // converting a multi-megabyte stereo ambient -- are rationed below.
    for (const auto &stack : source.stacks)
    {
    ArchiveSet set;
    std::vector<std::string> failures;
    set.openAll(stack.dataArchives, failures);
    set.openAll(stack.soundArchives, failures);
    const auto ids = set.resourceIds("tWAV");
    int stereoConverted = 0;

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        const std::uint16_t id = ids[i];
        const auto bytes = set.read("tWAV", id);
        if (bytes.empty() || looksDamaged(bytes))
            continue;

        ResourceReader r(bytes);
        const TwavInfo info = parseTwav(r);
        if (!info.ok)
        {
            ++unparsed;
            std::fprintf(stderr, "  tWAV %u: %s\n", id, info.error.c_str());
            continue;
        }
        if (info.encoding == TwavEncoding::Adpcm)
            ++adpcm;
        else if (info.encoding == TwavEncoding::Pcm)
            ++pcm;
        else
            ++mp2;
        if (info.channels == 1)
            ++monoSounds;
        else
            ++stereoSounds;

        if (info.encoding == TwavEncoding::Adpcm && info.channels == 1)
        {
            const std::uint8_t *data = bytes.data() + info.dataOffset;
            const std::size_t frames =
                std::min<std::size_t>(info.sampleCount, info.dataBytes * 2);

            const auto high = decodeIma(data, info.dataBytes, frames, 1, true);
            const auto low = decodeIma(data, info.dataBytes, frames, 1, false);

            // Only mono can answer this. A stereo stream alternates channels
            // nibble by nibble, so swapping the order there just relabels left
            // and right and both decodes are equally clean -- which is exactly
            // why two published decoders get it wrong and nobody notices.
            if (rms(high) > 300.0 || rms(low) > 300.0)
            {
                ++orderTotal;
                const double dcHigh = dcRatio(high);
                const double dcLow = dcRatio(low);
                const bool highWins = dcHigh < dcLow;
                if (highWins == kMohawkHighNibbleFirst)
                    ++orderAgreed;
                else
                    std::fprintf(stderr,
                                 "  tWAV %u prefers the other nibble order "
                                 "(DC high %.3f, low %.3f)\n",
                                 id, dcHigh, dcLow);
            }

            // Why the converter rebuilds its own seek table rather than using
            // ADPC: every entry Riven ships describes frame 0 with step index
            // 0, i.e. the state a decoder is in before it has done anything. A
            // handful of sounds carry a second entry at the very end of the
            // stream, and no decode from frame 0 -- either nibble order, either
            // of the two IMA delta formulations -- reproduces its predictor, so
            // it cannot be treated as a state either. If a copy of the game
            // ever turns up with real seek points in here, this is where it
            // shows up.
            const auto points = parseAdpc(r, info);
            if (!points.empty())
            {
                ++adpcTotal;
                if (points.front().frame == 0 && points.front().state[0].index == 0)
                    ++adpcTemplate;
                if (points.size() > 1)
                    ++adpcExtra;
            }

            // libvaht decodes low-nibble-first, which the measurement above
            // says is the wrong way round -- but it is still an independent
            // implementation of the same arithmetic, so given the same nibble
            // order it must agree with us sample for sample. This checks the
            // tables, the clamping and the update order, not the nibble order.
            if (const Archive *owner = set.find("tWAV", id); owner != nullptr)
            {
                int vahtChannels = 0;
                const auto ref = decodeThroughVaht(*owner, id, vahtChannels);
                if (!ref.empty() && vahtChannels == 1)
                {
                    ++vahtChecked;
                    const std::size_t n = std::min(ref.size(), low.size());
                    bool same = n > 0;
                    for (std::size_t k = 0; k < n; ++k)
                        same = same && ref[k] == low[k];
                    if (same)
                        ++vahtAgreed;
                    else
                        std::fprintf(stderr, "  tWAV %u disagrees with libvaht\n", id);
                }
            }
        }

        // Convert every mono sound, and two stereo ones per stack. A stereo
        // ambient is megabytes that have to be decoded, downmixed and
        // re-encoded, and doing all 270 of them would turn a unit test into a
        // full conversion run.
        if (info.channels != 1)
        {
            if (stereoConverted >= 2)
                continue;
            ++stereoConverted;
        }

        const fs::path outFile =
            outDir / (std::string(rivendata::stackName(stack.id)) + "-"
                      + std::to_string(id) + ".rsnd");
        const SoundResult res = convertSound(set, id, outFile);
        if (!res.ok)
        {
            std::fprintf(stderr, "  %s\n", res.error.c_str());
            continue;
        }
        ++converted;
        totalOut += res.bytes;

        // Re-read what was written, exactly as the DS will.
        std::vector<std::uint8_t> file(res.bytes);
        FILE *f = std::fopen(outFile.string().c_str(), "rb");
        const std::size_t got = f != nullptr ? std::fread(file.data(), 1, file.size(), f) : 0;
        if (f != nullptr)
            std::fclose(f);
        if (got != file.size() || got < sizeof(rivendata::RsndHeader))
        {
            check(false, "the written .rsnd reads back whole");
            continue;
        }

        rivendata::RsndHeader hdr{};
        std::memcpy(&hdr, file.data(), sizeof(hdr));
        if (!rivendata::isRsnd(hdr) || hdr.channels != 1 || hdr.sampleRate == 0
            || hdr.dataBytes == 0
            || file.size() != sizeof(hdr)
                                  + hdr.seekEntries * sizeof(rivendata::RsndSeekPoint)
                                  + hdr.dataBytes)
        {
            check(false, "tWAV " + std::to_string(id) + " wrote a coherent .rsnd");
            continue;
        }

        // Three dumps so the result can be listened to, which is the only check
        // a person can make on audio.
        if (dumped < 3 && hdr.sampleCount > 4096)
        {
            const std::uint8_t *payload = file.data() + sizeof(hdr)
                                        + hdr.seekEntries * sizeof(rivendata::RsndSeekPoint)
                                        + rivendata::kAdpcmStateBytes;
            const auto decoded =
                decodeIma(payload, hdr.dataBytes - rivendata::kAdpcmStateBytes,
                          hdr.sampleCount, 1, false);
            writeWav(outFile.string().substr(0, outFile.string().size() - 5) + ".wav",
                     decoded, hdr.sampleRate);
            ++dumped;
        }
    }
    } // per stack

    std::printf("  encodings: %d ADPCM, %d PCM, %d MPEG, %d unparsed\n", adpcm, pcm, mp2,
                unparsed);
    check(adpcm + pcm + mp2 > 0, "the install has sounds");
    check(unparsed == 0, "every tWAV parsed");
    check(converted > 0, "sounds converted");

    std::printf("  channels: %d mono, %d stereo\n", monoSounds, stereoSounds);

    if (orderTotal > 0)
    {
        std::printf("  nibble order: %d/%d mono sounds decode cleanly %s-nibble-first\n",
                    orderAgreed, orderTotal, kMohawkHighNibbleFirst ? "high" : "low");
        check(orderAgreed == orderTotal,
              "every mono sound agrees with the nibble order the converter ships");
    }
    else
    {
        std::printf("  nibble order: no mono sounds in this sample to measure\n");
    }

    if (adpcTotal > 0)
    {
        std::printf("  ADPC: %d/%d start at the frame-0 template, %d carry a trailing "
                    "entry\n",
                    adpcTemplate, adpcTotal, adpcExtra);
        check(adpcTemplate == adpcTotal,
              "ADPC carries no usable seek points, so rebuilding the table is right");
    }

    if (vahtChecked > 0)
    {
        std::printf("  libvaht cross-check: %d/%d identical\n", vahtAgreed, vahtChecked);
        check(vahtAgreed == vahtChecked, "our IMA decoder matches libvaht's");
    }
    else
    {
        std::printf("  libvaht cross-check: no sounds libvaht would open\n");
    }

    if (converted > 0)
        std::printf("  wrote %d sounds, %llu bytes, %llu bytes each on average\n", converted,
                    static_cast<unsigned long long>(totalOut),
                    static_cast<unsigned long long>(totalOut / converted));
    if (dumped > 0)
        std::printf("  listen: %s\n", outDir.string().c_str());

    std::printf("sound: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
