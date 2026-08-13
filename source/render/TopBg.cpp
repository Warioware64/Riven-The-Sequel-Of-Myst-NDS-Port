#include "TopBg.hpp"

#include <cstdio>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "RivenImage.hpp"
#include "data/ImageFile.hpp"
#include "tonccpy.h"

namespace rivenrt
{

TopBg topBg;

namespace
{
    /// 16 KB units, so 4 is 64 KB into bank C -- above the console's map at
    /// base 31 (62 KB). See the layout table in the header.
    constexpr int kBitmapBase = 4;
    constexpr int kBgW = 256;
    constexpr int kBgH = 256;
    /// Below the console (priority 0) and out of the way of everything else.
    constexpr unsigned kPriority = 3;

    /// The palette entries the picture may use. Kept in step with the
    /// converter's riven::kTopBgColours by the check in load().
    constexpr int kPictureColours = 240;

    std::uint8_t *bitmap()
    {
        return reinterpret_cast<std::uint8_t *>(BG_GFX_SUB + kBitmapBase * (16 * 1024 / 2));
    }
} // namespace

void TopBg::load()
{
    if (loaded() || !global.hasFat)
        return;

    // The debug trace wants the top screen to itself: it is a wall of small
    // text and a photograph behind it is not a background, it is noise. Not
    // loaded rather than loaded-and-hidden, so the 64 KB and the read are saved
    // too. Turning the trace off needs a restart, which is what the settings
    // row says.
    if (DebugLog::enabled())
    {
        DebugLog::log("TOPBG skipped: the trace has the top screen");
        return;
    }

    const std::string path = global.uiDir() + "topbg.rpiz";

    RpizImage img;
    std::string error;
    if (!loadRpizImage(path, img, error))
    {
        // Not an error the player can act on beyond reconverting, and the game
        // is entirely playable without it, so this is one line and no more.
        DebugLog::warn("no top-screen picture: %s", error.c_str());
        return;
    }
    if (img.width != kBgW || img.height > kBgH)
    {
        DebugLog::warn("top-screen picture is %dx%d, not %dx192", img.width, img.height,
                       kBgW);
        return;
    }

    // bgInitHidden, not bgInit: bank C holds whatever Global::Init's dmaFillWords
    // left, and showing the layer before the picture is in it would be a frame of
    // black at best and the previous contents at worst.
    bgId_ = bgInitHiddenSub(3, BgType_Bmp8, BgSize_B8_256x256, kBitmapBase, 0);
    if (bgId_ < 0)
    {
        DebugLog::warn("top-screen picture: no free sub background");
        return;
    }

    std::uint8_t *dst = bitmap();
    const std::size_t pictureBytes = static_cast<std::size_t>(kBgW) * img.height;

    // tonccpy and toncset, never memcpy/memset: VRAM discards byte-wide stores,
    // so a byte-at-a-time copy of an 8bpp image writes nothing at all on
    // hardware -- and silently works in some emulators, which is worse.
    tonccpy(dst, img.indices.data(), pictureBytes);
    // The rows the picture does not reach. Index 0 is black in both the file's
    // palette and libnds's console default, so this is the same black as the
    // backdrop whichever layer ends up showing.
    toncset(dst + pictureBytes, 0, static_cast<std::size_t>(kBgW) * kBgH - pictureBytes);

    // Only the picture's half of the palette. BG_PALETTE_SUB[240..255] holds the
    // console's ANSI colours, entry 255 being the white it actually prints in.
    tonccpy(BG_PALETTE_SUB, img.palette, kPictureColours * sizeof(rivendata::Texel));

    DebugLog::log("TOPBG %dx%d", img.width, img.height);

    bgSetPriority(bgId_, kPriority);
    bgShow(bgId_);
    // Mode 5's BG3 is an extended-rotation layer, and bgInit only STAGES its
    // identity affine matrix -- without this the picture is drawn through
    // whatever was in the registers.
    bgUpdate();
}

void TopBg::setVisible(bool visible)
{
    if (!loaded())
        return;
    if (visible)
        bgShow(bgId_);
    else
        bgHide(bgId_);
    bgUpdate();
}

} // namespace rivenrt
