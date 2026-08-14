#pragma once

// Riven's cursor, on the DS's main OBJ layer.
//
// The port shipped without one, on the argument that a stylus does not need a
// pointer. That is true of WHERE the pointer is and false of WHAT it is over:
// Riven says "you can touch this" only through the cursor's shape, every hotspot
// carries the id of the shape it wants (Hotspot::cursor), and at 256x165 a
// touchable thing is otherwise indistinguishable from scenery. So the cursor is
// back, and it is Riven's own -- read out of riven.exe by the converter.
//
// A stylus has no hover, which would make a cursor that exists only while the
// stylus is down useless for exactly the job it is here to do. So the pointer is
// persistent: touching moves it, lifting leaves it, and the D-pad nudges it with
// A as the click. That gives the port hover, which it has never had.

#include <cstdint>
#include <string>

#include "RivenCursor.hpp"
#include "global_header.hpp"
#include "render/RcurFile.hpp"

namespace rivenrt
{

class Cursor
{
public:
    /// Load cursors/cursors.rcur and build one sprite from it. False if the set
    /// is not on the card -- which is not fatal anywhere: a conversion made
    /// before the cursor stage existed must still boot, just without a cursor.
    bool create();
    void destroy();

    bool exists() const { return obj_ != nullptr; }

    /// Choose a shape by Riven's own cursor id. kCursorHide hides it; an id with
    /// no cel falls back to the main pointer and is reported once.
    void setShape(std::uint16_t id);
    std::uint16_t shape() const { return shape_; }

    /// Where the hot point is, in DS screen pixels.
    void moveTo(int x, int y);

    /// The mode-level owner: the screens that take the display away from the
    /// card view entirely (the menu, the zoom viewer) and have to put the
    /// pointer away with it.
    void setVisible(bool on);

    /// The per-frame one: engine state that hides the pointer without taking the
    /// display away. A blocking movie and a fullscreen cutscene, re-derived
    /// every frame in Engine::flushUploads.
    ///
    /// Separate from setVisible because the two have different owners and
    /// different lifetimes -- one bool shared between them would have a movie
    /// ending un-hide a pointer the menu had put away.
    void setBlocked(bool on);

    /// Push position and shape to OAM. Must run in the vblank window: it ends in
    /// a DMA to OAM, like every other upload in this engine.
    void flush();

private:
    void apply();

    RcurFile file_;
    /// One sprite, rebound to a different asset when the shape changes. The
    /// shared-asset API exists for this: rebinding costs nothing, where creating
    /// a sprite per shape would reallocate OBJ VRAM on every hotspot crossing.
    NEA_Hw2DOBJ *obj_ = nullptr;
    NEA_Hw2DOBJAsset *assets_[32] = {};

    std::uint16_t shape_ = rivendata::kCursorMain;
    int x_ = 128;
    int y_ = 96;
    bool visible_ = true;
    bool blocked_ = false;
    bool dirty_ = true;
};

} // namespace rivenrt
