#pragma once

// Turning Riven's cursors and its inventory art into the two .rcur sprite sets
// the DS reads.
//
// The two share everything but their source: one comes out of riven.exe's PE
// resources, the other out of extras.MHK's tBMPs, and both end as an 8bpp cel
// set with a palette in its own half of the DS's single OBJ palette. So the
// packing lives here once and both stages call it.
//
// shared/RivenCursor.hpp is the format and explains why it is paletted and why
// the pixels are in tile order.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenCursor.hpp"

namespace riven
{

/// One cel on its way into a set: RGB and an opacity plane, at the cel's final
/// size. Both pipelines produce this and then hand it to the packer.
struct RcurSourceCel
{
    std::uint16_t id = 0;
    int hotX = 0;
    int hotY = 0;
    int drawW = 0; ///< opaque extent, for a hit rect
    int drawH = 0;
    /// celW*celH RGB triples, and celW*celH opacity bytes (0 or 255).
    std::vector<std::uint8_t> rgb;
    std::vector<std::uint8_t> opaque;
};

/// Pack cels into a .rcur file: build the shared palette, index every pixel,
/// and lay the cels out in DS OBJ tile order.
///
/// `paletteBase`/`paletteMax` are the set's slice of the OBJ palette. If the
/// cels need more distinct colours than the slice holds, the palette is reduced
/// and `warnings` says by how much -- Riven's own sets do not come close, but a
/// silent quantisation is exactly the kind of thing that would be found later,
/// on hardware, as a wrong-coloured hand.
std::vector<std::uint8_t> encodeRcur(const std::vector<RcurSourceCel> &cels, int celW,
                                     int celH, int paletteBase, int paletteMax,
                                     std::vector<std::string> &warnings);

struct CursorResult
{
    bool ok = false;
    std::string error;
    int cels = 0;
    std::size_t bytes = 0;
};

/// riven.exe (or rivendmo.exe) -> cursors/cursors.rcur.
///
/// Downscales the 32x32 sources to kCursorCel and scales the hot points with
/// them. Cursors the PE reader could not decode are dropped and named in
/// `warnings`; the runtime falls back to the main pointer for a missing id, so
/// the cost of one bad cursor is that one cursor.
CursorResult convertCursors(const std::vector<std::uint8_t> &exeBytes,
                            const std::filesystem::path &out,
                            std::vector<std::string> &warnings);

/// extras.MHK -> extras/inventory.rcur, from tBMP 100/101/102.
///
/// Each book is scaled to the size Riven draws it at -- its strip rect, under
/// the one uniform factor RivenInventory.hpp derives -- and NOT to its own tBMP
/// dimensions, which the original ignores. `drawW`/`drawH` and `hotX`/`hotY`
/// record the art's size and corner inside its cel, which is how the runtime
/// hit-tests it.
CursorResult convertInventory(const std::filesystem::path &extrasMhk,
                              const std::filesystem::path &out,
                              std::vector<std::string> &warnings);

} // namespace riven
