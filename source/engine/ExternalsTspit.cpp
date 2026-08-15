// Temple Island's external commands -- ScummVM's riven_stacks/tspit.cpp.
//
// Where the game begins and where it ends, and four things need native code:
//
//   * the TELESCOPE, five positions cut out of one long movie, and the hatch
//     under it whose five buttons are a combination;
//   * the ENDING, which the telescope opens onto -- four of them, chosen by
//     what the player did with Catherine, Gehn and the trap book;
//   * the MARBLE GRID, the only puzzle in Riven whose pieces are dragged around
//     a board, and the only one that moves its own hotspots; and
//   * the DOME, whose five sliders are shared with four other islands.
//
// Plus three commands that are empty here because they are empty in ScummVM
// too, and one that belongs to the demo.

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "DebugLog.hpp"
#include "engine/DomeSpit.hpp"
#include "engine/Engine.hpp"
#include "engine/Externals.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    // --- the telescope -------------------------------------------------------

    /// TSpit::xtisland390_covercombo (tspit.cpp:165-178). The five buttons on the
    /// telescope cover: press them in the order written on the island and the
    /// hatch unlocks, get one wrong and the count goes back to nothing.
    void xtisland390_covercombo(Engine &e, const std::uint16_t *args, std::size_t argCount)
    {
        if (argCount < 1 || args == nullptr)
        {
            DebugLog::warn("xtisland390_covercombo: no button number");
            return;
        }

        std::uint32_t &correctDigits = e.vars().at(VarId::TCoverCombo);
        if (correctDigits < 5
            && args[0] == comboDigit(e.vars().get(VarId::TCorrectOrder), correctDigits))
            ++correctDigits;
        else
            correctDigits = 0;

        const Hotspot *openCover = e.hotspotByName("openCover");
        if (openCover != nullptr)
            e.enableHotspotByIndex(e.hotspotIndexOf(openCover), correctDigits == 5);
    }

    /// Where each of the tube's five positions starts in the travel movie, in
    /// the original's milliseconds (tspit.cpp:93 and :63).
    ///
    /// TWO TABLES, and they are not each other reversed -- 2660 and 1760 going
    /// down have no counterparts at 1680 and 2560 going up. The movies are
    /// different recordings of the same travel and their frames do not line up,
    /// so a single table would drift a few frames further out at every stop.
    constexpr std::uint32_t kUpStops[] = {0, 800, 1680, 2560, 3440, 4320};
    constexpr std::uint32_t kDownStops[] = {4320, 3440, 2660, 1760, 880, 0};

    /// TSpit::xtexterior300_telescopeup (tspit.cpp:102-136). One press raises the
    /// tube one of its five positions, and the animation is the matching slice of
    /// one long movie -- which is what Engine::playMovieRange exists for.
    ///
    /// The half that can decide to do nothing. Split out so the caller can put
    /// the card back exactly once, on every path -- see below.
    void telescopeUpMove(Engine &e)
    {
        // No power, no telescope, and NO SOUND: tspit.cpp:110-112 returns here
        // before the "can it still move" test, on this path and the down one
        // alike. The clunk belongs to a tube that is trying and cannot, which
        // is not what a dead control is doing.
        if (e.vars().get(VarId::TTeleValve) == 0)
            return;

        std::uint32_t &pos = e.vars().at(VarId::TTelescope);
        if (pos >= 5)
        {
            e.playCardSound("tTelDnMore");
            return; // already at the top
        }

        e.playCardSound("tTeleMove");

        const std::uint32_t from = pos >= 1 ? pos - 1 : 0;
        // Two movies of the same travel, one with the cover on and one without.
        e.playMovieRange(e.vars().get(VarId::TTeleCover) != 0 ? 4 : 5, kUpStops[from],
                         kUpStops[from + 1]);

        ++pos;
    }

    void xtopenfissure(Engine &e);

    /// TSpit::xtexterior300_telescopedown (tspit.cpp:58-101). The mirror of the
    /// above, with one difference that is the whole point of the machine: at the
    /// bottom, with the hatch open and the pin up, pressing down again ends the
    /// game.
    ///
    /// Returns false when it has done that, because the ending changes the card
    /// out from under the caller and there must be no refresh after it.
    bool telescopeDownMove(Engine &e)
    {
        if (e.vars().get(VarId::TTeleValve) == 0)
            return true; // no power, and silent -- see telescopeUpMove

        std::uint32_t &pos = e.vars().at(VarId::TTelescope);
        if (pos != 1)
        {
            e.playCardSound("tTeleMove");

            // Bounds: the variable is the only thing that says where the tube
            // is, and a corrupt one would index off the end of a six-entry table
            // and then decrement 0 to four billion.
            if (pos < 2 || pos > 5)
                pos = 5;

            // Indexed DIRECTLY by the position, not mirrored: kDownStops is
            // already the descending table, so slot n is where position n starts
            // and slot n-1 is where it ends (tspit.cpp:81-83). The pair ascends
            // in time even though the tube descends -- the down movie is its own
            // recording, run forwards.
            e.playMovieRange(e.vars().get(VarId::TTeleCover) != 0 ? 1 : 2,
                             kDownStops[pos], kDownStops[pos - 1]);
            --pos;
            return true;
        }

        // At the bottom. Either the fissure opens or it does not.
        if (e.vars().get(VarId::TTeleCover) == 1 && e.vars().get(VarId::TTelePin) == 1)
        {
            xtopenfissure(e);
            return false;
        }
        e.playCardSound("tTelDnMore");
        return true;
    }

    void xtexterior300_telescopeup(Engine &e)
    {
        // The button goes down whether or not anything comes of it.
        e.playMovie(3, true);
        telescopeUpMove(e);

        // ALWAYS, and not only when the tube moved, which is where ScummVM
        // stops -- it returns straight out of the two "nothing happens" paths.
        //
        // The button is a blocking play that runs to the movie's end, and both
        // engines BAKE such a frame into the card: ScummVM's playBlocking()
        // ends in disable() (riven_video.cpp:264-268), and this port matches it
        // (Engine::kBakeBlockingMovies). tMOV 38 ends on a frame that is
        // nothing like the still underneath it -- measured against all twenty
        // of card 137's stills -- so once baked it is the card, and pressing
        // the button with the valve shut leaves a 33x40 patch of it on screen
        // until something redraws. ScummVM has the same hole here; the port
        // does not want it, so it asks for the redraw.
        //
        // The card also draws the tube at its new position from the variable,
        // so the moving case needed this anyway (ScummVM: enter(false)).
        e.refreshCard();
    }

    void xtexterior300_telescopedown(Engine &e)
    {
        e.playMovie(3, true);
        // Not after the ending: the card the refresh would redraw is aspit's
        // menu by then, and the tube it draws is on an island the player has
        // just fallen off.
        if (telescopeDownMove(e))
            e.refreshCard();
    }

    // --- the ending ----------------------------------------------------------

    /// TSpit::xtopenfissure (tspit.cpp:138-163).
    ///
    /// Four endings, and which one the player gets is the whole of Riven's
    /// scoring. They are tested in order and the first that matches wins:
    /// Catherine freed beats Gehn trapped beats the trap book used, and the
    /// fourth is what the game gives someone who opened the hatch without
    /// having earned any of them.
    ///
    /// Choosing the movie is all this does; everything after it -- stopping the
    /// island, playing it, and throwing the game away -- is runEndGame, which
    /// ospit's and rspit's endings share (Externals.hpp).
    void xtopenfissure(Engine &e)
    {
        std::uint16_t index = 11; // the impossible ending
        if (e.vars().get(VarId::PCage) == 2)
            index = 8; // Catherine free, Gehn trapped, Atrus comes
        else if (e.vars().get(VarId::AGehn) == 4)
            index = 9; // Catherine still trapped, Gehn trapped
        else if (e.vars().get(VarId::ATrapBook) == 1)
            index = 10; // Gehn free, and everyone dies

        // The only one of the three endings that has to activate its own
        // record: tspit's card carries all four and picks between them here,
        // where ospit's and rspit's cards have already activated theirs.
        e.activateMlst(index, false);
        runEndGame(e, codeForMlstIndex(e, index));
    }

    // --- the marble grid -----------------------------------------------------
    //
    // Twenty-five slots by twenty-five, six marbles, and a combination the
    // player reads off a book. It is the only puzzle in the game whose pieces
    // move: each marble is a HOTSPOT that follows it around the board, which is
    // why Engine has hotspotRect() at all.
    //
    // A marble's position is one variable packing two 1-based coordinates: x in
    // the low byte, y in bits 16-23, and zero meaning "still in the receptacle".
    // That encoding is the original's and the save format inherits it, so the
    // shifts below are not an implementation detail that could be tidied.

    constexpr int kMarbleCount = 6;
    constexpr int kMarbleHotspotSize = 13;
    constexpr int kSmallMarbleWidth = 4;
    constexpr int kSmallMarbleHeight = 2;
    /// The large marbles in extras/marbles.rpic are laid out left to right in
    /// this order, and so are the small ones in tspit (tsmallred + n).
    constexpr int kLargeMarbleSize = 8;

    /// The variables are named for the colours, and so are the hotspots -- the
    /// SAME strings, which is what lets one loop do both (tspit.cpp:199).
    const char *const kMarbleNames[kMarbleCount] = {"tred",   "torange", "tyellow",
                                                    "tgreen", "tblue",   "tviolet"};
    constexpr VarId kMarbleVars[kMarbleCount] = {VarId::TRed,   VarId::TOrange,
                                                 VarId::TYellow, VarId::TGreen,
                                                 VarId::TBlue,  VarId::TViolet};

    int marbleX(std::uint32_t v) { return static_cast<int>(v & 0xff) - 1; }
    int marbleY(std::uint32_t v) { return static_cast<int>((v >> 16) & 0xff) - 1; }

    std::uint32_t withMarbleX(std::uint32_t v, int x)
    {
        return (v & 0xff00u) | static_cast<std::uint32_t>(x + 1);
    }
    std::uint32_t withMarbleY(std::uint32_t v, int y)
    {
        return (static_cast<std::uint32_t>(y + 1) << 16) | (v & 0xffu);
    }

    /// Is a packed position one the board actually has a slot for?
    ///
    /// The variable is 32 bits of a save file and only ever twenty-five values
    /// wide in each direction, so a corrupt or hand-edited one reaches here as
    /// a y of up to 254 -- and marbleGridRect would index a five-entry table
    /// with y/5. Every path that turns a position into a rectangle asks this
    /// first (ScummVM asks nowhere and would read off the end).
    bool marbleOnBoard(std::uint32_t pos)
    {
        return pos != 0 && marbleX(pos) >= 0 && marbleX(pos) < 25 && marbleY(pos) >= 0
               && marbleY(pos) < 25;
    }

    /// tspit.cpp:219-227. The board is five blocks of five in each direction,
    /// with a gap between blocks, so the offset is a block origin plus a stride.
    /// x and y must be 0..24 -- see marbleOnBoard.
    Rect marbleGridRect(int x, int y)
    {
        static const int kBlockX[] = {134, 202, 270, 338, 406};
        static const int kBlockY[] = {24, 92, 159, 227, 295};
        const int left = kBlockX[x / 5] + (x % 5) * kMarbleHotspotSize;
        const int top = kBlockY[y / 5] + (y % 5) * kMarbleHotspotSize;
        return Rect{static_cast<std::int16_t>(left), static_cast<std::int16_t>(top),
                    static_cast<std::int16_t>(left + kMarbleHotspotSize),
                    static_cast<std::int16_t>(top + kMarbleHotspotSize)};
    }

    /// The six receptacle rects, as the card data has them.
    ///
    /// ScummVM caches these on the stack object, which lives as long as the
    /// player is on the island (tspit.cpp:311-317, `if (_marbleBaseHotspots
    /// .empty())`). Here they are read fresh from the card each time instead:
    /// resetCardState restores hotspotRect_ from the data on every entry, so
    /// the data IS the cache and one that outlived the card would be a way for
    /// a stale rect to survive a card change.
    ///
    /// Returns false when the card is not the marble card, which is the only
    /// way any of this is reachable by mistake.
    bool marbleHotspots(Engine &e, const Hotspot *out[kMarbleCount])
    {
        for (int i = 0; i < kMarbleCount; ++i)
        {
            out[i] = e.hotspotByName(kMarbleNames[i]);
            if (out[i] == nullptr)
            {
                DebugLog::warn("marbles: card %u has no \"%s\" hotspot",
                               static_cast<unsigned>(e.cardId()), kMarbleNames[i]);
                return false;
            }
        }
        return true;
    }

    /// TSpit::setMarbleHotspots (tspit.cpp:298-309). Put each marble's hotspot
    /// where the marble is: its slot on the grid, or back on its receptacle.
    void setMarbleHotspots(Engine &e)
    {
        const Hotspot *spots[kMarbleCount];
        if (!marbleHotspots(e, spots))
            return;

        for (int i = 0; i < kMarbleCount; ++i)
        {
            const std::uint32_t pos = e.vars().get(kMarbleVars[i]);
            const std::size_t index = e.hotspotIndexOf(spots[i]);
            if (marbleOnBoard(pos))
            {
                e.setHotspotRect(index, marbleGridRect(marbleX(pos), marbleY(pos)));
            }
            else
            {
                // The card's own rect, which is the receptacle. Reading it back
                // out of the data rather than remembering it is the whole
                // reason resetCardState refills hotspotRect_.
                //
                // Also where a nonsense position lands: a marble whose variable
                // says it is off the board is a marble on its stand, which is
                // recoverable, and the player can put it back.
                e.setHotspotRect(index, e.card()->hotspots[index].rect);
            }
        }
    }

    /// TSpit::drawMarbles (tspit.cpp:324-342). All six at once, out of the one
    /// strip the converter writes -- which is what drawExtrasSections is for.
    void drawMarbles(Engine &e)
    {
        const Hotspot *spots[kMarbleCount];
        if (!marbleHotspots(e, spots))
            return;

        const std::uint32_t held = e.vars().get(VarId::TheMarble);

        CardSurface::Section sections[kMarbleCount];
        std::size_t count = 0;
        for (int i = 0; i < kMarbleCount; ++i)
        {
            // The one in the player's hand is not on the board to be drawn.
            if (held != 0 && static_cast<int>(held) - 1 == i)
                continue;

            Rect r = e.hotspotRect(spots[i]);
            // The hotspot is 13 wide and the marble is 8; the trim is the
            // original's and it is not centred (tspit.cpp:334-338).
            r.left += 3;
            r.top += 3;
            r.right -= 2;
            r.bottom -= 2;

            CardSurface::Section &s = sections[count++];
            s.dst = r;
            s.src = Rect{static_cast<std::int16_t>(i * kLargeMarbleSize), 0,
                         static_cast<std::int16_t>((i + 1) * kLargeMarbleSize),
                         kLargeMarbleSize};
        }

        e.beginScreenUpdate();
        e.drawExtrasSections("marbles", sections, count);
        e.applyScreenUpdate();
    }

    /// TSpit::xt7800_setup (tspit.cpp:311-322). Run from the closeup card's load
    /// script: put the hotspots where the variables say and let go of anything
    /// the player was holding.
    void xt7800_setup(Engine &e)
    {
        setMarbleHotspots(e);
        e.vars().set(VarId::TheMarble, 0);
    }

    /// TSpit::xt7500_checkmarbles (tspit.cpp:229-249). The answer, and the only
    /// thing on this island that turns the power on.
    void xt7500_checkmarbles(Engine &e)
    {
        // The six packed positions that spell the correct pattern
        // (tspit.cpp:233). Left as the original's decimal literals rather than
        // decomposed into coordinates: they are a constant of the game, and
        // rewriting them as (x, y) pairs would invite someone to "fix" one.
        static const std::uint32_t kSolution[kMarbleCount] = {1114121, 1441798, 0,
                                                              65552,   65558,   262146};

        bool valid = true;
        for (int i = 0; i < kMarbleCount && valid; ++i)
            valid = e.vars().get(kMarbleVars[i]) == kSolution[i];

        if (!valid)
        {
            e.vars().set(VarId::APower, 0);
            return;
        }

        e.vars().set(VarId::APower, 1);
        // Solved, and the marbles go back in their receptacles: the grid has
        // done its job and the player should not be able to un-solve it by
        // knocking one off.
        for (int i = 0; i < kMarbleCount; ++i)
            e.vars().set(kMarbleVars[i], 0);
    }

    /// TSpit::xt7600_setupmarbles (tspit.cpp:251-296). The SMALL marbles, seen
    /// from across the room on the viewer card -- 4x2 each, and pre-scaled
    /// inside tspit rather than taken from extras.
    void xt7600_setupmarbles(Engine &e)
    {
        // Where a grid row sits on the screen, and how far apart its columns are
        // (tspit.cpp:255-267). The board is seen at an angle, so both the origin
        // and the spacing change with the row.
        static const std::uint16_t kRowX[] = {246, 245, 244, 243, 243, 241, 240, 240, 239,
                                              238, 237, 237, 236, 235, 234, 233, 232, 231,
                                              230, 229, 228, 227, 226, 226, 225};
        static const std::uint16_t kRowY[] = {261, 263, 265, 267, 268, 270, 272, 274, 276,
                                              278, 281, 284, 285, 288, 290, 293, 295, 298,
                                              300, 303, 306, 309, 311, 314, 316};
        // The original's column spacing is fractional -- 4.56 pixels and up.
        // HUNDREDTHS, as integers, rather than the doubles ScummVM uses: every
        // one of these values is an exact multiple of 0.04, so a hundredth is
        // lossless, and the ARM9 has no hardware for the alternative.
        static const std::uint16_t kColStep100[] = {456, 468, 476, 484, 484, 496, 504,
                                                    504, 512, 520, 528, 528, 536, 544,
                                                    540, 560, 572, 580, 588, 596, 604,
                                                    612, 620, 620, 628};

        // Waffle up is 0, down is 1. Down hides the board, so a marble on it is
        // not drawn -- but one still in its receptacle is (tspit.cpp:280-285).
        const bool waffleDown = e.vars().get(VarId::TWaffle) != 0;

        const std::int32_t base = e.bitmapIdForName("tsmallred");
        if (base < 0)
        {
            DebugLog::warn("marbles: card %u has no \"tsmallred\"",
                           static_cast<unsigned>(e.cardId()));
            return;
        }

        for (int i = 0; i < kMarbleCount; ++i)
        {
            const std::uint32_t pos = e.vars().get(kMarbleVars[i]);
            int x = 0;
            int y = 0;

            if (!marbleOnBoard(pos))
            {
                // On its stand -- drawn even when the waffle is down
                // (tspit.cpp:280-285).
                static const int kRestX[] = {375, 377, 379, 381, 383, 385};
                static const int kRestY[] = {253, 257, 261, 265, 268, 273};
                x = kRestX[i];
                y = kRestY[i];
            }
            else if (waffleDown)
            {
                continue;
            }
            else
            {
                const int mx = marbleX(pos);
                const int my = marbleY(pos);
                // floor(mx * step + rowX + 0.5) in hundredths, which is the
                // same rounding without a float: +50 is the +0.5.
                x = (mx * static_cast<int>(kColStep100[my]) + kRowX[my] * 100 + 50) / 100;
                y = kRowY[my];
            }

            e.drawBitmap(static_cast<std::uint16_t>(base + i),
                         Rect{static_cast<std::int16_t>(x), static_cast<std::int16_t>(y),
                              static_cast<std::int16_t>(x + kSmallMarbleWidth),
                              static_cast<std::int16_t>(y + kSmallMarbleHeight)});
        }
    }

    /// TSpit::xtakeit (tspit.cpp:349-412). Pick a marble up, carry it while the
    /// stylus is down, and put it wherever it was let go.
    void xtakeit(Engine &e)
    {
        const Hotspot *spots[kMarbleCount];
        if (!marbleHotspots(e, spots))
            return;

        // Which one is under the pointer. ScummVM notes that this can find
        // nothing if a move event arrived between the script being queued and
        // run (tspit.cpp:364-369), and the port's dispatch has the same gap.
        int held = 0;
        for (int i = 0; i < kMarbleCount; ++i)
        {
            const Rect &r = e.hotspotRect(spots[i]);
            if (e.pointerCardX() >= r.left && e.pointerCardX() < r.right
                && e.pointerCardY() >= r.top && e.pointerCardY() < r.bottom)
            {
                held = i + 1;
                break;
            }
        }
        if (held == 0)
            return;

        e.vars().set(VarId::TheMarble, static_cast<std::uint32_t>(held));

        // PLST 1 is the bare grid. ScummVM stops there and leaves the board
        // EMPTY for the length of the drag (tspit.cpp:372), which is its
        // stand-in for the original's marble-follows-the-cursor: it has no
        // cheap way to redraw. The port puts the other five back, because the
        // whole skill of this puzzle is placing a marble relative to the ones
        // already down, and a board that goes blank the moment you pick one up
        // takes that away. The held one is skipped -- themarble is set.
        e.activatePlst(1);
        drawMarbles(e);

        e.setCursor(kCursorClosedHand);
        while (e.mouseIsDown() && !e.quitRequested())
            e.pumpInteractiveFrame();
        e.setCursor(kCursorMain);

        // Where did it land? The grid's rects are generated rather than stored,
        // because only six of the 625 slots are ever hotspots.
        std::uint32_t pos = e.vars().get(kMarbleVars[held - 1]);
        bool placed = false;
        for (int y = 0; y < 25 && !placed; ++y)
        {
            for (int x = 0; x < 25 && !placed; ++x)
            {
                const Rect r = marbleGridRect(x, y);
                if (e.pointerCardX() < r.left || e.pointerCardX() >= r.right
                    || e.pointerCardY() < r.top || e.pointerCardY() >= r.bottom)
                    continue;

                pos = withMarbleY(withMarbleX(pos, x), y);
                // Occupied? Then the marble goes home rather than stacking.
                for (int i = 0; i < kMarbleCount; ++i)
                    if (i != held - 1 && e.vars().get(kMarbleVars[i]) == pos)
                        pos = 0;
                placed = true;
            }
        }
        // Dropped off the board entirely.
        if (!placed)
            pos = 0;

        e.vars().set(kMarbleVars[held - 1], pos);
        e.vars().set(VarId::TheMarble, 0);
        setMarbleHotspots(e);
        drawMarbles(e);
    }

    // --- the dome ------------------------------------------------------------

    /// tspit's dome, on card 5056. The slots are BLST 24..48 and the artwork is
    /// named for card 190, where it was first used (TSpit's constructor,
    /// tspit.cpp:32-33).
    constexpr DomeConfig kTspitDome{24, "tsliders.190", "tsliderbg.190"};

} // namespace

