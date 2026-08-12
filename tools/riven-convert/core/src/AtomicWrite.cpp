#include "riven/AtomicWrite.hpp"

#include <cstdio>

namespace fs = std::filesystem;

namespace riven
{

bool writeFileAtomic(const fs::path &path, const void *data, std::size_t size,
                     std::string &error)
{
    error.clear();

    std::error_code ec;
    if (const fs::path parent = path.parent_path(); !parent.empty())
    {
        fs::create_directories(parent, ec);
        if (ec && !fs::is_directory(parent))
        {
            error = "cannot create " + parent.string() + ": " + ec.message();
            return false;
        }
        ec.clear();
    }

    fs::path tmp = path;
    tmp += ".tmp";

    {
        std::FILE *f = std::fopen(tmp.string().c_str(), "wb");
        if (f == nullptr)
        {
            error = "cannot open " + tmp.string() + " for writing";
            return false;
        }

        const bool wrote = size == 0 || std::fwrite(data, 1, size, f) == size;
        // Check the close too: a full disk usually surfaces here rather than at
        // fwrite, and silently producing a truncated asset is exactly the
        // failure this whole helper exists to prevent.
        const bool closed = std::fclose(f) == 0;

        if (!wrote || !closed)
        {
            error = "write failed for " + path.string() + " (disk full?)";
            fs::remove(tmp, ec);
            return false;
        }
    }

    // rename over an existing file is atomic on POSIX and on Windows via
    // MoveFileEx, which is what std::filesystem::rename uses.
    fs::rename(tmp, path, ec);
    if (ec)
    {
        error = "cannot rename into " + path.string() + ": " + ec.message();
        std::error_code ignored;
        fs::remove(tmp, ignored);
        return false;
    }
    return true;
}

bool isUpToDate(const fs::path &output, const fs::path &source)
{
    std::error_code ec;
    if (!fs::exists(output, ec) || ec)
        return false;

    const auto outTime = fs::last_write_time(output, ec);
    if (ec)
        return false;

    const auto srcTime = fs::last_write_time(source, ec);
    if (ec)
        return false; // cannot compare: redo, which is the safe direction

    return outTime >= srcTime;
}

int cleanStaleTempFiles(const fs::path &dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return 0;

    int removed = 0;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            ec.clear();
            continue;
        }
        if (it->is_regular_file(ec) && it->path().extension() == ".tmp")
        {
            std::error_code rmEc;
            if (fs::remove(it->path(), rmEc))
                ++removed;
        }
    }
    return removed;
}

} // namespace riven
