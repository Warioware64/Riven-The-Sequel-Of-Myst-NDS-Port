#pragma once

// The InstallShield v3 archive Riven's 5-CD release ships its executable and
// extras.mhk inside: program/arcriven.z.
//
// This exists because those two files are not on the CD in any other form, and
// two of the port's features are behind them -- the cursors are PE resources in
// riven.exe, and the inventory art is extras.mhk. ScummVM has exactly the same
// problem and solves it the same way, opening arcriven.z and searching inside it
// (riven.cpp:122-125, and the message at :156 telling the player the installer
// file will do).
//
// SPECIFICATION, NOT SOURCE: helpSrc/scummvm-master/common/compression/
// installshieldv3_archive.cpp describes the layout -- magic 0x135D658C read
// big-endian (:111), the directory table offset and count (:118-121), the
// per-directory file count and name length (:135-138), and each file's
// uncompressed size, compressed size and offset (:158-160). Written from that
// description per docs/licensing.md, which forbids copying the code itself.
//
// Only the first directory is walked. That is the same restriction ScummVM
// documents, and Riven's archive has exactly one.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace riven
{

class InstallerArchive
{
public:
    /// Read the table of contents. Nothing is decompressed here -- detection
    /// runs this on every source tree and must stay cheap.
    ///
    /// Never throws. An unreadable or non-InstallShield file comes back with
    /// isOpen() false, which is not an error: most installs have no arcriven.z.
    static InstallerArchive open(const std::filesystem::path &path);

    bool isOpen() const { return !entries_.empty(); }
    const std::filesystem::path &path() const { return path_; }

    /// Entry names as the archive spells them. Riven's are mixed case
    /// ("Riven.exe", "Extras.MHK"), which is why every lookup here is
    /// case-insensitive.
    std::vector<std::string> names() const;

    bool contains(std::string_view name) const;

    /// Decompress one entry. Empty with `error` set if it is absent or damaged.
    std::vector<std::uint8_t> read(std::string_view name, std::string &error) const;

private:
    struct Entry
    {
        std::string name; ///< as spelled in the archive
        std::uint32_t offset = 0;
        std::uint32_t compressed = 0;
        std::uint32_t uncompressed = 0;
    };

    /// The whole archive. 741 KB for Riven's, read once at open() so that
    /// listing names and pulling an entry are not two passes over the disk.
    std::vector<std::uint8_t> bytes_;
    std::vector<Entry> entries_;
    /// Lowercased name -> index into entries_.
    std::map<std::string, std::size_t> byLowerName_;
    std::filesystem::path path_;
};

} // namespace riven
