#pragma once

// Where the three inventory books go, shared between the converter (which bakes
// each book into a sprite cel at the right size and offset) and the ARM9 (which
// places the cels and hit-tests them).
//
// THE ONE THING TO KNOW: a book's shape does NOT come from its tBMP. Riven
// stretches each image into a fixed rect -- RivenInventory::draw calls
// drawExtrasImageToScreen(id, rect) (riven_inventory.cpp:69-79) and that scales
// to the rect, so tBMP 102 is 22x12 on disc and 20x11 on screen while tBMP 100
// is 24x36 on disc and 23x36 on screen. The rects below are therefore the only
// authority on how big a book is and what shape it is, and this port scales them
// rather than the bitmaps.
//
// Getting that wrong is what the port did until now: each book was fitted to the
// cel independently, so the three came out at three different scales (0.44, 0.67
// and 1.00). Catherine's journal -- a thin sliver in Riven -- became the widest
// thing on the strip and the trap book, the biggest item in Riven, the smallest.
//
// THE SCALE IS UNIFORM AND IT IS NOT THE CARD'S. One factor for all three books,
// so their sizes stay in the original's proportion to each other. It is chosen
// so the tallest rect exactly fills the cel: 16/36 = 0.444, which lands within
// 6% of the card's own 256/608 = 0.421 -- close enough that the strip is at the
// scale of everything else on screen, and it fills the cel rather than wasting
// rows.
//
// WHAT THIS COSTS: Atrus's journal comes out 8x11 and Catherine's 9x5. Those are
// small. They are, however, small in exactly the way they are small in Riven,
// and the touch target is no longer the art -- Inventory.cpp gives each book a
// fixed-width rect around it, which is what makes shrinking the art affordable.

#include <cstdint>

#include "RivenCursor.hpp" // the tBMP ids and the cel this all has to fit
#include "RivenData.hpp"   // kCardH: the picture the strip hangs under

namespace rivendata
{

/// The strip, in Riven's own screen. The window is 608x436: the 608x392 picture
/// (kCardH) and then 44 rows of inventory (riven_inventory.cpp:83-84).
inline constexpr int kInvStripTop = kCardH;
inline constexpr int kInvStripBottom = 436;

/// One book in one layout: the rect Riven stretches its tBMP into, in that
/// 608x436 screen.
struct RivenInvRect
{
    /// How many books are held when this rect is used -- see kInvRects.
    std::uint8_t layout;
    std::uint16_t id;
    std::int16_t left, top, right, bottom;

