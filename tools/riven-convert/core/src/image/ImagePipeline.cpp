#include "riven/ImagePipeline.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "RivenData.hpp" // kViewW/kViewH -- the one place the DS view size is defined
#include "riven/AtomicWrite.hpp"

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    using rivendata::Texel;

    // --- gamma -----------------------------------------------------------
    //
    // Averaging and dithering both have to happen in LINEAR light. Riven's
    // renders are sRGB-ish (gamma ~2.2), and averaging gamma-encoded values
    // darkens every edge -- over a 2.375x reduction that shows up as a dingy,
    // muddy image, most obviously on water and sky. FastVideoDS's Rgb555Frame
    // makes the same point and solves it the same way.
    struct GammaTables
    {
        std::array<float, 256> toLinear{};
        // 5-bit output levels in linear light, for the dither decision.
        std::array<float, 32> level5Linear{};

        GammaTables()
        {
            for (int i = 0; i < 256; ++i)
                toLinear[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
            for (int i = 0; i < 32; ++i)
            {
                // A 5-bit value is displayed as if it were the 8-bit value
                // (i << 3) | (i >> 2), which is the standard 5->8 expansion the
                // DS hardware effectively performs.
                const int expanded = (i << 3) | (i >> 2);
                level5Linear[i] = toLinear[expanded];
            }
        }
    };

    const GammaTables &gamma()
    {
        static const GammaTables g;
        return g;
    }

    /// 4x4 Bayer matrix, normalised to (-0.5, +0.5).
    inline float bayer(int x, int y)
    {
        static const int kBayer4[4][4] = {
            {0, 8, 2, 10},
            {12, 4, 14, 6},
            {3, 11, 1, 9},
            {15, 7, 13, 5},
        };
        return (static_cast<float>(kBayer4[y & 3][x & 3]) + 0.5f) / 16.0f - 0.5f;
    }

    /// Quantise one linear-light channel to 5 bits with an ordered-dither
    /// offset. The threshold is applied between the two candidate output
    /// levels, so the perturbation is always smaller than one output step and
    /// can never push a colour past its neighbour.
    inline int quantise5(float linear, float dither)
    {
        const auto &g = gamma();

        // Find the bracketing 5-bit levels.
        int lo = 0;
        while (lo < 31 && g.level5Linear[lo + 1] <= linear)
            ++lo;
        if (lo >= 31)
            return 31;
        const int hi = lo + 1;

        const float span = g.level5Linear[hi] - g.level5Linear[lo];
        if (span <= 0.0f)
            return lo;

        const float t = (linear - g.level5Linear[lo]) / span; // 0..1 within the step
        return (t + dither > 0.5f) ? hi : lo;
    }
} // namespace

std::vector<Texel> downscaleToTexels(const std::uint8_t *rgb, int srcW, int srcH, int dstW,
                                     int dstH)
{
    std::vector<Texel> out;
    if (rgb == nullptr || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0)
        return out;

    const auto &g = gamma();
    out.resize(static_cast<std::size_t>(dstW) * dstH);

    for (int dy = 0; dy < dstH; ++dy)
    {
        // Source rows covered by this destination row. Computed from exact
        // ratios rather than a fixed box so the last row is not short.
        const int y0 = static_cast<int>(static_cast<std::int64_t>(dy) * srcH / dstH);
        int y1 = static_cast<int>(static_cast<std::int64_t>(dy + 1) * srcH / dstH);
        if (y1 <= y0)
            y1 = y0 + 1;
        if (y1 > srcH)
            y1 = srcH;

        for (int dx = 0; dx < dstW; ++dx)
        {
            const int x0 = static_cast<int>(static_cast<std::int64_t>(dx) * srcW / dstW);
            int x1 = static_cast<int>(static_cast<std::int64_t>(dx + 1) * srcW / dstW);
            if (x1 <= x0)
                x1 = x0 + 1;
            if (x1 > srcW)
                x1 = srcW;

            float r = 0.0f, gg = 0.0f, b = 0.0f;
            int n = 0;
            for (int sy = y0; sy < y1; ++sy)
            {
                const std::uint8_t *row = rgb + (static_cast<std::size_t>(sy) * srcW + x0) * 3;
                for (int sx = x0; sx < x1; ++sx, row += 3)
                {
                    r += g.toLinear[row[0]];
                    gg += g.toLinear[row[1]];
                    b += g.toLinear[row[2]];
                    ++n;
                }
            }
            if (n == 0)
                n = 1;
            r /= static_cast<float>(n);
            gg /= static_cast<float>(n);
            b /= static_cast<float>(n);

            const float d = bayer(dx, dy);
            const int r5 = quantise5(r, d);
            const int g5 = quantise5(gg, d);
            const int b5 = quantise5(b, d);

            // Alpha is always set: Riven stills are opaque, and a texel with
            // bit 15 clear would be skipped by the DS blender.
            out[static_cast<std::size_t>(dy) * dstW + dx] =
                static_cast<Texel>(0x8000 | (b5 << 10) | (g5 << 5) | r5);
        }
    }
    return out;
}

