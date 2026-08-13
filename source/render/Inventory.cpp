#include "Inventory.hpp"

#include <cstdio>

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
        std::printf("%s\n", error.c_str());
        return false;
    }
    if (file_.celWidth() != kInvCelW || file_.celHeight() != kInvCelH)
    {
        std::printf("inventory cels are %dx%d, not %dx%d\n", file_.celWidth(),
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
            std::printf("no OBJ VRAM for the inventory\n");
            destroy();
            return false;
        }
        NEA_Hw2DOBJAssetLoadGfx(assets_[i], file_.pixels(file_.celAt(i)),
                                file_.celBytes());

        items_[i].id = file_.celAt(i).id;
        items_[i].obj = NEA_Hw2DOBJCreateFromAsset(assets_[i]);
        if (items_[i].obj == nullptr)
        {
            std::printf("no OAM entry for the inventory\n");
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
    shown_ = 0;
}

void Inventory::layout()
{
    // Spread whatever is held evenly across the band, in the original's order:
    // Atrus, then Catherine, then the trap book left to right
    // (riven_inventory.cpp:38-43, whose rects run 222 -> 386 in card space).
    // Even spacing rather than the original's exact positions, because the icons
    // here are drawn at a touchable size rather than a scaled one and would
    // otherwise overlap.
    int visible = 0;
    for (const Item &it : items_)
        if (it.id != 0 && (shown_ & (1 << (it.id - kInvTrapBook))) != 0)
            ++visible;
    if (visible == 0)
        return;

    const int share = kScreenW / visible;
    int slot = 0;
    for (const std::uint16_t want : {kInvAtrusJournal, kInvCathJournal, kInvTrapBook})
    {
        for (Item &it : items_)
        {
            if (it.id != want || (shown_ & (1 << (it.id - kInvTrapBook))) == 0)
                continue;
            it.hitLeft = slot * share;
            it.hitRight = (slot == visible - 1) ? kScreenW : (slot + 1) * share;
            it.x = it.hitLeft + (share - kInvCelW) / 2;
            ++slot;
        }
    }
}

void Inventory::update(Engine &e)
{
    if (!exists())
        return;

    // Which books the player holds. rrebel 5 or 6 is Catherine's journal and
    // atrapbook is the trap book (riven_inventory.cpp:63-77).
    const std::uint32_t rrebel = e.vars().get(VarId::RRebel);
    int want = 1 << (kInvAtrusJournal - kInvTrapBook);
    if (rrebel == 5 || rrebel == 6)
        want |= 1 << (kInvCathJournal - kInvTrapBook);
    if (e.vars().get(VarId::ATrapBook) == 1)
        want |= 1 << (kInvTrapBook - kInvTrapBook);

    // Hidden on aspit -- that is the menu and the journals themselves, and the
    // original hides it there too (riven_inventory.cpp:168-193). Hidden while a
    // fullscreen movie owns the screen, because it is a cutscene.
    const bool hide = forcedHidden_ || e.stack().id == StackId::Aspit
                      || e.fullscreenMoviePlaying();

    if (want != shown_ || hide != hidden_)
    {
        shown_ = want;
        hidden_ = hide;
        layout();
        dirty_ = true;
    }
}

std::uint16_t Inventory::hitTest(int x, int y) const
{
    if (!exists() || hidden_ || y < kBandTop || y >= kBandBottom)
        return 0;
    for (const Item &it : items_)
    {
        if (it.id == 0 || (shown_ & (1 << (it.id - kInvTrapBook))) == 0)
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

void Inventory::flush()
{
    if (!exists() || !dirty_)
        return;

    for (Item &it : items_)
    {
        if (it.obj == nullptr)
            continue;
        const bool on =
            !hidden_ && it.id != 0 && (shown_ & (1 << (it.id - kInvTrapBook))) != 0;
        NEA_Hw2DOBJSetVisible(it.obj, on);
        if (on)
        {
            // Two rows above the band, so a 16-row cel gets its full height
            // without the touch rect ever reaching into the card.
            NEA_Hw2DOBJSetPos(it.obj, it.x, kScreenH - kInvCelH);
        }
    }
    dirty_ = false;
}

} // namespace rivenrt
