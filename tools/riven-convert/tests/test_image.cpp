// Image pipeline: LZ77 round-trip, the downscaler, and the two encoders.
//
// The LZ77 checks matter most. Its decompressor on the other end is the DS
// BIOS, which this build cannot call, so a compressor bug would not surface
// until an image was opened on hardware. decompressLz77 exists purely so the
// format can be verified here instead.
//
// The image parts run with RIVEN_TEST_DATA when it is set, and on synthetic
// input otherwise, so the format is always covered even without game data.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "riven/Archive.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/Layout.hpp"

using namespace riven;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const std::string &what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++g_failures;
        }
    }

    bool roundTrips(const std::vector<std::uint8_t> &in, const char *what)
    {
        const auto packed = compressLz77(in.data(), in.size());
        const auto back = decompressLz77(packed.data(), packed.size());
        const bool same = back == in;
        check(same, std::string("lz77 round-trips: ") + what);
        return same;
    }

    // --- the quantiser, as it was written before it was made fast ------------
    //
    // downscaleToTexels' 5-bit quantiser was a linear scan for the bracketing
    // levels plus a divide, per channel per pixel. It is now a binary search over
    // precomputed cut points, which is the same function rearranged -- but "the
    // same function rearranged" is exactly the claim that needs checking, because
    // this decides the value of every pixel in the game and a mistake would look
    // like a slightly different picture rather than like a bug.
    //
    // So the original stays here, verbatim in behaviour, as the reference.
    // The types are float here for the same reason they are float in the
    // production path: the point of the comparison is the rearrangement, not the
    // precision, so anything else held identical.
    struct ReferenceQuantiser
    {
        float toLinear[256]{};
        float level5Linear[32]{};

        ReferenceQuantiser()
        {
            for (int i = 0; i < 256; ++i)
                toLinear[i] = std::pow(static_cast<float>(i) / 255.0f, 2.2f);
            for (int i = 0; i < 32; ++i)
                level5Linear[i] = toLinear[(i << 3) | (i >> 2)];
        }

        static float bayer(int x, int y)
        {
            static const int kBayer4[4][4] = {
                {0, 8, 2, 10},
                {12, 4, 14, 6},
                {3, 11, 1, 9},
                {15, 7, 13, 5},
            };
            return (static_cast<float>(kBayer4[y & 3][x & 3]) + 0.5f) / 16.0f - 0.5f;
        }

        int quantise(float linear, float dither) const
        {
            int lo = 0;
            while (lo < 31 && level5Linear[lo + 1] <= linear)
                ++lo;
            if (lo >= 31)
                return 31;
            const float span = level5Linear[lo + 1] - level5Linear[lo];
            if (span <= 0.0f)
                return lo;
            const float t = (linear - level5Linear[lo]) / span;
            return (t + dither > 0.5f) ? lo + 1 : lo;
        }
    };
} // namespace

