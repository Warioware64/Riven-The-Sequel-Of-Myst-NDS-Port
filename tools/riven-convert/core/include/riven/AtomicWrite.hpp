#pragma once

// Write a file so that it is either complete or absent -- never half-written.
//
// This is what makes a cancelled conversion resumable. The pipeline decides
// whether to redo an asset by asking whether its output already exists and is
// newer than the source archive; that question only has a meaningful answer if
// a file under its final name is guaranteed complete. So every output is
// written to "<name>.tmp" and renamed into place, and a run that is stopped or
// crashes leaves at most a stray .tmp.
//
// The Myst converter wrote outputs directly and had no resume at all: a
// cancelled run left "partial output kept" that nothing could distinguish from
// finished output. Its prefs.py used exactly this tmp-then-replace trick for
// the settings file; this is the same idea applied to the 10,000 assets that
// actually matter.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace riven
{

/// Write `bytes` to `path` atomically. Creates parent directories.
/// Returns false and fills `error` on failure, leaving no partial file behind.
bool writeFileAtomic(const std::filesystem::path &path, const void *data, std::size_t size,
                     std::string &error);

inline bool writeFileAtomic(const std::filesystem::path &path,
                            const std::vector<std::uint8_t> &bytes, std::string &error)
{
    return writeFileAtomic(path, bytes.data(), bytes.size(), error);
}

/// The same guarantee for a file that is too big to hold in memory.
///
/// Raw video is why this exists: a fullscreen movie is 84480 bytes a frame and
/// the longest is 4684 frames, so ospit's tMOV 21 alone is 377 MB. Assembling
/// that in a vector and handing it to writeFileAtomic would need it twice over.
///
/// Same contract as writeFileAtomic: everything goes to `<path>.tmp` and is
/// renamed into place by commit(), so a file under its final name is always
/// complete and an abandoned run leaves only a .tmp for cleanStaleTempFiles.
/// Destroying without committing discards the temporary.
class AtomicFileWriter
{
public:
    AtomicFileWriter() = default;
    ~AtomicFileWriter();

    AtomicFileWriter(const AtomicFileWriter &) = delete;
    AtomicFileWriter &operator=(const AtomicFileWriter &) = delete;

    /// Create the parent directories and open the temporary. False with `error`
    /// set on failure.
    bool open(const std::filesystem::path &path, std::string &error);

    /// Append. False (and the writer is poisoned) on the first failure; later
    /// calls are no-ops so a caller can check once at the end.
    bool write(const void *data, std::size_t size);

    bool ok() const { return ok_; }
    std::uint64_t bytesWritten() const { return written_; }

    /// Overwrite the start of the file, then seek back to the end.
    ///
    /// A one-pass streaming write cannot know everything its header needs: RVID's
    /// frame index is a table of byte offsets, and the largest frame is only known
    /// once every frame has been written. Both are laid down blank and patched
    /// here. `second` is an optional block written straight after `first`, which is
    /// how the header and the index it precedes go back together.
    bool rewriteHeader(const void *first, std::size_t firstSize, const void *second,
                       std::size_t secondSize, std::string &error);

    /// Flush, close and rename into place. False with `error` set if anything
    /// went wrong at any point, including during an earlier write().
    bool commit(std::string &error);

private:
    void discard();

    std::FILE *file_ = nullptr;
    std::filesystem::path path_;
    std::filesystem::path tmp_;
    std::uint64_t written_ = 0;
    bool ok_ = false;
};

/// True when `output` exists and is at least as new as `source`.
///
/// The pipeline uses this to skip finished work. Deliberately `>=` rather than
/// `>`: archives and outputs written in the same second are common on a fast
/// machine, and redoing an asset is far cheaper than the alternative failure
/// mode of this returning false forever.
bool isUpToDate(const std::filesystem::path &output, const std::filesystem::path &source);

/// A stage's on-card format version, recorded beside its outputs.
///
/// isUpToDate() compares mtimes against the SOURCE archive, which cannot see a
/// change at this end: when the runtime format is revised, every output on the
/// card is still newer than a 1997 CD and every asset is skipped as "up to date"
/// forever, while the ROM rejects each one for having the wrong version. That is
/// not hypothetical -- it is exactly what the raw-frame rewrite did to `.rvid`
/// when kVideoVersion went 1 -> 3, and the only escape was --force, which nothing
/// told anyone to use.
///
/// So each stage stamps the version it wrote into `<dir>/.format`. A stamp that
/// does not match, or is missing when the directory already holds output, means
/// the whole stage is stale whatever the mtimes say.
///
/// @returns true when `dir` carries no stamp for `version` and must be redone.
bool formatStampIsStale(const std::filesystem::path &dir, std::uint16_t version);

/// Record `version` as the format of everything in `dir`. Call after the stage
/// finishes, never before: a stamp written up front would claim outputs that a
/// cancelled run never produced.
bool writeFormatStamp(const std::filesystem::path &dir, std::uint16_t version,
                      std::string &error);

/// Remove any `*.tmp` left by an interrupted run under `dir`, recursively.
/// Returns how many were removed. Never throws.
int cleanStaleTempFiles(const std::filesystem::path &dir);

} // namespace riven
