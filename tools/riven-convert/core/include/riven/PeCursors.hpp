#pragma once

// Riven's cursors, read out of riven.exe's PE resources.
//
// The game has no cursor art in its Mohawk archives: the hand, the pointer and
// the rest are Win32 cursor resources in the executable, and the per-hotspot
// `cursor` field in HSPT is an id into them. ScummVM does the same thing --
// riven.cpp:139-143 builds a PECursorManager over "riven.exe", the ids are
// cursors.h:43-47, and riven_card.cpp:1015-1018 is where a hotspot's id becomes
// the pointer on screen.
//
// SPECIFICATION, NOT SOURCE. Per docs/licensing.md, ScummVM's cursors.cpp
// (WinCursorManager::loadCursors, ~:237) and Graphics::WinCursorGroup are cited
// as the description of the PE layout; the code below is written from the
// documented Win32 structures.
//
// Three details are worth stating because getting them wrong yields a cursor
// that is subtly wrong rather than absent:
//
//   * A RT_GROUP_CURSOR entry is 14 bytes, not the 16 of the icon form. Mixing
//     them up shifts every field after the first.
//   * The BITMAPINFOHEADER's height is DOUBLE the real height: the AND mask is
//     stacked underneath the colour plane in the same bitmap.
//   * A set bit in the AND mask means transparent, and for a colour cursor that
//     is all it means -- the XOR-invert case does not arise. Riven leans on this
//     heavily; most of a 32x32 cursor is mask.

#include <cstdint>
#include <string>
#include <vector>

namespace riven
{

/// One cursor, fully resolved: palette applied, mask applied, rows the right way
/// up. RGB rather than the DS's 555 because the downscaler wants the precision.
struct PeCursor
{
    std::uint16_t groupId = 0; ///< the RT_GROUP_CURSOR id, e.g. 3000 or 2003
    int width = 0;
    int height = 0;   ///< the REAL height, not the doubled one in the header
    int hotX = 0;     ///< hot point, in source pixels
    int hotY = 0;
    int bitCount = 0; ///< what the source was, for reporting

    /// width*height RGB triples, row 0 at the top.
    std::vector<std::uint8_t> rgb;
    /// width*height, 0 = transparent, 255 = opaque. Parallel to `rgb`.
    std::vector<std::uint8_t> opaque;
};

/// Every cursor group in a PE image, in id order.
///
/// A cursor whose depth or compression this reader does not handle is OMITTED
/// and named in `warnings`: one unreadable cursor should cost that cursor, not
/// the set. The runtime falls back to the main pointer for any id it has no cel
/// for, so the result degrades to a plain arrow rather than to nothing.
///
/// Returns empty (with a warning) if `exeBytes` is not a PE image at all -- a
/// Mac build of Riven keeps its cursors in a resource fork instead, which this
/// does not read.
std::vector<PeCursor> readPeCursors(const std::vector<std::uint8_t> &exeBytes,
                                    std::vector<std::string> &warnings);

} // namespace riven
