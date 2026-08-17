#include "render/Credits.hpp"

#include <cstdio>
#include <string>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "ScreenTakeover.hpp"
#include "data/ImageFile.hpp"
#include "engine/Engine.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "render/HintBar.hpp"
#include "tonccpy.h"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// The ids the converter writes, and the split between the two images that
    /// are held and the seventeen that scroll. ScummVM's
    /// kRivenCreditsZeroImage..kRivenCreditsLastImage (riven_graphics.h:54-58),
    /// spelled again here rather than shared with the converter's Credits.hpp:
    /// that header is host-side and pulls in <filesystem>.
    constexpr int kFirstId = 302;
    constexpr int kLastId = 320;
    constexpr int kFirstScrollId = 304;

    /// How long each of the two title cards stays up (riven_graphics.cpp:696-697
    /// picks 4000 for them and 1000/60 for everything after).
    constexpr std::uint32_t kStillHoldMs = 4000;

    /// The scroll's speed, as a fraction of a destination row per frame.
    ///
    /// ScummVM advances one row of a 392-row image per frame. Ours are 192 rows
    /// of the same picture, so a row here is worth 392/192 of a row there, and
    /// advancing one per frame would run the whole roll at double speed. The
    /// numerator is the converted height and the denominator Riven's own, which
    /// is exactly the scale the converter applied -- so this stays right if the
    /// geometry ever changes, and it is read off the file rather than assumed.
    constexpr int kSourceRows = 392;

    std::string creditsPath(int id)
    {
        return global.extrasDir() + "credits/" + std::to_string(id) + ".rpic";
    }

    /// Opaque black. The credits are drawn on nothing and the buffer's own rows
    /// below kViewH are TRANSPARENT from create(), so every row this writes has
    /// to be opaque across all 256 texels or the layer underneath shows through
    /// in the margins.
    constexpr Texel kBlack = static_cast<Texel>(0x8000);

    void fillBlack(Texel *row, int count)
    {
        // toncset16 rather than a loop: this runs once per scrolled row.
        for (int i = 0; i < count; ++i)
            row[i] = kBlack;
    }

    /// The roll's source: one image in RAM, and where in it the next row is.
    ///
    /// ONE IMAGE AT A TIME, and the seam is a visible cost worth naming: the
    /// next 67 KB file is read when the current one runs out, which stalls the
    /// scroll for something like a tenth of a second, seventeen times over two
    /// minutes. Holding two would only move the stall rather than remove it --
    /// loadRpicImage is a whole-file read and there is no asynchronous one to
    /// spread it over frames -- and the seams fall between images, which is
    /// where Riven's own credits leave a gap in the text anyway.
    struct Roll
    {
        RpicImage image;
        int id = kFirstScrollId;
        int pos = 0;      ///< next row of `image`
        bool live = true; ///< there are still images to come

        /// Read the next image in. False when there are none left.
        bool advance()
        {
            while (id <= kLastId)
            {
                std::string error;
                if (loadRpicImage(creditsPath(id), image, error) && image.valid())
                {
                    ++id;
                    pos = 0;
                    return true;
                }
                // A hole rather than a stop: sixteen images of credits are
                // better than none, and the converter already warned about
                // whichever one this is.
                DebugLog::warn("credits: %s", error.c_str());
                ++id;
            }
            live = false;
            return false;
        }

        /// Compose the next row into `out`, 256 texels wide. Black once the
        /// images have run out, which is what scrolls the last of them off.
        void nextRow(Texel *out)
        {
            fillBlack(out, kViewW);
            if (live && pos >= image.height && !advance())
                return;
            if (!live)
                return;

            const int w = image.width < kViewW ? image.width : kViewW;
            const int x0 = (kViewW - w) / 2;
            tonccpy(out + x0, image.texels.data() + static_cast<std::size_t>(pos) * image.width,
                    static_cast<std::size_t>(w) * sizeof(Texel));
            ++pos;
        }
    };

    /// Draw one whole image, centred, into `buf` -- and black everywhere else,
    /// on all 256 rows rather than the 192 that show. The rows past the screen
    /// are the ring's tail: the scroll walks into them a frame later, and
    /// anything left there from a card view would scroll into view as garbage.
    void drawStill(int buf, const RpicImage &image)
    {
        Texel *const dst = BgSurface::pixels(buf);
        fillBlack(dst, BgSurface::kBufW * BgSurface::kBufH);

        const int w = image.width < kViewW ? image.width : kViewW;
        const int h = image.height < BgSurface::kBufH ? image.height : BgSurface::kBufH;
        const int x0 = (kViewW - w) / 2;
        const int y0 = (kScreenH - h) / 2 > 0 ? (kScreenH - h) / 2 : 0;
        for (int y = 0; y < h; ++y)
            tonccpy(dst + static_cast<std::size_t>(y0 + y) * BgSurface::kBufW + x0,
                    image.texels.data() + static_cast<std::size_t>(y) * image.width,
                    static_cast<std::size_t>(w) * sizeof(Texel));
    }

    /// A button -- any button -- ends the roll. See the header.
    ///
    /// scanKeys() is safe to call from here: idleFrame() does not call it, and
    /// DebugLog::pollHotkeys deliberately reads REG_KEYINPUT itself rather than
    /// borrowing libnds's bookkeeping (DebugLog.cpp:35-37), so nothing else is
    /// consuming presses while this loop runs.
    bool skipped()
    {
        scanKeys();
        return keysDown() != 0;
    }
} // namespace

