#include "riven/SoundPipeline.hpp"

#include <algorithm>
#include <cstring>

#include "riven/AtomicWrite.hpp"

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    /// Decode the finished payload once to build the seek table. Doing it from
    /// the ENCODED nibbles rather than from the encoder's own state costs one
    /// cheap pass and makes the table true by construction: it is whatever the
    /// DS will actually have at that frame, whichever path produced the data.
    void buildSeekTable(RsndSource &src)
    {
        src.seek.clear();
        if (src.nibbles.empty() || src.sampleCount == 0)
            return;
        decodeIma(src.nibbles.data(), src.nibbles.size(), src.sampleCount, 1,
                  /*highNibbleFirst=*/false, {}, &src.seek, rivendata::kSeekInterval);
    }

    /// Mono PCM16 -> the payload fields of `src`, as PCM16 if it fits the budget
    /// and as ADPCM if it does not.
    ///
    /// This is the only place in the sound path that ever lost anything. The
    /// stereo sources have to be decoded and downmixed regardless -- SLST pans in
    /// hardware, so storing two channels and re-panning them would be worse --
    /// but re-encoding that downmix back to ADPCM was a second generation on top,
    /// and it bought only card space. Keeping the PCM is lossless.
    ///
    /// What it costs is RAM on the DS, four times over, and a hardware channel
    /// holds its whole sample while it plays. So the long ambient loops -- 157
    /// seconds is 6.9 MB as PCM16 -- stay ADPCM, where they are still audible.
    /// kPcm16Budget is the line, and it is shared so the runtime budgets against
    /// the same number.
    ///
    /// The loudness compressor lives HERE, at the funnel every PCM path reaches,
    /// for the same reason the master gain lives inside slotVolumes on the DS: a
    /// stage that has to be remembered at each call site is a stage that will be
    /// forgotten at the next one. src.sampleRate is set before every call.
    void encodeFromPcm(RsndSource &src, const std::vector<std::int16_t> &pcmIn,
                       bool compress)
    {
        const std::vector<std::int16_t> mono =
            compress ? compressMono(pcmIn, src.sampleRate) : pcmIn;

        src.sampleCount = static_cast<std::uint32_t>(mono.size());

        const std::uint64_t pcmBytes =
            static_cast<std::uint64_t>(mono.size()) * sizeof(std::int16_t);
        if (pcmBytes <= rivendata::kPcm16Budget)
        {
            src.codec = rivendata::SoundCodec::Pcm16;
            src.nibbles.resize(static_cast<std::size_t>(pcmBytes));
            // Both sides are little-endian, so the samples are the payload.
            if (!mono.empty())
                std::memcpy(src.nibbles.data(), mono.data(),
                            static_cast<std::size_t>(pcmBytes));
            // PCM is randomly accessible: there is no decoder state to resume, so
            // no seek table is written at all.
            src.seek.clear();
            return;
        }

        src.codec = rivendata::SoundCodec::ImaAdpcm;
        src.nibbles = encodeIma(mono.data(), mono.size());
        buildSeekTable(src);
    }

    /// Raw PCM as Mohawk stores it: unsigned at 8 bits, signed big-endian at 16
    /// (sound.cpp:242-254). Myst is full of these; Riven is not known to ship
    /// any, but the encoding field exists and costs twenty lines to honour.
    std::vector<std::int16_t> readRawPcm(const std::uint8_t *data, std::size_t bytes,
                                         int bits, int channels)
    {
        std::vector<std::int16_t> pcm;
        if (bits == 8)
        {
            pcm.resize(bytes);
            for (std::size_t i = 0; i < bytes; ++i)
                pcm[i] = static_cast<std::int16_t>((static_cast<int>(data[i]) - 128) << 8);
        }
        else if (bits == 16)
        {
            pcm.resize(bytes / 2);
            for (std::size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = static_cast<std::int16_t>((data[i * 2] << 8) | data[i * 2 + 1]);
        }
        return downmixToMonoWithMakeup(pcm, channels);
    }
} // namespace

std::vector<std::uint8_t> encodeRsnd(const RsndSource &src)
{
    std::vector<std::uint8_t> out;

    const bool adpcm = src.codec == rivendata::SoundCodec::ImaAdpcm;
    const std::size_t stateBytes = adpcm ? rivendata::kAdpcmStateBytes : 0;
    const std::size_t payloadBytes = stateBytes + src.nibbles.size();
    const std::size_t seekBytes = src.seek.size() * sizeof(rivendata::RsndSeekPoint);

    rivendata::RsndHeader hdr{};
    std::memcpy(hdr.magic, "RSND", 4);
    hdr.version = rivendata::kSoundVersion;
    hdr.sampleRate = src.sampleRate;
    hdr.sampleCount = src.sampleCount;
    hdr.dataBytes = static_cast<std::uint32_t>(payloadBytes);
    hdr.codec = static_cast<std::uint8_t>(src.codec);
    hdr.channels = 1;
    hdr.flags = src.loops ? rivendata::kSoundLoops : 0;
    hdr.loopStart = src.loopStart;
    hdr.loopEnd = src.loopEnd;
    hdr.seekInterval = src.seek.empty() ? 0 : rivendata::kSeekInterval;
    hdr.seekEntries = static_cast<std::uint16_t>(src.seek.size());

    out.resize(sizeof(hdr) + seekBytes + payloadBytes);
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    if (seekBytes > 0)
        std::memcpy(out.data() + sizeof(hdr), src.seek.data(), seekBytes);

    std::uint8_t *payload = out.data() + sizeof(hdr) + seekBytes;
    if (adpcm)
    {
        // The DS hardware reads its initial predictor and step index from the
        // first word of the sample, so writing it here means a short effect can
        // go from the card to a sound channel with no fixup at all.
        const rivendata::RsndSeekPoint start =
            src.seek.empty() ? rivendata::RsndSeekPoint{0, 0, 0} : src.seek.front();
        const std::uint32_t word = rivendata::makeAdpcmState(start.predictor, start.index);
        std::memcpy(payload, &word, sizeof(word));
        payload += sizeof(word);
    }
    if (!src.nibbles.empty())
        std::memcpy(payload, src.nibbles.data(), src.nibbles.size());

    return out;
}

