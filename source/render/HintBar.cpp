#include "HintBar.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "Settings.hpp"
#include "tonccpy.h"

namespace rivenrt
{

HintBar hintBar;

namespace
{
    /// The 8bpp (palette256) build of the font, and the metadata that goes with
    /// it. Both already ship: build.py runs ptexconv and copies the .fnt out of
    /// resources/font/, and nothing else in the port was using this pair.
    ///
    /// The palette file beside them is deliberately NOT read. ptexconv wrote
    /// eight entries and every one of them is black -- the source PNG carries
    /// the glyph in its alpha channel -- so the sheet is effectively one bit,
    /// and inkAt only asks whether a texel is set.
    const char *const kFontTexture = "font/DejaVuSans-BoldBlack_0_png_tex.bin";
    const char *const kFontMetadata = "font/DejaVuSans-BoldBlack.fnt";
    constexpr int kAtlasW = 128;
    constexpr int kAtlasH = 64;
    constexpr std::size_t kAtlasBytes = static_cast<std::size_t>(kAtlasW) * kAtlasH;

    // --- the band ---------------------------------------------------------
    //
    // Six tile rows at the bottom of the screen, y 128..175. Below that are the
    // two console rows this port already spends: row 22 is the profile readout
    // (RIVEN_PROFILE) and row 23 is DebugLog::status, which has to stay legible
    // because it is the answer to a button the player just pressed.
    constexpr int kBandW = 256;
    constexpr int kBandTop = 128;
    constexpr int kBandTilesX = kBandW / 8;
    constexpr int kBandTilesY = 6;
    constexpr int kBandH = kBandTilesY * 8;
    constexpr std::size_t kStageBytes = static_cast<std::size_t>(kBandW) * kBandH;

    /// Where each row's top pixel sits inside the band. Three lines of 14 with a
    /// two-pixel lead-in, ending at 44 of the 48 -- the shadow needs the extra
    /// row under the last line.
    constexpr int kRowY[HintBar::kRows] = {2, 16, 30};
    /// A left margin, so the text is not against the bezel.
    constexpr int kIndent = 8;

    // --- VRAM -------------------------------------------------------------
    //
    // Both bases are picked to fit around what is already in bank C; see the
    // table in the header. Nothing already there moves.
    constexpr int kTileBase = 0;
    constexpr int kMapBase = 30;
    /// The console's font is 3 KB at the same tile base, which is 96 tiles of
    /// 4bpp. Ours start immediately after it.
    constexpr int kTileFirst = 96;
    constexpr std::size_t kTileSize = 32;
    /// One blank tile for everything outside the band, then the band itself.
    /// 193 tiles = 6176 bytes, landing at 3 K..9 K.
    constexpr int kTileCount = 1 + kBandTilesX * kBandTilesY;
    constexpr std::size_t kTileBytes = kTileCount * kTileSize;

    /// Under the console (priority 0) and over the splash (priority 3).
    constexpr unsigned kPriority = 1;

    // --- palette ----------------------------------------------------------
    //
    // 4bpp bank 15, so colour N is BG_PALETTE_SUB[240 + N]. See the header for
    // why these two and no others.
    constexpr int kPaletteBank = 15;
    constexpr std::uint8_t kInk = 15;    ///< entry 255: the console's own white
    constexpr std::uint8_t kShadow = 14; ///< entry 254: black, written by create()
    constexpr int kShadowEntry = kPaletteBank * 16 + kShadow;

