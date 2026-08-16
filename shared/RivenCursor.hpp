#pragma once

// .rcur -- a set of hardware-sprite cels for the DS's main OBJ layer, shared
// between the converter and the ARM9 runtime.
//
// Two sets exist and they are NOT independent, because the DS has exactly ONE
// 256-entry OBJ palette per engine. (Extended OBJ palettes would want VRAM F or
// G, and both are the 3D texture palette here -- Global.cpp.) So the palette is
// split at a fixed boundary and each set owns a range:
//
//   cursors/cursors.rcur     entries   1..128   Riven's 19 cursors, from riven.exe
//   extras/inventory.rcur    entries 129..255   the three inventory books
//
// Index 0 is transparent and is never written by either set. That is what lets
// either stage be run, skipped or reconverted without disturbing the other.
//
// WHY PALETTED RATHER THAN ARGB1555, which is what every other picture in this
// port is. Nitro Engine Advanced's OBJ API (NEAHw2D.h) offers 4bpp and 8bpp and
// has no bitmap-sprite mode at all; going direct-colour would mean re-running
// oamInit behind NEA's back and giving up NEA_Hw2DOBJ entirely. And there is
// nothing to gain: Riven's cursors are natively 8bpp sharing ONE palette, with
// 1-bit alpha from a Win32 AND mask, so paletted is lossless here and half the
// VRAM. The inventory books are three small tBMPs and no different.
//
// PIXEL ORDER IS DS OBJ 1D TILE ORDER, NOT RASTER. A 16x16 8bpp cel is four 8x8
// tiles of 64 bytes, in the order (0,0) (1,0) (0,1) (1,1), each tile row-major.
// The converter writes them that way so the ARM9 can hand a cel straight to
// NEA_Hw2DOBJAssetLoadGfx with no repacking. Getting this wrong gives a sprite
// with the right colours in scrambled 8x8 blocks, which is why it is stated
// here rather than left to be rediscovered.

#include <cstdint>

namespace rivendata
{

/// 16-byte header. Followed by `paletteCount` RGB555 halfwords, then `celCount`
/// RcurCel records, then the cel pixels back to back.
struct RcurHeader
{
    char magic[4];             ///< "RCUR"
    std::uint16_t version;     ///< kCursorVersion
    std::uint8_t celWidth;     ///< pixels; a multiple of 8
    std::uint8_t celHeight;
    std::uint8_t paletteBase;  ///< first SPRITE_PALETTE entry this set owns
    std::uint8_t paletteCount; ///< halfwords following the header
    std::uint16_t celCount;
    std::uint32_t dataBytes;   ///< celCount * celWidth * celHeight
    std::uint32_t reserved;
};
static_assert(sizeof(RcurHeader) == 20, "RcurHeader must be 20 bytes");

/// One cel.
struct RcurCel
{
    /// The RT_GROUP_CURSOR id (cursors.h:43-47) or the extras.MHK tBMP id
    /// (100/101/102). Self-describing either way, so one reader serves both and
    /// the runtime asks for what the card data already names.
    std::uint16_t id;
    /// A cursor's hot point inside the cel, already scaled.
    ///
    /// An INVENTORY cel has no hot point and uses these for the art's top-left
    /// corner inside the cel instead. The books do not fill their cel and are
    /// not centred in it -- each sits where Riven's own rect puts it in the
    /// 44-row strip (RivenInventory.hpp) -- so the runtime cannot re-derive the
    /// offset from drawW/drawH and is told it.
    std::uint8_t hotX;
    std::uint8_t hotY;
    /// The opaque extent inside the cel. The inventory needs it to centre a
    /// touch rect on art that does not fill its cel; a cursor ignores it.
    std::uint8_t drawW;
    std::uint8_t drawH;
    std::uint16_t reserved;
};
static_assert(sizeof(RcurCel) == 8, "RcurCel must be 8 bytes");

/// Bumped to 2 when hotX/hotY took on their inventory meaning. A version-1 set
/// carries 0 there, which reads as "the art is in the cel's top-left corner"
/// and is not -- so the touch rects would sit ten columns left of the books.
/// Refusing the old file instead costs a stale card its cursors and its strip
/// until the converter is re-run, which both sides already survive and log, and
/// both stages take seconds.
inline constexpr std::uint16_t kCursorVersion = 2;

/// The palette split. Riven's 19 cursors share one palette in riven.exe and use
/// 85 entries of it; the three inventory books use fewer still. Both stay near
/// those counts because the converter snaps each downscaled pixel back onto the
/// colours its source actually used -- without that, averaging 32x32 down to
/// 16x16 invents intermediate shades and the cursors alone want 191, which is
/// more than one set can have if the other is to have any.
inline constexpr int kCursorPaletteBase = 1;
inline constexpr int kCursorPaletteMax = 128;
inline constexpr int kExtrasPaletteBase = 129;
inline constexpr int kExtrasPaletteMax = 127;

/// Cursors are 32x32 in riven.exe. The card view is 256 wide against the
/// original's 608, so a 1:1 cursor would cover an eighth of the screen where the
/// original covers a twentieth; 16x16 is the nearest sprite size to that ratio
/// and the smallest that keeps the hand readable.
inline constexpr int kCursorCel = 16;

/// The inventory books live in a 14-row letterbox band, so they get their own
/// cel shape: wide enough for the widest book at the widest layout, and exactly
/// as tall as the tallest. What goes INSIDE the cel -- the size each book is
/// drawn at and where it sits -- is RivenInventory.hpp's business, because it
/// comes from Riven's own strip rects and not from the tBMPs' own dimensions.
inline constexpr int kInvCelW = 32;
inline constexpr int kInvCelH = 16;

/// Riven's cursor ids. cursors.h:43-47.
inline constexpr std::uint16_t kCursorOpenHand = 2003;
inline constexpr std::uint16_t kCursorClosedHand = 2004;
inline constexpr std::uint16_t kCursorMain = 3000;
inline constexpr std::uint16_t kCursorPellet = 5000;
inline constexpr std::uint16_t kCursorHide = 9000;

/// extras.MHK tBMP ids. riven_inventory.cpp:69-77.
inline constexpr std::uint16_t kInvTrapBook = 100;
inline constexpr std::uint16_t kInvAtrusJournal = 101;
inline constexpr std::uint16_t kInvCathJournal = 102;

inline constexpr char kCursorMagic[4] = {'R', 'C', 'U', 'R'};

inline bool isRcur(const RcurHeader &h)
{
    return h.magic[0] == 'R' && h.magic[1] == 'C' && h.magic[2] == 'U'
        && h.magic[3] == 'R' && h.version == kCursorVersion;
}

/// Bytes one cel takes. 8bpp, no padding: the cel is already a whole number of
/// 8x8 tiles.
inline constexpr std::uint32_t rcurCelBytes(int w, int h)
{
    return static_cast<std::uint32_t>(w) * static_cast<std::uint32_t>(h);
}

} // namespace rivendata