SoundResult convertSound(const ArchiveSet &set, std::uint16_t id, const fs::path &out,
                         bool compress)
{
    const auto bytes = set.read("tWAV", id);
    if (bytes.empty())
    {
        SoundResult res;
        res.error = "tWAV " + std::to_string(id) + " could not be read";
        return res;
    }
    return convertSoundBytes(bytes, id, out, compress);
}

SoundResult convertSoundBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                              const fs::path &out, bool compress)
{
    SoundResult res;

    if (looksDamaged(bytes))
    {
        res.error = "tWAV " + std::to_string(id) + " is zero-filled in this copy of the game";
        return res;
    }

    ResourceReader r(bytes);
    const TwavInfo info = parseTwav(r);
    if (!info.ok)
    {
        res.error = "tWAV " + std::to_string(id) + ": " + info.error;
        res.unsupported = info.rawEncoding > 2;
        return res;
    }

    const std::uint8_t *data = bytes.data() + info.dataOffset;
    const std::size_t dataBytes = info.dataBytes;

    RsndSource src;
    src.sampleRate = info.sampleRate;

    switch (info.encoding)
    {
    case TwavEncoding::Adpcm:
        if (info.channels == 1 && !compress)
        {
            // The whole point of choosing IMA for the DS: the source already IS
            // IMA, one continuous stream from state {0,0}, so the samples pass
            // through untouched apart from the nibble order the DS expects.
            // Nothing is decoded and re-quantised, so nothing is lost.
            //
            // Only reachable with the compressor off. Compressing is a change to
            // the samples, so it cannot be done without decoding them, and a mono
            // sound left alone here while every other sound was lifted would be
            // the one thing worse than the extra ADPCM generation: a handful of
            // sounds 10 dB below the rest of the game.
            src.nibbles.assign(data, data + dataBytes);
            if (kMohawkHighNibbleFirst)
                swapNibbles(src.nibbles.data(), src.nibbles.size());
            src.sampleCount = static_cast<std::uint32_t>(
                std::min<std::size_t>(info.sampleCount, dataBytes * 2));
            buildSeekTable(src);
        }
        else
        {
            const auto decoded = decodeIma(data, dataBytes, info.sampleCount, info.channels,
                                           kMohawkHighNibbleFirst);
            encodeFromPcm(src, downmixToMonoWithMakeup(decoded, info.channels), compress);
        }
        break;

    case TwavEncoding::Pcm:
        if (info.bitsPerSample != 8 && info.bitsPerSample != 16)
        {
            res.error = "tWAV " + std::to_string(id) + ": "
                      + std::to_string(info.bitsPerSample) + "-bit PCM";
            res.unsupported = true;
            return res;
        }
        encodeFromPcm(src, readRawPcm(data, dataBytes, info.bitsPerSample, info.channels),
                      compress);
        break;

    case TwavEncoding::Mp2:
    {
        res.wasMpeg = true;
        int channels = 0;
        int rate = 0;
        std::string error;
        const auto pcm = decodeMpegAudio(data, dataBytes, channels, rate, error);
        if (pcm.empty())
        {
            res.error = "tWAV " + std::to_string(id) + ": " + error;
            res.unsupported = true;
            return res;
        }
        auto mono = downmixToMonoWithMakeup(pcm, channels);
        if (rate > kTargetRate)
        {
            mono = resampleMono(mono, rate, kTargetRate);
            rate = kTargetRate;
        }
        src.sampleRate = static_cast<std::uint16_t>(rate);
        encodeFromPcm(src, mono, compress);
        break;
    }
    }

    if (src.sampleCount == 0 || src.nibbles.empty())
    {
        res.error = "tWAV " + std::to_string(id) + " decoded to nothing";
        return res;
    }

    src.loops = info.loop != 0;
    src.loopStart = std::min(info.loopStart, src.sampleCount);
    src.loopEnd = std::min(info.loopEnd, src.sampleCount);

    const auto file = encodeRsnd(src);
    if (!writeFileAtomic(out, file, res.error))
        return res;

    res.bytes = file.size();
    res.ok = true;
    return res;
}

} // namespace riven
