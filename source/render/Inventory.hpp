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
// WHERE IT GOES. The card view is 256x165 centred in a 256x192 screen, so there
// are two 13-row letterbox bands, and the lower one is where the strip lives.
// That band is 14 rows where the original's strip is 44.
//
// The top screen was the other candidate and cannot work: it is not a touch
// screen. Shrinking the card view to make room for a true 44-row strip would
// throw away a tenth of every picture in the game for three icons.
//
// THE ART AND THE TOUCH TARGET ARE TWO DIFFERENT RECTS, and separating them is
// what lets the books be drawn at Riven's own sizes and positions
// (RivenInventory.hpp). An earlier version of this file conflated them: the
// icons were blown up to a size a stylus could hit and each was given its whole
// share of the band to be hit in, which with one book held meant the ENTIRE
// bottom 14 rows of the touchscreen opened Atrus's journal -- so a tap aimed at
// a step-down hotspot at the base of the picture, or a stylus drag that happened
// to end low, linked you out of the card. Now the art is 8x11 where Riven's is
// 18x24 in a 608-wide screen, and the touch rect is kInvTouchW wide around it.
//
// Engine::processInput adds the other half of that guard: a book opens only if
// the press AND the release land on the same one, so nothing that starts on the
// card can end in the strip.

#include <cstdint>

#include "RivenCursor.hpp"
#include "RivenData.hpp"      // the view geometry the band is measured against
#include "RivenInventory.hpp" // Riven's own strip rects, scaled
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

    /// Recompute which books are shown, from the game's own variables and from
    /// where the pointer is. Cheap; called once per frame from the engine's
    /// frame tail.
    ///
    /// HOVER. The strip is only up while it is being pointed at, which is the
    /// original's rule (`mouse.y >= 392`, riven_inventory.cpp:194) and not a
    /// literal one here: the pointer is moved by the stylus AND by the D-pad,
    /// and both count, because a DS has no hover of its own to borrow. Reading
    /// it from Engine rather than taking a coordinate keeps the one pointer the
    /// hotspots already hover with.
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

    /// The other direction: show the strip even though nothing is pointing at
    /// it (RivenInventory::forceVisible, riven_inventory.cpp:197-200).
    ///
    /// ospit's cage sequence is the only caller in the game and needs exactly
    /// this: the player has just linked out with the trap book, and the two
    /// seconds it is held up are how they are told they got it back. There is
    /// no pointer anywhere near the strip at that moment, and on a DS there
    /// need not be one at all.
    ///
    /// Does NOT beat the reasons the strip is blocked outright -- a cutscene,
    /// aspit, a port screen. Those are about whether the strip may exist at
    /// all; this is only about the hover.
    void setForcedVisible(bool on);

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

    /// Whether a port screen currently has it hidden.
    ///
    /// For a screen that can be opened from ANOTHER port screen -- the notebook,
    /// off the in-game menu -- which has to put the flag back the way it found
    /// it rather than clear it, or leaving the inner screen would show the strip
    /// over the outer one.
    bool suppressed() const { return suppressed_; }

    /// Push to OAM. Vblank only, like every other upload here.
    void flush();

private:
    /// Three items at most: Atrus's journal, Catherine's, and the trap book.
    static constexpr int kMaxItems = 3;

    struct Item
    {
        std::uint16_t id = 0;
        NEA_Hw2DOBJ *obj = nullptr;
        /// Where the art is inside the cel, from the .rcur. The books do not
        /// fill their cels and the touch rect is centred on the art, not on the
        /// sprite.
        int artLeft = 0;
        int artW = 0;
        bool inLayout = false; ///< drawn at all in the layout now held
        int x = 0;             ///< sprite left edge
        int hitLeft = 0;
        int hitRight = 0;
    };

    void layout();

    RcurFile file_;
    NEA_Hw2DOBJAsset *assets_[kMaxItems] = {};
    Item items_[kMaxItems];
    /// Which of Riven's three arrangements is up: 1, 2 or 3 books. Not a bitmask
    /// of what is held, because the original does not place a book independently
    /// of the others -- the books MOVE when one is added (RivenInventory.hpp).
    /// 0 until update() has run once.
    int layout_ = 0;
    bool forcedHidden_ = false;  ///< set by Riven's scripts
    bool forcedVisible_ = false; ///< set by Riven's scripts; beats the hover only
    bool suppressed_ = false;   ///< set by the port's own screens
    /// The strip may not be shown at all: a script, a port screen, aspit or a
    /// cutscene. Separate from hidden_, which adds "and is being pointed at" --
    /// hitTest wants the first question and flush() the second.
    bool blocked_ = false;
    bool hidden_ = false;
    bool dirty_ = true;
};

} // namespace rivenrt
