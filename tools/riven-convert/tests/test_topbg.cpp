// The BMP reader and the top-screen background pipeline.
//
// Two things are worth checking here and neither is visible on hardware.
//
// The BMP reader has to handle BOTH shapes AUTORUN.BMP ships in -- 8bpp
// paletted on the English disc, 24bpp on the French one -- and it has to get
// the row order right, because BMPs are stored bottom-up and an un-flipped
// image looks like a *different picture*, not like a bug. So the tests build
// bitmaps whose top row differs from their bottom row and assert which is
// which.
//
// The pipeline's contract is the one the ARM9's TopBg and its palette
// partitioning depend on: 256x192, at most kTopBgColours indices, index 0
// black. Breaking any of those shows up on a DS as a top screen that has eaten
// the console's colours.

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenImage.hpp"
#include "riven/Bmp.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/Layout.hpp"
#include "riven/TopBg.hpp"

namespace fs = std::filesystem;
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

    void put16(std::vector<std::uint8_t> &v, std::uint16_t x)
    {
        v.push_back(static_cast<std::uint8_t>(x & 0xFF));
        v.push_back(static_cast<std::uint8_t>(x >> 8));
    }

    void put32(std::vector<std::uint8_t> &v, std::uint32_t x)
    {
        for (int i = 0; i < 4; ++i)
            v.push_back(static_cast<std::uint8_t>((x >> (i * 8)) & 0xFF));
    }

    /// A BITMAPFILEHEADER + BITMAPINFOHEADER with the caller's palette size.
    void putHeaders(std::vector<std::uint8_t> &v, int w, int h, int bits, std::size_t paletteEntries,
                    std::size_t pixelBytes)
    {
        const std::uint32_t offBits =
            static_cast<std::uint32_t>(14 + 40 + paletteEntries * 4);
        v.push_back('B');
        v.push_back('M');
        put32(v, offBits + static_cast<std::uint32_t>(pixelBytes));
        put16(v, 0);
        put16(v, 0);
        put32(v, offBits);

        put32(v, 40);
        put32(v, static_cast<std::uint32_t>(w));
        put32(v, static_cast<std::uint32_t>(h)); // positive: bottom-up
        put16(v, 1);
        put16(v, static_cast<std::uint16_t>(bits));
        put32(v, 0); // BI_RGB
        put32(v, static_cast<std::uint32_t>(pixelBytes));
        put32(v, 2835);
        put32(v, 2835);
        put32(v, static_cast<std::uint32_t>(paletteEntries));
        put32(v, 0);
    }

    std::size_t stride(int w, int bits) { return ((static_cast<std::size_t>(w) * bits + 31) / 32) * 4; }

    /// An 8bpp bitmap: row 0 (the TOP once decoded) is palette index 1 = red,
    /// every other row is index 2 = green.
    std::vector<std::uint8_t> make8bpp(int w, int h)
    {
        const std::size_t rowBytes = stride(w, 8);
        std::vector<std::uint8_t> v;
        putHeaders(v, w, h, 8, 256, rowBytes * static_cast<std::size_t>(h));
        for (int i = 0; i < 256; ++i)
        {
            // B, G, R, reserved
            if (i == 1) { v.push_back(0); v.push_back(0); v.push_back(255); }
            else if (i == 2) { v.push_back(0); v.push_back(255); v.push_back(0); }
            else { v.push_back(0); v.push_back(0); v.push_back(0); }
            v.push_back(0);
        }
        // Stored bottom-up, so the LAST row written is the top one.
        for (int y = h - 1; y >= 0; --y)
        {
            const std::uint8_t idx = (y == 0) ? 1 : 2;
            for (int x = 0; x < w; ++x)
                v.push_back(idx);
            for (std::size_t pad = static_cast<std::size_t>(w); pad < rowBytes; ++pad)
                v.push_back(0);
        }
        return v;
    }

    /// The same picture at 24bpp, so the two paths can be compared.
    std::vector<std::uint8_t> make24bpp(int w, int h)
    {
        const std::size_t rowBytes = stride(w, 24);
        std::vector<std::uint8_t> v;
        putHeaders(v, w, h, 24, 0, rowBytes * static_cast<std::size_t>(h));
        for (int y = h - 1; y >= 0; --y)
        {
            for (int x = 0; x < w; ++x)
            {
                if (y == 0) { v.push_back(0); v.push_back(0); v.push_back(255); }
                else { v.push_back(0); v.push_back(255); v.push_back(0); }
            }
            for (std::size_t pad = static_cast<std::size_t>(w) * 3; pad < rowBytes; ++pad)
                v.push_back(0);
        }
        return v;
    }

    void checkDecoded(const BmpImage &img, int w, int h, const char *what)
    {
        check(img.valid(), std::string(what) + ": decodes");
        if (!img.valid())
            return;
        check(img.width == w && img.height == h, std::string(what) + ": right size");
        const std::uint8_t *top = img.rgb.data();
        const std::uint8_t *below = img.rgb.data() + static_cast<std::size_t>(w) * 3;
        check(top[0] > 200 && top[1] < 60,
              std::string(what) + ": top row is the red one (bottom-up unflipped)");
        check(below[1] > 200 && below[0] < 60, std::string(what) + ": the rest is green");
    }

    /// Read a .rpiz back the way the DS does: header, palette, LZ77 payload.
    bool readRpiz(const fs::path &p, rivendata::RpizHeader &hdr,
                  std::vector<rivendata::Texel> &palette, std::vector<std::uint8_t> &indices)
    {
        std::FILE *f = std::fopen(p.string().c_str(), "rb");
        if (f == nullptr)
            return false;
        std::vector<std::uint8_t> bytes;
        std::fseek(f, 0, SEEK_END);
        bytes.resize(static_cast<std::size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        if (got != bytes.size() || bytes.size() < sizeof(hdr) + rivendata::kPaletteBytes)
            return false;

        std::memcpy(&hdr, bytes.data(), sizeof(hdr));
        palette.resize(rivendata::kPaletteEntries);
        std::memcpy(palette.data(), bytes.data() + sizeof(hdr), rivendata::kPaletteBytes);

        const std::uint8_t *payload = bytes.data() + sizeof(hdr) + rivendata::kPaletteBytes;
        if (hdr.compression == static_cast<std::uint8_t>(rivendata::ImageCompression::Lz77))
            indices = decompressLz77(payload, hdr.dataBytes);
        else
            indices.assign(payload, payload + hdr.dataBytes);
        return true;
    }

    void checkTopBg(const fs::path &out, const char *what)
    {
        rivendata::RpizHeader hdr{};
        std::vector<rivendata::Texel> palette;
        std::vector<std::uint8_t> indices;
        if (!readRpiz(out, hdr, palette, indices))
        {
            check(false, std::string(what) + ": the .rpiz reads back");
            return;
        }

        check(rivendata::isRpiz(hdr), std::string(what) + ": magic and version");
        check(hdr.width == kTopBgW && hdr.height == kTopBgH,
              std::string(what) + ": 256x192");
        check(indices.size() == static_cast<std::size_t>(kTopBgW) * kTopBgH,
              std::string(what) + ": one index per pixel");
        if (indices.empty())
            return;

        const std::uint8_t highest = *std::max_element(indices.begin(), indices.end());
        check(highest < kTopBgColours,
              std::string(what) + ": leaves BG_PALETTE_SUB[240..255] to the console");
        check(palette[0] == rivendata::makeTexel(0, 0, 0),
              std::string(what) + ": index 0 is black");
    }
} // namespace

