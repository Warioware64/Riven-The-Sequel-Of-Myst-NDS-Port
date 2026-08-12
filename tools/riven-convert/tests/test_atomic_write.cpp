// Atomic output writing and the up-to-date check that resumability rests on.
//
// If writeFileAtomic can ever leave a partial file under its final name, then
// "the output exists" stops meaning "the output is complete", and the skip rule
// silently ships truncated assets to the DS. That makes these cheap checks
// worth more than they look.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "riven/AtomicWrite.hpp"

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

    /// Count entries whose name ends in .tmp, recursively.
    int countTemps(const fs::path &dir)
    {
        int n = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(dir, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (it->is_regular_file(ec) && it->path().extension() == ".tmp")
                ++n;
        }
        return n;
    }
} // namespace

int main()
{
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) / "riven-atomic-test";
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const std::vector<std::uint8_t> payload{1, 2, 3, 4, 5, 6, 7, 8};
    std::string err;

    // --- basic write, including creating parent directories -----------------
    {
        const fs::path out = root / "a" / "b" / "thing.rpic";
        check(writeFileAtomic(out, payload, err), "writes through missing directories");
        check(err.empty(), "no error text on success");
        check(fs::exists(out), "the output exists under its final name");
        check(readAll(out) == payload, "contents round-trip");
        check(countTemps(root) == 0, "no .tmp is left behind on success");
    }

    // --- overwrite ----------------------------------------------------------
    {
        const fs::path out = root / "a" / "b" / "thing.rpic";
        const std::vector<std::uint8_t> second{9, 9, 9};
        check(writeFileAtomic(out, second, err), "overwrites an existing file");
        check(readAll(out) == second, "overwrite replaces the contents entirely");
        check(countTemps(root) == 0, "no .tmp is left behind on overwrite");
    }

    // --- empty payload ------------------------------------------------------
    {
        const fs::path out = root / "empty.bin";
        check(writeFileAtomic(out, {}, err), "an empty write succeeds");
        check(fs::exists(out) && fs::file_size(out) == 0, "and produces an empty file");
    }

    // --- failure leaves nothing --------------------------------------------
    {
        // A path whose parent cannot exist: "file/child" where file is a file.
        const fs::path blocker = root / "blocker";
        check(writeFileAtomic(blocker, payload, err), "set up the blocker file");
        const fs::path out = blocker / "nested" / "thing.bin";

        const bool ok = writeFileAtomic(out, payload, err);
        check(!ok, "writing under a regular file fails");
        check(!err.empty(), "and reports why");
        check(!fs::exists(out), "and leaves no output");
    }

    // --- isUpToDate ---------------------------------------------------------
    {
        const fs::path src = root / "source.mhk";
        const fs::path out = root / "derived.bin";

        check(writeFileAtomic(src, payload, err), "create the source");
        check(!isUpToDate(out, src), "a missing output is never up to date");

        // Sleep past filesystem timestamp granularity so the ordering is real
        // rather than an artefact of both landing in the same tick.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(writeFileAtomic(out, payload, err), "create the output");
        check(isUpToDate(out, src), "an output written after its source is up to date");

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(writeFileAtomic(src, payload, err), "touch the source");
        check(!isUpToDate(out, src), "a newer source invalidates the output");

        check(!isUpToDate(out, root / "does-not-exist"),
              "an unreadable source means redo, not skip");
    }

    // --- cleanStaleTempFiles ------------------------------------------------
    {
        const fs::path dir = root / "stale";
        fs::create_directories(dir / "deep", ec);
        std::string e;
        (void)writeFileAtomic(dir / "one.bin.tmp", payload, e);
        (void)writeFileAtomic(dir / "deep" / "two.rpic.tmp", payload, e);
        (void)writeFileAtomic(dir / "keep.bin", payload, e);

        check(countTemps(dir) == 2, "the stale temps are there to begin with");
        check(cleanStaleTempFiles(dir) == 2, "both stale temps are removed");
        check(countTemps(dir) == 0, "and none remain");
        check(fs::exists(dir / "keep.bin"), "a real output is not touched");
        check(cleanStaleTempFiles(root / "no-such-dir") == 0,
              "cleaning a missing directory is a no-op, not an error");
    }

    fs::remove_all(root, ec);

    if (g_failures == 0)
        std::printf("atomic write: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
