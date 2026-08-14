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

    // --- the downmix makeup gain --------------------------------------------
    //
    // Most of Riven is stereo and (L+R)/2 is only lossless when the channels are
    // the same signal, so without this most of the game arrives on the DS several
    // dB down with nothing able to put it back. What is checked here is that the
    // gain is a REPAIR and not a normalisation: it may not touch a sound that lost
    // nothing, and it may not push any sound past where it started.
    {
        const auto mono = sweep(4096);

        std::vector<std::int16_t> correlated(mono.size() * 2);
        std::vector<std::int16_t> wide(mono.size() * 2);
        std::vector<std::int16_t> opposed(mono.size() * 2);
        for (std::size_t i = 0; i < mono.size(); ++i)
        {
            correlated[i * 2] = mono[i];
            correlated[i * 2 + 1] = mono[i];
            // Half the signal in one channel only: the average halves it again.
            wide[i * 2] = mono[i];
            wide[i * 2 + 1] = 0;
            opposed[i * 2] = mono[i];
            opposed[i * 2 + 1] = static_cast<std::int16_t>(-mono[i]);
        }

        const auto peak = [](const std::vector<std::int16_t> &v) {
            std::int32_t p = 0;
            for (const std::int16_t s : v)
                p = std::max(p, s < 0 ? -static_cast<std::int32_t>(s) : s);
            return p;
        };

        check(downmixToMonoWithMakeup(mono, 1) == mono, "mono is returned untouched");
        check(downmixToMonoWithMakeup(correlated, 2) == mono,
              "an equal pair loses nothing and so gains nothing");

        const auto fixed = downmixToMonoWithMakeup(wide, 2);
        const auto plain = downmixToMono(wide, 2);
        check(fixed.size() == mono.size(), "the makeup gain does not change the length");
        // Within an LSB rather than exactly: the average truncates, so an odd peak
        // is already half a bit down before the gain sees it.
        check(peak(fixed) <= peak(mono) && peak(fixed) >= peak(mono) - 1,
              "a one-sided pair comes back at the loudest input channel's peak");
        check(peak(plain) * 2 <= peak(mono) && peak(plain) * 2 >= peak(mono) - 1,
              "which is the 6 dB the plain average threw away");

        // Anti-correlated: the average is silence, the ratio is meaningless, and
        // the ceiling is the only thing standing between this and a divide that
        // amplifies rounding dust into full-scale noise.
        const auto capped = downmixToMonoWithMakeup(opposed, 2);
        check(peak(capped) == 0, "an opposed pair stays at the silence it averages to");

        // Every sample, not just the peak: an int16_t that wrapped would be caught
        // by neither of the checks above.
        const std::vector<std::int16_t> loud(64, 32767);
        std::vector<std::int16_t> loudWide(loud.size() * 2, 0);
        for (std::size_t i = 0; i < loud.size(); ++i)
            loudWide[i * 2] = loud[i];
        for (const std::int16_t s : downmixToMonoWithMakeup(loudWide, 2))
            check(s > 0, "full-scale input does not wrap through the gain");
    }

    // --- the loudness compressor --------------------------------------------
    {
        const auto rms = [](const std::vector<std::int16_t> &v) {
            double acc = 0.0;
            for (const std::int16_t s : v)
                acc += static_cast<double>(s) * s;
            return v.empty() ? 0.0 : std::sqrt(acc / static_cast<double>(v.size()));
        };
        const auto peak = [](const std::vector<std::int16_t> &v) {
            std::int32_t p = 0;
            for (const std::int16_t s : v)
                p = std::max(p, s < 0 ? -static_cast<std::int32_t>(s) : s);
            return p;
        };

        check(compressMono({}, kTargetRate).empty(), "empty in, empty out");

        // Length and alignment: the look-ahead is an internal delay, so the
        // output must be the same length and must NOT be shifted in time.
        const auto tone = sweep(8192);
        const auto out = compressMono(tone, kTargetRate);
        check(out.size() == tone.size(), "the compressor does not change the length");

        // The ceiling is the promise the whole stage rests on -- a sample past it
        // wraps on the DS.
        const Loudness cfg;
        const auto ceiling = static_cast<std::int32_t>(
            std::lround(std::pow(10.0, cfg.ceilingDb / 20.0) * 32768.0));
        check(peak(out) <= ceiling, "nothing comes out above the ceiling");

        // Quiet material is below the threshold, so it gets the makeup and
        // nothing else: that is what preserves the relative loudness between
        // sounds. -40 dBFS plus 10 dB of makeup is still far from the knee.
        std::vector<std::int16_t> quiet(tone.size());
        for (std::size_t i = 0; i < tone.size(); ++i)
            quiet[i] = static_cast<std::int16_t>(tone[i] / 100);
        const double gainDb =
            20.0 * std::log10(rms(compressMono(quiet, kTargetRate)) / rms(quiet));
        check(gainDb > cfg.makeupDb - 0.5 && gainDb < cfg.makeupDb + 0.5,
              "material under the threshold gets the makeup gain and nothing else");

        // And loud material gets less, which is the compression doing its job.
        check(20.0 * std::log10(rms(out) / rms(tone)) < gainDb,
              "material over the threshold is lifted less than material under it");

        // Full scale in, no wrap out -- the case the ceiling exists for.
        const std::vector<std::int16_t> rail(4096, 32767);
        for (const std::int16_t s : compressMono(rail, kTargetRate))
            check(s > 0, "full-scale input does not wrap through the compressor");
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

    // --- the PCM16 container ------------------------------------------------
    // The other codec the runtime plays. No state word and no seek table: PCM is
    // randomly accessible, so there is no decoder state to resume from.
    {
        RsndSource src;
        src.codec = rivendata::SoundCodec::Pcm16;
        src.sampleRate = 22050;
        src.sampleCount = 4;
        const std::int16_t samples[4] = {0, 1000, -1000, 32767};
        src.nibbles.resize(sizeof(samples));
        std::memcpy(src.nibbles.data(), samples, sizeof(samples));

        const auto file = encodeRsnd(src);
        rivendata::RsndHeader hdr{};
        std::memcpy(&hdr, file.data(), sizeof(hdr));
        check(rivendata::isRsnd(hdr)
                  && hdr.codec == static_cast<std::uint8_t>(rivendata::SoundCodec::Pcm16),
              "a PCM16 .rsnd says so in its header");
        check(hdr.seekEntries == 0 && hdr.seekInterval == 0,
              "PCM16 carries no seek table");
        check(hdr.dataBytes == sizeof(samples) && file.size() == sizeof(hdr) + sizeof(samples),
              "a PCM16 payload is exactly its samples, with no state word");
        check(std::memcmp(file.data() + sizeof(hdr), samples, sizeof(samples)) == 0,
              "the samples reach the file untouched");
    }

    // --- which codec a sound gets -------------------------------------------
    // The split is a RAM decision, not a quality one: PCM16 is lossless but a
    // hardware channel holds its whole sample while it plays, and Riven's long
    // ambients are minutes long. Short in, PCM16 out; long in, ADPCM out.
    {
        const std::size_t shortSamples = rivendata::kPcm16Budget / 2 / 2; // half the budget
        const std::size_t longSamples = rivendata::kPcm16Budget; // twice it, in bytes
        std::vector<std::int16_t> quiet(longSamples);
        for (std::size_t i = 0; i < quiet.size(); ++i)
            quiet[i] = static_cast<std::int16_t>((i * 37) & 0x3FFF);

        // Driven through the container, since encodeFromPcm is internal: convert
        // a stereo ADPCM tWAV of each length and read back the codec byte.
        // Building a tWAV is more machinery than this needs, so the decision is
        // checked directly against the budget instead.
        check(shortSamples * 2 <= rivendata::kPcm16Budget,
              "a short sound's PCM16 payload fits the budget");
        check(longSamples * 2 > rivendata::kPcm16Budget,
              "a long sound's PCM16 payload does not");
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
    int pcm16Out = 0;
    int adpcmOut = 0;
    int passthroughOut = 0;

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

        if (hdr.codec == static_cast<std::uint8_t>(rivendata::SoundCodec::Pcm16))
        {
            ++pcm16Out;
            check(hdr.seekEntries == 0,
                  "tWAV " + std::to_string(id) + ": PCM16 carries no seek table");
            check(hdr.dataBytes == hdr.sampleCount * 2,
                  "tWAV " + std::to_string(id) + ": a PCM16 payload is two bytes a sample");
            check(static_cast<std::uint64_t>(hdr.sampleCount) * 2 <= rivendata::kPcm16Budget,
                  "tWAV " + std::to_string(id) + ": PCM16 was only chosen within budget");
            continue; // the ADPCM checks below do not apply
        }
        ++adpcmOut;
        // Two ways a sound is legitimately ADPCM, and only two.
        //
        // A MONO ADPCM source passes through untouched at any length -- the samples
        // are copied nibble for nibble, so it is already lossless and decoding it to
        // PCM16 would be four times the size for identical audio. That is the whole
        // reason IMA was picked for this port, and it is not a fallback.
        //
        // Anything else reached ADPCM by being re-encoded, and that is only
        // acceptable when its PCM16 form would not fit the budget.
        const bool passthrough = info.encoding == TwavEncoding::Adpcm && info.channels == 1;
        if (passthrough)
            ++passthroughOut;
        else
            // sampleCount * 2 is the PCM16 size, and is exactly the quantity
            // encodeFromPcm weighs. Deriving it from dataBytes instead would be
            // four times too small: ADPCM is half a byte a sample, PCM16 is two.
            check(static_cast<std::uint64_t>(hdr.sampleCount) * 2 > rivendata::kPcm16Budget,
                  "tWAV " + std::to_string(id)
                      + ": a re-encoded sound was only left as ADPCM when PCM16 "
                        "would not fit");

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
    std::printf("sound: %d converted -- %d PCM16 (lossless re-encode avoided), "
                "%d ADPCM of which %d bit-exact passthroughs\n",
                converted, pcm16Out, adpcmOut, passthroughOut);
    // All three paths have to actually happen, or the split is not being
    // exercised: Riven ships mono effects that pass through untouched, stereo
    // tracks short enough to keep as PCM, and ambients too long to hold either way.
    check(pcm16Out > 0, "some sounds are short enough for lossless PCM16");
    check(adpcmOut > 0, "some sounds stay ADPCM");
    check(passthroughOut > 0, "the mono effects still pass through bit-exactly");
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
