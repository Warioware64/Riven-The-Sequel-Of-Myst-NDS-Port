// The card surface's reduction filter, compiled for the host.
//
// Not a converter test. render/Resample.hpp is free of <nds.h> so that it can be
// checked here, because nothing on hardware can check a filter -- a resampling
// mistake does not crash, it just draws a slightly different picture, and the
// one picture where "slightly different" is not slight is the dome combination.
//
// Gehn's lab journal prints it as five D'ni numerals, 32x24 in Riven's frame and
// 13x10 on the DS. D'ni numerals are a box with strokes inside it and they
// differ from each other BY those strokes, so a filter that steps over a stroke
// does not make the numeral worse, it makes it a DIFFERENT numeral, and the
// player reads the wrong combination off the page. That is the claim this file
// exists to defend, and the nearest-neighbour sample that used to do this job
// fails it -- the test below shows it failing.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "render/Resample.hpp"

using namespace rivenrt;
using rivendata::Texel;

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const std::string &what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++g_failures;
        }
    }

    constexpr Texel kInk = 0x8000;                          // black, opaque
    constexpr Texel kPaper = static_cast<Texel>(0x8000 | (31 << 10) | (31 << 5) | 31);

    int red(Texel t) { return t & 0x1F; }

    /// One D'ni numeral's worth of art: a 32x24 box with `strokes` vertical
    /// lines inside it, each one pixel wide -- which is what a numeral's
    /// interior actually looks like, and what a point sample loses.
    std::vector<Texel> glyph(int w, int h, const std::vector<int> &strokes)
    {
        std::vector<Texel> px(static_cast<std::size_t>(w) * h, kPaper);
        for (int x = 0; x < w; ++x)
        {
            px[x] = kInk;                                              // top rule
            px[static_cast<std::size_t>(h - 1) * w + x] = kInk;        // bottom rule
        }
        for (int y = 1; y < h - 1; ++y)
            for (const int sx : strokes)
                px[static_cast<std::size_t>(y) * w + sx] = kInk;
        return px;
    }

    /// The filter, over a whole rectangle, the way CardSurface drives it.
    std::vector<Texel> reduce(const std::vector<Texel> &src, int w, int h, int dw, int dh)
    {
        std::vector<Span> cols(static_cast<std::size_t>(dw));
        buildColumnSpans(0, w, dw, cols.data());
        std::vector<Texel> out(static_cast<std::size_t>(dw) * dh);
        for (int y = 0; y < dh; ++y)
            boxFilterRow(src.data(), w, rowSpan(0, h, dh, y), cols.data(), dw,
                         out.data() + static_cast<std::size_t>(y) * dw);
        return out;
    }

    /// What the card surface used to do, kept so the improvement is a measured
    /// difference rather than an assertion about intent.
    std::vector<Texel> nearest(const std::vector<Texel> &src, int w, int h, int dw, int dh)
    {
        std::vector<Texel> out(static_cast<std::size_t>(dw) * dh);
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
                out[static_cast<std::size_t>(y) * dw + x] =
                    src[static_cast<std::size_t>(y * h / dh) * w + x * w / dw];
        return out;
    }

    /// How many of a row's pixels carry any ink at all.
    int inked(const std::vector<Texel> &px, int w, int y)
    {
        int n = 0;
        for (int x = 0; x < w; ++x)
            if (red(px[static_cast<std::size_t>(y) * w + x]) < 31)
                ++n;
        return n;
    }
} // namespace

