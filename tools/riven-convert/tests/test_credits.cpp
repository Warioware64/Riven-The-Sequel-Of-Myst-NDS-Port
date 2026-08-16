// The credits stage: the geometry rule, and the nineteen files it writes.
//
// The geometry is the half worth testing without game data, because it is a
// decision rather than a transcription. Every other picture in the port is
// scaled by the card view's 256/608; the credits are scaled to fit 192 rows
// instead, and if that rule ever drifts the roll either overflows the screen or
// scrolls at the wrong speed -- neither of which looks like a bug on hardware,
// they just look like Riven's credits being slightly wrong.
//
// The other half needs extras.MHK and so runs only under RIVEN_TEST_DATA, in
// the same shape the topbg and image tests use.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenImage.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/Credits.hpp"
#include "riven/Installer.hpp"
#include "riven/Layout.hpp"

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

    /// Read a .rpic header back the way the DS does.
    bool readRpicHeader(const fs::path &p, rivendata::RpicHeader &hdr)
    {
        std::FILE *f = std::fopen(p.string().c_str(), "rb");
        if (f == nullptr)
            return false;
        const std::size_t got = std::fread(&hdr, 1, sizeof(hdr), f);
        std::fclose(f);
        return got == sizeof(hdr);
    }
} // namespace

int main()
{
    // --- the geometry rule ------------------------------------------------
    int w = 0;
    int h = 0;

    // Riven's own credits: 360x392, and 176x192 is the answer the whole stage
    // is built around. Spelled as a literal on purpose -- deriving it here from
    // the same expression the implementation uses would test nothing.
    creditsSize(360, 392, w, h);
    check(w == 176 && h == 192, "360x392 -> 176x192");

    // The aspect is kept, which is what stops the names being stretched.
    check(w * 392 >= 360 * h - 392 && w * 392 <= 360 * h + 392,
          "the aspect ratio survives the scale");

    // Never off the screen, on either axis. A wide image loses rows rather
    // than columns, because the width is what the scroller writes per step.
    creditsSize(1000, 100, w, h);
    check(w <= kCreditsViewW && h <= kCreditsViewH, "a wide image still fits");
    creditsSize(100, 1000, w, h);
    check(w <= kCreditsViewW && h <= kCreditsViewH, "a tall image still fits");

    // Smaller than the screen is left alone rather than blown up: upscaling
    // credits art would be softer than the original for no gain.
    creditsSize(120, 100, w, h);
    check(w == 120 && h == 100, "a small image is not upscaled");

    // Degenerate input reports nothing rather than a 1x1 file.
    creditsSize(0, 0, w, h);
    check(w == 0 && h == 0, "a zero-sized source is refused");

    // --- the ids ----------------------------------------------------------
    // ScummVM's kRivenCreditsZeroImage..kRivenCreditsLastImage, and the split
    // between the two that are held and the seventeen that scroll.
    check(kCreditsFirstId == 302 && kCreditsLastId == 320, "tBMP 302..320");
    check(kCreditsCount == 19, "nineteen images");
    check(kCreditsFirstScrollId == 304, "302 and 303 are the stills");

    // --- the real archive, when there is one ------------------------------
    if (const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
        dataEnv != nullptr && dataEnv[0] != '\0')
    {
        const Source src = detectSource(dataEnv);

        // extras.MHK is loose on an installed copy and inside the installer's
        // own archive on a disc, and the converter takes it either way
        // (Converter.cpp:333-348). The test has to as well, or an install of
        // the second kind silently skips the only checks that touch real art.
        const fs::path unpacked = fs::temp_directory_path() / "riven_credits_extras.mhk";
        fs::path extras = src.extrasArchive;
        if (extras.empty() && !src.installerArchive.empty())
        {
            InstallerArchive installer = InstallerArchive::open(src.installerArchive);
            std::string err;
            const auto bytes = installer.isOpen() ? installer.read("extras.mhk", err)
                                                  : std::vector<std::uint8_t>();
            if (!bytes.empty() && writeFileAtomic(unpacked, bytes, err))
                extras = unpacked;
        }

        if (extras.empty())
        {
            std::printf("  (this install has no reachable extras.mhk: skipped)\n");
        }
        else
        {
            const fs::path outDir = fs::temp_directory_path() / "riven_credits_test";
            fs::remove_all(outDir);

            std::vector<std::string> warnings;
            const auto r = convertCredits(extras, outDir, warnings);
            for (const std::string &warning : warnings)
                std::printf("  warning: %s\n", warning.c_str());

            check(r.ok, "the real extras.mhk converts (" + r.error + ")");
            check(r.images == kCreditsCount, "all nineteen decoded");
            check(r.width == 176 && r.height == 192,
                  "the shipped credits really are 360x392");

            for (int id = kCreditsFirstId; id <= kCreditsLastId; ++id)
            {
                const fs::path p = outDir / (std::to_string(id) + ".rpic");
                rivendata::RpicHeader hdr{};
                if (!readRpicHeader(p, hdr))
                {
                    check(false, std::to_string(id) + ".rpic exists and reads");
                    continue;
                }
                check(rivendata::isRpic(hdr), std::to_string(id) + ": magic and version");
                check(hdr.width == r.width && hdr.height == r.height,
                      std::to_string(id) + ": the size the stage reported");
                // The source size is the tBMP's own, not the resampled one --
                // the header's contract, and the only thing a later reader
                // could use to work out the scale that was applied.
                check(hdr.srcWidth == 360 && hdr.srcHeight == 392,
                      std::to_string(id) + ": srcWidth/srcHeight are the tBMP's");
                // Every image has to fit the ARM9's reader, which caps a
                // picture at kMaxRpicH rows and decodes it whole.
                check(hdr.height <= 1024, std::to_string(id) + ": within kMaxRpicH");
            }

            std::printf("  %s -> %d images, %zu bytes, %dx%d each\n",
                        extras.string().c_str(), r.images, r.bytes, r.width, r.height);
            fs::remove_all(outDir);
        }
        fs::remove(unpacked);

        // A source that is not an archive at all must fail with a message
        // rather than write a partial roll.
        std::vector<std::string> warnings;
        const auto bad = convertCredits(fs::path(dataEnv) / "nope.mhk",
                                        fs::temp_directory_path() / "riven_credits_bad",
                                        warnings);
        check(!bad.ok && !bad.error.empty(), "a missing archive fails with a message");
        fs::remove_all(fs::temp_directory_path() / "riven_credits_bad");
    }
    else
    {
        std::printf("  (RIVEN_TEST_DATA unset: skipped the real-data checks)\n");
    }

    std::printf("credits: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
