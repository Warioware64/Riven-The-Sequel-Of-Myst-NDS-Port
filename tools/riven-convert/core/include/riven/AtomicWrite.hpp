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

/// True when `output` exists and is at least as new as `source`.
///
/// The pipeline uses this to skip finished work. Deliberately `>=` rather than
/// `>`: archives and outputs written in the same second are common on a fast
/// machine, and redoing an asset is far cheaper than the alternative failure
/// mode of this returning false forever.
bool isUpToDate(const std::filesystem::path &output, const std::filesystem::path &source);

/// Remove any `*.tmp` left by an interrupted run under `dir`, recursively.
/// Returns how many were removed. Never throws.
int cleanStaleTempFiles(const std::filesystem::path &dir);

} // namespace riven
