#pragma once

// Packing a finished card into one file an emulator can mount as its SD card.
//
// A DS emulator has no card slot. melonDS and DeSmuME both read a FAT image
// instead -- melonDS as the DSi SD or a DLDI-backed SD, DeSmuME as an
// MPCF/R4 image -- and neither will take the directory tree the converter
// writes. So the run can finish by packing that tree into a single .bin.
//
// MTOOLS, NOT OUR OWN FAT32 WRITER, and for the same reasons ffmpeg is a
// subprocess rather than libavcodec (FFmpeg.hpp says it at length):
//
//   * A FAT32 formatter and writer is a boot sector, an FSInfo block, two
//     mirrored allocation tables, long-filename directory entries and cluster
//     chaining -- several hundred lines whose bugs corrupt a user's card
//     silently, against a tool every Linux and macOS box already has.
//   * Invoking a separate binary is not linking, so nothing about mtools's
//     licence reaches this program. See docs/licensing.md.
//
// The cost is a second external dependency, and unlike ffmpeg it is not
// ordinary on Windows. Preflight reports that before the run rather than after
// two hours of conversion.
//
// PACKED FROM THE FINISHED FOLDER, not written to directly. The conversion
// still produces <dest>/_nds/riven_nds/data/, which is what makes a cancelled
// run resumable; the image is built from it at the end. That means an emulator
// user needs room for both, which estimateOutput() accounts for.

#include <cstdint>
#include <filesystem>
#include <string>

#include "riven/Progress.hpp"

namespace riven
{

/// The two mtools binaries the pack needs.
struct ImageTools
{
    /// mkfs.vfat, or mkfs.fat -- the same program under two names, and which
    /// one exists depends on the distribution's dosfstools packaging.
    std::filesystem::path mkfs;
    std::filesystem::path mcopy; ///< from mtools
    std::filesystem::path mmd;   ///< likewise; makes the directories to copy into

    bool usable() const { return !mkfs.empty() && !mcopy.empty() && !mmd.empty(); }

    /// Which of the three was not found, for the error message.
    std::string missing() const;
};

/// Look for all three on PATH.
ImageTools findImageTools();

/// How big to make the image for `contentBytes` of files.
///
/// Headroom rather than an exact fit: FAT32 rounds every file up to a cluster,
/// and Riven writes thousands of small ones -- 556 water effects and 2307 cards
/// -- so an image sized to the byte would fill before the last of them landed.
/// Floored at the smallest thing mkfs.vfat will agree to call FAT32.
std::uint64_t imageSizeFor(std::uint64_t contentBytes);

struct CardImageResult
{
    bool ok = false;
    std::uint64_t bytes = 0; ///< the image's size on success
    std::string error;
};

/// Total bytes of every file under `dir`, recursively. 0 if it is not there.
std::uint64_t directorySize(const std::filesystem::path &dir);

/// Format `image` and copy everything under `cardDir` into it.
///
/// The size comes from MEASURING `cardDir` rather than from a caller's estimate:
/// by the time this runs the files exist, so there is no reason to guess, and a
/// guess that came in low would fill the image partway through the copy.
///
/// Written to "<image>.tmp" and renamed, on the same principle as every other
/// output: an interrupted pack must not leave a file that looks like a card and
/// mounts as a corrupt one.
CardImageResult buildCardImage(const std::filesystem::path &cardDir,
                           const std::filesystem::path &image, const ImageTools &tools,
                           ProgressSink &sink, CancelToken &cancel);

} // namespace riven
