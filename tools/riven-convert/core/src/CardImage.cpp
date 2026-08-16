#include "riven/CardImage.hpp"

#include <algorithm>
#include <vector>

#include "riven/Converter.hpp"
#include "riven/FFmpeg.hpp"
#include "riven/Preflight.hpp" // humanBytes

namespace fs = std::filesystem;

namespace riven
{
namespace
{
    /// FAT32's own floor. mkfs.vfat refuses -F 32 below 65525 clusters, and at
    /// the 512-byte sectors it picks for small volumes that lands around 33 MB.
    /// 64 MB clears it on every cluster size mkfs might choose.
    constexpr std::uint64_t kMinImageBytes = 64ull * 1024 * 1024;

    /// Slack over the estimate. Riven writes thousands of small files -- 2307
    /// cards, 556 water effects -- and FAT32 rounds every one up to a cluster,
    /// which at 32 KB clusters is up to 32 KB wasted each. A sixth covers that
    /// with room to spare, and the estimate itself is not exact either.
    constexpr std::uint64_t kHeadroomNumerator = 7;
    constexpr std::uint64_t kHeadroomDenominator = 6;

    /// mkfs.vfat takes a block count in kilobytes, so everything rounds to one.
    constexpr std::uint64_t kBlock = 1024;

    /// Run one of the tools and turn a non-zero exit into a sentence.
    bool run(const fs::path &exe, const std::vector<std::string> &argv, std::string &error)
    {
        std::string out;
        std::string childError;
        if (runCapture(exe, argv, out, childError))
            return true;

        error = exe.filename().string() + " failed";
        if (!childError.empty())
            error += ": " + childError;
        return false;
    }
} // namespace

std::string ImageTools::missing() const
{
    std::string names;
    const auto note = [&names](const fs::path &p, const char *name) {
        if (!p.empty())
            return;
        if (!names.empty())
            names += ", ";
        names += name;
    };
    note(mkfs, "mkfs.vfat");
    note(mcopy, "mcopy");
    note(mmd, "mmd");
    return names;
}

ImageTools findImageTools()
{
    ImageTools tools;
    // mkfs.vfat and mkfs.fat are the same program; which name is installed
    // depends on the dosfstools version and the distribution.
    tools.mkfs = findOnPath("mkfs.vfat");
    if (tools.mkfs.empty())
        tools.mkfs = findOnPath("mkfs.fat");
    tools.mcopy = findOnPath("mcopy");
    tools.mmd = findOnPath("mmd");
    return tools;
}

std::uint64_t imageSizeFor(std::uint64_t contentBytes)
{
    std::uint64_t size = contentBytes / kHeadroomDenominator * kHeadroomNumerator;
    size = std::max(size, kMinImageBytes);
    // Up to a whole kilobyte, because that is mkfs.vfat's unit and rounding
    // down would hand it a size smaller than what was asked for.
    return (size + kBlock - 1) / kBlock * kBlock;
}

std::uint64_t directorySize(const fs::path &dir)
{
    std::error_code ec;
    std::uint64_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec))
            total += it->file_size(ec);
    }
    return total;
}

CardImageResult buildCardImage(const fs::path &cardDir, const fs::path &image,
                               const ImageTools &tools, ProgressSink &sink,
                               CancelToken &cancel)
{
    CardImageResult result;
    constexpr const char *kStage = "image";

    if (!tools.usable())
    {
        result.error = "mtools is not installed: " + tools.missing() + " not found";
        return result;
    }

    std::error_code ec;
    const fs::path data = Converter::dataDir(cardDir);
    if (!fs::is_directory(data, ec))
    {
        result.error = "there is nothing to pack: " + data.string() + " does not exist";
        return result;
    }

    // Everything goes to a temporary and is renamed, so an interrupted pack
    // leaves no file that looks like a card and mounts as a corrupt one.
    const fs::path tmp = fs::path(image).concat(".tmp");
    fs::create_directories(image.parent_path(), ec);
    fs::remove(tmp, ec);

    const auto fail = [&](const std::string &why) {
        std::error_code rm;
        fs::remove(tmp, rm);
        result.error = why;
        return result;
    };

    // --- format -------------------------------------------------------------
    //
    // Measured now rather than taken from the caller's estimate: the files are
    // all on disk by this point, so there is nothing left to guess about.
    const std::uint64_t sizeBytes = imageSizeFor(directorySize(cardDir));

    sink.info(kStage, "formatting " + image.filename().string() + " ("
                          + humanBytes(sizeBytes) + ")");
    cancel.throwIfCancelled();

    if (std::string e;
        !run(tools.mkfs,
             {"-F", "32", "-n", "RIVEN", "-C", tmp.string(),
              std::to_string(sizeBytes / kBlock)},
             e))
    {
        return fail(e);
    }

    // --- the tree -----------------------------------------------------------
    //
    // The directories are made first and each of the data folders is copied
    // separately, rather than handing mcopy the whole of _nds at once. Two
    // reasons, and both matter on a job this size: there is somewhere to check
    // for cancellation between subdirectories, and "pics_hi" in the progress
    // line is worth more over the ten minutes it takes than "_nds" would be.
    if (std::string e; !run(tools.mmd, {"-i", tmp.string(), "::/_nds"}, e))
        return fail(e);
    if (std::string e; !run(tools.mmd, {"-i", tmp.string(), "::/_nds/riven_nds"}, e))
        return fail(e);
    if (std::string e; !run(tools.mmd, {"-i", tmp.string(), "::/_nds/riven_nds/data"}, e))
        return fail(e);

    std::vector<fs::path> entries;
    for (const auto &entry : fs::directory_iterator(data, ec))
        entries.push_back(entry.path());
    // Alphabetical, so the progress line is the same order every run rather
    // than whatever order the filesystem happens to hand back.
    std::sort(entries.begin(), entries.end());

    // The .nds and anything else sitting at the card root, which is where a
    // loader looks for something to boot.
    std::vector<fs::path> rootFiles;
    for (const auto &entry : fs::directory_iterator(cardDir, ec))
        if (entry.is_regular_file(ec))
            rootFiles.push_back(entry.path());
    std::sort(rootFiles.begin(), rootFiles.end());

    const std::uint64_t total = entries.size() + rootFiles.size();
    std::uint64_t done = 0;

    for (const auto &entry : entries)
    {
        cancel.throwIfCancelled();
        sink.progress(done, total, kStage, entry.filename().string());
        // -s recurses, -Q stops at the first failure rather than filling a
        // full image with a hundred identical errors, -b batches the writes.
        if (std::string e;
            !run(tools.mcopy,
                 {"-s", "-Q", "-b", "-i", tmp.string(), entry.string(),
                  "::/_nds/riven_nds/data/"},
                 e))
        {
            return fail(e);
        }
        ++done;
    }

    for (const auto &file : rootFiles)
    {
        cancel.throwIfCancelled();
        sink.progress(done, total, kStage, file.filename().string());
        if (std::string e;
            !run(tools.mcopy, {"-Q", "-b", "-i", tmp.string(), file.string(), "::/"}, e))
        {
            return fail(e);
        }
        ++done;
    }

    // --- commit -------------------------------------------------------------
    fs::remove(image, ec);
    fs::rename(tmp, image, ec);
    if (ec)
        return fail("could not put the image in place: " + ec.message());

    result.ok = true;
    result.bytes = fs::file_size(image, ec);
    if (ec)
        result.bytes = sizeBytes;
    sink.progress(total, total, kStage, "done");
    return result;
}

} // namespace riven