void runCredits(Engine &e, std::uint32_t delayMs)
{
    if (!bgs.exists())
        return;

    // Nothing to roll. An install converted before this stage existed, or one
    // whose extras.mhk was missing, still has to be able to finish the game --
    // so this is a silent return to the menu rather than a failure.
    RpicImage first;
    std::string error;
    if (!loadRpicImage(creditsPath(kFirstId), first, error) || !first.valid())
    {
        // notice(), not warn(): warn is held back with debug mode off, and this
        // is the one asset whose absence the player meets at the END of a
        // playthrough with no other explanation for it. Global::ReportOptionalData
        // says the same thing at boot, which is where it is actually useful.
        DebugLog::notice("no credits art -- convert again");
        DebugLog::warn("credits: %s", error.c_str());
        return;
    }

    ScreenTakeover screen;
    Engine::CursorHide hide{e};
    // ANY button ends the roll, so naming one would be a lie about the other
    // nine. The band goes blank instead -- this is the last thing in the game
    // and it should have the screen to itself.
    HintScope hint{nullptr};

    // ANY button ends the roll (skipped(), above), and this loop spins
    // Engine::pumpIdleFrame -- which is where both halves of L are polled. So
    // without this, pressing L to skip the credits would also silently write a
    // note of a credits page on the way out, and in debug mode a shotNNN.bmp
    // too. Nothing is lost by holding them off: a screen you cannot press a
    // button on without leaving is a screen you cannot photograph anyway.
    DebugLog::HotkeyHold holdHotkeys;

    // BLACK FIRST, and it does two jobs.
    //
    // ScummVM's is one of them: beginCredits() clears the screen before the
    // first title card, so 302 fades up out of nothing rather than out of
    // whatever the ending left behind (riven_graphics.cpp:683-684).
    //
    // The other is this port's. ScreenTakeover parks the card view in the front
    // buffer, and until something has flipped, the buffer the blend below would
    // fade INTO is that parked card -- and the one the scroll would later write
    // over is whichever of the other two the flip leaves behind. One flip fixes
    // both: BgSurface::vblank rebinds any layer that lands on the parked buffer
    // to the spare, so after this the two buffers in play are the two that are
    // free.
    {
        Texel *const dst = BgSurface::pixels(bgs.backBuffer());
        fillBlack(dst, BgSurface::kBufW * BgSurface::kBufH);
    }
    bgs.setScrollY(0);
    bgs.requestFlip();
    e.pumpIdleFrame();

    // The pause ScummVM measures from the ending video's last picture frame.
    e.delay(delayMs);
    if (e.quitRequested())
        return;

    // --- the two title cards ---------------------------------------------
    // Each fades up and holds. The blend is the hardware's, through the same
    // door opcode 18 uses.
    //
    // The blend ends by putting the scroll registers back where the letterbox
    // last left them (BgSurface::vblank), which for this screen is zero --
    // ScreenTakeover turned the letterbox off on the way in. That used to be a
    // hardcoded kRestY and this loop turned the letterbox off again after every
    // card to undo it, which could not help being a frame late: the card jumped
    // thirteen rows down the screen for the gap in between, and the gap grew the
    // moment the ending's soundtrack started being read off the card in it.
    for (int id = kFirstId; id < kFirstScrollId; ++id)
    {
        RpicImage still;
        if (id == kFirstId)
            still = first;
        else if (!loadRpicImage(creditsPath(id), still, error) || !still.valid())
        {
            DebugLog::warn("credits: %s", error.c_str());
            continue;
        }

        drawStill(bgs.backBuffer(), still);
        bgs.beginTransition(Transition::Blend);
        while (bgs.transitionActive() && !e.quitRequested())
            e.pumpIdleFrame();

        for (std::uint32_t f = 0; f < kStillHoldMs * 60 / 1000; ++f)
        {
            if (skipped() || e.quitRequested())
                return;
            e.pumpIdleFrame();
        }
    }

    // --- the scroll --------------------------------------------------------
    Roll roll;
    if (!roll.advance())
        return; // the seventeen scrolling images are all missing

    // Prime the window with the first screenful, then flip to it. From here on
    // the buffer being written IS the one on screen: the row written each step
    // is the one about to arrive at the bottom, which the beam has already
    // passed, so there is nothing to double-buffer.
    const int buf = bgs.backBuffer();
    {
        Texel *const dst = BgSurface::pixels(buf);
        fillBlack(dst, BgSurface::kBufW * BgSurface::kBufH);
        for (int y = 0; y < kScreenH; ++y)
            roll.nextRow(dst + static_cast<std::size_t>(y) * BgSurface::kBufW);
    }
    bgs.setScrollY(0);
    bgs.requestFlip();
    e.pumpIdleFrame();

    Texel *const ring = BgSurface::pixels(bgs.frontBuffer());
    int scrollY = 0;
    int accumulator = 0;

    // Once the images have run out, the last screenful still has to travel off
    // the top. That is kScreenH more rows of black behind it.
    int blackRowsLeft = kScreenH;
    while ((roll.live || blackRowsLeft > 0) && !e.quitRequested())
    {
        if (skipped())
            break;

        // Fractional advance: `image.height` destination rows stand for
        // kSourceRows of ScummVM's, so the roll keeps ScummVM's wall clock.
        accumulator += roll.image.height > 0 ? roll.image.height : kScreenH;
        while (accumulator >= kSourceRows)
        {
            accumulator -= kSourceRows;
            ++scrollY;
            // The row that is about to become screen row 191.
            const int row = (scrollY + kScreenH - 1) % BgSurface::kBufH;
            roll.nextRow(ring + static_cast<std::size_t>(row) * BgSurface::kBufW);
            if (!roll.live && blackRowsLeft > 0)
                --blackRowsLeft;
        }

        bgs.setScrollY(scrollY % BgSurface::kBufH);
        e.pumpIdleFrame();
    }
}

} // namespace rivenrt
