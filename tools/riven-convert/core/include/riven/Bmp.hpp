#pragma once

// A minimal Windows BMP reader, for the one picture in a Riven install that is
// not a Mohawk resource: Autorun/AUTORUN.BMP, the splash the CD's autorun shell
// puts behind its two buttons. It is the port's top-screen background.
//
// It has to be read here rather than shipped with the ROM because it is Cyan's
// artwork: docs/licensing.md is explicit that no game data goes in the .nds, so
// this comes out of the player's own copy like everything else does.
//
// THE FILE IS NOT ONE FORMAT. The English 5-CD release ships 319x214 at 8bpp
// with a 256-entry palette; the French one ships the same image at 24bpp. Both
// are BI_RGB, so both are read, and 32bpp is accepted too because it costs one
// branch and a BMP writer that pads to 32 is not unusual.
//
// pe/PeCursors.cpp also parses a BITMAPINFOHEADER and is deliberately not
// reused: it is hard-wired to RT_CURSOR's double-height XOR+AND layout, where
// the header's height is twice the image's and the colour depth is 1, 4 or 8
// with an implicit mask plane. None of that is true of a plain .bmp.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace riven
{

/// A decoded BMP, always expanded to 8 bits per channel and always TOP-DOWN --
/// the row order the rest of the image code assumes. BMPs are usually stored
/// bottom-up, and un-flipping them here is what keeps that fact from leaking.
struct BmpImage
{
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb; ///< width*height*3, R,G,B

    bool valid() const
    {
        return width > 0 && height > 0
            && rgb.size() == static_cast<std::size_t>(width) * height * 3;
    }
};

/// Decode a BMP already in memory. Returns an invalid image with `error` set on
/// anything this does not read: a bad magic, a compressed (RLE / bitfield) DIB,
/// or a colour depth below 8.
BmpImage decodeBmp(const std::uint8_t *data, std::size_t size, std::string &error);

/// decodeBmp() on the contents of `path`.
BmpImage readBmp(const std::filesystem::path &path, std::string &error);

} // namespace riven
