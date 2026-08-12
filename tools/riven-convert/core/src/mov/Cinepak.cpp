#include "riven/VideoCodecs.hpp"

#include <algorithm>
#include <cstring>

namespace riven
{
namespace
{
    // Chunk ids. The encoding is bitwise and worth reading as such:
    //   bit 0 -- codebook update is selective (a bitmask says which entries)
    //   bit 1 -- in a vector chunk, there are no V1/V4 flags at all
    //   bit 2 -- codebook entries are 4 bytes rather than 6 (no chroma)
    constexpr std::uint8_t kV4Full = 0x20;
    constexpr std::uint8_t kV1Full = 0x22;
    constexpr std::uint8_t kVectorsIntra = 0x30;
    constexpr std::uint8_t kVectorsInter = 0x31;
    constexpr std::uint8_t kVectorsV1Only = 0x32;

    constexpr std::uint8_t kSelective = 0x01;
    constexpr std::uint8_t kNoFlags = 0x02;
    constexpr std::uint8_t kShortEntry = 0x04;

    inline std::uint32_t be24(const std::uint8_t *p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 16)
             | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
    }
    inline std::uint32_t be16(const std::uint8_t *p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
    }
    inline std::uint32_t be32(const std::uint8_t *p)
    {
        return (static_cast<std::uint32_t>(p[0]) << 24)
             | (static_cast<std::uint32_t>(p[1]) << 16)
             | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
    }

