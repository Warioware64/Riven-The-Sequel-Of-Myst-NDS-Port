// What a re-run decides to redo.
//
// "Work already done is skipped unless Options::force" (Converter.hpp:18) is
// what makes a cancelled conversion resumable, and it is also the rule most
// able to go wrong quietly: a stage that skips something it should have redone
// leaves a card that LOOKS converted and that the ROM rejects, with nothing
// saying so. The specific failure this pins down did exactly that.
//
// A stack file must be redone when its schema version is not this build's, and
// that decision has to be per FILE. Every other stage stamps a directory with
// the format version it wrote, and can, because pics/, pics_hi/, video/ and
// sound/ each get one directory per stack -- but all eight stack files share
// stacks/. Stamping that shared directory after converting one stack marks the
// other seven current, and no later run ever redoes them.
//
// Needs a real install, and skips cleanly without one.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "RivenData.hpp"
#include "riven/Converter.hpp"
#include "riven/Layout.hpp"
#include "riven/Options.hpp"
#include "riven/Progress.hpp"

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

    /// Says nothing. The conversions here are expected to be quiet and their
    /// output is not what is under test.
    class SilentSink : public ProgressSink
    {
    public:
        void log(Severity, std::string_view, std::string_view) override {}
        void progress(std::uint64_t, std::uint64_t, std::string_view,
                      std::string_view) override
        {
        }
    };

    fs::path stackFile(const fs::path &dest, const char *name)
    {
        return dest / "_nds" / "riven_nds" / "data" / "stacks" / (std::string(name) + ".bin");
    }

    /// The ARM9's own acceptance test, which is the whole point of the header
    /// (StackFile.cpp:35-42).
    bool readable(const fs::path &p)
    {
        std::FILE *f = std::fopen(p.string().c_str(), "rb");
        if (f == nullptr)
            return false;
        rivendata::StackFileHeader hdr{};
        const bool got = std::fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
        std::fclose(f);
        return got && rivendata::headerLooksValid(hdr);
    }

    /// Rewrite the schema version in place, which is what a file left behind by
    /// an older build of the converter looks like.
    bool ageToPreviousSchema(const fs::path &p)
    {
        std::FILE *f = std::fopen(p.string().c_str(), "r+b");
        if (f == nullptr)
            return false;
        rivendata::StackFileHeader hdr{};
        if (std::fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
        {
            std::fclose(f);
            return false;
        }
        hdr.schemaVersion = static_cast<std::uint16_t>(rivendata::kSchemaVersion - 1);
        std::fseek(f, 0, SEEK_SET);
        const bool ok = std::fwrite(&hdr, 1, sizeof(hdr), f) == sizeof(hdr);
        std::fclose(f);
        return ok;
    }

    ConversionResult convertCards(const fs::path &source, const fs::path &dest,
                                  std::initializer_list<rivendata::StackId> stacks)
    {
        Options opts;
        opts.source = source;
        opts.dest = dest;
        opts.images = opts.hires = opts.water = opts.audio = opts.video = false;
        opts.cursors = opts.extras = false;
        for (const rivendata::StackId id : stacks)
            opts.stacks.insert(id);

        SilentSink sink;
        CancelToken cancel;
        return Converter().run(opts, sink, cancel);
    }
} // namespace

int main()
{
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("resume: skipped (set RIVEN_TEST_DATA to a Riven install)\n");
        return 0;
    }

    const Source src = detectSource(dataEnv);
    if (!src.valid() || src.find(rivendata::StackId::Aspit) == nullptr
        || src.find(rivendata::StackId::Tspit) == nullptr)
    {
        std::printf("resume: skipped (this install has no aspit and tspit)\n");
        return 0;
    }

    const fs::path dest = fs::temp_directory_path() / "riven_resume_test";
    fs::remove_all(dest);

    convertCards(src.root, dest, {rivendata::StackId::Aspit, rivendata::StackId::Tspit});
    const fs::path aspit = stackFile(dest, "aspit");
    const fs::path tspit = stackFile(dest, "tspit");
    check(readable(aspit) && readable(tspit), "both stacks convert");

    // A file this build cannot read has to be redone even though its mtime is
    // newer than the archives it came from -- which is the only thing the
    // freshness check would otherwise look at.
    check(ageToPreviousSchema(tspit), "aged tspit to the previous schema");
    check(!readable(tspit), "the aged file is rejected");

    // THE REGRESSION. A run restricted to aspit must not mark tspit current.
    // Under the directory-stamp version this wrote stacks/.format, and the run
    // below then skipped tspit for good.
    convertCards(src.root, dest, {rivendata::StackId::Aspit});
    check(!readable(tspit), "a run that skipped tspit did not touch it");

    const ConversionResult r = convertCards(src.root, dest, {rivendata::StackId::Tspit});
    check(readable(tspit), "the stale stack is redone by the next run");
    check(r.cardsWritten > 0, "and it was rewritten, not counted as skipped");

    // The other half of the rule: a file that IS current stays skipped, or a
    // re-run would cost as much as a first run.
    const ConversionResult again = convertCards(src.root, dest, {rivendata::StackId::Tspit});
    check(again.skipped > 0 && again.cardsWritten == 0,
          "a current stack file is still skipped");

    fs::remove_all(dest);

    std::printf("resume: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
