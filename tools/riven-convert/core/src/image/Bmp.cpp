#include "riven/Bmp.hpp"

#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    // Everything in a BMP is little-endian, and the two headers are unaligned
    // with respect to each other (the file header is 14 bytes), so the fields
    // are read by hand rather than by casting a packed struct over the buffer.
    std::uint16_t u16(const std::uint8_t *p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

    std::uint32_t u32(const std::uint8_t *p)
    {
        return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8)
             | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
    }

    std::int32_t i32(const std::uint8_t *p) { return static_cast<std::int32_t>(u32(p)); }

    constexpr std::size_t kFileHeaderBytes = 14;
    constexpr std::uint32_t kBiRgb = 0;
    /// BITMAPINFOHEADER. V4 (108) and V5 (124) are supersets and are accepted;
    /// only the first 40 bytes are read from any of them.
    constexpr std::uint32_t kInfoHeaderBytes = 40;

    /// One row of a BMP is padded to a 4-byte boundary.
    std::size_t strideFor(int width, int bitCount)
    {
        return ((static_cast<std::size_t>(width) * bitCount + 31) / 32) * 4;
    }
} // namespace

BmpImage decodeBmp(const std::uint8_t *data, std::size_t size, std::string &error)
{
    BmpImage img;
    error.clear();

    if (data == nullptr || size < kFileHeaderBytes + kInfoHeaderBytes)
    {
        error = "not a BMP: too short";
        return img;
    }
    if (data[0] != 'B' || data[1] != 'M')
    {
        error = "not a BMP: bad magic";
        return img;
    }

    const std::uint32_t pixelOffset = u32(data + 10);
    const std::uint8_t *dib = data + kFileHeaderBytes;
    const std::uint32_t dibBytes = u32(dib);
    if (dibBytes < kInfoHeaderBytes || kFileHeaderBytes + dibBytes > size)
    {
        error = "unsupported BMP: DIB header is " + std::to_string(dibBytes) + " bytes";
        return img;
    }

    const std::int32_t width = i32(dib + 4);
    const std::int32_t rawHeight = i32(dib + 8);
    const std::uint16_t bitCount = u16(dib + 14);
    const std::uint32_t compression = u32(dib + 16);
    std::uint32_t paletteCount = u32(dib + 32);

    // A negative height means the rows are already top-down.
    const bool bottomUp = rawHeight > 0;
    const std::int64_t height = rawHeight < 0 ? -static_cast<std::int64_t>(rawHeight) : rawHeight;

    if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
    {
        error = "unsupported BMP: " + std::to_string(width) + "x" + std::to_string(rawHeight);
        return img;
    }
    if (compression != kBiRgb)
    {
        // BI_RLE8 and BI_BITFIELDS exist; no Riven-shipped bitmap uses either,
        // and guessing would be worse than saying so.
        error = "unsupported BMP: compression " + std::to_string(compression);
        return img;
    }
    if (bitCount != 8 && bitCount != 24 && bitCount != 32)
    {
        error = "unsupported BMP: " + std::to_string(bitCount) + " bits per pixel";
        return img;
    }

    const std::uint8_t *palette = nullptr;
    if (bitCount == 8)
    {
        if (paletteCount == 0 || paletteCount > 256)
            paletteCount = 256;
        palette = dib + dibBytes;
        // Palette entries are BGRA quads, hence *4.
        if (static_cast<std::size_t>(palette - data) + paletteCount * 4 > size)
        {
            error = "truncated BMP: palette runs past the end of the file";
            return img;
        }
    }

    const std::size_t stride = strideFor(width, bitCount);
    const std::size_t needed = pixelOffset + stride * static_cast<std::size_t>(height);
    if (pixelOffset >= size || needed > size)
    {
        error = "truncated BMP: pixels run past the end of the file";
        return img;
    }

    img.width = width;
    img.height = static_cast<int>(height);
    img.rgb.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3);

    for (std::int64_t y = 0; y < height; ++y)
    {
        const std::int64_t srcRow = bottomUp ? (height - 1 - y) : y;
        const std::uint8_t *src = data + pixelOffset + stride * static_cast<std::size_t>(srcRow);
        std::uint8_t *dst = img.rgb.data() + static_cast<std::size_t>(y) * width * 3;

        for (int x = 0; x < width; ++x, dst += 3)
        {
            if (bitCount == 8)
            {
                // Out-of-range indices happen in the wild; black is the safe read.
                const std::uint8_t idx = src[x];
                if (idx < paletteCount)
                {
                    const std::uint8_t *e = palette + static_cast<std::size_t>(idx) * 4;
                    dst[0] = e[2]; // the quad is B,G,R,reserved
                    dst[1] = e[1];
                    dst[2] = e[0];
                }
                else
                {
                    dst[0] = dst[1] = dst[2] = 0;
                }
            }
            else
            {
                const std::uint8_t *e = src + static_cast<std::size_t>(x) * (bitCount / 8);
                dst[0] = e[2];
                dst[1] = e[1];
                dst[2] = e[0];
            }
        }
    }

    return img;
}

BmpImage readBmp(const fs::path &path, std::string &error)
{
    BmpImage img;
    error.clear();

    std::FILE *f = std::fopen(path.string().c_str(), "rb");
    if (f == nullptr)
    {
        error = "could not open " + path.string();
        return img;
    }
    std::fseek(f, 0, SEEK_END);
    const long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (len <= 0)
    {
        std::fclose(f);
        error = path.string() + " is empty";
        return img;
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(len));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size())
    {
        error = "short read on " + path.string();
        return img;
    }

    img = decodeBmp(bytes.data(), bytes.size(), error);
    if (!error.empty())
        error = path.filename().string() + ": " + error;
    return img;
}

} // namespace riven
