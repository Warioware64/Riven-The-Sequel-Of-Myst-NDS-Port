#include "riven/TopBg.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "RivenImage.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/Bmp.hpp"
#include "riven/ImagePipeline.hpp"

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    using rivendata::Texel;

    /// A texel with its alpha bit dropped, which is the 15-bit colour identity.
    inline int colourOf(Texel t) { return t & 0x7FFF; }

    inline int red5(int c) { return c & 31; }
    inline int green5(int c) { return (c >> 5) & 31; }
    inline int blue5(int c) { return (c >> 10) & 31; }

    /// 5-bit component back to 8, the way the DS expands it.
    inline std::uint8_t expand5(int v)
    {
        return static_cast<std::uint8_t>((v << 3) | (v >> 2));
    }

    /// Pick at most `want` colours out of `canvas` and map every pixel onto one.
    ///
    /// Popularity, not median cut. The input is 49 152 pixels of an image that
    /// on the English disc had only 256 distinct colours to begin with and on
    /// the French one is a photograph of a title card -- in both cases the
    /// long tail past the 240th most common colour is a handful of dither
    /// speckles, and mapping those to their nearest neighbour is invisible.
    /// Median cut would be the upgrade if a future source ever banded.
    ///
    /// Index 0 is reserved for black before anything is counted: the canvas has
    /// letterbox bands, the DS clears the rows below the picture to 0, and
    /// BG_PALETTE_SUB[0] is already black, so all three agree for free.
    int quantise(const std::vector<Texel> &canvas, int want, std::vector<std::uint8_t> &indices,
                 std::array<std::uint8_t, 768> &paletteRgb)
    {
        // 32768 buckets is 128 KB of counts on the host and turns "how many of
        // each colour" into a single pass with no hashing.
        std::vector<std::uint32_t> count(32768, 0);
        for (const Texel t : canvas)
            ++count[static_cast<std::size_t>(colourOf(t))];

        std::vector<int> present;
        present.reserve(1024);
        for (int c = 0; c < 32768; ++c)
            if (count[static_cast<std::size_t>(c)] != 0 && c != 0)
                present.push_back(c);

        std::sort(present.begin(), present.end(), [&](int a, int b) {
            if (count[static_cast<std::size_t>(a)] != count[static_cast<std::size_t>(b)])
                return count[static_cast<std::size_t>(a)] > count[static_cast<std::size_t>(b)];
            return a < b; // stable across runs, so the output is reproducible
        });

        std::vector<int> chosen;
        chosen.reserve(static_cast<std::size_t>(want));
        chosen.push_back(0); // black, index 0
        for (const int c : present)
        {
            if (static_cast<int>(chosen.size()) >= want)
                break;
            chosen.push_back(c);
        }

        paletteRgb.fill(0);
        for (std::size_t i = 0; i < chosen.size(); ++i)
        {
            std::uint8_t *e = paletteRgb.data() + i * 3;
            e[0] = expand5(red5(chosen[i]));
            e[1] = expand5(green5(chosen[i]));
            e[2] = expand5(blue5(chosen[i]));
        }

        // -1 = not resolved yet. Filled on demand, so the nearest-colour search
        // runs once per DISTINCT colour rather than once per pixel.
        std::vector<std::int16_t> lut(32768, -1);
        for (std::size_t i = 0; i < chosen.size(); ++i)
            lut[static_cast<std::size_t>(chosen[i])] = static_cast<std::int16_t>(i);

        indices.resize(canvas.size());
        for (std::size_t p = 0; p < canvas.size(); ++p)
        {
            const int c = colourOf(canvas[p]);
            std::int16_t &slot = lut[static_cast<std::size_t>(c)];
            if (slot < 0)
            {
                const int r = red5(c), g = green5(c), b = blue5(c);
                int best = 0;
                int bestDist = 1 << 30;
                for (std::size_t i = 0; i < chosen.size(); ++i)
                {
                    const int dr = r - red5(chosen[i]);
                    const int dg = g - green5(chosen[i]);
                    const int db = b - blue5(chosen[i]);
                    const int d = dr * dr + dg * dg + db * db;
                    if (d < bestDist)
                    {
                        bestDist = d;
                        best = static_cast<int>(i);
                    }
                }
                slot = static_cast<std::int16_t>(best);
            }
            indices[p] = static_cast<std::uint8_t>(slot);
        }

        return static_cast<int>(chosen.size());
    }
} // namespace

TopBgResult convertTopBackground(const fs::path &bmp, const fs::path &outPath)
{
    TopBgResult result;

    BmpImage src = readBmp(bmp, result.error);
    if (!src.valid())
    {
        if (result.error.empty())
            result.error = bmp.filename().string() + ": not a usable bitmap";
        return result;
    }

    // Fit whole, keeping the aspect: width first, height only if that overflows.
    int dstW = kTopBgW;
    int dstH = static_cast<int>((static_cast<std::int64_t>(src.height) * dstW + src.width / 2)
                                / src.width);
    if (dstH > kTopBgH)
    {
        dstH = kTopBgH;
        dstW = static_cast<int>((static_cast<std::int64_t>(src.width) * dstH + src.height / 2)
                                / src.height);
    }
    dstW = std::clamp(dstW, 1, kTopBgW);
    dstH = std::clamp(dstH, 1, kTopBgH);

    // The same linear-light box filter and Bayer dither every card still goes
    // through. A title card is mostly gradient, which is exactly what the
    // dither is there for.
    const std::vector<Texel> scaled =
        downscaleToTexels(src.rgb.data(), src.width, src.height, dstW, dstH);

    std::vector<Texel> canvas(static_cast<std::size_t>(kTopBgW) * kTopBgH,
                              rivendata::makeTexel(0, 0, 0));
    const int offX = (kTopBgW - dstW) / 2;
    const int offY = (kTopBgH - dstH) / 2;
    for (int y = 0; y < dstH; ++y)
    {
        std::memcpy(canvas.data() + static_cast<std::size_t>(y + offY) * kTopBgW + offX,
                    scaled.data() + static_cast<std::size_t>(y) * dstW,
                    static_cast<std::size_t>(dstW) * sizeof(Texel));
    }

    std::vector<std::uint8_t> indices;
    std::array<std::uint8_t, 768> palette{};
    result.colours = quantise(canvas, kTopBgColours, indices, palette);

    const std::vector<std::uint8_t> file =
        encodeRpiz(indices.data(), palette.data(), kTopBgW, kTopBgH, true);
    if (file.empty())
    {
        result.error = "could not encode " + outPath.filename().string();
        return result;
    }

    if (!writeFileAtomic(outPath, file, result.error))
        return result;

    result.ok = true;
    result.bytes = file.size();
    return result;
}

} // namespace riven