    constexpr int width() const { return right - left; }
    constexpr int height() const { return bottom - top; }
};

/// RivenInventory's three layouts, verbatim (riven_inventory.cpp:37-42, chosen
/// between at :64-80). Riven does not place a book independently of the others:
/// it has one arrangement for "Atrus's journal alone", one for "both journals"
/// and one for "both journals and the trap book", and the books move when the
/// arrangement changes. Note the middle one is keyed on Catherine's journal, so
/// holding the trap book without it still shows Atrus's alone.
inline constexpr RivenInvRect kInvRects[] = {
    {1, kInvAtrusJournal, 295, 402, 313, 426},
    {2, kInvAtrusJournal, 259, 402, 278, 426},
    {2, kInvCathJournal, 328, 408, 348, 419},
    {3, kInvAtrusJournal, 222, 402, 240, 426},
    {3, kInvCathJournal, 291, 408, 311, 419},
    {3, kInvTrapBook, 363, 396, 386, 432},
};
inline constexpr int kInvRectCount = static_cast<int>(sizeof(kInvRects) / sizeof(kInvRects[0]));

/// The tallest rect any book is drawn at -- the trap book's 36 rows. The scale
/// is pinned to it, so that book exactly fills the cel and nothing overflows.
inline constexpr int invTallestRect()
{
    int tallest = 0;
    for (const RivenInvRect &r : kInvRects)
        if (r.height() > tallest)
            tallest = r.height();
    return tallest;
}

inline constexpr int kInvScaleNum = kInvCelH;
inline constexpr int kInvScaleDen = invTallestRect();
static_assert(kInvScaleDen == 36, "the trap book's rect is the tallest, at 36 rows");

/// Original rows/columns -> DS pixels, rounded rather than truncated: these are
/// single-digit results and dropping half a pixel off an 11-row book is a 5%
/// error in its shape.
inline constexpr int invScale(int v)
{
    return (v * kInvScaleNum + kInvScaleDen / 2) / kInvScaleDen;
}

/// The size a book is drawn at, in DS pixels. One size per book across all three
/// layouts -- Riven's Atrus rects differ by a single column between layouts and
/// that is below this scale's resolution -- so one cel serves every layout and
/// the runtime never re-scales.
inline constexpr int invDrawW(std::uint16_t id)
{
    int widest = 0;
    for (const RivenInvRect &r : kInvRects)
        if (r.id == id && invScale(r.width()) > widest)
            widest = invScale(r.width());
    return widest;
}

inline constexpr int invDrawH(std::uint16_t id)
{
    int tallest = 0;
    for (const RivenInvRect &r : kInvRects)
        if (r.id == id && invScale(r.height()) > tallest)
            tallest = invScale(r.height());
    return tallest;
}

/// Where the art sits inside its cel.
///
/// Horizontally centred, because the cel is centred on the rect's centre.
/// Vertically the books are NOT level with each other in Riven -- the trap book
/// hangs nearly the full 44 rows while Catherine's journal floats in the middle
/// of the strip -- so the offset preserves each book's gap to the bottom of the
/// strip, which is the bottom of the DS screen. Clamped for the trap book, whose
/// faithful 2-row gap does not fit under a 36-row book in a 16-row cel.
inline constexpr int invDrawOffX(std::uint16_t id)
{
    return (kInvCelW - invDrawW(id)) / 2;
}

inline constexpr int invDrawOffY(std::uint16_t id)
{
    int gap = kInvCelH; // no rect for this id: pin it to the top, harmlessly
    for (const RivenInvRect &r : kInvRects)
        if (r.id == id)
        {
            const int g = invScale(kInvStripBottom - r.bottom);
            if (g < gap)
                gap = g;
        }
    const int off = kInvCelH - gap - invDrawH(id);
    return off < 0 ? 0 : off;
}

static_assert(invDrawW(kInvAtrusJournal) == 8 && invDrawH(kInvAtrusJournal) == 11);
static_assert(invDrawW(kInvCathJournal) == 9 && invDrawH(kInvCathJournal) == 5);
static_assert(invDrawW(kInvTrapBook) == 10 && invDrawH(kInvTrapBook) == 16);
static_assert(invDrawOffX(kInvTrapBook) + invDrawW(kInvTrapBook) <= kInvCelW);
static_assert(invDrawOffY(kInvCathJournal) + invDrawH(kInvCathJournal) <= kInvCelH);

/// Which layout N books means, from the two variables Riven itself reads
/// (riven_inventory.cpp:66-67, :124-125). Catherine's journal decides between 1
/// and 2 and the trap book only ever adds to it, which is why the trap book on
/// its own shows nothing.
inline constexpr int invLayoutFor(bool hasCathBook, bool hasTrapBook)
{
    if (!hasCathBook)
        return 1;
    return hasTrapBook ? 3 : 2;
}

/// The centre of a book's rect in DS columns, or -1 when that book is not in
/// that layout. Rounded, for the same reason invScale is.
///
/// Uses the CARD's scale and not the strip's: this is a position on the screen,
/// and it has to line up with the picture above it, whereas the sizes above are
/// a compromise the 14-row band forces. Riven clusters the books around the
/// middle (0.38, 0.50 and 0.62 of the width), which is the arrangement the
/// player recognises.
inline constexpr int invCentreX(int layout, std::uint16_t id)
{
    for (const RivenInvRect &r : kInvRects)
        if (r.layout == layout && r.id == id)
            return ((r.left + r.right) * kScaleNum + kScaleDen) / (2 * kScaleDen);
    return -1;
}

static_assert(invCentreX(1, kInvAtrusJournal) == 128);
static_assert(invCentreX(3, kInvAtrusJournal) == 97);
static_assert(invCentreX(3, kInvCathJournal) == 127);
static_assert(invCentreX(3, kInvTrapBook) == 158);
static_assert(invCentreX(1, kInvTrapBook) == -1);

/// How wide a book's touch rect is, centred on its art.
///
/// DELIBERATELY WIDER THAN THE ART, which is the whole reason the art could be
/// shrunk to Riven's proportions. 8 columns is not a stylus target; 24 is. The
/// closest two books ever get is layout 3's 30-column gap between Atrus's and
/// Catherine's centres, so 24 still leaves clear space between them and a tap
/// cannot be ambiguous.
inline constexpr int kInvTouchW = 24;
static_assert(kInvTouchW < invCentreX(3, kInvCathJournal) - invCentreX(3, kInvAtrusJournal),
              "touch rects must not meet in the tightest layout");
static_assert(kInvTouchW < invCentreX(3, kInvTrapBook) - invCentreX(3, kInvCathJournal),
              "touch rects must not meet in the tightest layout");
static_assert(kInvTouchW < invCentreX(2, kInvCathJournal) - invCentreX(2, kInvAtrusJournal),
              "touch rects must not meet in the two-book layout");

} // namespace rivendata
