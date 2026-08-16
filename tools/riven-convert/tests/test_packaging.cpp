// The two things the run does after every stage: put the game itself on the
// card, and pack the finished card into an image an emulator can mount.
//
// The wizard's last promise is that the thing boots. That rests on what is
// checked here: that both are refused BEFORE the run when what they need is not
// there, that their bytes are in the free-space estimate, and that the .nds
// that lands is byte-identical -- a ROM copied 99% of the way is a card that
// boots into a hang.
//
// The copy itself needs a real source to run against, because Converter::run
// scans one before it does anything; that half is skipped without
// RIVEN_TEST_DATA, exactly as test_resume does. The image's own packing is not
// exercised here: it is three mtools invocations, and asserting that mcopy
// copies would be testing mtools.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "riven/AtomicWrite.hpp"
#include "riven/CardImage.hpp"
#include "riven/Converter.hpp"
#include "riven/Options.hpp"
#include "riven/Preflight.hpp"

namespace fs = std::filesystem;
using namespace riven;

namespace
{
    int g_failures = 0;

    void check(bool cond, const char *what)
    {
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }

    std::vector<std::uint8_t> readAll(const fs::path &p)
    {
        std::ifstream in(p, std::ios::binary);
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    }

    /// A stand-in .nds. Nothing reads its contents -- the point is that every
    /// byte of whatever it is comes out the other end.
    std::vector<std::uint8_t> fakeRom(std::size_t size)
    {
        std::vector<std::uint8_t> rom(size);
        for (std::size_t i = 0; i < size; ++i)
            rom[i] = static_cast<std::uint8_t>((i * 31u + 7u) & 0xFF);
        return rom;
    }

    /// Discards everything. The copy step's own logging is not what is under
    /// test and a full conversion's output would drown the failures that are.
    struct NullSink final : ProgressSink
    {
        void log(Severity, std::string_view, std::string_view) override {}
        void progress(std::uint64_t, std::uint64_t, std::string_view,
                      std::string_view) override
        {
        }
    };
} // namespace

