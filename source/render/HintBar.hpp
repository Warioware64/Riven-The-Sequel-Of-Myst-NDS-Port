#pragma once

// The control legend on the top screen: what the buttons do, in the port's own
// font, changing with what the player is looking at.
//
// WHY IT EXISTS. Every control in this port is the port's invention -- Riven
// itself was a mouse game -- and until now the only place any of them was ever
// stated was a status line that expires after two seconds
// (Engine::toggleZoom's "zoom: touch acts, pad pans"). A player who puts the
// game down for a week has no way to rediscover that Y opens the notebook.
//
// Modelled on the Myst port's TopScreenText, which does the same job under its
// logo, down to the font, the row height and the width lesson recorded below.
//
// THE ONE THING THAT COULD NOT BE COPIED IS THE SURFACE. Myst gave its legend a
// 256x256 8bpp bitmap background -- 64 KB of its free bank C. This port's bank C
// has no 64 KB left: the sub engine gets exactly one bank (TopBg.hpp), and the
// only 64 KB-aligned window in it is the one the splash picture occupies. The
// authoritative map is the table in engine/DebugConsole.cpp:
//
//     0 - 3 K       console font (tile base 0)       Global::Init
//     3 K - 16 K    free            <-- 6 KB of this is the glyph tiles below
//     16 K - 43 K   keyboard tiles (debug only)
//     43 K - 56 K   free
//     56 K - 60 K   keyboard map (debug only)
//     60 K - 62 K   free            <-- exactly one screen block: the map below
//     62 K - 64 K   console map (base 31)            Global::Init
//     64 K - 128 K  TopBg picture (bitmap base 4)    TopBg::load
//
// So this is a TILED 4bpp background on BG1 instead of a bitmap. Glyphs are
// still rasterised in RAM exactly as Myst does them -- libdsf writes 8bpp text
// a byte at a time and VRAM discards byte stores -- and then packed to 4bpp and
// swizzled into tiles. It relocates nothing: the tiles go after the console
// font at the SAME tile base, and the map takes the one free screen block.
//
// BG1 is otherwise the debug keyboard's, and this whole layer is switched off in
// debug mode (the trace owns the top screen), so the two never coexist.
//
// PALETTE. 4bpp colour N reads BG_PALETTE_SUB[bank * 16 + N], and this layer
// uses bank 15 -- entries 240..255, the only range the splash does not claim
// (TopBg.hpp explains the 240 split). Colour 15 is entry 255, which is ALREADY
// the white the console prints in, so the ink costs no palette entry and is
// never written. Colour 14 is entry 254, set to black for a one-pixel drop
// shadow, which is what keeps white text legible over a photograph. Entry 254
// is free: consoleInit only fills bank*16+15, so 255 and nothing else.
//
// WIDTH. Rows are right-trimmed silently, so a row that runs long simply loses
// its end. The Myst port lost the word "save" that way and recorded the budget
// it found: keep every row well under ~230 px, which is about 23 characters of
// this font. That is why the legend is three short rows and not one long one.

#include <string>

#include "global_header.hpp"

namespace rivenrt
{

class HintBar
{
public:
    /// Rows of text in the band, top to bottom.
    static constexpr int kRows = 3;

    /// Bring up the layer and load the font. False -- and every other entry
    /// point a no-op -- in debug mode, or without NitroFS, or with the font
    /// missing, exactly the way TextLayer::create and TopBg::load degrade.
    ///
    /// After Global::Init (which owns the console's half of tile base 0) and
    /// after DebugLog::begin (which decides whether the trace has the screen).
    bool create();
    void destroy();
    bool ready() const { return ctx_ != nullptr; }

    /// The player's setting. Hides the layer without forgetting what is on it,
    /// so switching it back on restores the hint for wherever they are.
    void setEnabled(bool on);

    /// Replace the legend. Null or empty rows are blank. Repaints only when
    /// something actually changed, so calling this per frame would be free --
    /// nothing does, because nothing here expires.
    void set(const char *r0, const char *r1 = nullptr, const char *r2 = nullptr);

    /// The standing legend for card mode. Every screen that borrows the band
    /// comes back to this through HintScope.
    void setPlayHint() { set(kPlayRow0, kPlayRow1, kPlayRow2); }

    /// What the buttons do on a card. These are the REAL bindings, which are not
    /// quite the obvious ones: Y opens the notebook and L takes the note
    /// (NoteView.cpp), X is the zoom viewer, and B only means anything while a
    /// movie is playing -- so it is not here, it is on the cutscene legend.
    ///
    /// TWO rows where the modal screens below use three, and deliberately: this
    /// is the legend that is up nearly all the time, so it should cover as
    /// little of the splash as it can. They measure 187 px and 212 px against
    /// the ~230 budget above, which is as much as fits in two.
    static constexpr const char *kPlayRow0 = "DPAD move   A click   X zoom";
    static constexpr const char *kPlayRow1 = "Y notebook   L note   START menu";
    static constexpr const char *kPlayRow2 = nullptr;

    /// The port's own list screens -- the boot menu, the in-game menu, the
    /// settings, the save and load pickers. They all move and choose alike, so
    /// they share one legend rather than five copies of it.
    static constexpr const char *kMenuRow0 = "DPAD move   A choose";
    static constexpr const char *kMenuRow1 = "touch works too";
    static constexpr const char *kMenuRow2 = "B back";

    /// One line of the font, from DejaVuSans-BoldBlack.fnt's common block.
    static constexpr int kLineHeight = 14;

    /// What is on row `r` now. For HintScope, which has to put it back.
    const std::string &currentRow(int r) const { return rows_[r]; }

private:
    void repaint();
    /// The 4bpp colour for one band pixel: ink, its shadow, or transparent.
    std::uint8_t inkAt(int x, int y) const;
    void writeMap();

    NEA_Hw2DTextCtx *ctx_ = nullptr;
    std::uint8_t *atlas_ = nullptr;   ///< the 8bpp glyph sheet, as ptexconv wrote it
    std::uint8_t *staging_ = nullptr; ///< the band rasterised 8bpp, IN RAM (see above)
    std::uint8_t *tiles_ = nullptr;   ///< the same band packed to 4bpp tiles
    int bgId_ = -1;
    bool enabled_ = true;
    std::string rows_[kRows];
};

extern HintBar hintBar;

/// Lend the band to a screen that has its own controls, and give it back.
///
/// Scope-bound for the reason ScreenTakeover and Engine::CursorHide are: the
/// screens that borrow it -- the notebook, the menus, the credits -- all have
/// early returns, and a hand-written set/restore pair leaks the wrong legend on
/// every one of them.
struct HintScope
{
    explicit HintScope(const char *r0, const char *r1 = nullptr,
                       const char *r2 = nullptr);
    ~HintScope();
    HintScope(const HintScope &) = delete;
    HintScope &operator=(const HintScope &) = delete;

private:
    std::string saved_[HintBar::kRows];
};

} // namespace rivenrt