int main()
{
    // --- LZ77 -------------------------------------------------------------
    {
        roundTrips({}, "empty");
        roundTrips({42}, "one byte");
        roundTrips(std::vector<std::uint8_t>(1000, 0x5A), "a long run");

        // A run is encoded as an overlapping match (disp < len), which is the
        // case a bulk memcpy in the decoder would get wrong.
        std::vector<std::uint8_t> ramp(4096);
        for (std::size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = static_cast<std::uint8_t>(i & 0xFF);
        roundTrips(ramp, "a repeating ramp");

        // Incompressible data exercises the literal path and the worst case.
        std::mt19937 rng(12345);
        std::vector<std::uint8_t> noise(5000);
        for (auto &b : noise)
            b = static_cast<std::uint8_t>(rng() & 0xFF);
        roundTrips(noise, "random noise");

        // Something shaped like a real 8bpp image: smooth bands with edges.
        std::vector<std::uint8_t> plane(608 * 392);
        for (int y = 0; y < 392; ++y)
            for (int x = 0; x < 608; ++x)
                plane[y * 608 + x] = static_cast<std::uint8_t>(((x / 16) + (y / 24)) & 0xFF);
        if (roundTrips(plane, "a synthetic 608x392 index plane"))
        {
            const auto packed = compressLz77(plane.data(), plane.size());
            check(packed.size() < plane.size(),
                  "lz77 actually compresses a smooth index plane");
            std::printf("  synthetic plane: %zu -> %zu bytes (%.1f%%)\n", plane.size(),
                        packed.size(), 100.0 * packed.size() / plane.size());
        }

        // A truncated payload must not read past the end or loop forever.
        const auto packed = compressLz77(ramp.data(), ramp.size());
        for (std::size_t cut : {std::size_t{0}, std::size_t{3}, packed.size() / 2})
        {
            const auto back = decompressLz77(packed.data(), cut);
            check(back.size() <= ramp.size(), "a truncated lz77 payload decodes safely");
        }
    }

    // --- the quantiser rearrangement ---------------------------------------
    // Driven through downscaleToTexels at 1:1, where each destination pixel is
    // exactly one source pixel, so the linear value reaching the quantiser is
    // known and the reference can be evaluated against it. 256 columns x 16 rows
    // covers every 8-bit value at every one of the 16 Bayer positions.
    {
        const ReferenceQuantiser ref;
        const int w = 256, h = 16;
        std::vector<std::uint8_t> src(static_cast<std::size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                std::uint8_t *p = &src[(static_cast<std::size_t>(y) * w + x) * 3];
                // A different channel value per component, so a red/blue mix-up
                // in the texel packing cannot pass either.
                p[0] = static_cast<std::uint8_t>(x);
                p[1] = static_cast<std::uint8_t>((x * 7 + 13) & 0xFF);
                p[2] = static_cast<std::uint8_t>(255 - x);
            }

        const auto texels = downscaleToTexels(src.data(), w, h, w, h);
        int mismatches = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const std::uint8_t *p = &src[(static_cast<std::size_t>(y) * w + x) * 3];
                const float d = ReferenceQuantiser::bayer(x, y);
                const int r = ref.quantise(ref.toLinear[p[0]], d);
                const int g = ref.quantise(ref.toLinear[p[1]], d);
                const int b = ref.quantise(ref.toLinear[p[2]], d);
                const rivendata::Texel want =
                    static_cast<rivendata::Texel>(0x8000 | (b << 10) | (g << 5) | r);
                if (texels[static_cast<std::size_t>(y) * w + x] != want)
                    ++mismatches;
            }
        check(mismatches == 0, "the fast quantiser matches the original on every "
                              "8-bit value at every dither position ("
                                  + std::to_string(mismatches) + " differ)");

        // And on averaged values, which is what a real downscale produces: the
        // 1:1 case only ever hands the quantiser one of 256 exact levels.
        std::mt19937 rng(20260812);
        std::uniform_int_distribution<int> byte(0, 255);
        const int aw = 64, ah = 64;
        std::vector<std::uint8_t> noise(static_cast<std::size_t>(aw) * ah * 3);
        for (auto &v : noise)
            v = static_cast<std::uint8_t>(byte(rng));
        const auto small = downscaleToTexels(noise.data(), aw, ah, 27, 27);
        int averagedMismatches = 0;
        for (int dy = 0; dy < 27; ++dy)
            for (int dx = 0; dx < 27; ++dx)
            {
                const int x0 = dx * aw / 27, x1 = std::max((dx + 1) * aw / 27, x0 + 1);
                const int y0 = dy * ah / 27, y1 = std::max((dy + 1) * ah / 27, y0 + 1);
                float acc[3] = {0.0f, 0.0f, 0.0f};
                int n = 0;
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx, ++n)
                        for (int c = 0; c < 3; ++c)
                            acc[c] += ref.toLinear[noise[(static_cast<std::size_t>(sy) * aw + sx)
                                                             * 3
                                                         + c]];
                const float d = ReferenceQuantiser::bayer(dx, dy);
                const int r = ref.quantise(acc[0] / n, d);
                const int g = ref.quantise(acc[1] / n, d);
                const int b = ref.quantise(acc[2] / n, d);
                const rivendata::Texel want =
                    static_cast<rivendata::Texel>(0x8000 | (b << 10) | (g << 5) | r);
                if (small[static_cast<std::size_t>(dy) * 27 + dx] != want)
                    ++averagedMismatches;
            }
        check(averagedMismatches == 0,
              "the fast quantiser matches the original on averaged values ("
                  + std::to_string(averagedMismatches) + " of 729 differ)");

        // The out-parameter form is what the video path calls in a loop; it has to
        // agree with the returning form it replaced.
        std::vector<rivendata::Texel> reused;
        reused.assign(9999, 0x1234); // deliberately dirty and the wrong size
        downscaleToTexels(src.data(), w, h, w, h, reused);
        check(reused == texels, "the out-parameter downscale matches the returning one");
    }

    // --- downscale + encoders on synthetic input ---------------------------
    {
        const int w = 608, h = 392;
        std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                std::uint8_t *p = &rgb[(static_cast<std::size_t>(y) * w + x) * 3];
                p[0] = static_cast<std::uint8_t>(x * 255 / (w - 1)); // horizontal ramp
                p[1] = static_cast<std::uint8_t>(y * 255 / (h - 1)); // vertical ramp
                p[2] = 128;
            }

        const auto texels = downscaleToTexels(rgb.data(), w, h, rivendata::kViewW,
                                              rivendata::kViewH);
        check(texels.size()
                  == static_cast<std::size_t>(rivendata::kViewW) * rivendata::kViewH,
              "the downscale produces exactly the DS view size");

        // Every texel must be opaque, or the DS blender drops it.
        bool allOpaque = true;
        for (const auto t : texels)
            if ((t & 0x8000) == 0)
                allOpaque = false;
        check(allOpaque, "every downscaled texel has its alpha bit set");

        // The ramps must still be monotonic after dithering: an ordered dither
        // may perturb by less than one output step, never reverse a gradient.
        const int midRow = rivendata::kViewH / 2;
        int reversals = 0;
        for (int x = 1; x < rivendata::kViewW; ++x)
        {
            const int prev = texels[midRow * rivendata::kViewW + x - 1] & 0x1F;
            const int cur = texels[midRow * rivendata::kViewW + x] & 0x1F;
            if (cur < prev - 1)
                ++reversals;
        }
        check(reversals == 0, "the red ramp stays monotonic across a row after dithering");

        // A flat field must not dither into visible noise beyond one step.
        std::vector<std::uint8_t> flat(static_cast<std::size_t>(w) * h * 3, 200);
        const auto flatTexels = downscaleToTexels(flat.data(), w, h, 64, 64);
        int distinct = 0;
        std::vector<int> seen;
        for (const auto t : flatTexels)
        {
            const int r = t & 0x1F;
            if (std::find(seen.begin(), seen.end(), r) == seen.end())
            {
                seen.push_back(r);
                ++distinct;
            }
        }
        check(distinct <= 2, "a flat field quantises to at most two adjacent levels");

        const auto rpic = encodeRpic(texels, rivendata::kViewW, rivendata::kViewH);
        const auto *ph = reinterpret_cast<const rivendata::RpicHeader *>(rpic.data());
        check(rivendata::isRpic(*ph), "the .rpic header validates");
        check(ph->width == rivendata::kViewW && ph->height == rivendata::kViewH,
              "the .rpic header carries the right dimensions");
        check(rpic.size() == sizeof(rivendata::RpicHeader) + texels.size() * 2,
              "the .rpic is header plus exactly one texel per pixel");

        std::vector<std::uint8_t> indices(static_cast<std::size_t>(w) * h);
        for (std::size_t i = 0; i < indices.size(); ++i)
            indices[i] = static_cast<std::uint8_t>((i / 97) & 0xFF);
        std::vector<std::uint8_t> pal(256 * 3);
        for (int i = 0; i < 256; ++i)
        {
            pal[i * 3 + 0] = static_cast<std::uint8_t>(i);
            pal[i * 3 + 1] = static_cast<std::uint8_t>(255 - i);
            pal[i * 3 + 2] = 0;
        }

        const auto rpiz = encodeRpiz(indices.data(), pal.data(), w, h, true);
        const auto *zh = reinterpret_cast<const rivendata::RpizHeader *>(rpiz.data());
        check(rivendata::isRpiz(*zh), "the .rpiz header validates");
        check(zh->width == w && zh->height == h,
              "the .rpiz keeps the ORIGINAL resolution");
        check(rpiz.size() == sizeof(rivendata::RpizHeader) + rivendata::kPaletteBytes
                                 + zh->dataBytes,
              "the .rpiz is header plus palette plus payload");

        // The palette must survive as RGB555 with alpha set.
        const auto *palOut = reinterpret_cast<const std::uint16_t *>(
            rpiz.data() + sizeof(rivendata::RpizHeader));
        check((palOut[0] & 0x8000) != 0, "palette entries are opaque");
        check((palOut[255] & 0x1F) == 31, "palette red survives the 8->5 bit conversion");

        // And the indices must come back byte-exact through the BIOS format.
        if (zh->compression == static_cast<std::uint8_t>(rivendata::ImageCompression::Lz77))
        {
            const auto back = decompressLz77(
                rpiz.data() + sizeof(rivendata::RpizHeader) + rivendata::kPaletteBytes,
                zh->dataBytes);
            check(back == indices, "the .rpiz index plane round-trips through lz77");
        }
    }

    // --- against real game data, when available ----------------------------
    if (const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
        dataEnv != nullptr && dataEnv[0] != '\0')
    {
        const Source src = detectSource(dataEnv);
        const StackSource *aspit = src.find(rivendata::StackId::Aspit);
        if (aspit != nullptr)
        {
            ArchiveSet set;
            std::vector<std::string> failures;
            set.openAll(aspit->dataArchives, failures);

            const auto ids = set.resourceIds("tBMP");
            check(!ids.empty(), "aspit has tBMP resources");

            std::size_t rawTotal = 0, packedTotal = 0;
            int decoded = 0;
            for (std::size_t i = 0; i < ids.size() && i < 24; ++i)
            {
                Bitmap bmp = set.readBitmap(ids[i]);
                if (!bmp.valid())
                    continue;
                ++decoded;
                check(bmp.width() > 0 && bmp.height() > 0,
                      "a real tBMP decodes to a sane size");

                if (bmp.indices() != nullptr && bmp.palette() != nullptr)
                {
                    const std::size_t plane =
                        static_cast<std::size_t>(bmp.width()) * bmp.height();
                    const auto packed = compressLz77(bmp.indices(), plane);
                    const auto back = decompressLz77(packed.data(), packed.size());
                    check(back.size() == plane,
                          "a real index plane round-trips to its original size");
                    check(std::memcmp(back.data(), bmp.indices(), plane) == 0,
                          "a real index plane round-trips byte-exactly");
                    rawTotal += plane;
                    packedTotal += packed.size();
                }
            }
            check(decoded > 0, "at least one real tBMP decoded");
            if (rawTotal > 0)
                std::printf("  real aspit art: %zu -> %zu bytes (%.1f%% of raw 8bpp)\n",
                            rawTotal, packedTotal, 100.0 * packedTotal / rawTotal);
        }
    }
    else
    {
        std::printf("  (RIVEN_TEST_DATA unset: skipped the real-data checks)\n");
    }

    std::printf("image: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
