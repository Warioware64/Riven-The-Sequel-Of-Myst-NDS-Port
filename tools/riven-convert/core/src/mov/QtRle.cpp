#include "riven/VideoCodecs.hpp"

#include <cstring>

#include "RivenSound.hpp"

namespace riven
{
namespace
{
    // Depth as the sample description stores it: the low bits are the real bit
    // depth and 0x20 marks the greyscale variants (qtrle.cpp:584-620).
    constexpr int kGreyFlag = 0x20;

    inline int realDepth(int depth) { return depth & 0x1F; }

    inline std::uint32_t be16(const std::uint8_t *p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
    }
} // namespace

bool QtRleDecoder::supports(int depth)
{
    switch (realDepth(depth))
    {
    case 8:
    case 16:
    case 24:
        return true;
    default:
        // 1, 2 and 4 bpp are deliberately refused rather than guessed at: no
        // Riven movie uses them, and ScummVM's own unpackers for those depths
        // are visibly suspect (qtrle.cpp:77-119, :144-149). A conversion that
        // stops and says so beats one that writes silent corruption.
        return false;
    }
}

QtRleDecoder::QtRleDecoder(int width, int height, int depth,
                           const std::vector<std::uint8_t> &paletteRgb)
{
    width_ = width;
    height_ = height;
    depth_ = depth;
    palette_ = paletteRgb;
    if (palette_.size() < 256 * 3)
    {
        // No colour table: the greyscale ramp the sample description would
        // have implied (qt_decoder.cpp:170-180).
        palette_.assign(256 * 3, 0);
        for (int i = 0; i < 256; ++i)
        {
            const std::uint8_t g = static_cast<std::uint8_t>(255 - i);
            palette_[i * 3 + 0] = g;
            palette_[i * 3 + 1] = g;
            palette_[i * 3 + 2] = g;
        }
    }
    rgb_.assign(static_cast<std::size_t>(width) * height * 3, 0);
}

bool QtRleDecoder::decode(const std::uint8_t *data, std::size_t size)
{
    error_.clear();

    // Under eight bytes means "identical to the previous frame" -- not an
    // error, and not an empty frame (qtrle.cpp:562-563).
    if (data == nullptr || size < 8)
        return true;

    std::size_t p = 0;
    p += 4; // chunk size, unused

    const std::uint32_t headerFlags = be16(data + p);
    p += 2;

    int startLine = 0;
    int numLines = height_;
    if ((headerFlags & 0x0008) != 0)
    {
        if (p + 8 > size)
        {
            error_ = "truncated partial-update header";
            return false;
        }
        startLine = static_cast<int>(be16(data + p));
        numLines = static_cast<int>(be16(data + p + 4));
        p += 8;
    }

    const int bpp = realDepth(depth_);
    // How many pixels one "unit" of skip/run/literal covers. At 8bpp the unit
    // is four pixels, not one (qtrle.cpp:181-224).
    const int unitPixels = bpp == 8 ? 4 : 1;
    const int unitBytes = bpp == 8 ? 4 : (bpp == 16 ? 2 : 3);

    auto putUnit = [&](int x, int y, const std::uint8_t *src) {
        for (int i = 0; i < unitPixels; ++i)
        {
            const int px = x + i;
            if (px < 0 || px >= width_ || y < 0 || y >= height_)
                continue;
            std::uint8_t *out = &rgb_[(static_cast<std::size_t>(y) * width_ + px) * 3];
            if (bpp == 8)
            {
                const std::uint8_t *e = &palette_[static_cast<std::size_t>(src[i]) * 3];
                out[0] = e[0];
                out[1] = e[1];
                out[2] = e[2];
            }
            else if (bpp == 16)
            {
                // Big-endian 555, top bit unused (qtrle.cpp:641).
                const std::uint32_t v = be16(src);
                const int r5 = static_cast<int>((v >> 10) & 0x1F);
                const int g5 = static_cast<int>((v >> 5) & 0x1F);
                const int b5 = static_cast<int>(v & 0x1F);
                out[0] = static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2));
                out[1] = static_cast<std::uint8_t>((g5 << 3) | (g5 >> 2));
                out[2] = static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2));
            }
            else
            {
                out[0] = src[0];
                out[1] = src[1];
                out[2] = src[2];
            }
        }
    };

    int y = startLine;
    for (int line = 0; line < numLines && y < height_; ++line, ++y)
    {
        if (p >= size)
            break;

        // Skips are 1-based and counted in units, not pixels.
        int x = (static_cast<int>(data[p++]) - 1) * unitPixels;

        while (p < size)
        {
            const std::int8_t code = static_cast<std::int8_t>(data[p++]);
            if (code == -1)
                break; // end of line
            if (code == 0)
            {
                // A mid-line skip, NOT the terminator.
                if (p >= size)
                    break;
                x += (static_cast<int>(data[p++]) - 1) * unitPixels;
                continue;
            }

            if (code < 0)
            {
                const int count = -code;
                if (p + unitBytes > size)
                    break;
                const std::uint8_t *src = data + p;
                p += unitBytes;
                for (int i = 0; i < count; ++i, x += unitPixels)
                    putUnit(x, y, src);
            }
            else
            {
                const std::size_t bytes = static_cast<std::size_t>(code) * unitBytes;
                if (p + bytes > size)
                    break;
                for (int i = 0; i < code; ++i, x += unitPixels)
                    putUnit(x, y, data + p + static_cast<std::size_t>(i) * unitBytes);
                p += bytes;
            }
        }
    }

    return true;
}

