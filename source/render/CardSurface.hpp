#pragma once

// The card picture on the bottom screen: a RAM copy of what should be on it,
// and the dirty bookkeeping that gets the changed parts into a BgSurface buffer.
//
// Drawing goes through a RAM buffer rather than straight to VRAM because Riven
// composites. A card is a still, plus whatever pictures its scripts draw over it
// (opcode 1 and 39), plus any LITE movie overlays; only the result should ever
// reach the screen. That RAM buffer is also what opcodes 20 and 21
// (BeginScreenUpdate / ApplyScreenUpdate) mean on this hardware: mark rows dirty,
// then publish them.
//
// The RAM picture is 256x165 and a BgSurface buffer is 256x256 with the picture
// in its first 165 rows, so a row here is the same row there and publishing is a
// straight copy at the same offset.
//
// DIRTY MASKS ARE PER BUFFER. The display is double buffered, so the buffer being
// written is two frames behind the RAM picture, not one: publishing only the rows
// that changed since the LAST publish would leave the older changes missing from
// that buffer for good. One mask per buffer is the whole fix.

#include <cstdint>
#include <string>

#include "RivenData.hpp"
#include "RivenImage.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"

namespace rivenrt
{

class CardSurface
{
public:
    /// Rows per dirty unit. Eight matches the video decoder's block height, so a
    /// LITE overlay's dirty blocks map onto these one for one.
    static constexpr int kRowBlock = 8;
    static constexpr int kRowBlocks = (rivendata::kViewH + kRowBlock - 1) / kRowBlock; // 21
    static constexpr std::uint32_t kAllDirty = (1u << kRowBlocks) - 1u;

    /// Allocate the RAM picture. False if there is no room for it.
    bool create();

    /// Release everything.
    void destroy();

    /// True while there is a RAM picture.
    bool exists() const { return texels_ != nullptr; }

    /// The RAM picture, kViewW x kViewH ARGB1555. Write to it, then markRows().
    rivendata::Texel *texels() { return texels_; }
    const rivendata::Texel *texels() const { return texels_; }

    /// Draw a .rpic into `cardRect`, which is a PLST rectangle in Riven's
    /// original 608x392 coordinates. The file's own size is whatever the
    /// converter produced, so it is scaled to the rectangle -- the rectangle is
    /// where the picture belongs on the card, and the only thing that knows.
    ///
    /// Marks the rows it touched dirty.
    bool drawPicture(const std::string &path, const rivendata::Rect &cardRect,
                     std::string &error);

    /// Fill the picture with black. Marks everything dirty.
    void clear();

    /// Note that rows [y, y + height) have changed.
    void markRows(int y, int height);
    /// Note that the block rows in `mask` have changed -- the form a LITE movie's
    /// composite returns.
    void markRowMask(std::uint32_t mask);
    void markAll();

    bool anyDirty(int buf) const { return dirty_[buf] != 0; }

    /// Everything in `buf` is stale, so the next publish must send all of it.
    /// Used when a fullscreen movie hands a buffer back.
    void invalidate(int buf) { dirty_[buf] = kAllDirty; }

    /// Copy this buffer's outstanding rows into it. Unlike the texture path this
    /// replaces, there is no upload window and no partial write: the buffer is
    /// not the one being scanned out, so the whole 84 KB can go at once whenever
    /// the caller likes.
    void publish(BgSurface &bg, int buf);

private:
    rivendata::Texel *texels_ = nullptr;
    std::uint32_t dirty_[BgSurface::kBuffers] = {};
};

} // namespace rivenrt