int main()
{
    // --- the spans tile the source exactly ---------------------------------
    //
    // Every source column must land in exactly one destination column: a gap
    // drops a stroke and an overlap counts one twice, and both look like a
    // plausible picture.
    {
        for (const int sw : {32, 25, 608, 800, 7})
            for (const int dw : {13, 10, 256, 3, 32})
            {
                std::vector<Span> cols(static_cast<std::size_t>(dw));
                buildColumnSpans(100, sw, dw, cols.data());

                const std::string what = " (" + std::to_string(sw) + " -> "
                    + std::to_string(dw) + ")";
                bool ordered = true, contiguous = true, nonEmpty = true;
                for (int x = 0; x < dw; ++x)
                {
                    if (cols[x].hi <= cols[x].lo)
                        nonEmpty = false;
                    if (cols[x].lo < 100 || cols[x].hi > 100 + sw)
                        ordered = false;
                    if (x > 0 && cols[x].lo != cols[x - 1].hi && sw >= dw)
                        contiguous = false;
                }
                check(nonEmpty, "every span covers at least one column" + what);
                check(ordered, "no span leaves the source rectangle" + what);
                if (sw >= dw)
                {
                    check(contiguous, "the spans tile the source without gaps" + what);
                    check(cols[0].lo == 100 && cols[dw - 1].hi == 100 + sw,
                          "the spans span the whole source" + what);
                }
            }
    }

    // --- a flat field must not drift ---------------------------------------
    //
    // The reciprocal is a fixed-point divide and the rounding in it is the only
    // thing between a grey wall and a grey wall one level darker.
    {
        for (const int level : {0, 1, 7, 16, 30, 31})
        {
            const Texel t =
                static_cast<Texel>(0x8000 | (level << 10) | (level << 5) | level);
            const std::vector<Texel> flat(32 * 24, t);
            const auto out = reduce(flat, 32, 24, 13, 10);
            bool same = true;
            for (const Texel o : out)
                if (o != t)
                    same = false;
            check(same, "a flat field survives the reduction unchanged, level "
                            + std::to_string(level));
        }
    }

    // --- every texel comes out opaque --------------------------------------
    {
        const auto out = reduce(glyph(32, 24, {8, 16, 24}), 32, 24, 13, 10);
        bool opaque = true;
        for (const Texel o : out)
            if ((o & 0x8000) == 0)
                opaque = false;
        check(opaque, "every filtered texel keeps its alpha bit");
    }

    // --- THE ONE THAT MATTERS: a one-pixel stroke survives ------------------
    //
    // 32 source columns into 13 destination columns is a 2.46x reduction, so a
    // one-pixel stroke is thinner than a destination pixel. The box filter has
    // to render it as grey; the point sample either hits it or does not, and
    // over three strokes it will miss at least one.
    {
        const std::vector<int> strokes = {8, 16, 24};
        const auto art = glyph(32, 24, strokes);
        const auto box = reduce(art, 32, 24, 13, 10);
        const auto pt = nearest(art, 32, 24, 13, 10);

        // Row 5 of 10 is inside the box, so the only ink there is the strokes.
        const int boxInk = inked(box, 13, 5);
        const int ptInk = inked(pt, 13, 5);

        check(boxInk >= static_cast<int>(strokes.size()),
              "the box filter keeps every interior stroke");
        check(ptInk < static_cast<int>(strokes.size()),
              "the point sample loses at least one -- the bug this filter fixes");
        check(boxInk > ptInk, "the box filter keeps strictly more of the numeral");

        // And the strokes must be GREY, not black: a one-pixel line under a
        // 2.46x reduction that came out full black would mean the filter had
        // widened it, which is the other way to make a numeral unreadable.
        bool anyGrey = false;
        for (int x = 0; x < 13; ++x)
        {
            const int v = red(box[5 * 13 + x]);
            if (v > 0 && v < 31)
                anyGrey = true;
        }
        check(anyGrey, "a sub-pixel stroke is rendered as grey, not widened to black");
    }

    // --- two numerals that differ by one stroke stay different --------------
    //
    // This is the failure the player would actually see: not a blurry numeral,
    // but the wrong one.
    {
        const auto a = reduce(glyph(32, 24, {8, 16, 24}), 32, 24, 13, 10);
        const auto b = reduce(glyph(32, 24, {8, 24}), 32, 24, 13, 10);
        check(a != b, "two numerals differing by one stroke reduce to different art");

        const auto pa = nearest(glyph(32, 24, {8, 16, 24}), 32, 24, 13, 10);
        const auto pb = nearest(glyph(32, 24, {8, 24}), 32, 24, 13, 10);
        check(pa == pb,
              "point-sampled, those two numerals were indistinguishable -- the "
              "combination could be misread off the card");
    }

    // --- a magnified axis degenerates rather than breaking ------------------
    //
    // CardSurface only calls the filter when it is reducing, but the kernel must
    // not produce nonsense if an axis grows: a span of one source pixel is the
    // point sample, which is the right answer.
    {
        std::vector<Span> cols(40);
        buildColumnSpans(0, 13, 40, cols.data());
        bool single = true;
        for (const Span &s : cols)
            if (s.hi != s.lo + 1)
                single = false;
        check(single, "a magnified axis falls back to one source column per pixel");
    }

    std::printf("resample: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
