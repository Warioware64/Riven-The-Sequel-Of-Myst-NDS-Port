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

// ---------------------------------------------------------------------------
// AtomicFileWriter
// ---------------------------------------------------------------------------

AtomicFileWriter::~AtomicFileWriter() { discard(); }

void AtomicFileWriter::discard()
{
    if (file_ != nullptr)
    {
        std::fclose(file_);
        file_ = nullptr;
    }
    if (!tmp_.empty())
    {
        std::error_code ec;
        fs::remove(tmp_, ec);
        tmp_.clear();
    }
    ok_ = false;
}

bool AtomicFileWriter::open(const fs::path &path, std::string &error)
{
    discard();
    error.clear();
    written_ = 0;

    std::error_code ec;
    if (const fs::path parent = path.parent_path(); !parent.empty())
    {
        fs::create_directories(parent, ec);
        if (ec && !fs::is_directory(parent))
        {
            error = "cannot create " + parent.string() + ": " + ec.message();
            return false;
        }
    }

    path_ = path;
    tmp_ = path;
    tmp_ += ".tmp";

    file_ = std::fopen(tmp_.string().c_str(), "wb");
    if (file_ == nullptr)
    {
        error = "cannot open " + tmp_.string() + " for writing";
        tmp_.clear();
        return false;
    }
    ok_ = true;
    return true;
}

bool AtomicFileWriter::write(const void *data, std::size_t size)
{
    if (!ok_ || file_ == nullptr)
        return false;
    if (size == 0)
        return true;
    if (std::fwrite(data, 1, size, file_) != size)
    {
        // Poison rather than throw: the caller writes thousands of frames and
        // checking every one would bury the loop. commit() reports it.
        ok_ = false;
        return false;
    }
    written_ += size;
    return true;
}

bool AtomicFileWriter::rewriteHeader(const void *first, std::size_t firstSize,
                                     const void *second, std::size_t secondSize,
                                     std::string &error)
{
    error.clear();
    if (!ok_ || file_ == nullptr)
    {
        error = "write failed for " + path_.string() + " (disk full?)";
        return false;
    }

    if (std::fseek(file_, 0, SEEK_SET) != 0)
    {
        ok_ = false;
        error = "cannot seek back to the header of " + path_.string();
        return false;
    }

    bool wrote = firstSize == 0 || std::fwrite(first, 1, firstSize, file_) == firstSize;
    if (wrote && secondSize > 0)
        wrote = std::fwrite(second, 1, secondSize, file_) == secondSize;

    // Back to the end, so a caller may keep appending and so bytesWritten stays
    // meaningful.
    if (!wrote || std::fseek(file_, 0, SEEK_END) != 0)
    {
        ok_ = false;
        error = "cannot rewrite the header of " + path_.string() + " (disk full?)";
        return false;
    }
    return true;
}

bool AtomicFileWriter::commit(std::string &error)
{
    error.clear();
    if (file_ == nullptr)
    {
        error = "nothing open to commit";
        return false;
    }

    // The close is checked as well as the writes: a full disk usually surfaces
    // there rather than at fwrite, and a silently truncated asset is exactly what
    // this class exists to prevent.
    const bool closed = std::fclose(file_) == 0;
    file_ = nullptr;

    if (!ok_ || !closed)
    {
        error = "write failed for " + path_.string() + " (disk full?)";
        discard();
        return false;
    }

    std::error_code ec;
    fs::rename(tmp_, path_, ec);
    if (ec)
    {
        error = "cannot rename into " + path_.string() + ": " + ec.message();
        discard();
        return false;
    }
    tmp_.clear();
    ok_ = false; // committed; further writes would have nowhere to go
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

namespace
{
    fs::path formatStampPath(const fs::path &dir) { return dir / ".format"; }

    /// True when `dir` holds anything a previous run could have written. An empty
    /// or absent directory is not stale -- there is nothing to redo -- and saying
    /// otherwise would make --force the only way to convert a fresh card.
    bool dirHasOutput(const fs::path &dir)
    {
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            return false;
        for (auto it = fs::recursive_directory_iterator(
                 dir, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec))
        {
            if (ec)
            {
                ec.clear();
                continue;
            }
            if (it->is_regular_file(ec) && it->path().filename() != ".format")
                return true;
        }
        return false;
    }
} // namespace

bool formatStampIsStale(const fs::path &dir, std::uint16_t version)
{
    if (!dirHasOutput(dir))
        return false;

    std::FILE *f = std::fopen(formatStampPath(dir).string().c_str(), "rb");
    if (f == nullptr)
        return true; // output written before stamps existed: assume the worst

    unsigned stamped = 0;
    const bool read = std::fscanf(f, "%u", &stamped) == 1;
    std::fclose(f);
    return !read || stamped != version;
}

bool writeFormatStamp(const fs::path &dir, std::uint16_t version, std::string &error)
{
    char text[16];
    const int n = std::snprintf(text, sizeof(text), "%u\n", static_cast<unsigned>(version));
    if (n <= 0)
    {
        error = "could not format the version stamp";
        return false;
    }
    return writeFileAtomic(formatStampPath(dir), text, static_cast<std::size_t>(n), error);
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
