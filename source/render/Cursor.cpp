#include "Cursor.hpp"

#include <cstdio>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "tonccpy.h"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// NEA's OBJ size enum for a square cel. Only the sizes the two sets use.
    bool objSizeFor(int w, int h, NEA_OBJSize &out)
    {
        if (w == 16 && h == 16)
            out = NEA_OBJ_SIZE_16x16;
        else if (w == 32 && h == 16)
            out = NEA_OBJ_SIZE_32x16;
        else if (w == 32 && h == 32)
            out = NEA_OBJ_SIZE_32x32;
        else
            return false;
        return true;
    }
} // namespace

bool Cursor::create()
{
    destroy();

    std::string error;
    if (!file_.load(global.cursorsDir() + "cursors.rcur", error))
    {
        DebugLog::notice("%s", error.c_str());
        return false;
    }

    NEA_OBJSize size;
    if (!objSizeFor(file_.celWidth(), file_.celHeight(), size))
    {
        DebugLog::notice("cursor cels are %dx%d, which is not a sprite size",
                    file_.celWidth(), file_.celHeight());
        file_.unload();
        return false;
    }

    // The palette goes in by hand rather than through NEA_Hw2DOBJLoadPalette,
    // which writes from entry 0 and has no base-offset form. This set owns
    // entries [paletteBase, paletteBase + count) and nothing else may be touched:
    // the inventory owns the rest of the same 256-entry palette, and either set
    // has to survive the other being reconverted or absent.
    const int base = file_.paletteBase();
    if (base + file_.paletteCount() <= 256)
        tonccpy(&SPRITE_PALETTE[base], file_.palette(),
                static_cast<std::size_t>(file_.paletteCount()) * sizeof(std::uint16_t));

    const std::size_t cels = file_.celCount() < 32 ? file_.celCount() : 32;
    for (std::size_t i = 0; i < cels; ++i)
    {
        assets_[i] = NEA_Hw2DOBJAssetCreate(NEA_ENGINE_MAIN, size, NEA_OBJ_COLOR_256);
        if (assets_[i] == nullptr)
        {
            DebugLog::notice("no OBJ VRAM for the cursor set");
            destroy();
            return false;
        }
        NEA_Hw2DOBJAssetLoadGfx(assets_[i], file_.pixels(file_.celAt(i)),
                                file_.celBytes());
    }

    obj_ = NEA_Hw2DOBJCreateFromAsset(assets_[0]);
    if (obj_ == nullptr)
    {
        DebugLog::notice("no OAM entry for the cursor");
        destroy();
        return false;
    }
    // Priority 0 puts the sprite over the 3D layer, which is the card.
    NEA_Hw2DOBJSetPriority(obj_, 0);

    shape_ = kCursorMain;
    dirty_ = true;
    apply();
    return true;
}

void Cursor::destroy()
{
    if (obj_ != nullptr)
    {
        NEA_Hw2DOBJDelete(obj_);
        obj_ = nullptr;
    }
    for (NEA_Hw2DOBJAsset *&a : assets_)
    {
        if (a != nullptr)
            NEA_Hw2DOBJAssetDelete(a);
        a = nullptr;
    }
    file_.unload();
}

void Cursor::setShape(std::uint16_t id)
{
    if (id == shape_)
        return;
    shape_ = id;
    dirty_ = true;
}

void Cursor::moveTo(int x, int y)
{
    if (x == x_ && y == y_)
        return;
    x_ = x;
    y_ = y;
    dirty_ = true;
}

void Cursor::setVisible(bool on)
{
    if (on == visible_)
        return;
    visible_ = on;
    dirty_ = true;
}

void Cursor::setBlocked(bool on)
{
    if (on == blocked_)
        return;
    blocked_ = on;
    dirty_ = true;
}

void Cursor::apply()
{
    if (obj_ == nullptr)
        return;

    // 9000 is a real cursor in riven.exe and is entirely transparent -- the
    // original hides the pointer by drawing nothing. Hiding the sprite is the
    // same thing and costs no OAM.
    if (shape_ == kCursorHide || !visible_ || blocked_)
    {
        NEA_Hw2DOBJSetVisible(obj_, false);
        return;
    }

    const RcurCel *cel = file_.find(shape_);
    if (cel == nullptr)
    {
        cel = file_.find(kCursorMain);
        if (cel == nullptr)
        {
            NEA_Hw2DOBJSetVisible(obj_, false);
            return;
        }
    }

    const std::size_t index = static_cast<std::size_t>(cel - &file_.celAt(0));
    if (index < 32 && assets_[index] != nullptr)
        NEA_Hw2DOBJBindAsset(obj_, assets_[index]);

    // The hot point is what sits at the pointer, not the top-left corner.
    NEA_Hw2DOBJSetPos(obj_, x_ - cel->hotX, y_ - cel->hotY);
    NEA_Hw2DOBJSetVisible(obj_, true);
}

void Cursor::flush()
{
    if (obj_ == nullptr)
        return;
    if (dirty_)
    {
        apply();
        dirty_ = false;
    }
    NEA_Hw2DOBJUpdate(NEA_ENGINE_MAIN);
}

} // namespace rivenrt
