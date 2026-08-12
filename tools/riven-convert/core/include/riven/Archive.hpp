#pragma once

// RAII wrappers over libvaht's Mohawk archive reader, plus the priority search
// that turns a stack's several archives into one namespace.
//
// Riven splits a stack across archives and ships patch archives that OVERRIDE
// resources in the base ones. ScummVM opens them so that the patch comes first
// and takes the first hit in a linear scan (riven.cpp:441-443). ArchiveSet is
// that rule: it holds the archives in the order Layout.hpp computed and answers
// every lookup from the first archive that has the resource.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct vaht_archive_s;
struct vaht_bmp_s;

namespace riven
{

/// One open Mohawk archive.
class Archive
{
public:
    Archive() = default;
    ~Archive();
    Archive(Archive &&) noexcept;
    Archive &operator=(Archive &&) noexcept;
    Archive(const Archive &) = delete;
    Archive &operator=(const Archive &) = delete;

    /// Returns an unopened Archive on failure; check with isOpen().
    static Archive open(const std::filesystem::path &path);

    bool isOpen() const { return handle_ != nullptr; }
    const std::filesystem::path &path() const { return path_; }

    /// Resource ids of `type` ("CARD", "tBMP", ...), in archive order.
    std::vector<std::uint16_t> resourceIds(const char *type) const;
    std::size_t resourceCount(const char *type) const;
    bool has(const char *type, std::uint16_t id) const;

    /// Whole resource as bytes. Empty on any failure.
    std::vector<std::uint8_t> read(const char *type, std::uint16_t id) const;

    vaht_archive_s *raw() const { return handle_; }

private:
    vaht_archive_s *handle_ = nullptr;
    std::filesystem::path path_;
};

/// A decoded tBMP. Riven's stills are 608x392, 8bpp with a 256-entry CLUT, so
/// both the indexed and the truecolour views are available from one decode --
/// which is exactly what the two outputs need (.rpic wants RGB to resample,
/// .rpiz wants the indices and the palette).
class Bitmap
{
public:
    Bitmap() = default;
    ~Bitmap();
    Bitmap(Bitmap &&) noexcept;
    Bitmap &operator=(Bitmap &&) noexcept;
    Bitmap(const Bitmap &) = delete;
    Bitmap &operator=(const Bitmap &) = delete;

    bool valid() const { return handle_ != nullptr; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool indexed() const;

    /// width*height*3 bytes, RGB. Null if the decode failed.
    const std::uint8_t *rgb() const;
    /// width*height bytes of palette indices, or null when truecolour.
    const std::uint8_t *indices() const;
    /// 256*3 bytes RGB, or null when truecolour.
    const std::uint8_t *palette() const;

private:
    friend class ArchiveSet;
    friend class Archive;
    vaht_bmp_s *handle_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

/// Several archives searched in priority order.
class ArchiveSet
{
public:
    /// Opens each path in turn. Paths that fail to open are reported through
    /// `failures` rather than aborting -- a damaged archive in a set should
    /// cost that archive, not the stack.
    void openAll(const std::vector<std::filesystem::path> &paths,
                 std::vector<std::string> &failures);

    bool empty() const { return archives_.empty(); }
    std::size_t size() const { return archives_.size(); }
    const Archive &at(std::size_t i) const { return archives_[i]; }

    /// Union of resource ids of `type` across every archive, sorted and
    /// deduplicated. A patched resource appears once.
    std::vector<std::uint16_t> resourceIds(const char *type) const;

    /// Total count of `type` across every archive, counting duplicates. This is
    /// the number to compare against a raw per-archive census, not resourceIds().size().
    std::size_t rawResourceCount(const char *type) const;

    /// First archive that has the resource, or nullptr.
    const Archive *find(const char *type, std::uint16_t id) const;

    std::vector<std::uint8_t> read(const char *type, std::uint16_t id) const;

    /// Decode a tBMP through libvaht. Invalid on any failure.
    Bitmap readBitmap(std::uint16_t id) const;

    /// A tMOV as a standalone QuickTime file.
    ///
    /// This is the one place libvaht's movie code is worth using. Riven's
    /// movies store their stco chunk offsets relative to the ARCHIVE, not to
    /// the resource -- ScummVM compensates with setChunkBeginOffset
    /// (quicktime.cpp:757-763, riven_video.cpp:65) and libvaht by rewriting the
    /// tables as it reads (vaht_mov.c:77-90). Using vaht_mov here means the
    /// demuxer never has to know where in the .MHK the resource sits.
    ///
    /// Empty on any failure, including a resource libvaht's own atom walk
    /// cannot follow; the caller falls back to the raw bytes.
    std::vector<std::uint8_t> readMovie(std::uint16_t id) const;

private:
    std::vector<Archive> archives_;
};

/// True when a resource is entirely zero bytes -- the signature of a download
/// or disc rip that did not finish. No real Riven resource is all zeroes: every
/// one starts with a magic word, a record count or a dimension. Used to tell
/// "the source is damaged here" apart from "the parser is wrong here", which
/// are different findings and must be reported differently.
bool looksDamaged(const std::vector<std::uint8_t> &bytes);

} // namespace riven
