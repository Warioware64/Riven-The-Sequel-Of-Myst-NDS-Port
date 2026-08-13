// The sprite-set reader the DS uses, compiled for the host.
//
// Not a converter test. It compiles source/render/RcurFile.cpp -- which is free
// of <nds.h> so that it can be -- writes a set with the converter, reads it back
// the way the ARM9 reads it, and checks that what came out is what went in. On
// hardware nothing can check that.
//
// The claim it is really here to defend is the PIXEL ORDER. A .rcur cel is in DS
// OBJ 1D tile order, not raster, so that the ARM9 can hand it straight to
// NEA_Hw2DOBJAssetLoadGfx. Get that wrong and the sprite still has the right
// colours in the right proportions -- it is just scrambled into 8x8 blocks, and
// on a 16x16 cursor that is subtle enough to survive a code review and obvious
// only on hardware. So the test reconstructs a raster image from the tiles and
// compares it against the raster the encoder was given.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenCursor.hpp"
#include "render/RcurFile.hpp"
#include "riven/CursorPipeline.hpp"

namespace fs = std::filesystem;
using namespace riven;
using namespace rivendata;

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

    /// Where the encoder puts pixel (x, y). Restated here on purpose: if this
    /// and the encoder ever disagree, one of them is wrong and the test is the
    /// only thing that would say so.
    std::size_t tileOffset(int x, int y, int celW)
    {
        const int tilesAcross = celW / 8;
        const std::size_t tile =
            static_cast<std::size_t>(y / 8) * tilesAcross + static_cast<std::size_t>(x / 8);
        return tile * 64 + static_cast<std::size_t>(y % 8) * 8 + static_cast<std::size_t>(x % 8);
    }

    /// A cel with a different colour in every 8x8 tile and a recognisable shape
    /// inside each, so a tile-order mistake cannot look like a pass.
    RcurSourceCel makeCel(std::uint16_t id, int w, int h)
    {
        RcurSourceCel c;
        c.id = id;
        c.hotX = 3;
        c.hotY = 5;
        c.drawW = w;
        c.drawH = h;
        c.rgb.assign(static_cast<std::size_t>(w) * h * 3, 0);
        c.opaque.assign(static_cast<std::size_t>(w) * h, 0);

        const int tilesAcross = w / 8;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
            {
                const std::size_t at = static_cast<std::size_t>(y) * w + x;
                const int tile = (y / 8) * tilesAcross + (x / 8);
                // A transparent diagonal through every tile, so the mask is
                // exercised as well as the colours.
                if ((x % 8) == (y % 8))
                    continue;
                c.opaque[at] = 255;
                c.rgb[at * 3 + 0] = static_cast<std::uint8_t>(8 + tile * 40);
                c.rgb[at * 3 + 1] = static_cast<std::uint8_t>(200 - tile * 24);
                c.rgb[at * 3 + 2] = static_cast<std::uint8_t>(40 + (x % 8) * 24);
            }
        return c;
    }
} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "riven-rcur-arm9-test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path out = dir / "cursors.rcur";

    const int celW = 16, celH = 16;
    std::vector<RcurSourceCel> cels = {makeCel(3000, celW, celH), makeCel(2003, celW, celH),
                                       makeCel(9000, celW, celH)};

    std::vector<std::string> warnings;
    const auto bytes =
        encodeRcur(cels, celW, celH, kCursorPaletteBase, kCursorPaletteMax, warnings);
    check(warnings.empty(), "a three-cel set needs no palette reduction");
    check(!bytes.empty(), "the set encodes to something");

    {
        std::FILE *f = std::fopen(out.string().c_str(), "wb");
        check(f != nullptr, "the test file can be written");
        if (f != nullptr)
        {
            std::fwrite(bytes.data(), 1, bytes.size(), f);
            std::fclose(f);
        }
    }

    // --- read it back the way the DS does -----------------------------------
    rivenrt::RcurFile file;
    std::string error;
    check(file.load(out.string(), error), "the DS reader loads it: " + error);
    check(file.celWidth() == celW && file.celHeight() == celH, "the cel size survives");
    check(file.celCount() == cels.size(), "every cel is there");
    check(file.paletteBase() == kCursorPaletteBase, "the palette base survives");
    check(file.paletteCount() > 0 && file.paletteCount() <= kCursorPaletteMax,
          "the palette fits its slice");

    for (const RcurSourceCel &src : cels)
    {
        const RcurCel *cel = file.find(src.id);
        if (cel == nullptr)
        {
            check(false, "cel " + std::to_string(src.id) + " is findable by id");
            continue;
        }
        check(cel->hotX == src.hotX && cel->hotY == src.hotY,
              "cel " + std::to_string(src.id) + " keeps its hot point");

        const std::uint8_t *pixels = file.pixels(*cel);
        if (pixels == nullptr)
        {
            check(false, "cel " + std::to_string(src.id) + " has pixels");
            continue;
        }

        // Reconstruct the raster from the tiles and compare against the source.
        // Colours go through RGB555 and a palette, so they are compared as
        // palette entries rather than as bytes -- what must match exactly is
        // WHICH pixel got WHICH colour.
        int mismatched = 0;
        int transparentWrong = 0;
        for (int y = 0; y < celH; ++y)
            for (int x = 0; x < celW; ++x)
            {
                const std::size_t at = static_cast<std::size_t>(y) * celW + x;
                const std::uint8_t index = pixels[tileOffset(x, y, celW)];

                if (src.opaque[at] == 0)
                {
                    if (index != 0)
                        ++transparentWrong;
                    continue;
                }
                if (index == 0)
                {
                    ++mismatched;
                    continue;
                }

                const std::uint16_t got = file.palette()[index - file.paletteBase()];
                const int r5 = (src.rgb[at * 3 + 0] * 31 + 127) / 255;
                const int g5 = (src.rgb[at * 3 + 1] * 31 + 127) / 255;
                const int b5 = (src.rgb[at * 3 + 2] * 31 + 127) / 255;
                if (got != static_cast<std::uint16_t>(r5 | (g5 << 5) | (b5 << 10)))
                    ++mismatched;
            }

        check(transparentWrong == 0,
              "cel " + std::to_string(src.id) + ": every masked pixel is index 0");
        check(mismatched == 0, "cel " + std::to_string(src.id)
                                   + ": every pixel is where the encoder put it ("
                                   + std::to_string(mismatched) + " wrong)");
    }

    // --- a set that is not one --------------------------------------------
    {
        const fs::path bad = dir / "truncated.rcur";
        std::FILE *f = std::fopen(bad.string().c_str(), "wb");
        if (f != nullptr)
        {
            std::fwrite(bytes.data(), 1, bytes.size() / 2, f);
            std::fclose(f);
        }
        rivenrt::RcurFile broken;
        std::string e;
        check(!broken.load(bad.string(), e) && !e.empty(),
              "a truncated set is refused rather than read short");
        check(!broken.loaded(), "and leaves nothing loaded behind it");

        rivenrt::RcurFile missing;
        check(!missing.load((dir / "nope.rcur").string(), e),
              "a missing set is refused");
    }

    fs::remove_all(dir, ec);

    std::printf("rcur arm9: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
