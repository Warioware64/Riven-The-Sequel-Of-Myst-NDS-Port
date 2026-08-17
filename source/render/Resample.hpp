#pragma once

// Reducing a picture onto the card, with a box filter.
//
// The card surface draws two kinds of thing. A full-card still arrives already
// at 256x165 and is copied a row at a time; anything else -- an overlay the
// converter left at its source size, or a section sliced out of a strip -- is in
// Riven's 608x392 pixels and has to be reduced by 2.375x on the way in. That
// reduction used to be nearest neighbour, on the argument that the eye cannot
// resolve the difference at this size.
//
// It can, for one picture in the game. Gehn's lab journal prints the dome's
// combination as five D'ni numerals, 32x24 each, which land on 13x10 DS pixels.
// D'ni numerals are a box with strokes inside it and they differ from each other
// BY those strokes, so a point sample that steps over one does not produce a
// slightly worse numeral, it produces a different one: the combination can be
// misread from the card view. A box filter keeps every stroke, as grey where it
// is thinner than a pixel.
//
// AVERAGED IN THE STORED (GAMMA) SPACE, not in light. Linear-light is the
// correct average and the converter does it that way, but it is the wrong one
// here: it would need a table on the DS and it lightens exactly the thin dark
// strokes this exists to save. Ink on paper reads better averaged naively.
//
// Free of <nds.h> so the kernel can be checked on the host -- see
// tests/test_resample_arm9.cpp. Nothing on hardware can check a filter.

#include <cstdint>

#include "RivenImage.hpp"

namespace rivenrt
{

/// The source columns that fall under one destination column, as [lo, hi).
struct Span
{
    std::uint16_t lo;
    std::uint16_t hi;
};

/// Fill `out[0..dw)` with the source column ranges for a `sw`-wide source rect
/// starting at `sx0`. Once per rectangle, so the inner loop has no divides --
/// the ARM9 has no divide instruction and this is the difference between a few
/// hundred of them and one per pixel.
///
/// Every span is at least one column wide, so a magnified axis degenerates to
/// the nearest sample rather than to nothing.
inline void buildColumnSpans(int sx0, int sw, int dw, Span *out)
{
    for (int x = 0; x < dw; ++x)
    {
        const int lo = x * sw / dw;
        int hi = (x + 1) * sw / dw;
        if (hi <= lo)
            hi = lo + 1;
        if (hi > sw)
            hi = sw;
        out[x] = Span{static_cast<std::uint16_t>(sx0 + lo),
                      static_cast<std::uint16_t>(sx0 + hi)};
    }
}

/// The source rows under destination row `y`, by the same rule.
inline Span rowSpan(int sy0, int sh, int dh, int y)
{
    const int lo = y * sh / dh;
    int hi = (y + 1) * sh / dh;
    if (hi <= lo)
        hi = lo + 1;
    if (hi > sh)
        hi = sh;
    return Span{static_cast<std::uint16_t>(sy0 + lo), static_cast<std::uint16_t>(sy0 + hi)};
}

/// Average one destination row out of `img`, writing `dw` texels to `out`.
///
/// `rows` is this row's source range and `cols` the spans built above. The alpha
/// bit is set on the result rather than averaged: every texel the converter
/// writes is opaque, and a cleared bit is a texel the DS blender drops.
inline void boxFilterRow(const rivendata::Texel *img, int stride, Span rows,
                         const Span *cols, int dw, rivendata::Texel *out)
{
    // The count only changes where a span is a column or a row wider than its
    // neighbours, so the reciprocal is worth carrying between pixels: it turns
    // one divide per texel into a handful per row.
    int lastCount = 0;
    std::uint32_t recip = 0;

    for (int x = 0; x < dw; ++x)
    {
        const Span col = cols[x];
        std::uint32_t r = 0, g = 0, b = 0;
        for (int sy = rows.lo; sy < rows.hi; ++sy)
        {
            const rivendata::Texel *row = img + static_cast<std::size_t>(sy) * stride;
            for (int sx = col.lo; sx < col.hi; ++sx)
            {
                const rivendata::Texel t = row[sx];
                r += t & 0x1F;
                g += (t >> 5) & 0x1F;
                b += (t >> 10) & 0x1F;
            }
        }

        const int count = (rows.hi - rows.lo) * (col.hi - col.lo);
        if (count != lastCount)
        {
            lastCount = count;
            // 16.16, rounded up, so the reciprocal never sums to less than the
            // true average and a flat field cannot drift a level darker.
            recip = (0x10000u + static_cast<std::uint32_t>(count) - 1)
                / static_cast<std::uint32_t>(count);
        }

        // +0x8000 is the half-step that makes this round rather than truncate.
        r = (r * recip + 0x8000u) >> 16;
        g = (g * recip + 0x8000u) >> 16;
        b = (b * recip + 0x8000u) >> 16;
        if (r > 31)
            r = 31;
        if (g > 31)
            g = 31;
        if (b > 31)
            b = 31;

        out[x] = static_cast<rivendata::Texel>(0x8000u | (b << 10) | (g << 5) | r);
    }
}

} // namespace rivenrt
