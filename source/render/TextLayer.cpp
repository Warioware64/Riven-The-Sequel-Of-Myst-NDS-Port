#include "TextLayer.hpp"

#include <cstdio>
#include <cstdlib>

#include "Global.hpp"
#include "render/BgSurface.hpp"
#include "tonccpy.h"

namespace rivenrt
{

TextLayer textLayer;

namespace
{
    /// The A1RGB5 build of the menu font. Paired with DejaVuSans-Bold.fnt,
    /// which is the same typeface's metadata and carries no format of its own.
    const char *const kFontTexture = "font/DejaVuSansMenu_0_png_tex.bin";
    const char *const kFontMetadata = "font/DejaVuSans-Bold.fnt";
    constexpr int kFontW = 128;
    constexpr int kFontH = 64;
    constexpr std::size_t kFontBytes = static_cast<std::size_t>(kFontW) * kFontH * 2;

    /// One text context per BgSurface buffer. A context's destination is fixed
    /// when it is created, and the renderer has to be able to write whichever
    /// buffer is currently off screen, so there is one for each rather than a
    /// recreate on every flip. They share one font texture.
    NEA_Hw2DTextCtx *g_ctx[BgSurface::kBuffers] = {};

    void *loadFontTexture()
    {
        std::FILE *f = std::fopen(kFontTexture, "rb");
        if (f == nullptr)
            return nullptr;
        void *buf = std::malloc(kFontBytes);
        if (buf == nullptr)
        {
            std::fclose(f);
            return nullptr;
        }
        const std::size_t got = std::fread(buf, 1, kFontBytes, f);
        std::fclose(f);
        if (got != kFontBytes)
        {
            std::free(buf);
            return nullptr;
        }
        return buf;
    }
} // namespace

bool TextLayer::create()
{
    if (ready())
        return true;
    if (!global.hasNitroFS || !bgs.exists())
    {
        std::printf("no menu font: %s\n",
                    global.hasNitroFS ? "the card view is not up" : "no NitroFS");
        return false;
    }

    fontTexture_ = loadFontTexture();
    if (fontTexture_ == nullptr)
    {
        std::printf("no menu font: %s is missing\n", kFontTexture);
        return false;
    }

    for (int b = 0; b < BgSurface::kBuffers; ++b)
    {
        // The buffer IS VRAM. Legal here and not for the Myst port's 8bpp top
        // screen, because a 16-bit pixel is a halfword store and VRAM only
        // discards byte-wide ones -- see the header.
        g_ctx[b] = NEA_Hw2DTextCtxCreateBuffer(NEA_HW2D_BG_BITMAP_16,
                                               BgSurface::pixels(b), BgSurface::kBufW,
                                               BgSurface::kBufH);
        if (g_ctx[b] == nullptr)
        {
            destroy();
            std::printf("no menu font: could not make a text context\n");
            return false;
        }
        NEA_Hw2DTextCtxMetadataLoadFAT(g_ctx[b], kFontMetadata);
        // Null palette: a 16bpp context has no palette, and passing one would
        // be copied over BG_PALETTE.
        NEA_Hw2DTextCtxBitmapSet(g_ctx[b], fontTexture_, kFontW, kFontH, nullptr, 0);
        // A menu row is a row. Wrapping a label that does not fit would push
        // the rows below it out of the positions the hit test knows about.
        NEA_Hw2DTextCtxSetWordWrap(g_ctx[b], false, nullptr);
        NEA_Hw2DTextCtxSetOverflow(g_ctx[b], NEA_HW2D_TEXT_RIGHT_TRIM,
                                   NEA_HW2D_TEXT_BOTTOM_TRIM);
    }

    ctx_ = g_ctx[0];
    buffer_ = 0;
    return true;
}

void TextLayer::destroy()
{
    for (int b = 0; b < BgSurface::kBuffers; ++b)
    {
        if (g_ctx[b] != nullptr)
        {
            NEA_Hw2DTextCtxDelete(g_ctx[b]);
            g_ctx[b] = nullptr;
        }
    }
    // Ours, not NEA's: BitmapSet does not take ownership.
    std::free(fontTexture_);
    fontTexture_ = nullptr;
    ctx_ = nullptr;
    buffer_ = -1;
}

bool TextLayer::target(int buffer)
{
    if (buffer < 0 || buffer >= BgSurface::kBuffers || g_ctx[buffer] == nullptr)
        return false;
    ctx_ = g_ctx[buffer];
    buffer_ = buffer;
    return true;
}

void TextLayer::clear()
{
    if (!ready())
        return;
    // Not NEA_Hw2DTextCtxClear: that only wipes the ctx's canvas rectangle, and
    // this has to reach the rows below the card view as well -- the buffer is
    // 256x256 and the screen is 192 of it.
    toncset16(BgSurface::pixels(buffer_), 0,
              static_cast<std::size_t>(BgSurface::kBufW) * BgSurface::kBufH);
}

void TextLayer::draw(int x, int y, const std::string &text)
{
    if (!ready() || text.empty())
        return;
    // The canvas is set per draw rather than once: it is what clips the string,
    // and a row that runs long should be cut at the right edge rather than
    // wrapped onto the row below it.
    NEA_Hw2DTextCtxSetCanvas(ctx_, x, y, BgSurface::kBufW, y + kLineHeight);
    NEA_Hw2DTextCtxCursorSet(ctx_, x, y);
    NEA_Hw2DTextCtxPrint(ctx_, text.c_str());
}

} // namespace rivenrt