// ---------------------------------------------------------------------------

std::vector<std::int16_t> decodeIma4(const std::uint8_t *data, std::size_t size, int channels)
{
    std::vector<std::int16_t> pcm;
    if (data == nullptr || size == 0 || channels < 1 || channels > 2)
        return pcm;

    // The IMA tables the sound stage already validated against libvaht.
    const auto &stepTable = rivendata::kImaStepTable;
    const auto &indexTable = rivendata::kImaIndexTable;

    const std::size_t packetBytes = 34;
    const std::size_t samplesPerPacket = 64;
    const std::size_t groupBytes = packetBytes * static_cast<std::size_t>(channels);
    const std::size_t groups = size / groupBytes;

    pcm.resize(groups * samplesPerPacket * static_cast<std::size_t>(channels));

    for (std::size_t g = 0; g < groups; ++g)
    {
        for (int c = 0; c < channels; ++c)
        {
            // Stereo alternates WHOLE packets: 34 bytes of left, then 34 of
            // right (adpcm.cpp:247-306). Nibble-interleaving them gives two
            // channels of noise.
            const std::uint8_t *packet = data + g * groupBytes
                                       + static_cast<std::size_t>(c) * packetBytes;

            // The preamble is a big-endian u16: the predictor is the top 9
            // bits AT FULL SCALE (mask, do not shift), the step index the low
            // 7, and the index has to be clamped -- real files carry values
            // past 88 (adpcm.cpp:264-277).
            const std::uint16_t preamble =
                static_cast<std::uint16_t>((packet[0] << 8) | packet[1]);
            std::int32_t predictor = static_cast<std::int16_t>(preamble & 0xFF80);
            std::int32_t index = preamble & 0x007F;
            if (index > 88)
                index = 88;

            for (std::size_t i = 0; i < samplesPerPacket; ++i)
            {
                const std::uint8_t byte = packet[2 + i / 2];
                // Low nibble first -- the opposite of the game's own DVI
                // sounds, which is the single easiest thing to get wrong here.
                const std::uint8_t code =
                    (i % 2 == 0) ? (byte & 0x0F) : static_cast<std::uint8_t>(byte >> 4);

                const std::int32_t step = stepTable[index];
                const std::int32_t diff =
                    ((2 * static_cast<std::int32_t>(code & 0x07) + 1) * step) >> 3;
                predictor += (code & 0x08) ? -diff : diff;
                predictor = predictor < -32768 ? -32768 : (predictor > 32767 ? 32767 : predictor);

                index += indexTable[code];
                index = index < 0 ? 0 : (index > 88 ? 88 : index);

                const std::size_t out =
                    (g * samplesPerPacket + i) * static_cast<std::size_t>(channels)
                    + static_cast<std::size_t>(c);
                pcm[out] = static_cast<std::int16_t>(predictor);
            }
        }
    }

    return pcm;
}

} // namespace riven