    inline std::uint8_t clampByte(int v)
    {
        return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    /// A vector chunk's flag bits and its codebook indices come from ONE
    /// cursor, not two: when the 32-bit flag word runs out, the next word is
    /// read from wherever the index bytes have got to, and vice versa. Two
    /// independent cursors desynchronise the rest of the strip, which is the
    /// classic way to get a Cinepak decoder that works on intra frames and
    /// smears on inter ones.
    ///
    /// Flag words are big-endian and consumed MSB first.
    class VectorStream
    {
    public:
        VectorStream(const std::uint8_t *data, std::size_t size)
            : p_(data), end_(data + size)
        {
        }

        bool nextFlag(bool &bit)
        {
            if (mask_ == 0)
            {
                if (p_ + 4 > end_)
                    return false;
                word_ = be32(p_);
                p_ += 4;
                mask_ = 0x80000000u;
            }
            bit = (word_ & mask_) != 0;
            mask_ >>= 1;
            return true;
        }

        bool nextByte(std::uint8_t &out)
        {
            if (p_ >= end_)
                return false;
            out = *p_++;
            return true;
        }

    private:
        const std::uint8_t *p_;
        const std::uint8_t *end_;
        std::uint32_t word_ = 0;
        std::uint32_t mask_ = 0;
    };
} // namespace

CinepakDecoder::CinepakDecoder(int width, int height)
{
    width_ = width;
    height_ = height;
    rgb_.assign(static_cast<std::size_t>(width) * height * 3, 0);
}

bool CinepakDecoder::decode(const std::uint8_t *data, std::size_t size)
{
    error_.clear();
    if (data == nullptr || size < 10)
    {
        error_ = "frame is shorter than its header";
        return false;
    }

    const std::uint8_t flags = data[0];
    const std::uint32_t stripCount = be16(data + 8);
    std::size_t pos = 10;

    // Codebooks persist per strip index across frames, so the vector only ever
    // grows.
    if (strips_.size() < stripCount)
        strips_.resize(stripCount);

    int stripTop = 0;

    for (std::uint32_t s = 0; s < stripCount; ++s)
    {
        if (pos + 12 > size)
        {
            error_ = "truncated strip header";
            return false;
        }

        const std::uint32_t declared = be16(data + pos + 2);
        // The stored left/top/right are not trustworthy -- encoders disagree on
        // whether they are absolute or strip-relative. Only "bottom" is, and it
        // is a HEIGHT: strips are horizontal bands of the full frame width,
        // stacked downwards (cinepak.cpp:370-378 substitutes exactly this).
        const int stripHeight = static_cast<int>(be16(data + pos + 8));
        const int stripBottom = std::min(stripTop + stripHeight, height_);

        // Strip N inherits strip N-1's codebooks unless the frame says
        // otherwise (cinepak.cpp:357-368).
        if ((flags & 0x01) == 0 && s > 0)
            strips_[s] = strips_[s - 1];

        const std::size_t stripStart = pos + 12;
        std::size_t stripEnd = declared >= 12 ? pos + declared : size;
        if (stripEnd > size)
            stripEnd = size;
        pos = stripStart;

        while (pos + 4 <= stripEnd)
        {
            const std::uint8_t chunkId = data[pos];
            std::uint32_t chunkSize = be24(data + pos + 1);
            const std::size_t chunkStart = pos + 4;
            if (chunkSize < 4)
                break;
            chunkSize -= 4; // the size counts its own 4-byte header
            std::size_t chunkEnd = chunkStart + chunkSize;
            if (chunkEnd > stripEnd)
                chunkEnd = stripEnd;

            if (chunkId >= kV4Full && chunkId <= 0x27)
            {
                // 0x22/0x23/0x26/0x27 are V1; 0x20/0x21/0x24/0x25 are V4. The
                // bit that separates them is 0x02, which in a VECTOR chunk
                // means something else entirely.
                Codebook *book = ((chunkId & 0x02) != 0) ? strips_[s].v1 : strips_[s].v4;

                const bool selective = (chunkId & kSelective) != 0;
                const std::size_t entryBytes = (chunkId & kShortEntry) != 0 ? 4u : 6u;

                std::size_t p = chunkStart;
                std::uint32_t mask = 0;
                std::uint32_t word = 0;

                for (int i = 0; i < 256; ++i)
                {
                    if (selective)
                    {
                        if (mask == 0)
                        {
                            if (p + 4 > chunkEnd)
                                break;
                            word = be32(data + p);
                            p += 4;
                            mask = 0x80000000u;
                        }
                        const bool update = (word & mask) != 0;
                        mask >>= 1;
                        if (!update)
                            continue; // keeps its previous value
                    }

                    if (p + entryBytes > chunkEnd)
                        break;
                    Codebook &c = book[i];
                    c.y[0] = data[p + 0];
                    c.y[1] = data[p + 1];
                    c.y[2] = data[p + 2];
                    c.y[3] = data[p + 3];
                    if (entryBytes == 6)
                    {
                        c.u = static_cast<std::int8_t>(data[p + 4]);
                        c.v = static_cast<std::int8_t>(data[p + 5]);
                    }
                    else
                    {
                        // A 4-byte entry has no chroma at all.
                        c.u = 0;
                        c.v = 0;
                    }
                    p += entryBytes;
                }
            }
            else if (chunkId >= kVectorsIntra && chunkId <= kVectorsV1Only)
            {
                const bool hasCodedFlags = chunkId == kVectorsInter;
                const bool hasTypeFlags = (chunkId & kNoFlags) == 0;

                VectorStream vs(data + chunkStart, chunkEnd - chunkStart);
                bool ranOut = false;
                for (int y = stripTop; y < stripBottom && !ranOut; y += 4)
                {
                    for (int x = 0; x < width_ && !ranOut; x += 4)
                    {
                        bool coded = true;
                        if (hasCodedFlags && !vs.nextFlag(coded))
                        {
                            ranOut = true;
                            break;
                        }
                        if (!coded)
                            continue; // leave the previous frame's pixels

                        bool isV4 = false;
                        if (hasTypeFlags && !vs.nextFlag(isV4))
                        {
                            ranOut = true;
                            break;
                        }

                        const Codebook *quad[4] = {nullptr, nullptr, nullptr, nullptr};
                        if (isV4)
                        {
                            std::uint8_t idx[4];
                            for (int q = 0; q < 4 && !ranOut; ++q)
                                if (!vs.nextByte(idx[q]))
                                    ranOut = true;
                            if (ranOut)
                                break;
                            for (int q = 0; q < 4; ++q)
                                quad[q] = &strips_[s].v4[idx[q]];
                        }
                        else
                        {
                            std::uint8_t idx;
                            if (!vs.nextByte(idx))
                            {
                                ranOut = true;
                                break;
                            }
                            quad[0] = quad[1] = quad[2] = quad[3] = &strips_[s].v1[idx];
                        }

                        // Both block kinds fill four 2x2 quadrants. V4 gives
                        // each quadrant its own entry, so luma is full
                        // resolution and chroma is 2x2; V1 repeats one entry,
                        // whose four luma samples ARE the four quadrants, so
                        // luma is 2x2 and chroma 4x4.
                        for (int q = 0; q < 4; ++q)
                        {
                            const Codebook &c = *quad[q];
                            const int qx = x + (q & 1) * 2;
                            const int qy = y + (q >> 1) * 2;
                            for (int py = 0; py < 2; ++py)
                            {
                                for (int px = 0; px < 2; ++px)
                                {
                                    const int ox = qx + px;
                                    const int oy = qy + py;
                                    if (ox >= width_ || oy >= height_)
                                        continue;
                                    const int luma =
                                        isV4 ? c.y[py * 2 + px] : c.y[q];
                                    std::uint8_t *out =
                                        &rgb_[(static_cast<std::size_t>(oy) * width_ + ox) * 3];
                                    // Cinepak's own colour space: no luma
                                    // expansion, and u is halved with an
                                    // ARITHMETIC shift (cinepak.cpp:40-44).
                                    out[0] = clampByte(luma + 2 * c.v);
                                    out[1] = clampByte(luma - (c.u >> 1) - c.v);
                                    out[2] = clampByte(luma + 2 * c.u);
                                }
                            }
                        }
                    }
                }
            }

            // Seek past the chunk whatever happened inside it: a truncated or
            // over-long chunk costs its own contents, not the rest of the strip.
            pos = chunkEnd;
        }

        pos = stripEnd;
        stripTop = stripBottom;
        if (stripTop >= height_)
            break;
    }

    return true;
}

} // namespace riven
