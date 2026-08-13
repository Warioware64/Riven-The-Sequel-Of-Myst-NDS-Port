#pragma once

// The inventory strip: the books you carry, and the only route to Atrus's
// journal.
//
// Riven puts a 44-pixel strip under its 608x392 picture area and hangs three
// items in it (riven_inventory.cpp). Clicking one links to aspit card 5, 6 or 7
// and the card's own back hotspot links you home again. Nothing else in the game
// opens a journal -- which is why xaatrusopenbook and atrusBookPage in
// Externals.cpp have been written and unreachable.
//
// WHERE IT GOES, and this is a real departure from the original. The card view
// is 256x165 centred in a 256x192 screen, so there are two 13-row letterbox
// bands, and the lower one is where the strip lives. That band is 14 rows where
// the original's strip is 44, and 44 scaled by 256/608 would be 18 -- but the
// items themselves scale to about 8x10 pixels at that ratio, which no stylus can
// hit. So the mapping is deliberately not literal: the icons are drawn at a size
// a person can touch, spaced across the band, and each takes the whole width of
// its share of it. Their horizontal ORDER and rough position still come from the
// original's rects, so the trap book is still on the right.
//
// The top screen was the other candidate and cannot work: it is not a touch
// screen. Shrinking the card view to make room for a true 44-row strip would
// throw away a tenth of every picture in the game for three icons.

#include <cstdint>

#include "RivenCursor.hpp"
#include "RivenData.hpp" // the view geometry the band is measured against
#include "global_header.hpp"
#include "render/RcurFile.hpp"

namespace rivenrt
{

class Engine;

class Inventory
{
public:
    /// The band, in DS screen rows. Below the card view, which ends at
    /// kViewOffsetY + kViewH = 178.
    static constexpr int kBandTop = rivendata::kViewOffsetY + rivendata::kViewH;
    static constexpr int kBandBottom = rivendata::kScreenH;

    /// Load extras/inventory.rcur. False when the set is not on the card, which
    /// is not fatal: the game runs, and the journals are simply unreachable --
    /// exactly as they were before any of this existed.
    bool create();
    void destroy();

    bool exists() const { return file_.loaded(); }

    /// Recompute which books are shown, from the game's own variables. Cheap;
    /// called once per frame from the engine's frame tail.
    void update(Engine &e);

    /// The tBMP id of the item at a screen point, or 0. Only the band is tested:
    /// the sprites overhang the card by two rows so they get their full height,
    /// but stealing touch rows from the card would break the "step down"
    /// hotspots Riven puts at the very bottom of a view.
    std::uint16_t hitTest(int x, int y) const;

    /// Follow a click on the strip: remember where the player was, then link to
    /// the item's card (riven_inventory.cpp:128-153).
    void click(Engine &e, std::uint16_t item);

    /// ospit's cage sequence forces it visible; the menu and the journals
    /// themselves force it hidden (aspit.cpp:441, :450).
    ///
    /// FOR RIVEN'S SCRIPTS ONLY. See setSuppressed.
    void setForcedHidden(bool on);

    /// Hide it because one of the PORT's own screens is up -- the zoom viewer,
    /// the settings screen -- and put it back afterwards.
    ///
    /// A second flag rather than a second caller of setForcedHidden, because
    /// the two have different owners and a shared boolean cannot serve both: a
    /// screen that set the script's flag and cleared it on the way out would
    /// clear it whatever the script had wanted, and did -- leaving the zoom
    /// viewer was making Atrus's journal reachable on a card whose script had
    /// hidden it.
    void setSuppressed(bool on);

    /// Push to OAM. Vblank only, like every other upload here.
    void flush();

private:
    /// Three items at most: Atrus's journal, Catherine's, and the trap book.
    static constexpr int kMaxItems = 3;

    struct Item
    {
        std::uint16_t id = 0;
        NEA_Hw2DOBJ *obj = nullptr;
        int x = 0;      ///< sprite left edge
        int hitLeft = 0;
        int hitRight = 0;
    };

    void layout();

    RcurFile file_;
    NEA_Hw2DOBJAsset *assets_[kMaxItems] = {};
    Item items_[kMaxItems];
    int shown_ = 0;
    bool forcedHidden_ = false; ///< set by Riven's scripts
    bool suppressed_ = false;   ///< set by the port's own screens
    bool hidden_ = false;
    bool dirty_ = true;
};

} // namespace rivenrt
