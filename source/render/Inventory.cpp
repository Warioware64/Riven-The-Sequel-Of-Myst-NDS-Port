#include "Inventory.hpp"

#include <cstdio>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "engine/Engine.hpp"
#include "tonccpy.h"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// aspit's card for each item (riven_inventory.cpp:130-153).
    std::uint16_t cardForItem(std::uint16_t item)
    {
        switch (item)
        {
        case kInvAtrusJournal: return 5;
        case kInvCathJournal:  return 6;
        case kInvTrapBook:     return 7;
        default:               return 0;
        }
    }
} // namespace

bool Inventory::create()
{
    destroy();

    std::string error;
    if (!file_.load(global.extrasDir() + "inventory.rcur", error))
    {
        DebugLog::notice("%s", error.c_str());
        return false;
    }
    if (file_.celWidth() != kInvCelW || file_.celHeight() != kInvCelH)
    {
        DebugLog::notice("inventory cels are %dx%d, not %dx%d", file_.celWidth(),
                    file_.celHeight(), kInvCelW, kInvCelH);
        file_.unload();
        return false;
    }

    // The set's own slice of the OBJ palette. See Cursor::create: written by
    // hand because NEA's loader has no base offset, and the two sets share the
    // one palette the hardware has.
    const int base = file_.paletteBase();
    if (base + file_.paletteCount() <= 256)
        tonccpy(&SPRITE_PALETTE[base], file_.palette(),
                static_cast<std::size_t>(file_.paletteCount()) * sizeof(std::uint16_t));

    const std::size_t cels = file_.celCount() < kMaxItems ? file_.celCount() : kMaxItems;
    for (std::size_t i = 0; i < cels; ++i)
    {
        assets_[i] = NEA_Hw2DOBJAssetCreate(NEA_ENGINE_MAIN, NEA_OBJ_SIZE_32x16,
                                            NEA_OBJ_COLOR_256);
        if (assets_[i] == nullptr)
        {
            DebugLog::notice("no OBJ VRAM for the inventory");
            destroy();
            return false;
        }
        NEA_Hw2DOBJAssetLoadGfx(assets_[i], file_.pixels(file_.celAt(i)),
                                file_.celBytes());

        items_[i].id = file_.celAt(i).id;
        // hotX/drawW on an inventory cel: where the art is inside it. See
        // RcurCel -- the books do not fill their cels and are not centred in
        // them, so this is the only thing that knows what to hit-test.
        items_[i].artLeft = file_.celAt(i).hotX;
        items_[i].artW = file_.celAt(i).drawW;
        items_[i].obj = NEA_Hw2DOBJCreateFromAsset(assets_[i]);
        if (items_[i].obj == nullptr)
        {
            DebugLog::notice("no OAM entry for the inventory");
            destroy();
            return false;
        }
        NEA_Hw2DOBJSetPriority(items_[i].obj, 0);
        NEA_Hw2DOBJSetVisible(items_[i].obj, false);
    }

    dirty_ = true;
    return true;
}

void Inventory::destroy()
{
    for (Item &it : items_)
    {
        if (it.obj != nullptr)
            NEA_Hw2DOBJDelete(it.obj);
        it = Item{};
    }
    for (NEA_Hw2DOBJAsset *&a : assets_)
    {
        if (a != nullptr)
            NEA_Hw2DOBJAssetDelete(a);
        a = nullptr;
    }
    file_.unload();
    layout_ = 0;
}

void Inventory::layout()
{
    // Riven's own positions, scaled -- invCentreX picks the rect this book has
    // in the arrangement now held, and returns -1 for a book the arrangement
    // does not draw. That last part matters: holding the trap book without
    // Catherine's journal shows Atrus's journal ALONE in the original, which is
    // the state the game starts you in.
    //
    // The books were spread evenly across the band here until the icons stopped
    // being oversized. They no longer have to be: at 8-10 columns wide the
    // original's cluster around the middle of the strip fits with room to spare
    // between the touch rects.
    for (Item &it : items_)
    {
        const int centre = it.id == 0 ? -1 : invCentreX(layout_, it.id);
        it.inLayout = centre >= 0;
        if (!it.inLayout)
            continue;

        it.x = centre - kInvCelW / 2;

        // Centred on the ART, not on the cel or on the sprite: a fixed-width
        // target that is bigger than the book it belongs to, which is what pays
        // for drawing the book at the size Riven draws it at.
        const int artCentre = it.x + it.artLeft + it.artW / 2;
        it.hitLeft = artCentre - kInvTouchW / 2;
        it.hitRight = it.hitLeft + kInvTouchW;
    }
}

