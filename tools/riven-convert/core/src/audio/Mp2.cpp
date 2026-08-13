#include "riven/SoundPipeline.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

#include <minimp3/minimp3.h>

namespace riven
{
namespace
{
    // A Layer II frame is 1152 samples; a Layer I frame is 384. minimp3 writes
    // at most MINIMP3_MAX_SAMPLES_PER_FRAME (1152*2) values per call.
    constexpr int kMaxFrameSamples = MINIMP3_MAX_SAMPLES_PER_FRAME;

    // Riven's longest sounds are a couple of minutes; a stream that keeps
    // producing frames past this is a runaway parse of non-audio bytes.
    constexpr std::size_t kMaxOutputSamples = 30u * 1000 * 1000;
} // namespace

std::vector<std::int16_t> decodeMpegAudio(const std::uint8_t *data, std::size_t bytes,
                                          int &channels, int &sampleRate, std::string &error)
{
    std::vector<std::int16_t> pcm;
    channels = 0;
    sampleRate = 0;

    if (data == nullptr || bytes == 0)
    {
        error = "empty stream";
        return pcm;
    }

    mp3dec_t dec;
    mp3dec_init(&dec);

    std::int16_t frame[kMaxFrameSamples];
    std::size_t pos = 0;

    while (pos < bytes)
    {
        mp3dec_frame_info_t info{};
        const int remaining = static_cast<int>(
            std::min<std::size_t>(bytes - pos, static_cast<std::size_t>(INT32_MAX)));
        const int samples = mp3dec_decode_frame(&dec, data + pos, remaining, frame, &info);

        if (info.frame_bytes == 0)
            break; // no frame header found in what is left: end of stream

        pos += static_cast<std::size_t>(info.frame_bytes);

        if (samples == 0)
            continue; // ID3 tag, junk, or the first frame of a stream that needs two

        if (channels == 0)
        {
            channels = info.channels;
            sampleRate = info.hz;
        }
        else if (info.channels != channels || info.hz != sampleRate)
        {
            // Riven's sounds do not switch format mid-stream. If one does,
            // stopping at the switch is better than interleaving two formats.
            error = "stream changes format part way through";
            break;
        }

        const std::size_t count =
            static_cast<std::size_t>(samples) * static_cast<std::size_t>(info.channels);
        if (pcm.size() + count > kMaxOutputSamples)
        {
            error = "implausibly long stream";
            break;
        }
        pcm.insert(pcm.end(), frame, frame + count);
    }

    if (pcm.empty() && error.empty())
        error = "no MPEG audio frames found";
    if (channels < 1 || channels > 2 || sampleRate <= 0)
    {
        if (error.empty())
            error = "unusable MPEG audio format";
        pcm.clear();
    }

    return pcm;
}

std::vector<std::int16_t> downmixToMono(const std::vector<std::int16_t> &pcm, int channels)
{
    if (channels <= 1)
        return pcm;

    std::vector<std::int16_t> mono;
    const std::size_t frames = pcm.size() / static_cast<std::size_t>(channels);
    mono.resize(frames);
    for (std::size_t f = 0; f < frames; ++f)
    {
        std::int32_t sum = 0;
        for (int c = 0; c < channels; ++c)
            sum += pcm[f * static_cast<std::size_t>(channels) + static_cast<std::size_t>(c)];
        mono[f] = static_cast<std::int16_t>(sum / channels);
    }
    return mono;
}

std::vector<std::int16_t> downmixToMonoWithMakeup(const std::vector<std::int16_t> &pcm,
                                                  int channels)
{
    std::vector<std::int16_t> mono = downmixToMono(pcm, channels);
    if (channels <= 1 || mono.empty())
        return mono;

    const auto peakOf = [](const std::vector<std::int16_t> &v) {
        std::int32_t peak = 0;
        for (const std::int16_t s : v)
        {
            // Negated in 32 bits: -(-32768) does not fit in an int16_t.
            const std::int32_t mag = s < 0 ? -static_cast<std::int32_t>(s) : s;
            if (mag > peak)
                peak = mag;
        }
        return peak;
    };

    const std::int32_t peakMono = peakOf(mono);
    std::int32_t peakSrc = peakOf(pcm);
    if (peakMono == 0 || peakSrc <= peakMono)
        return mono; // silence, or an average that cancelled nothing

    // The ceiling. Without it, two channels that are near-exact opposites give a
    // mono peak of a handful of LSBs and a ratio in the thousands.
    if (peakSrc > peakMono * 2)
        peakSrc = peakMono * 2;

    for (std::int16_t &s : mono)
    {
        const std::int64_t num = static_cast<std::int64_t>(s) * peakSrc * 2;
        const std::int64_t bias = s < 0 ? -peakMono : peakMono; // round away from zero
        std::int64_t v = (num + bias) / (static_cast<std::int64_t>(peakMono) * 2);
        // peakSrc is a real sample magnitude and the scale is peakSrc/peakMono, so
        // the loudest sample lands exactly on it and cannot exceed 32767 -- except
        // for the one value 32768 a peak of -32768 would produce.
        if (v > 32767)
            v = 32767;
        else if (v < -32768)
            v = -32768;
        s = static_cast<std::int16_t>(v);
    }
    return mono;
}

std::vector<std::int16_t> resampleMono(const std::vector<std::int16_t> &pcm, int srcRate,
                                       int dstRate)
{
    if (srcRate == dstRate || srcRate <= 0 || dstRate <= 0 || pcm.size() < 2)
        return pcm;

    // Linear interpolation. It is the right amount of effort here: the only
    // sounds that reach this path are the DVD release's MP2 tracks going 44100
    // -> 22050, i.e. a 2:1 reduction whose alias products land above 11 kHz,
    // and the result is then quantised to 4-bit ADPCM anyway. A windowed-sinc
    // kernel would be inaudible under that.
    const std::size_t outCount = static_cast<std::size_t>(
        static_cast<std::int64_t>(pcm.size()) * dstRate / srcRate);
    std::vector<std::int16_t> out(outCount);

    for (std::size_t i = 0; i < outCount; ++i)
    {
        const double srcPos = static_cast<double>(i) * srcRate / dstRate;
        const std::size_t i0 = static_cast<std::size_t>(srcPos);
        const std::size_t i1 = std::min(i0 + 1, pcm.size() - 1);
        const double frac = srcPos - static_cast<double>(i0);
        const double v = pcm[i0] * (1.0 - frac) + pcm[i1] * frac;
        out[i] = static_cast<std::int16_t>(std::lround(v));
    }

    return out;
}

} // namespace riven
