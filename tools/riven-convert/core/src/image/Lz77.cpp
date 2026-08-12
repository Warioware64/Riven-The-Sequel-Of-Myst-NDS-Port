#include "riven/ImagePipeline.hpp"

#include <array>
#include <cstring>

namespace riven
{
namespace
{
    // The GBA/NDS BIOS LZ77 format, as decoded by swiDecompressLZSSWram and by
    // libnds' decompress(..., LZ77):
    //
    //   u32 header, little-endian: (uncompressedSize << 8) | 0x10
    //                              0x1 in the low nibble-pair is the LZ77 type.
    //   then, repeatedly:
    //     u8  flags, MSB first, one bit per following unit
    //         bit 0 -> one literal byte
    //         bit 1 -> a two-byte back reference:
    //                    byte0 = ((len - 3) << 4) | ((disp - 1) >> 8)
    //                    byte1 =  (disp - 1) & 0xFF
    //
    // So a match is 3..18 bytes long and reaches back 1..4096 bytes.
    constexpr int kMinMatch = 3;
    constexpr int kMaxMatch = 18;
    constexpr int kMaxDisp = 4096;

    // Hash-chain search, capped so that compressing 2000+ images stays quick.
    // A wider search buys a percent or two of ratio for several times the time,
    // which is the wrong trade when the whole point of the twin is that it is
    // cheaper to store than a direct-colour copy.
    constexpr int kMaxChain = 64;
    constexpr std::size_t kHashSize = 1 << 15;

    inline std::size_t hash3(const std::uint8_t *p)
    {
        return (static_cast<std::size_t>(p[0]) * 0x9E3779B1u
                ^ static_cast<std::size_t>(p[1]) * 0x85EBCA77u
                ^ static_cast<std::size_t>(p[2]) * 0xC2B2AE3Du)
             % kHashSize;
    }
} // namespace

std::vector<std::uint8_t> compressLz77(const std::uint8_t *data, std::size_t size)
{
    std::vector<std::uint8_t> out;
    out.reserve(size / 2 + 16);

    // The size field is 24 bits, which caps a single payload at 16 MB. Riven's
    // largest index plane is 608*392 = 238,336 bytes, so this is only a guard
    // against being handed something unexpected.
    if (size > 0xFFFFFFu)
        return out;

    out.push_back(0x10);
    out.push_back(static_cast<std::uint8_t>(size & 0xFF));
    out.push_back(static_cast<std::uint8_t>((size >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((size >> 16) & 0xFF));

    if (size == 0)
        return out;

    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(size, -1);

    std::size_t pos = 0;
    while (pos < size)
    {
        // One flag byte governs the next eight units; its position is reserved
        // now and filled in once those units are known.
        const std::size_t flagAt = out.size();
        out.push_back(0);
        std::uint8_t flags = 0;

        for (int unit = 0; unit < 8 && pos < size; ++unit)
        {
            int bestLen = 0;
            int bestDisp = 0;

            if (pos + kMinMatch <= size)
            {
                const std::size_t h = hash3(data + pos);
                int candidate = head[h];
                int chain = 0;
                const std::size_t maxLen = std::min<std::size_t>(kMaxMatch, size - pos);

                while (candidate >= 0 && chain < kMaxChain)
                {
                    const std::size_t disp = pos - static_cast<std::size_t>(candidate);
                    if (disp > kMaxDisp)
                        break;

                    std::size_t len = 0;
                    while (len < maxLen && data[candidate + len] == data[pos + len])
                        ++len;

                    if (static_cast<int>(len) > bestLen)
                    {
                        bestLen = static_cast<int>(len);
                        bestDisp = static_cast<int>(disp);
                        if (bestLen == kMaxMatch)
                            break; // cannot do better
                    }
                    candidate = prev[static_cast<std::size_t>(candidate)];
                    ++chain;
                }
            }

            if (bestLen >= kMinMatch)
            {
                flags |= static_cast<std::uint8_t>(0x80 >> unit);
                const int encLen = bestLen - kMinMatch;
                const int encDisp = bestDisp - 1;
                out.push_back(static_cast<std::uint8_t>((encLen << 4) | (encDisp >> 8)));
                out.push_back(static_cast<std::uint8_t>(encDisp & 0xFF));

                // Index every position the match covered, so later matches can
                // start inside it.
                for (int i = 0; i < bestLen; ++i)
                {
                    if (pos + kMinMatch <= size)
                    {
                        const std::size_t h = hash3(data + pos);
                        prev[pos] = head[h];
                        head[h] = static_cast<int>(pos);
                    }
                    ++pos;
                }
            }
            else
            {
                out.push_back(data[pos]);
                if (pos + kMinMatch <= size)
                {
                    const std::size_t h = hash3(data + pos);
                    prev[pos] = head[h];
                    head[h] = static_cast<int>(pos);
                }
                ++pos;
            }
        }

        out[flagAt] = flags;
    }

    return out;
}

std::vector<std::uint8_t> decompressLz77(const std::uint8_t *data, std::size_t size)
{
    std::vector<std::uint8_t> out;
    if (size < 4 || (data[0] & 0xF0) != 0x10)
        return out;

    const std::size_t expected = static_cast<std::size_t>(data[1])
                               | (static_cast<std::size_t>(data[2]) << 8)
                               | (static_cast<std::size_t>(data[3]) << 16);
    out.reserve(expected);

    std::size_t in = 4;
    while (out.size() < expected)
    {
        if (in >= size)
            break;
        const std::uint8_t flags = data[in++];

        for (int unit = 0; unit < 8 && out.size() < expected; ++unit)
        {
            if ((flags & (0x80 >> unit)) == 0)
            {
                if (in >= size)
                    return out;
                out.push_back(data[in++]);
                continue;
            }

            if (in + 1 >= size)
                return out;
            const std::uint8_t b0 = data[in++];
            const std::uint8_t b1 = data[in++];
            const std::size_t len = (b0 >> 4) + kMinMatch;
            const std::size_t disp = (((b0 & 0x0F) << 8) | b1) + 1;

            if (disp > out.size())
                return out; // corrupt: reference before the start of the output

            // Byte-at-a-time on purpose: an overlapping match (disp < len) is
            // legal and is how runs are encoded, so a bulk copy would be wrong.
            for (std::size_t i = 0; i < len && out.size() < expected; ++i)
                out.push_back(out[out.size() - disp]);
        }
    }
    return out;
}

} // namespace riven
