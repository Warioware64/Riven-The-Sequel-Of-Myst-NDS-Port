// The notebook: taking a note, and the page the player scribbles on.
//
// Defined out of line as Engine members, the way the Myst port keeps
// BookNoteView.cpp apart from its engine, and for the same reason: this reaches
// the card the note is being taken of and the frame pump that keeps the audio
// alive, without either of them becoming public API. BookNotes.hpp is the
// storage and the argument for the format; this file is only the screen.
//
// SINGLE-BUFFERED, WHICH IS THE ONE STRUCTURAL DECISION HERE. Everything else
// on this screen follows from it.
//
// BgSurface shows two layers at once and flips by swapping their priority, so a
// screen that draws into the back buffer and flips is showing the buffer it
// drew TWO repaints ago on alternate frames. That is fine for a menu, which
// redraws itself whole. It is not fine for ink: a stylus stroke is a handful of
// pixels a frame, and pushing only those to a buffer that holds a stale page
// would leave every other frame's worth of line missing. Myst solved this with
// per-buffer delta tracking; this port has a cheaper answer, because it does not
// need to flip at all once the page is up.
//
// So: draw the page into the back buffer once, flip once, and from then on write
// the buffer that is ON SCREEN and never flip again. A stroke is then a direct
// VRAM write of the pixels it actually covers. The one rule is that the buffer
// written must never be BgSurface::parkedBuffer() -- that one holds the card the
// game comes back to -- and taking the front buffer AFTER the flip is exactly
// what guarantees it is not, because the takeover parked the buffer that was
// front before it.
//
// THE PAGE IS COMPOSED IN RAM, not on screen. `page_` is the capture with the
// surviving strokes drawn over it, which is what makes the eraser exact: a
// stroke is dropped from the list and the page is rebuilt from a capture that
// was never drawn on. Painting the ink onto the picture would make erasing one
// of two crossing lines impossible.

#include "engine/Engine.hpp"

#include <cstdio>

#include "BookNotes.hpp"
#include "DebugLog.hpp"
#include "Global.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "render/TextLayer.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// The toolbar's band at the bottom of the screen, and the rows left over
    /// for the page above it.
    ///
    /// Fixed, so it does not jump when a shorter note is opened -- the page
    /// scrolls under it instead. A note is kViewH (165) tall and only 157 rows
    /// show, so there are eight rows to pan through.
    constexpr int kBarH = 35;
    constexpr int kBarTop = kScreenH - kBarH; // 157
    constexpr int kPageVisH = kBarTop;

    constexpr int kCellCount = 6;
    constexpr int kCellW = 41;
    constexpr int kCellX0 = (kScreenW - kCellCount * kCellW) / 2;
    constexpr int kCellTop = kBarTop + 4;
    constexpr int kCellH = 18;

    enum Cell
    {
        CellPrev = 0,
        CellDraw,
        CellInk,
        CellErase,
        CellDel,
        CellNext,
    };

    constexpr int kBrushWidth = 2;
    /// Generous on purpose. An eraser the size of the brush is unusable with a
    /// stylus -- the player aims at a line they can see and misses it by the
    /// two pixels their hand moved putting the stylus down.
    constexpr int kEraseRadius = 6;

    constexpr Texel kBarBg = 0x8000;                                  // opaque black
    constexpr Texel kBarLine = static_cast<Texel>(0x8000 | (12 << 10) | (12 << 5) | 12);
    constexpr Texel kBarSel = static_cast<Texel>(0x8000 | (20 << 10) | (20 << 5) | 20);

    int cellX(int c) { return kCellX0 + c * kCellW; }

    /// Which toolbar cell a touch landed in, or -1.
    int cellAt(int x, int y)
    {
        if (y < kCellTop || y >= kCellTop + kCellH)
            return -1;
        if (x < kCellX0 || x >= kCellX0 + kCellCount * kCellW)
            return -1;
        return (x - kCellX0) / kCellW;
    }

    /// A filled square of side `w` centred on (x, y), clipped to the page.
    ///
    /// A square and not a disc: at widths of two and three pixels the two are
    /// the same handful of texels, and the square is a bounds check instead of a
    /// distance test per pixel.
    void brushDot(Texel *page, int pw, int ph, int x, int y, Texel colour, int w)
    {
        const int r = w / 2;
        for (int dy = -r; dy <= r; ++dy)
        {
            const int py = y + dy;
            if (py < 0 || py >= ph)
                continue;
            for (int dx = -r; dx <= r; ++dx)
            {
                const int px = x + dx;
                if (px < 0 || px >= pw)
                    continue;
                page[py * pw + px] = colour;
            }
        }
    }

    /// Bresenham, because the stylus is sampled once a frame and a fast hand
    /// leaves gaps of twenty pixels between samples.
    void brushLine(Texel *page, int pw, int ph, int x0, int y0, int x1, int y1,
                   Texel colour, int w)
    {
        int dx = x1 - x0;
        int dy = y1 - y0;
        dx = dx < 0 ? -dx : dx;
        dy = dy < 0 ? -dy : dy;
        const int sx = x0 < x1 ? 1 : -1;
        const int sy = y0 < y1 ? 1 : -1;
        int err = dx - dy;
        while (true)
        {
            brushDot(page, pw, ph, x0, y0, colour, w);
            if (x0 == x1 && y0 == y1)
                break;
            const int e2 = err * 2;
            if (e2 > -dy)
            {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx)
            {
                err += dx;
                y0 += sy;
            }
        }
    }

    /// Is any point of `s` within `r` of (x, y)?
    ///
    /// Squared distance against every stored point rather than against the
    /// segments between them. The points are at most a frame apart, so the gap
    /// this misses is smaller than the eraser is wide.
    bool strokeNear(const BookNotes::Stroke &s, int x, int y, int r)
    {
        const int rr = r * r;
        const std::size_t n = s.points();
        for (std::size_t i = 0; i < n; ++i)
        {
            const int dx = static_cast<int>(s.x[i]) - x;
            const int dy = static_cast<int>(s.y[i]) - y;
            if (dx * dx + dy * dy <= rr)
                return true;
        }
        return false;
    }
} // namespace