    std::uint8_t *loadAtlas()
    {
        std::FILE *f = std::fopen(kFontTexture, "rb");
        if (f == nullptr)
            return nullptr;
        auto *buf = static_cast<std::uint8_t *>(std::malloc(kAtlasBytes));
        if (buf == nullptr)
        {
            std::fclose(f);
            return nullptr;
        }
        const std::size_t got = std::fread(buf, 1, kAtlasBytes, f);
        std::fclose(f);
        if (got != kAtlasBytes)
        {
            std::free(buf);
            return nullptr;
        }
        return buf;
    }
} // namespace

bool HintBar::create()
{
    if (ready())
        return true;

    // The trace owns the whole top screen and replaces the picture with a wall
    // of small text; a legend on top of that is noise over noise. TopBg::load
    // bows out of debug mode for the same reason and in the same words.
    if (DebugLog::enabled())
        return false;

    if (!global.hasNitroFS)
    {
        DebugLog::notice("no control hints: no NitroFS");
        return false;
    }

    atlas_ = loadAtlas();
    if (atlas_ == nullptr)
    {
        DebugLog::notice("no control hints: %s is missing", kFontTexture);
        return false;
    }

    staging_ = static_cast<std::uint8_t *>(std::malloc(kStageBytes));
    tiles_ = static_cast<std::uint8_t *>(std::malloc(kTileBytes));
    if (staging_ == nullptr || tiles_ == nullptr)
    {
        destroy();
        DebugLog::notice("no control hints: out of memory");
        return false;
    }

    // The destination is RAM and not VRAM, and that is not a preference: libdsf
    // writes an 8bpp glyph one byte at a time and VRAM discards byte-wide
    // stores, so rasterising straight into the layer would write nothing at all
    // on hardware. TextLayer gets away with a VRAM target only because its
    // buffer is 16bpp, which is a halfword store (TextLayer.hpp).
    ctx_ = NEA_Hw2DTextCtxCreateBuffer(NEA_HW2D_BG_BITMAP_8, staging_, kBandW, kBandH);
    if (ctx_ == nullptr)
    {
        destroy();
        DebugLog::notice("no control hints: could not make a text context");
        return false;
    }

    // Null palette: NEA would otherwise memcpy the font's palette over
    // BG_PALETTE_SUB[0..], which is the splash's half and would wipe it. The
    // Myst port hit exactly this (TopScreenText.cpp:95-96).
    if (NEA_Hw2DTextCtxMetadataLoadFAT(ctx_, kFontMetadata) != 0
        || NEA_Hw2DTextCtxBitmapSet(ctx_, atlas_, kAtlasW, kAtlasH, nullptr, 0) != 0)
    {
        destroy();
        DebugLog::notice("no control hints: %s would not load", kFontMetadata);
        return false;
    }

    // A row is a row: a legend that does not fit should lose its end rather than
    // wrap onto the row below, which is already spoken for.
    NEA_Hw2DTextCtxSetWordWrap(ctx_, false, nullptr);
    NEA_Hw2DTextCtxSetOverflow(ctx_, NEA_HW2D_TEXT_RIGHT_TRIM, NEA_HW2D_TEXT_BOTTOM_TRIM);

    // Hidden until there is something in the tiles: bank C holds whatever
    // Global::Init's fill left, and showing the layer first would be a frame of
    // whatever that is. TopBg opens with the same move.
    bgId_ = bgInitHiddenSub(1, BgType_Text4bpp, BgSize_T_256x256, kMapBase, kTileBase);
    if (bgId_ < 0)
    {
        destroy();
        DebugLog::notice("no control hints: no free sub background");
        return false;
    }
    bgSetPriority(bgId_, kPriority);
    writeMap();

    // The ink is entry 255, which consoleInit already set to white and which the
    // splash is quantised to stay out of -- so only the shadow is ours to write.
    BG_PALETTE_SUB[kShadowEntry] = RGB15(0, 0, 0);

    // Read BEFORE the first paint, not left to settings.apply(): apply() does
    // not run until the audio system is up, and a player who turned this off
    // would see it flash on for those frames every boot.
    enabled_ = settings.controlHints;

    // The card legend is the RESTING state, set here rather than by whoever
    // starts the game: every screen that borrows the band does it through a
    // HintScope, so this is the thing they all unwind back to. The boot menu
    // runs next and has a scope of its own, so it is not what the player sees
    // first.
    setPlayHint();
    return true;
}

void HintBar::destroy()
{
    if (bgId_ >= 0)
    {
        // libnds has no per-layer teardown, so hiding it is the whole of it --
        // but bgId_ has to go with it, or a second create() would leave the
        // first layer displayed and drive a different one.
        bgHide(bgId_);
        bgUpdate();
        bgId_ = -1;
    }
    if (ctx_ != nullptr)
    {
        NEA_Hw2DTextCtxDelete(ctx_);
        ctx_ = nullptr;
    }
    // Ours, not NEA's: BitmapSet does not take ownership of the sheet.
    std::free(atlas_);
    std::free(staging_);
    std::free(tiles_);
    atlas_ = nullptr;
    staging_ = nullptr;
    tiles_ = nullptr;
}

void HintBar::writeMap()
{
    // Written once and never again: a repaint rewrites tile PIXELS, so the map
    // stays as it is laid out here for the life of the layer.
    std::uint16_t *const map = bgGetMapPtr(bgId_);
    for (int row = 0; row < 32; ++row)
    {
        const int bandRow = row - kBandTop / 8;
        for (int col = 0; col < 32; ++col)
        {
            // Tile kTileFirst is the blank one; the band follows it in reading
            // order. Everything off the band points at the blank rather than at
            // tile 0, which belongs to the console's font and is not ours to
            // assume anything about.
            const int tile = (bandRow >= 0 && bandRow < kBandTilesY)
                                 ? kTileFirst + 1 + bandRow * kBandTilesX + col
                                 : kTileFirst;
            // The palette bank is in the map entry, not the tile: without
            // TILE_PALETTE the layer would read bank 0, which is the splash's
            // first sixteen colours rather than the sixteen reserved here.
            map[row * 32 + col] =
                static_cast<std::uint16_t>(tile | TILE_PALETTE(kPaletteBank));
        }
    }
}

std::uint8_t HintBar::inkAt(int x, int y) const
{
    if (staging_[y * kBandW + x] != 0)
        return kInk;
    // The shadow is the glyph offset by one down and right, drawn only where the
    // glyph itself is not -- so it never eats into a letter, and white on a
    // photograph keeps an edge.
    if (x > 0 && y > 0 && staging_[(y - 1) * kBandW + (x - 1)] != 0)
        return kShadow;
    return 0;
}

void HintBar::repaint()
{
    if (!ready())
        return;

    std::memset(staging_, 0, kStageBytes);
    for (int r = 0; r < kRows; ++r)
    {
        if (rows_[r].empty())
            continue;
        const int y = kRowY[r];
        // Set per row rather than once: the canvas is what clips, and a row that
        // runs long has to be cut at the right edge instead of spilling.
        NEA_Hw2DTextCtxSetCanvas(ctx_, kIndent, y, kBandW, y + kLineHeight);
        NEA_Hw2DTextCtxCursorSet(ctx_, kIndent, y);
        // Print returns +1 when the canvas filled up, which with RIGHT_TRIM is
        // the only sign that a row lost its end. Said out loud so the width
        // budget in the header is enforced by something other than eyesight.
        if (NEA_Hw2DTextCtxPrint(ctx_, rows_[r].c_str()) > 0)
            DebugLog::log("HINT row clipped: %s", rows_[r].c_str());
    }

    // 8bpp band -> 4bpp tiles. The blank tile at the front stays zero.
    std::memset(tiles_, 0, kTileBytes);
    std::uint8_t *const band = tiles_ + kTileSize;
    for (int ty = 0; ty < kBandTilesY; ++ty)
    {
        for (int tx = 0; tx < kBandTilesX; ++tx)
        {
            std::uint8_t *const dst = band + (ty * kBandTilesX + tx) * kTileSize;
            for (int py = 0; py < 8; ++py)
            {
                const int y = ty * 8 + py;
                for (int px = 0; px < 8; px += 2)
                {
                    const int x = tx * 8 + px;
                    // Low nibble is the left pixel of the pair.
                    dst[py * 4 + px / 2] = static_cast<std::uint8_t>(
                        inkAt(x, y) | (inkAt(x + 1, y) << 4));
                }
            }
        }
    }

    tonccpy(reinterpret_cast<std::uint8_t *>(bgGetGfxPtr(bgId_)) + kTileFirst * kTileSize,
            tiles_, kTileBytes);

    // Only now is there anything to look at. Idempotent, so this doubles as the
    // "the setting is on" path after setEnabled(false) hid the layer.
    if (enabled_)
    {
        bgShow(bgId_);
        bgUpdate();
    }
}

void HintBar::set(const char *r0, const char *r1, const char *r2)
{
    const char *const in[kRows] = {r0, r1, r2};
    bool changed = false;
    for (int r = 0; r < kRows; ++r)
    {
        const char *const text = in[r] != nullptr ? in[r] : "";
        if (rows_[r] != text)
        {
            rows_[r] = text;
            changed = true;
        }
    }
    // Stored either way, so a legend set while the setting is off is the one
    // that appears when it is switched back on.
    if (changed)
        repaint();
}

void HintBar::setEnabled(bool on)
{
    enabled_ = on;
    if (!ready())
        return;
    if (on)
        bgShow(bgId_);
    else
        bgHide(bgId_);
    bgUpdate();
}

HintScope::HintScope(const char *r0, const char *r1, const char *r2)
{
    for (int r = 0; r < HintBar::kRows; ++r)
        saved_[r] = hintBar.currentRow(r);
    hintBar.set(r0, r1, r2);
}

HintScope::~HintScope()
{
    hintBar.set(saved_[0].c_str(), saved_[1].c_str(), saved_[2].c_str());
}

} // namespace rivenrt