std::vector<std::uint8_t> encodeRpic(const std::vector<Texel> &texels, int w, int h)
{
    std::vector<std::uint8_t> out;
    const std::size_t pixelBytes = texels.size() * sizeof(Texel);

    rivendata::RpicHeader hdr{};
    std::memcpy(hdr.magic, "RPIC", 4);
    hdr.version = rivendata::kImageVersion;
    hdr.width = static_cast<std::uint16_t>(w);
    hdr.height = static_cast<std::uint16_t>(h);
    hdr.compression = static_cast<std::uint8_t>(rivendata::ImageCompression::None);
    hdr.dataBytes = static_cast<std::uint32_t>(pixelBytes);

    out.resize(sizeof(hdr) + pixelBytes);
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    // Texels are uint16 and both sides are little-endian, so the in-memory
    // layout is already the on-disc layout.
    std::memcpy(out.data() + sizeof(hdr), texels.data(), pixelBytes);
    return out;
}

std::vector<std::uint8_t> encodeRpiz(const std::uint8_t *indices, const std::uint8_t *paletteRgb,
                                     int w, int h, bool compress)
{
    std::vector<std::uint8_t> out;
    if (indices == nullptr || paletteRgb == nullptr || w <= 0 || h <= 0)
        return out;

    const std::size_t planeBytes = static_cast<std::size_t>(w) * h;

    std::vector<std::uint8_t> payload;
    auto compression = rivendata::ImageCompression::None;
    if (compress)
    {
        payload = compressLz77(indices, planeBytes);
        // Fall back when compression does not pay. Small or noisy planes can
        // come out larger, and shipping a bigger file that also costs decode
        // time would be the worst of both.
        if (!payload.empty() && payload.size() < planeBytes)
            compression = rivendata::ImageCompression::Lz77;
        else
            payload.clear();
    }
    if (compression == rivendata::ImageCompression::None)
        payload.assign(indices, indices + planeBytes);

    rivendata::RpizHeader hdr{};
    std::memcpy(hdr.magic, "RPIZ", 4);
    hdr.version = rivendata::kImageVersion;
    hdr.width = static_cast<std::uint16_t>(w);
    hdr.height = static_cast<std::uint16_t>(h);
    hdr.compression = static_cast<std::uint8_t>(compression);
    hdr.dataBytes = static_cast<std::uint32_t>(payload.size());

    out.resize(sizeof(hdr) + rivendata::kPaletteBytes + payload.size());
    std::memcpy(out.data(), &hdr, sizeof(hdr));

    // Palette: libvaht hands us 256 RGB triplets; the DS wants RGB555 with the
    // alpha bit set, which is the same makeTexel the display copy uses.
    auto *pal = reinterpret_cast<std::uint16_t *>(out.data() + sizeof(hdr));
    for (int i = 0; i < rivendata::kPaletteEntries; ++i)
    {
        const std::uint8_t *e = paletteRgb + i * 3;
        pal[i] = rivendata::makeTexel(e[0], e[1], e[2]);
    }

    std::memcpy(out.data() + sizeof(hdr) + rivendata::kPaletteBytes, payload.data(),
                payload.size());
    return out;
}

ImageResult convertBitmap(const ArchiveSet &set, std::uint16_t id, const fs::path &rpicPath,
                          bool wantRpic, const fs::path &rpizPath, bool wantRpiz)
{
    ImageResult res;

    Bitmap bmp = set.readBitmap(id);
    if (!bmp.valid() || bmp.width() <= 0 || bmp.height() <= 0)
    {
        res.error = "tBMP " + std::to_string(id) + " did not decode";
        return res;
    }
    res.width = bmp.width();
    res.height = bmp.height();

    if (wantRpic)
    {
        const std::uint8_t *rgb = bmp.rgb();
        if (rgb == nullptr)
        {
            res.error = "tBMP " + std::to_string(id) + " has no truecolour data";
            return res;
        }

        // Preserve aspect from the source rather than assuming 608x392: Riven
        // ships a handful of odd-sized tBMPs (inventory art, credits), and
        // stretching those to the card ratio would visibly distort them.
        const int dstW = std::min(rivendata::kViewW, bmp.width());
        int dstH = static_cast<int>(static_cast<std::int64_t>(bmp.height()) * dstW
                                    / bmp.width());
        if (dstH < 1)
            dstH = 1;

        const auto texels = downscaleToTexels(rgb, bmp.width(), bmp.height(), dstW, dstH);
        const auto bytes = encodeRpic(texels, dstW, dstH);
        if (!writeFileAtomic(rpicPath, bytes, res.error))
            return res;
        res.rpicBytes = bytes.size();
    }

    if (wantRpiz)
    {
        const std::uint8_t *indices = bmp.indices();
        const std::uint8_t *palette = bmp.palette();
        if (indices == nullptr || palette == nullptr)
        {
            // Truecolour tBMPs exist in the format but Riven does not use them
            // for card art. Not an error: there is simply no paletted twin to
            // make, and the display copy above is already written.
            res.ok = wantRpic;
            return res;
        }

        const auto bytes = encodeRpiz(indices, palette, bmp.width(), bmp.height(), true);
        if (!writeFileAtomic(rpizPath, bytes, res.error))
            return res;
        res.rpizBytes = bytes.size();
    }

    res.ok = true;
    return res;
}

} // namespace riven