int main()
{
    // --- the reader ------------------------------------------------------
    // 319 is deliberately odd: it is AUTORUN.BMP's real width, and it is what
    // makes every row need padding at both 8 and 24 bits.
    const auto bmp8 = make8bpp(319, 214);
    const auto bmp24 = make24bpp(319, 214);

    std::string err;
    checkDecoded(decodeBmp(bmp8.data(), bmp8.size(), err), 319, 214, "8bpp");
    check(err.empty(), "8bpp: no error reported");
    checkDecoded(decodeBmp(bmp24.data(), bmp24.size(), err), 319, 214, "24bpp");
    check(err.empty(), "24bpp: no error reported");

    // Refusals, so a format this cannot read never becomes a silently wrong
    // picture.
    decodeBmp(bmp8.data(), 8, err);
    check(!err.empty(), "a truncated file is refused");
    std::vector<std::uint8_t> notBmp = bmp8;
    notBmp[0] = 'X';
    decodeBmp(notBmp.data(), notBmp.size(), err);
    check(!err.empty(), "a bad magic is refused");
    std::vector<std::uint8_t> rle = bmp8;
    rle[14 + 16] = 1; // biCompression = BI_RLE8
    decodeBmp(rle.data(), rle.size(), err);
    check(!err.empty(), "a compressed DIB is refused rather than guessed at");

    // --- the pipeline ----------------------------------------------------
    const fs::path tmp = fs::temp_directory_path() / "riven_topbg_test";
    fs::remove_all(tmp);

    for (const auto &[name, data] :
         {std::pair<const char *, const std::vector<std::uint8_t> *>{"8bpp", &bmp8},
          std::pair<const char *, const std::vector<std::uint8_t> *>{"24bpp", &bmp24}})
    {
        const fs::path src = tmp / (std::string(name) + ".bmp");
        fs::create_directories(tmp);
        std::FILE *f = std::fopen(src.string().c_str(), "wb");
        check(f != nullptr, std::string(name) + ": wrote the temporary bitmap");
        if (f == nullptr)
            continue;
        std::fwrite(data->data(), 1, data->size(), f);
        std::fclose(f);

        const fs::path out = tmp / (std::string(name) + ".rpiz");
        const auto r = convertTopBackground(src, out);
        check(r.ok, std::string(name) + ": converts (" + r.error + ")");
        check(r.colours > 0 && r.colours <= kTopBgColours,
              std::string(name) + ": reports a usable palette size");
        if (r.ok)
            checkTopBg(out, name);
    }

    // A source that is not there at all must fail cleanly, because that is the
    // common case: the disc's autorun shell is not part of an installed copy.
    const auto missing = convertTopBackground(tmp / "nope.bmp", tmp / "nope.rpiz");
    check(!missing.ok && !missing.error.empty(), "a missing source fails with a message");

    fs::remove_all(tmp);

    // --- the real bitmap, when there is one -------------------------------
    if (const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
        dataEnv != nullptr && dataEnv[0] != '\0')
    {
        const Source src = detectSource(dataEnv);
        if (src.autorunBitmap.empty())
        {
            std::printf("  (this install has no Autorun/AUTORUN.BMP: skipped)\n");
        }
        else
        {
            const fs::path out = fs::temp_directory_path() / "riven_topbg_real.rpiz";
            const auto r = convertTopBackground(src.autorunBitmap, out);
            check(r.ok, "the real AUTORUN.BMP converts (" + r.error + ")");
            if (r.ok)
            {
                checkTopBg(out, "real");
                std::printf("  %s -> %zu bytes, %d colours\n",
                            src.autorunBitmap.string().c_str(), r.bytes, r.colours);
            }
            fs::remove(out);
        }
    }
    else
    {
        std::printf("  (RIVEN_TEST_DATA unset: skipped the real-data checks)\n");
    }

    std::printf("topbg: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