void Inventory::update(Engine &e)
{
    if (!exists())
        return;

    // Which books the player holds. rrebel 5 or 6 is Catherine's journal and
    // atrapbook is the trap book (riven_inventory.cpp:66-67), and those two
    // between them name one of the three arrangements.
    const std::uint32_t rrebel = e.vars().get(VarId::RRebel);
    const int want = invLayoutFor(rrebel == 5 || rrebel == 6,
                                  e.vars().get(VarId::ATrapBook) == 1);

    // Reasons the strip may not be shown AT ALL, whatever the pointer is doing.
    // Hidden on aspit -- that is the menu and the journals themselves, and the
    // original hides it there too (riven_inventory.cpp:168-193). Hidden while a
    // fullscreen movie owns the screen, because it is a cutscene.
    const bool blocked = forcedHidden_ || suppressed_ || e.stack().id == StackId::Aspit
                         || e.fullscreenMoviePlaying();

    // And the last condition, which is the pointer's: the strip appears when it
    // is pointed at and not before. RivenInventory::isVisible ends on
    // `mouse.y >= 392` (riven_inventory.cpp:194-195) -- the band under the
    // picture -- and kBandTop is that line in DS rows. Without it Atrus's
    // journal sat on the screen for the whole game.
    const bool hide = blocked || (!forcedVisible_ && e.pointerY() < kBandTop);

    if (want != layout_ || hide != hidden_ || blocked != blocked_)
    {
        layout_ = want;
        hidden_ = hide;
        blocked_ = blocked;
        layout();
        dirty_ = true;
    }
}

std::uint16_t Inventory::hitTest(int x, int y) const
{
    // blocked_ and not hidden_: the band test below IS the hover test, and
    // hidden_ is one frame behind it. Engine::processInput calls this before
    // Engine::frame gets to update(), so on the frame the pointer first enters
    // the band hidden_ is still true -- and a tap that arrives with it, which is
    // what a stylus tap on the strip looks like, would be thrown away.
    if (!exists() || blocked_ || y < kBandTop || y >= kBandBottom)
        return 0;
    for (const Item &it : items_)
    {
        if (!it.inLayout)
            continue;
        if (x >= it.hitLeft && x < it.hitRight)
            return it.id;
    }
    return 0;
}

void Inventory::click(Engine &e, std::uint16_t item)
{
    const std::uint16_t card = cardForItem(item);
    if (card == 0)
        return;

    // Already in a book: the strip is hidden on aspit, so this cannot normally
    // happen, and the original guards it the same way (riven_inventory.cpp:118).
    if (e.stack().id == StackId::Aspit)
        return;

    // Where to come back to. The journals' own back hotspots read these, which
    // is the whole reason the strip has to write them before it links away.
    e.vars().at(VarId::ReturnStackId) = static_cast<std::uint32_t>(e.stack().id);
    e.vars().at(VarId::ReturnCardId) = e.globalCardId(e.cardId());

    if (e.changeToStack(StackId::Aspit))
        e.changeToCard(card);
}

void Inventory::setForcedHidden(bool on)
{
    if (on == forcedHidden_)
        return;
    forcedHidden_ = on;
    dirty_ = true;
}

void Inventory::setForcedVisible(bool on)
{
    if (on == forcedVisible_)
        return;
    forcedVisible_ = on;
    dirty_ = true;
    // Nothing else to do either way: update() runs every frame and re-derives
    // hidden_ from this, which is the only thing it changes.
}

void Inventory::setSuppressed(bool on)
{
    if (on == suppressed_)
        return;
    suppressed_ = on;
    dirty_ = true;

    // Hiding is settled here; showing again is left to update().
    //
    // The asymmetry is the point. A port screen owns the frame while it is up,
    // so update() is not running and cannot be relied on to hide the strip
    // before the screen draws over it -- but it IS running again by the time
    // anything could see the strip come back, and it is the only thing that
    // knows the rest of the answer (the stack, a movie). Deciding "visible"
    // here would mean duplicating that test and getting to disagree with it.
    if (on)
    {
        blocked_ = true;
        hidden_ = true;
    }
}

void Inventory::flush()
{
    if (!exists() || !dirty_)
        return;

    for (Item &it : items_)
    {
        if (it.obj == nullptr)
            continue;
        const bool on = !hidden_ && it.inLayout;
        NEA_Hw2DOBJSetVisible(it.obj, on);
        if (on)
        {
            // The cel's bottom row IS the screen's, which is what makes the
            // per-book offsets the converter baked in read as Riven's own gaps
            // to the bottom of the strip. The cel starts two rows above the
            // band; the touch rect never follows it up there.
            NEA_Hw2DOBJSetPos(it.obj, it.x, kScreenH - kInvCelH);
        }
    }
    dirty_ = false;
}

} // namespace rivenrt