int main()
{
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "riven-rom-copy-test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const auto rom = fakeRom(4096);
    const fs::path romPath = root / "riven-nds-port.nds";
    if (std::string e; !writeFileAtomic(romPath, rom, e))
    {
        std::fprintf(stderr, "could not lay down the test ROM: %s\n", e.c_str());
        return 1;
    }

    // --- checkRom -----------------------------------------------------------
    {
        Options o;
        check(checkRom(o).level == Check::Level::Ok,
              "with the copy off, the ROM is not a condition of the run");

        o.copyRom = true;
        check(checkRom(o).level == Check::Level::Fail,
              "asking for the copy without naming a ROM fails");

        o.romPath = root / "not-here.nds";
        check(checkRom(o).level == Check::Level::Fail, "a ROM that is not there fails");

        o.romPath = root; // a directory, not a file
        check(checkRom(o).level == Check::Level::Fail, "a directory is not a ROM");

        o.romPath = romPath;
        check(checkRom(o).level == Check::Level::Ok, "a real file passes");
        check(checkRom(o).detail.find("riven-nds-port.nds") != std::string::npos,
              "and the check says which file it will copy");
    }

    // --- the estimate -------------------------------------------------------
    {
        // An empty census, so the ROM is the only thing in the number and the
        // arithmetic cannot be confused with a stage's.
        SourceInfo info;
        Options o;
        o.cards = o.images = o.hires = o.water = o.audio = o.video = false;
        o.cursors = o.extras = false;

        check(estimateOutput(info, o) == 0, "nothing selected estimates nothing");

        o.copyRom = true;
        o.romPath = romPath;
        check(estimateOutput(info, o) == rom.size(),
              "the ROM's exact size is in the free-space estimate");

        o.romPath = root / "not-here.nds";
        check(estimateOutput(info, o) == 0,
              "a ROM that is not there adds nothing rather than guessing");
    }

    // --- empty() ------------------------------------------------------------
    {
        Options o;
        o.cards = o.images = o.hires = o.water = o.audio = o.video = false;
        o.cursors = o.extras = false;
        check(o.empty(), "no stages and no ROM is an empty run");

        o.copyRom = true;
        o.romPath = romPath;
        check(!o.empty(), "copying the ROM alone is still something to do");
    }

    // --- checkImage ---------------------------------------------------------
    {
        const bool haveTools = findImageTools().usable();

        Options o;
        o.dest = root / "card";
        check(checkImage(o).level == Check::Level::Ok,
              "with the image off, mtools is not a condition of the run");

        o.makeImage = true;
        o.imagePath = root / "riven-card.bin";
        // Without mtools every makeImage run fails this check, which is the
        // point of the check -- so the two cases are asserted separately rather
        // than the test quietly passing for the wrong reason on either machine.
        check(checkImage(o).level == (haveTools ? Check::Level::Ok : Check::Level::Fail),
              haveTools ? "a sane image path passes when mtools is installed"
                        : "the image is refused when mtools is not installed");

        if (haveTools)
        {
            o.imagePath.clear();
            check(checkImage(o).level == Check::Level::Fail,
                  "asking for an image without naming one fails");

            // Inside the folder it would be packed FROM, so it would be asked
            // to copy itself.
            o.imagePath = o.dest / "riven-card.bin";
            check(checkImage(o).level == Check::Level::Fail,
                  "an image inside the card folder is refused");
        }
    }

    // --- imageSizeFor -------------------------------------------------------
    {
        // FAT32's floor dominates anything small, which is every cards-only run.
        check(imageSizeFor(0) == imageSizeFor(1024),
              "small content all lands on the same FAT32 minimum");
        check(imageSizeFor(0) >= 33ull * 1024 * 1024,
              "and that minimum is above what mkfs.vfat will call FAT32");

        const std::uint64_t big = 3ull * 1024 * 1024 * 1024;
        check(imageSizeFor(big) > big, "a real conversion gets headroom over its size");
        check(imageSizeFor(big) < big * 2, "but not so much that it doubles it");
        check(imageSizeFor(big) % 1024 == 0, "and the size is a whole number of blocks");
    }

    // --- the copy itself ----------------------------------------------------
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || *dataEnv == '\0')
    {
        std::printf("packaging: static checks passed "
                    "(set RIVEN_TEST_DATA to a Riven install for the copy itself)\n");
        fs::remove_all(root, ec);
        return g_failures == 0 ? 0 : 1;
    }

    {
        const fs::path card = root / "card";
        fs::create_directories(card, ec);

        // Cards only: the cheapest run that still reaches the end, which is
        // where the ROM copy lives.
        Options o;
        o.source = dataEnv;
        o.dest = card;
        o.cards = true;
        o.images = o.hires = o.water = o.audio = o.video = false;
        o.cursors = o.extras = false;
        o.copyRom = true;
        o.romPath = romPath;

        NullSink sink;
        CancelToken cancel;
        Converter converter;
        const ConversionResult result = converter.run(o, sink, cancel);

        check(result.outcome == ConversionResult::Outcome::Ok, "the run finished");
        check(result.romCopied, "and reports that the game was copied");

        const fs::path landed = card / "riven-nds-port.nds";
        check(fs::exists(landed), "the .nds is at the card ROOT, beside _nds/");
        check(readAll(landed) == rom, "and is byte-identical to the source ROM");
        check(!fs::exists(Converter::dataDir(card) / "riven-nds-port.nds"),
              "and is not buried in the data folder, where no loader would find it");
        check(result.bytesWritten >= rom.size(), "its bytes are counted in the total");
    }

    // --- the copy off -------------------------------------------------------
    {
        const fs::path card = root / "card-no-rom";
        fs::create_directories(card, ec);

        Options o;
        o.source = dataEnv;
        o.dest = card;
        o.cards = true;
        o.images = o.hires = o.water = o.audio = o.video = false;
        o.cursors = o.extras = false;

        NullSink sink;
        CancelToken cancel;
        Converter converter;
        const ConversionResult result = converter.run(o, sink, cancel);

        check(result.outcome == ConversionResult::Outcome::Ok, "the run finished");
        check(!result.romCopied, "nothing was copied when nothing was asked for");
        check(!fs::exists(card / "riven-nds-port.nds"),
              "and the card root is left alone");
    }

    fs::remove_all(root, ec);

    if (g_failures == 0)
        std::printf("packaging: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
