#include "riven/SoundPipeline.hpp"

namespace riven
{
namespace
{
    // A tWAV is a nested Mohawk form: "MHWK" u32(size) "WAVE", then tagged
    // chunks. Three exist: Cue# (named cue points), ADPC (periodic ADPCM state
    // for seeking) and Data (the header below plus the samples). Riven uses the
    // cue points in exactly one sound and ScummVM ignores them (sound.cpp:138-140),
    // so Cue# is skipped outright.
    //
    // ADPC is only located, not used: the converter rebuilds an equivalent seek
    // table from the samples so that a copy of the game with a missing or
    // damaged ADPC still converts. It is located because it is the game's own
    // record of what a decoder's state should be at a given frame, which makes
    // it the one piece of ground truth available for testing a decoder against.
    constexpr std::size_t kDataHeaderBytes = 20;

    // No Riven sound is anywhere near a minute; a Data chunk claiming tens of
    // millions of samples is a misparse, not a long sound.
    constexpr std::uint32_t kMaxSamples = 30u * 1000 * 1000;

    // Chunk headers are tag + u32 size. A resource with more chunks than this
    // before Data is not a tWAV.
    constexpr int kMaxChunks = 16;
} // namespace

TwavInfo parseTwav(ResourceReader &r)
{
    TwavInfo info;

    if (r.tag() != "MHWK")
    {
        info.error = "not a Mohawk form";
        return info;
    }
    r.u32(); // form size: not trusted, the resource length is authoritative
    if (r.tag() != "WAVE")
    {
        info.error = "not a WAVE form";
        return info;
    }

    bool haveData = false;
    std::uint32_t dataChunkSize = 0;
    for (int i = 0; i < kMaxChunks && !haveData; ++i)
    {
        const std::string type = r.tag();
        const std::uint32_t size = r.u32();
        if (!r.ok())
            break;

        if (type == "Data")
        {
            haveData = true;
            dataChunkSize = size;
            break;
        }
        if (type != "ADPC" && type != "Cue#")
        {
            info.error = "unknown chunk '" + type + "'";
            return info;
        }
        if (type == "ADPC")
        {
            info.adpcOffset = r.pos();
            info.adpcBytes = size;
        }
        r.skip(size);
    }

    if (!haveData)
    {
        info.error = r.ok() ? "no Data chunk" : "truncated";
        return info;
    }

    info.sampleRate = r.u16();
    info.sampleCount = r.u32();
    info.bitsPerSample = r.u8();
    info.channels = r.u8();
    info.rawEncoding = r.u16();
    info.loop = r.u16();
    info.loopStart = r.u32();
    info.loopEnd = r.u32();
    if (!r.ok())
    {
        info.error = "Data header is truncated";
        return info;
    }

    info.dataOffset = r.pos();

    // The Data chunk's size counts its own tag and length words and the 20-byte
    // header as well as the samples, which is why libvaht calls it a liar
    // (vaht_wav.c:93-95) and then ignores it. It is worth honouring: a Riven
    // tWAV has bytes AFTER the samples -- 4410 of them in ospit 0, 152720 in
    // ospit 2 -- and reading to the end of the resource would treat those as
    // audio. Checked against the other derivation available: size - 28 equals
    // sampleCount * channels / 2 exactly on every sound in a 5-CD install.
    //
    // A size that does not fit the resource is not trusted; falling back to
    // "everything that is left" is what a damaged copy of the game needs.
    info.dataBytes = r.remaining();
    if (dataChunkSize > kDataHeaderBytes + 8)
    {
        const std::size_t declared = dataChunkSize - kDataHeaderBytes - 8;
        if (declared <= info.dataBytes)
            info.dataBytes = declared;
    }

    if (info.rawEncoding > 2)
    {
        info.error = "unknown encoding " + std::to_string(info.rawEncoding);
        return info;
    }
    info.encoding = static_cast<TwavEncoding>(info.rawEncoding);

    if (info.channels < 1 || info.channels > 2)
    {
        info.error = "unsupported channel count " + std::to_string(info.channels);
        return info;
    }
    if (info.sampleRate == 0)
    {
        info.error = "sample rate is zero";
        return info;
    }
    if (info.sampleCount > kMaxSamples)
    {
        info.error = "implausible sample count " + std::to_string(info.sampleCount);
        return info;
    }
    if (info.dataBytes == 0)
    {
        info.error = "no sample data";
        return info;
    }

    info.ok = true;
    return info;
}

std::vector<TwavAdpcPoint> parseAdpc(ResourceReader &r, const TwavInfo &info)
{
    std::vector<TwavAdpcPoint> points;
    if (info.adpcBytes == 0)
        return points;

    r.seek(info.adpcOffset);
    const std::uint16_t count = r.u16();
    const std::uint16_t channels = r.u16();
    if (!r.ok() || channels < 1 || channels > 2)
        return points;

    // 4 bytes of frame number plus 4 per channel.
    const std::size_t entryBytes = 4 + 4u * channels;
    if (static_cast<std::size_t>(count) * entryBytes + 4 > info.adpcBytes)
        return points;

    points.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        TwavAdpcPoint p;
        p.frame = r.u32();
        for (int c = 0; c < channels; ++c)
        {
            p.state[c].predictor = r.i16();
            p.state[c].index = static_cast<std::uint8_t>(r.u16());
        }
        if (!r.ok())
            break;
        points.push_back(p);
    }
    return points;
}

} // namespace riven