bool runTspitExternal(Engine &e, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount)
{
    // --- the telescope -------------------------------------------------------
    if (key == "xtisland390_covercombo")
    {
        xtisland390_covercombo(e, args, argCount);
        return true;
    }
    if (key == "xtexterior300_telescopeup")
    {
        xtexterior300_telescopeup(e);
        return true;
    }
    if (key == "xtexterior300_telescopedown")
    {
        xtexterior300_telescopedown(e);
        return true;
    }

    // --- the marble grid -----------------------------------------------------
    if (key == "xt7800_setup")
    {
        xt7800_setup(e);
        return true;
    }
    if (key == "xt7500_checkmarbles")
    {
        xt7500_checkmarbles(e);
        return true;
    }
    if (key == "xt7600_setupmarbles")
    {
        xt7600_setupmarbles(e);
        return true;
    }
    if (key == "xdrawmarbles")
    {
        drawMarbles(e);
        return true;
    }
    if (key == "xtakeit")
    {
        xtakeit(e);
        return true;
    }

    // --- the dome ------------------------------------------------------------
    if (key == "xtisland5056_resetsliders")
    {
        resetDomeSliders(e, kTspitDome);
        return true;
    }
    if (key == "xtisland5056_slidermd")
    {
        dragDomeSlider(e, kTspitDome);
        return true;
    }
    if (key == "xtisland5056_slidermw")
    {
        checkSliderCursorChange(e, kTspitDome);
        return true;
    }
    if (key == "xtisland5056_opencard")
    {
        checkDomeSliders(e);
        return true;
    }
    if (key == "xtscpbtn")
    {
        runDomeButtonMovie(e);
        return true;
    }
    if (key == "xtisland4990_domecheck")
    {
        runDomeCheck(e);
        return true;
    }

    // --- empty, and empty in ScummVM too -------------------------------------
    // tspit.cpp has these as a comment and nothing else. The inventory the first
    // two describe is granted by the cards' own variables; the commands exist so
    // the original's script had somewhere to call. Listed rather than left to
    // the fallback so the intro does not print two "not implemented" lines every
    // time it plays.
    if (key == "xtatrusgivesbooks")
        return true; // "Give the player Atrus' Journal and the Trap book"
    if (key == "xtchotakesbook")
        return true; // "And now Cho takes the trap book"
    if (key == "xthideinventory")
    {
        // EMPTY, and matching ScummVM's, which is also empty (tspit.cpp:190).
        //
        // The name invites forceHidden(true) and this port had it, with no
        // counterpart anywhere -- so from the opening cutscene onwards the strip
        // was hidden for the rest of the game and Atrus's journal, which nothing
        // else reaches, was unreachable. Nothing needs it: the strip is already
        // hidden during the cutscene by fullscreenMoviePlaying() and on aspit by
        // the stack test (Inventory::update), which is why ScummVM can afford to
        // leave it empty too.
        return true;
    }
    if (key == "xtatboundary")
        return true; // the demo's "buy the game" dialogue (tspit.cpp:438-440)

    return false;
}

} // namespace rivenrt