void Engine::captureNote()
{
    if (!global.hasFat)
    {
        DebugLog::warn("note: no card to write to");
        return;
    }
    if (!bgs.exists())
        return;
    if (fullscreenMoviePlaying())
    {
        // The buffers belong to the movie and the frame on screen is halfway
        // through being replaced. Nothing useful could be captured.
        setStatus("no note during a cutscene");
        return;
    }

    // HOW TALL depends on which screen is up, and getting this wrong is the one
    // mistake here that would not look like a mistake.
    //
    // A card view is letterboxed: BgSurface rests the layers at -kViewOffsetY,
    // so bitmap row r is card row r and the picture is kViewH (165) rows. The
    // zoom viewer turns the letterbox OFF and draws a 1:1 window over all 192
    // rows with no chrome in the bottom 27 (ZoomView.cpp) -- so reading 165 rows
    // there would silently drop the bottom of what the player is looking at, and
    // the note would come back looking perfectly fine.
    //
    // Either way bitmap row r is SCREEN row r for the rows being read, so there
    // is no offset term in the loop.
    const int noteH = mode_ == Mode::Zoom ? kScreenH : kViewH;

    // The BgSurface buffer and not the card's RAM picture, for the reason
    // DebugLog::screenshot gives: it is whatever actually owns the screen -- a
    // movie overlay, the zoom viewer, the water running over a still -- and not
    // just the picture the engine believes it drew.
    //
    // Mid-pan the zoom viewer still owes a redraw (ZoomView::redrawsLeft_), so a
    // capture taken on the exact frame of a pan can be up to 8 px behind the
    // screen. Not worth forcing a redraw for: the player has to let go of the
    // D-pad to reach L anyway.
    const Texel *src = BgSurface::pixels(bgs.frontBuffer());
    std::vector<std::uint16_t> px(static_cast<std::size_t>(kViewW) * noteH);
    for (int y = 0; y < noteH; ++y)
        for (int x = 0; x < kViewW; ++x)
            px[static_cast<std::size_t>(y) * kViewW + x] = src[y * BgSurface::kBufW + x];

    // Up to 96 KB of SD write. Done between two pumped frames so the audio
    // stream is fed across the stall instead of dropping a chunk of the
    // ambience.
    idleFrame();
    const int index = BookNotes::capture(px.data(), kViewW, noteH,
                                         static_cast<std::uint8_t>(stack_.id), cardId_);
    idleFrame();

    if (index < 0)
    {
        DebugLog::warn("note: not taken (notebook full, or the card refused it)");
        setStatus("notebook full");
        return;
    }
    DebugLog::note("NOTE %d from %s/%u", index, stackName(stack_.id), cardId_);
    BookNotes::setLastOpened(index);
    setStatus("note taken");
}

