#pragma once

// Reading a .rcur sprite set off the card.
//
// Deliberately free of <nds.h>, like source/rvid/RvidFile.cpp and for the same
// reason: the host test can compile it and check that what the converter wrote
// is what the DS reads back. On hardware nothing can check that.
//
// The whole file is held in RAM -- a cursor set is about 5 KB and the inventory
// under 2 KB -- because it is read once at boot and then only looked up.
//
// shared/RivenCursor.hpp is the format, including why the pixels are in DS tile
// order rather than raster.

#include <cstdint>
#include <string>
#include <vector>

#include "RivenCursor.hpp"

namespace rivenrt
{

class RcurFile
{
public:
    /// Read the whole set. False with `error` set when the file is missing,
    /// is not an RCUR this build reads, or is truncated.
    bool load(const std::string &path, std::string &error);
    void unload();

    bool loaded() const { return !cels_.empty(); }

    const rivendata::RcurHeader &header() const { return header_; }
    int celWidth() const { return header_.celWidth; }
    int celHeight() const { return header_.celHeight; }
    std::uint32_t celBytes() const
    {
        return rivendata::rcurCelBytes(header_.celWidth, header_.celHeight);
    }

    /// RGB555 entries, to be written into SPRITE_PALETTE at paletteBase().
    const std::uint16_t *palette() const { return palette_.data(); }
    int paletteCount() const { return static_cast<int>(palette_.size()); }
    int paletteBase() const { return header_.paletteBase; }

    std::size_t celCount() const { return cels_.size(); }
    const rivendata::RcurCel &celAt(std::size_t i) const { return cels_[i]; }

    /// The cel for a cursor id or a tBMP id, or null. Callers fall back rather
    /// than fail: a set converted before an id existed must still be usable.
    const rivendata::RcurCel *find(std::uint16_t id) const;

    /// A cel's pixels, celBytes() of them, already in OBJ tile order.
    const std::uint8_t *pixels(const rivendata::RcurCel &cel) const;

private:
    rivendata::RcurHeader header_{};
    std::vector<std::uint16_t> palette_;
    std::vector<rivendata::RcurCel> cels_;
    std::vector<std::uint8_t> pixels_;
};

} // namespace rivenrt
