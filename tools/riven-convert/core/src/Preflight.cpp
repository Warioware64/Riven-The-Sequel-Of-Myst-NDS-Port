#include "riven/Preflight.hpp"

#include <array>
#include <cstdio>

#include "RivenImage.hpp"
#include "riven/Archive.hpp"

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    // Measured, not guessed. A .rpic is exactly the DS view size at 2 bytes a
    // pixel; a .rpiz is the original 608x392 index plane plus a 512-byte
    // palette, and test_image measured LZ77 at ~55% of raw across real aspit
    // art. Card data was measured from a full conversion of the readable
    // stacks; it is small enough that being 30% out changes nothing.
    constexpr std::uint64_t kRpicBytes =
        static_cast<std::uint64_t>(rivendata::kViewW) * rivendata::kViewH * 2 + 16;
    constexpr double kLz77Ratio = 0.55;
    constexpr std::uint64_t kRpizBytes = static_cast<std::uint64_t>(
        static_cast<double>(rivendata::kCardW) * rivendata::kCardH * kLz77Ratio + 16
        + rivendata::kPaletteBytes);
    constexpr std::uint64_t kCardBytes = 3 * 1024;   ///< per card, in stacks/<stack>.bin
    constexpr std::uint64_t kEffectBytes = 24 * 1024; ///< per SFXE
    /// Per tWAV. Measured: converting every sound of a 5-CD install produces
    /// 73 MB across 312 files. Most of that is the stereo ambient tracks, which
    /// halve as they are downmixed; the mono effects pass through at their
    /// source size.
    constexpr std::uint64_t kSoundBytes = 245 * 1024;
    /// Per tMOV. Measured: converting every movie of a 5-CD install produces
    /// 464.6 MB across 1055 files. Video is 355 MB of that and the interleaved
    /// audio 108 MB -- see docs/video.md, which also explains why the audio
    /// figure is a floor no codec setting can move.
    constexpr std::uint64_t kMovieBytes = 451 * 1024;

    std::string joinStackNames(const std::vector<rivendata::StackId> &ids)
    {
        std::string s;
        for (const auto id : ids)
        {
            if (!s.empty())
                s += ", ";
            s += rivendata::stackName(id);
        }
        return s;
    }

    /// True when `child` is inside `parent` (or is `parent`).
    bool isInside(const fs::path &child, const fs::path &parent)
    {
        std::error_code ec;
        const fs::path c = fs::weakly_canonical(child, ec);
        if (ec)
            return false;
        const fs::path p = fs::weakly_canonical(parent, ec);
        if (ec)
            return false;

        auto ci = c.begin();
        auto pi = p.begin();
        for (; pi != p.end(); ++pi, ++ci)
        {
            if (ci == c.end() || *ci != *pi)
                return false;
        }
        return true;
    }
} // namespace

std::string humanBytes(std::uint64_t bytes)
{
    char buf[64];
    const double b = static_cast<double>(bytes);
    if (bytes < 1024ull)
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    else if (bytes < 1024ull * 1024)
        std::snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof(buf), "%.1f MB", b / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof(buf), "%.2f GB", b / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string SourceInfo::summary() const
{
    if (!source.valid())
        return "No Riven data found here.";

    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%s: %zu stacks, %d cards, %d images, %d movies, %d sounds",
                  toString(source.layout), stacks.size(), totalCards, totalImages,
                  totalMovies, totalSounds);
    return buf;
}

SourceInfo scanSource(const fs::path &root)
{
    SourceInfo info;
    info.source = detectSource(root);
    if (!info.source.valid())
        return info;

    for (const auto &ss : info.source.stacks)
    {
        // Data archives first, then the sound ones, which is both the priority
        // order ScummVM searches in and exactly the set the audio stage builds
        // -- so a tWAV that exists in both is counted once here and converted
        // once there, and the census matches what the run reports.
        ArchiveSet set;
        std::vector<std::string> failures;
        set.openAll(ss.dataArchives, failures);
        set.openAll(ss.soundArchives, failures);
        for (auto &f : failures)
            info.unreadable.push_back(std::move(f));

        if (set.empty())
            continue;

        StackInfo si;
        si.id = ss.id;
        // Counted across the whole set including duplicates: a patched resource
        // is still one asset to convert, but the census is about how much work
        // there is, and reading headers is the cheap part.
        si.cards = static_cast<int>(set.resourceIds("CARD").size());
        si.images = static_cast<int>(set.resourceIds("tBMP").size());
        si.movies = static_cast<int>(set.resourceIds("tMOV").size());
        si.sounds = static_cast<int>(set.resourceIds("tWAV").size());
        si.effects = static_cast<int>(set.resourceIds("SFXE").size());

        std::error_code ec;
        for (const auto &p : ss.dataArchives)
        {
            const auto sz = fs::file_size(p, ec);
            if (!ec)
                si.archiveBytes += sz;
            ec.clear();
        }

        info.totalCards += si.cards;
        info.totalImages += si.images;
        info.totalMovies += si.movies;
        info.totalSounds += si.sounds;
        info.totalEffects += si.effects;
        info.stacks.push_back(si);
    }
    return info;
}