void Engine::runNotebook()
{
    if (!global.hasFat || !bgs.exists())
        return;
    if (fullscreenMoviePlaying())
    {
        // A cutscene owns the background buffers and flips them itself
        // (Engine::flushUploads), so it would fight this screen for the one it
        // claims below. The zoom viewer is refused during one for the same
        // reason, and a note taken here would be of a half-replaced frame
        // anyway.
        setStatus("not during a cutscene");
        return;
    }

    std::vector<BookNotes::NoteInfo> notes = BookNotes::scan();
    if (notes.empty())
    {
        DebugLog::note("NOTES none yet -- L takes one");
        setStatus("the notebook is empty");
        return;
    }

    // --- take the screen ---------------------------------------------------
    //
    // The same protocol every port screen follows (MainMenu's ScreenTakeover),
    // restated here because this file is not allowed to reach into that one's
    // anonymous namespace.
    const bool inGame = booted();
    if (inGame)
        bgs.beginMovieTakeover();
    bgs.setLetterbox(false);
    CursorHide cursorGuard{*this};
    const bool invWasSuppressed = inventory_.suppressed();
    inventory_.setSuppressed(true);
    inventory_.flush();

    // --- page state --------------------------------------------------------
    std::vector<Texel> base;  ///< the capture, never written
    std::vector<Texel> page_; ///< capture + surviving strokes: what is shown
    std::vector<BookNotes::Stroke> strokes;
    BookNotes::NoteInfo info;
    int noteW = 0;
    int noteH = 0;
    int scrollY = 0;
    int scrollMax = 0;
    bool strokesDirty = false;

    int at = 0;
    // Open on the note the player last looked at. Its INDEX is what was stored,
    // so this is a search and not a subscript -- positions shift when a note is
    // deleted and indices do not.
    const int want = BookNotes::lastOpened();
    for (std::size_t i = 0; i < notes.size(); ++i)
        if (notes[i].index == want)
            at = static_cast<int>(i);

    bool drawing = false;
    bool erasing = false;
    int ink = 0;
    bool penDown = false;
    int lastX = 0;
    int lastY = 0;
    bool pageDirty = true;
    bool barDirty = true;
    bool confirmDelete = false;

    // The buffer this screen owns. Claimed after the first flip, below.
    int buf = -1;

    const auto saveStrokes = [&]() {
        if (!strokesDirty || notes.empty())
            return;
        strokesDirty = false;
        if (!BookNotes::writeStrokes(notes[at].index, strokes))
            DebugLog::warn("note %d: strokes not saved", notes[at].index);
    };

    /// Rebuild the visible page from the untouched capture plus what is left of
    /// the strokes. The whole reason strokes are stored as strokes.
    const auto recompose = [&]() {
        page_ = base;
        for (const BookNotes::Stroke &s : strokes)
        {
            const Texel colour = BookNotes::kInk[s.color < BookNotes::kInkCount ? s.color : 0];
            const std::size_t n = s.points();
            if (n == 0)
                continue;
            brushDot(page_.data(), noteW, noteH, s.x[0], s.y[0], colour, s.width);
            for (std::size_t i = 1; i < n; ++i)
                brushLine(page_.data(), noteW, noteH, s.x[i - 1], s.y[i - 1], s.x[i], s.y[i],
                          colour, s.width);
        }
    };

    const auto openNote = [&](int which) {
        saveStrokes();
        at = which;
        strokes.clear();
        scrollY = 0;
        // The page can be turned with R or RIGHT while the stylus is still
        // down. Without this the next iteration takes the "already drawing"
        // branch and appends a point to strokes.back() -- of a vector that was
        // just cleared. A lifted pen is also the truthful state: the line the
        // player was drawing belongs to the note they have just left.
        penDown = false;

        if (!BookNotes::readPixels(notes[at].index, base, info))
        {
            // A blank page on its own reads as "my note is gone". Say what
            // happened and what can be done about it, on the page itself.
            DebugLog::warn("NOTE %d picture unreadable", notes[at].index);
            noteW = kViewW;
            noteH = kViewH;
            base.assign(static_cast<std::size_t>(noteW) * noteH, kBarBg);
            page_ = base;
        }
        else
        {
            noteW = info.w;
            noteH = info.h;
            strokes = BookNotes::readStrokes(notes[at].index);
            recompose();
            DebugLog::note("NOTE %d  %s", notes[at].index,
                           BookNotes::label(info).c_str());
        }
        scrollMax = noteH > kPageVisH ? noteH - kPageVisH : 0;
        BookNotes::setLastOpened(notes[at].index);
        pageDirty = true;
        barDirty = true;
    };

    /// Push page rows to VRAM. `y0`/`h` are in NOTE coordinates; anything
    /// outside the visible window is clipped away.
    const auto pushRows = [&](int x0, int y0, int w, int h) {
        if (buf < 0 || page_.empty())
            return;
        Texel *dst = BgSurface::pixels(buf);
        for (int y = y0; y < y0 + h; ++y)
        {
            const int sy = y - scrollY;
            if (y < 0 || y >= noteH || sy < 0 || sy >= kPageVisH)
                continue;
            for (int x = x0; x < x0 + w; ++x)
            {
                if (x < 0 || x >= noteW || x >= kScreenW)
                    continue;
                dst[sy * BgSurface::kBufW + x] = page_[static_cast<std::size_t>(y) * noteW + x];
            }
        }
    };

    /// The whole visible page, plus black wherever the note does not reach.
    const auto pushPage = [&]() {
        if (buf < 0)
            return;
        Texel *dst = BgSurface::pixels(buf);
        for (int sy = 0; sy < kPageVisH; ++sy)
        {
            const int y = sy + scrollY;
            Texel *row = dst + sy * BgSurface::kBufW;
            for (int x = 0; x < kScreenW; ++x)
            {
                row[x] = (y >= 0 && y < noteH && x < noteW)
                             ? page_[static_cast<std::size_t>(y) * noteW + x]
                             : kBarBg;
            }
        }
    };

    /// The toolbar. Its own function because toggling a mode must not cost a
    /// repaint of the page above it.
    const auto pushBar = [&]() {
        if (buf < 0)
            return;
        Texel *dst = BgSurface::pixels(buf);
        for (int y = kBarTop; y < kScreenH; ++y)
        {
            Texel *row = dst + y * BgSurface::kBufW;
            for (int x = 0; x < kScreenW; ++x)
                row[x] = y == kBarTop ? kBarLine : kBarBg;
        }
        // A box around whichever cells are switched on, so the mode is visible
        // without a second line of text saying what it is.
        const auto box = [&](int c, Texel colour) {
            const int x0 = cellX(c);
            for (int x = x0; x < x0 + kCellW - 1 && x < kScreenW; ++x)
            {
                dst[kCellTop * BgSurface::kBufW + x] = colour;
                dst[(kCellTop + kCellH - 1) * BgSurface::kBufW + x] = colour;
            }
            for (int y = kCellTop; y < kCellTop + kCellH; ++y)
            {
                dst[y * BgSurface::kBufW + x0] = colour;
                if (x0 + kCellW - 2 < kScreenW)
                    dst[y * BgSurface::kBufW + x0 + kCellW - 2] = colour;
            }
        };
        if (drawing)
            box(CellDraw, kBarSel);
        if (erasing)
            box(CellErase, kBarSel);
        if (confirmDelete)
            box(CellDel, BookNotes::kInk[0]);
        // The ink cell is boxed in the colour it would draw in, which says which
        // ink is selected without spending a cell's width on its name.
        box(CellInk, BookNotes::kInk[ink]);

        // Text last: a glyph is blitted as a rectangle, so anything drawn under
        // it inside its box is punched out (TextLayer.hpp).
        if (textLayer.ready() && textLayer.target(buf))
        {
            const int ty = kCellTop + 2;
            textLayer.draw(cellX(CellPrev) + 14, ty, "<");
            textLayer.draw(cellX(CellDraw) + 4, ty, "draw");
            textLayer.draw(cellX(CellInk) + 8, ty, "ink");
            textLayer.draw(cellX(CellErase) + 2, ty, "erase");
            textLayer.draw(cellX(CellDel) + 6, ty, confirmDelete ? "SURE?" : "del");
            textLayer.draw(cellX(CellNext) + 14, ty, ">");
        }
    };

    openNote(at);

    // --- claim a buffer ----------------------------------------------------
    //
    // Draw the first page into the back buffer, flip, and let the flip land.
    // After this the front buffer is the one just drawn, and it is not the
    // parked card -- so everything below writes it in place and never flips.
    buf = bgs.backBuffer();
    pushPage();
    pushBar();
    pageDirty = barDirty = false;
    bgs.requestFlip();
    idleFrame();
    buf = bgs.frontBuffer();
    // The flip put a different buffer on screen than the one just filled only
    // if something else had a flip pending; redraw into whatever is now front so
    // the two cannot disagree.
    pushPage();
    pushBar();

    // --- the loop ----------------------------------------------------------
    while (true)
    {
        idleFrame();
        scanKeys();
        const std::uint32_t down = keysDown();
        const std::uint32_t held = keysHeld();

        touchPosition t = {};
        touchRead(&t);

        if (down & (KEY_B | KEY_START))
            break;

        const int tapped = (down & KEY_TOUCH) != 0 ? cellAt(t.px, t.py) : -1;

        if (confirmDelete)
        {
            // A yes/no question owns the input while it is up, so the answer
            // cannot land on a different note than the one being asked about.
            if ((down & KEY_A) != 0 || tapped == CellDel)
            {
                const int index = notes[at].index;
                strokesDirty = false; // the file is going away
                BookNotes::remove(index);
                DebugLog::note("NOTE %d deleted", index);
                notes.erase(notes.begin() + at);
                confirmDelete = false;
                if (notes.empty())
                    break;
                at = at < static_cast<int>(notes.size()) ? at : static_cast<int>(notes.size()) - 1;
                openNote(at);
            }
            else if ((down & (KEY_B | KEY_X | KEY_Y)) != 0 || tapped >= 0)
            {
                confirmDelete = false;
                barDirty = true;
            }
            if (barDirty)
            {
                barDirty = false;
                pushBar();
            }
            if (pageDirty)
            {
                pageDirty = false;
                pushPage();
                pushBar();
            }
            continue;
        }

        // Every action has a button and a cell, resolved into one path so the
        // two can never drift apart.
        const bool wantPrev = tapped == CellPrev || (down & (KEY_L | KEY_LEFT)) != 0;
        const bool wantNext = tapped == CellNext || (down & (KEY_R | KEY_RIGHT)) != 0;

        if (wantPrev || wantNext)
        {
            const int n = static_cast<int>(notes.size());
            openNote(((at + (wantNext ? 1 : -1)) % n + n) % n);
        }
        else if (tapped == CellDraw || (down & KEY_A) != 0)
        {
            drawing = !drawing;
            if (drawing)
                erasing = false;
            saveStrokes();
            barDirty = true;
        }
        else if (tapped == CellErase || (down & KEY_X) != 0)
        {
            erasing = !erasing;
            if (erasing)
                drawing = false;
            saveStrokes();
            barDirty = true;
        }
        else if (tapped == CellInk || (down & KEY_Y) != 0)
        {
            ink = (ink + 1) % BookNotes::kInkCount;
            barDirty = true;
        }
        else if (tapped == CellDel || (down & KEY_SELECT) != 0)
        {
            saveStrokes();
            confirmDelete = true;
            barDirty = true;
        }
        else if (scrollMax > 0 && (down & (KEY_UP | KEY_DOWN)) != 0)
        {
            const int was = scrollY;
            scrollY += (down & KEY_DOWN) != 0 ? 8 : -8;
            scrollY = scrollY < 0 ? 0 : (scrollY > scrollMax ? scrollMax : scrollY);
            if (scrollY != was)
                pageDirty = true;
        }

        // --- the stylus on the page ---------------------------------------
        const bool onPage = t.py < kBarTop;
        if ((held & KEY_TOUCH) != 0 && onPage && (drawing || erasing))
        {
            // Note coordinates, not screen: strokes have to stay where they were
            // put when the page is scrolled under them.
            const int px = t.px;
            const int py = t.py + scrollY;

            if (erasing)
            {
                bool hit = false;
                for (std::size_t i = strokes.size(); i-- > 0;)
                {
                    if (strokeNear(strokes[i], px, py, kEraseRadius))
                    {
                        strokes.erase(strokes.begin() + static_cast<std::ptrdiff_t>(i));
                        hit = true;
                    }
                }
                if (hit)
                {
                    strokesDirty = true;
                    recompose();
                    pageDirty = true;
                }
            }
            else if (!penDown)
            {
                penDown = true;
                BookNotes::Stroke s;
                s.color = static_cast<std::uint8_t>(ink);
                s.width = kBrushWidth;
                s.x.push_back(static_cast<std::uint8_t>(px < 255 ? px : 255));
                s.y.push_back(static_cast<std::uint8_t>(py < 255 ? py : 255));
                strokes.push_back(std::move(s));
                strokesDirty = true;
                brushDot(page_.data(), noteW, noteH, px, py, BookNotes::kInk[ink],
                         kBrushWidth);
                pushRows(px - kBrushWidth, py - kBrushWidth, kBrushWidth * 2 + 1,
                         kBrushWidth * 2 + 1);
                lastX = px;
                lastY = py;
            }
            else if (px != lastX || py != lastY)
            {
                BookNotes::Stroke &s = strokes.back();
                s.x.push_back(static_cast<std::uint8_t>(px < 255 ? px : 255));
                s.y.push_back(static_cast<std::uint8_t>(py < 255 ? py : 255));
                brushLine(page_.data(), noteW, noteH, lastX, lastY, px, py,
                          BookNotes::kInk[ink], kBrushWidth);
                // Only the segment's own box, grown by the brush. This is what
                // the single-buffered design buys: a few hundred texels a frame
                // instead of a 96 KB page push.
                const int x0 = (lastX < px ? lastX : px) - kBrushWidth;
                const int y0 = (lastY < py ? lastY : py) - kBrushWidth;
                const int x1 = (lastX > px ? lastX : px) + kBrushWidth;
                const int y1 = (lastY > py ? lastY : py) + kBrushWidth;
                pushRows(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
                lastX = px;
                lastY = py;
            }
        }
        else if (penDown)
        {
            penDown = false;
            // One small write per finished stroke, rather than one per sample or
            // one per session -- a flat battery mid-page then costs the line
            // being drawn and nothing before it.
            saveStrokes();
        }

        if (pageDirty)
        {
            pageDirty = false;
            barDirty = false;
            pushPage();
            pushBar();
        }
        else if (barDirty)
        {
            barDirty = false;
            pushBar();
        }
    }

    saveStrokes();

    // --- give the screen back ----------------------------------------------
    bgs.setLetterbox(true);
    if (inGame)
    {
        for (int b = 0; b < BgSurface::kBuffers; ++b)
            if (b != bgs.parkedBuffer())
                bgs.resetBuffer(b);
        (void)bgs.endMovieTakeover();
        surface_.invalidateAll();
        applyScreenUpdate(true);
    }
    inventory_.setSuppressed(invWasSuppressed);
    inventory_.flush();
}

} // namespace rivenrt
