#include "riven/SoundPipeline.hpp"

#include <algorithm>

namespace riven
{
namespace
{
    /// One decode step. Shared by the decoder and the encoder, because an ADPCM
    /// encoder MUST track the decoder exactly: it predicts from what the
    /// decoder will have, not from the original samples. Two copies of this
    /// arithmetic that drift apart produce audio that degrades over the length
    /// of the sound rather than failing visibly.
    inline std::int16_t imaStep(ImaState &s, std::uint8_t nibble)
    {
        const std::int32_t step = rivendata::kImaStepTable[s.index];

        std::int32_t delta = step >> 3;
        if (nibble & 1)
            delta += step >> 2;
        if (nibble & 2)
            delta += step >> 1;
        if (nibble & 4)
            delta += step;

        std::int32_t predictor = s.predictor;
        predictor += (nibble & 8) ? -delta : delta;
        predictor = std::clamp(predictor, -32768, 32767);
        s.predictor = static_cast<std::int16_t>(predictor);

        std::int32_t index = s.index + rivendata::kImaIndexTable[nibble];
        s.index = static_cast<std::uint8_t>(std::clamp(index, 0, 88));

        return s.predictor;
    }

    /// The inverse: the nibble whose decode lands closest to `sample`. This is
    /// the standard IMA quantiser -- a greedy binary search over the three
    /// magnitude bits, which is what every reference encoder does and what the
    /// step table was designed around.
    inline std::uint8_t imaQuantise(const ImaState &s, std::int32_t sample)
    {
        const std::int32_t step = rivendata::kImaStepTable[s.index];

        std::int32_t diff = sample - s.predictor;
        std::uint8_t nibble = 0;
        if (diff < 0)
        {
            nibble = 8;
            diff = -diff;
        }

        std::int32_t magnitude = step;
        for (std::uint8_t bit = 4; bit != 0; bit >>= 1)
        {
            if (diff >= magnitude)
            {
                nibble |= bit;
                diff -= magnitude;
            }
            magnitude >>= 1;
        }
        return nibble;
    }
} // namespace

std::vector<std::int16_t> decodeIma(const std::uint8_t *data, std::size_t bytes,
                                    std::size_t frames, int channels, bool highNibbleFirst,
                                    ImaState state,
                                    std::vector<rivendata::RsndSeekPoint> *seek,
                                    unsigned seekInterval)
{
    std::vector<std::int16_t> pcm;
    if (data == nullptr || bytes == 0 || channels < 1 || channels > 2)
        return pcm;

    // Two nibbles per byte, `channels` nibbles per frame.
    const std::size_t available = (bytes * 2) / static_cast<std::size_t>(channels);
    frames = std::min(frames, available);
    pcm.resize(frames * static_cast<std::size_t>(channels));

    ImaState st[2] = {state, state};

    for (std::size_t f = 0; f < frames; ++f)
    {
        if (seek != nullptr && seekInterval != 0 && f % seekInterval == 0)
            seek->push_back({st[0].predictor, st[0].index, 0});

        for (int c = 0; c < channels; ++c)
        {
            const std::size_t n = f * static_cast<std::size_t>(channels)
                                + static_cast<std::size_t>(c);
            const std::uint8_t byte = data[n / 2];
            const bool high = (n % 2 == 0) == highNibbleFirst;
            const std::uint8_t nibble = high ? (byte >> 4) : (byte & 0x0F);
            pcm[n] = imaStep(st[c], nibble);
        }
    }

    return pcm;
}

std::vector<std::uint8_t> encodeIma(const std::int16_t *pcm, std::size_t count)
{
    std::vector<std::uint8_t> out;
    if (pcm == nullptr || count == 0)
        return out;

    // A trailing odd sample would leave half a byte; pad it with a repeat of
    // itself rather than silence, which would click.
    out.resize((count + 1) / 2, 0);

    ImaState st;
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint8_t nibble = imaQuantise(st, pcm[i]);
        imaStep(st, nibble);

        // Low nibble first: DS order, which is what the payload is written in.
        if (i % 2 == 0)
            out[i / 2] = nibble;
        else
            out[i / 2] |= static_cast<std::uint8_t>(nibble << 4);
    }

    return out;
}

std::vector<std::uint8_t> ImaStream::encode(const std::int16_t *pcm, std::size_t count)
{
    std::vector<std::uint8_t> out;
    if (pcm == nullptr || count == 0)
        return out;

    // Blocks are whole bytes, so an odd sample count would leave half a byte
    // dangling and the next block would start mid-byte. Callers hand over even
    // counts; a stray odd one pads rather than corrupts the stream.
    out.assign((count + 1) / 2, 0);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::uint8_t nibble = imaQuantise(state_, pcm[i]);
        imaStep(state_, nibble);
        if (i % 2 == 0)
            out[i / 2] = nibble;
        else
            out[i / 2] |= static_cast<std::uint8_t>(nibble << 4);
    }
    return out;
}

void swapNibbles(std::uint8_t *data, std::size_t bytes)
{
    if (data == nullptr)
        return;
    for (std::size_t i = 0; i < bytes; ++i)
        data[i] = static_cast<std::uint8_t>((data[i] >> 4) | (data[i] << 4));
}

} // namespace riven