std::uint64_t estimateOutput(const SourceInfo &info, const Options &opts)
{
    std::uint64_t total = 0;
    for (const auto &s : info.stacks)
    {
        if (!opts.stacks.empty() && opts.stacks.find(s.id) == opts.stacks.end())
            continue;
        if (opts.cards)
            total += static_cast<std::uint64_t>(s.cards) * kCardBytes;
        if (opts.images)
            total += static_cast<std::uint64_t>(s.images) * kRpicBytes;
        if (opts.hires)
            total += static_cast<std::uint64_t>(s.images) * kRpizBytes;
        if (opts.water)
            total += static_cast<std::uint64_t>(s.effects) * kEffectBytes;
        if (opts.audio)
            total += static_cast<std::uint64_t>(s.sounds) * kSoundBytes;
        if (opts.video)
            total += static_cast<std::uint64_t>(s.movies) * kMovieBytes;
    }
    return total;
}

std::vector<Check> runChecks(const SourceInfo &info, const Options &opts)
{
    std::vector<Check> checks;

    // --- source -------------------------------------------------------------
    if (!info.ok())
    {
        checks.push_back({Check::Level::Fail, "Riven data",
                          "Nothing readable here. Point this at the folder holding "
                          "Data/ and All/, or at a DVD install."});
        return checks; // nothing else can be judged
    }
    checks.push_back({Check::Level::Ok, "Riven data", info.summary()});

    if (!info.source.missingStacks.empty())
    {
        checks.push_back({Check::Level::Warn, "Incomplete game",
                          "These stacks are absent or unreadable and will be skipped: "
                              + joinStackNames(info.source.missingStacks)
                              + ". An interrupted copy can leave a full-size file of "
                                "zeroes, which looks present but is not."});
    }
    if (!info.unreadable.empty())
    {
        std::string names;
        for (const auto &n : info.unreadable)
        {
            if (!names.empty())
                names += ", ";
            names += n;
        }
        checks.push_back({Check::Level::Warn, "Damaged archives", names});
    }

    // --- what to do ---------------------------------------------------------
    if (opts.empty())
    {
        checks.push_back({Check::Level::Fail, "Nothing selected",
                          "Choose at least one thing to convert."});
    }

    const std::uint64_t estimate = estimateOutput(info, opts);
    checks.push_back({Check::Level::Ok, "Estimated output", "about " + humanBytes(estimate)});

    // --- destination --------------------------------------------------------
    std::error_code ec;
    if (opts.dest.empty())
    {
        checks.push_back({Check::Level::Fail, "Destination", "Choose where to write."});
        return checks;
    }

    // Writing inside the source would feed the converter its own output on a
    // second run, and on a card-root destination it would also mean writing
    // into the game folder the user asked us to read.
    if (isInside(opts.dest, opts.source))
    {
        checks.push_back({Check::Level::Fail, "Destination",
                          "The destination is inside the source folder. Choose a "
                          "different place, or the converter will end up reading "
                          "its own output."});
        return checks;
    }

    const fs::path probe = fs::exists(opts.dest, ec) ? opts.dest : opts.dest.parent_path();
    if (!fs::exists(probe, ec))
    {
        checks.push_back({Check::Level::Fail, "Destination",
                          "'" + opts.dest.string() + "' does not exist."});
        return checks;
    }
    checks.push_back({Check::Level::Ok, "Destination", opts.dest.string()});

    // --- free space ---------------------------------------------------------
    const auto space = fs::space(probe, ec);
    if (ec)
    {
        checks.push_back({Check::Level::Warn, "Free space", "Could not be determined."});
    }
    else if (space.available < estimate)
    {
        checks.push_back({Check::Level::Fail, "Free space",
                          humanBytes(space.available) + " available, about "
                              + humanBytes(estimate) + " needed."});
    }
    else if (space.available < estimate + estimate / 5)
    {
        checks.push_back({Check::Level::Warn, "Free space",
                          "Tight: " + humanBytes(space.available) + " available for about "
                              + humanBytes(estimate) + "."});
    }
    else
    {
        checks.push_back({Check::Level::Ok, "Free space", humanBytes(space.available)
                                                              + " available"});
    }

    // --- already converted --------------------------------------------------
    const fs::path dataDir = opts.dest / "_nds" / "riven_nds" / "data";
    if (fs::exists(dataDir / "stacks", ec))
    {
        checks.push_back({Check::Level::Warn, "Already converted",
                          opts.force ? "Rebuild everything is on: all existing files "
                                       "will be redone."
                                     : "Existing output will be kept where it is still "
                                       "up to date. Turn on Rebuild everything to redo it."});
    }

    return checks;
}

bool checksPass(const std::vector<Check> &checks)
{
    for (const auto &c : checks)
        if (c.level == Check::Level::Fail)
            return false;
    return true;
}

} // namespace riven
