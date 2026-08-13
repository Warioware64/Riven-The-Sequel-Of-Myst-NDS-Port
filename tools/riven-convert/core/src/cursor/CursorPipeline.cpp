#include "riven/CursorPipeline.hpp"

#include <algorithm>
#include <cstring>
#include <map>

#include "riven/Archive.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/PeCursors.hpp"

namespace fs = std::filesystem;
using namespace rivendata;

namespace riven
{
namespace
{
    /// RGB888 -> RGB555, the same rounding the card art uses. No dither: these
    /// are 16-pixel-wide icons, and ordered dithering on one is speckle rather
    /// than gradient.
    std::uint16_t toRgb555(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        const int r5 = (r * 31 + 127) / 255;
        const int g5 = (g * 31 + 127) / 255;
        const int b5 = (b * 31 + 127) / 255;
        return static_cast<std::uint16_t>(r5 | (g5 << 5) | (b5 << 10));
    }

    int distanceSquared(std::uint16_t a, std::uint16_t b)
    {
        const int dr = (a & 31) - (b & 31);
        const int dg = ((a >> 5) & 31) - ((b >> 5) & 31);
        const int db = ((a >> 10) & 31) - ((b >> 10) & 31);
        return dr * dr + dg * dg + db * db;
    }

    /// Area-average an RGB image with a parallel opacity plane down to
    /// `dstW x dstH`. Colour is averaged over the OPAQUE source pixels only --
    /// averaging in the transparent ones would drag every edge towards whatever
    /// happens to be behind the mask, which on a Win32 cursor is black, and give
    /// the hand a dark fringe.
    ///
    /// Opacity is a majority vote, ties opaque. A cursor is mostly outline and
    /// rounding thin strokes away is what makes a downscaled one disappear.
    void downscaleCel(const std::uint8_t *rgb, const std::uint8_t *opaque, int srcW,
                      int srcH, int dstW, int dstH, std::vector<std::uint8_t> &outRgb,
                      std::vector<std::uint8_t> &outOpaque)
    {
        outRgb.assign(static_cast<std::size_t>(dstW) * dstH * 3, 0);
        outOpaque.assign(static_cast<std::size_t>(dstW) * dstH, 0);

        for (int y = 0; y < dstH; ++y)
        {
            const int y0 = y * srcH / dstH;
            const int y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
            for (int x = 0; x < dstW; ++x)
            {
                const int x0 = x * srcW / dstW;
                const int x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);

                int sum[3] = {0, 0, 0};
                int opaqueCount = 0;
                int total = 0;
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                    {
                        const std::size_t at = static_cast<std::size_t>(sy) * srcW + sx;
                        ++total;
                        if (opaque[at] == 0)
                            continue;
                        ++opaqueCount;
                        sum[0] += rgb[at * 3 + 0];
                        sum[1] += rgb[at * 3 + 1];
                        sum[2] += rgb[at * 3 + 2];
                    }

                const std::size_t dst = static_cast<std::size_t>(y) * dstW + x;
                outOpaque[dst] = opaqueCount * 2 >= total && opaqueCount > 0 ? 255 : 0;
                if (opaqueCount > 0)
                    for (int i = 0; i < 3; ++i)
                        outRgb[dst * 3 + i] =
                            static_cast<std::uint8_t>(sum[i] / opaqueCount);
            }
        }
    }

    /// Restrict a downscaled cel to colours its source actually used.
    ///
    /// Averaging 2x2 blocks invents colours that are in neither the source nor
    /// anything else: Riven's 19 cursors use 85 distinct colours between them and
    /// come out of the downscale using 191, which is more than the DS's single
    /// OBJ palette can give one set alongside the other. Snapping each blended
    /// pixel to the nearest colour the artwork already contains puts the count
    /// back where it started, and the error is invisible at 16x16 because the
    /// source palette is a full-range one -- the nearest colour is a near miss,
    /// not a different colour.
    void snapToSourceColours(std::vector<std::uint8_t> &rgb,
                             const std::vector<std::uint8_t> &opaque,
                             const std::vector<std::uint16_t> &allowed)
    {
        if (allowed.empty())
            return;
        for (std::size_t i = 0; i < opaque.size(); ++i)
        {
            if (opaque[i] == 0)
                continue;
            const std::uint16_t want =
                toRgb555(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
            std::uint16_t best = allowed.front();
            int bestDistance = 1 << 30;
            for (const std::uint16_t c : allowed)
            {
                const int d = distanceSquared(want, c);
                if (d < bestDistance)
                {
                    bestDistance = d;
                    best = c;
                }
            }
            // Back to 8 bits so the rest of the pipeline stays in one form. The
            // round trip is exact: 5 bits scaled up and re-quantised is itself.
            rgb[i * 3 + 0] = static_cast<std::uint8_t>((best & 31) * 255 / 31);
            rgb[i * 3 + 1] = static_cast<std::uint8_t>(((best >> 5) & 31) * 255 / 31);
            rgb[i * 3 + 2] = static_cast<std::uint8_t>(((best >> 10) & 31) * 255 / 31);
        }
    }

    /// The distinct opaque colours in a source image, as RGB555.
    void collectColours(const std::uint8_t *rgb, const std::uint8_t *opaque, std::size_t n,
                        std::vector<std::uint16_t> &into)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            if (opaque != nullptr && opaque[i] == 0)
                continue;
            into.push_back(toRgb555(rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]));
        }
        std::sort(into.begin(), into.end());
        into.erase(std::unique(into.begin(), into.end()), into.end());
    }

    /// Where a pixel lands in a cel's tile-ordered pixel block. DS OBJ 1D
    /// mapping: 8x8 tiles, left to right then top to bottom, each row-major.
    std::size_t tileOffset(int x, int y, int celW)
    {
        const int tilesAcross = celW / 8;
        const int tx = x / 8, ty = y / 8;
        const std::size_t tile = static_cast<std::size_t>(ty) * tilesAcross + tx;
        return tile * 64 + static_cast<std::size_t>(y % 8) * 8 + (x % 8);
    }
} // namespace

std::vector<std::uint8_t> encodeRcur(const std::vector<RcurSourceCel> &cels, int celW,
                                     int celH, int paletteBase, int paletteMax,
                                     std::vector<std::string> &warnings)
{
    // --- the shared palette -------------------------------------------------
    std::vector<std::uint16_t> palette;
    {
        std::map<std::uint16_t, int> seen;
        for (const RcurSourceCel &c : cels)
            for (std::size_t i = 0; i < c.opaque.size(); ++i)
            {
                if (c.opaque[i] == 0)
                    continue;
                const std::uint16_t v =
                    toRgb555(c.rgb[i * 3], c.rgb[i * 3 + 1], c.rgb[i * 3 + 2]);
                ++seen[v];
            }

        if (static_cast<int>(seen.size()) <= paletteMax)
        {
            for (const auto &[colour, count] : seen)
            {
                (void)count;
                palette.push_back(colour);
            }
        }
        else
        {
            // Keep the most-used colours and map the rest onto them. Riven's own
            // sets have 85 distinct colours against 127 slots, so this is a
            // guard rather than a path that runs -- but a set that silently lost
            // colours would be found much later and much more confusingly.
            std::vector<std::pair<int, std::uint16_t>> byUse;
            byUse.reserve(seen.size());
            for (const auto &[colour, count] : seen)
                byUse.emplace_back(count, colour);
            std::sort(byUse.begin(), byUse.end(),
                      [](const auto &a, const auto &b) { return a.first > b.first; });
            byUse.resize(static_cast<std::size_t>(paletteMax));
            for (const auto &[count, colour] : byUse)
            {
                (void)count;
                palette.push_back(colour);
            }
            std::sort(palette.begin(), palette.end());
            warnings.push_back("the sprite set has " + std::to_string(seen.size())
                               + " colours and only " + std::to_string(paletteMax)
                               + " palette entries; the rest were matched to the nearest");
        }
    }

    const auto indexOf = [&](std::uint16_t colour) -> std::uint8_t {
        const auto it = std::lower_bound(palette.begin(), palette.end(), colour);
        if (it != palette.end() && *it == colour)
            return static_cast<std::uint8_t>(paletteBase + (it - palette.begin()));

        int best = 0;
        int bestDistance = 1 << 30;
        for (std::size_t i = 0; i < palette.size(); ++i)
        {
            const int d = distanceSquared(colour, palette[i]);
            if (d < bestDistance)
            {
                bestDistance = d;
                best = static_cast<int>(i);
            }
        }
        return static_cast<std::uint8_t>(paletteBase + best);
    };

    // --- the file -----------------------------------------------------------
    const std::uint32_t celBytes = rcurCelBytes(celW, celH);

    RcurHeader header{};
    std::memcpy(header.magic, kCursorMagic, 4);
    header.version = kCursorVersion;
    header.celWidth = static_cast<std::uint8_t>(celW);
    header.celHeight = static_cast<std::uint8_t>(celH);
    header.paletteBase = static_cast<std::uint8_t>(paletteBase);
    header.paletteCount = static_cast<std::uint8_t>(palette.size());
    header.celCount = static_cast<std::uint16_t>(cels.size());
    header.dataBytes = celBytes * static_cast<std::uint32_t>(cels.size());

    std::vector<std::uint8_t> out(sizeof(header));
    std::memcpy(out.data(), &header, sizeof(header));

    for (const std::uint16_t colour : palette)
    {
        out.push_back(static_cast<std::uint8_t>(colour & 0xFF));
        out.push_back(static_cast<std::uint8_t>(colour >> 8));
    }

    for (const RcurSourceCel &c : cels)
    {
        RcurCel rec{};
        rec.id = c.id;
        rec.hotX = static_cast<std::uint8_t>(std::clamp(c.hotX, 0, celW - 1));
        rec.hotY = static_cast<std::uint8_t>(std::clamp(c.hotY, 0, celH - 1));
        rec.drawW = static_cast<std::uint8_t>(std::clamp(c.drawW, 0, celW));
        rec.drawH = static_cast<std::uint8_t>(std::clamp(c.drawH, 0, celH));
        const std::size_t at = out.size();
        out.resize(at + sizeof(rec));
        std::memcpy(out.data() + at, &rec, sizeof(rec));
    }

    for (const RcurSourceCel &c : cels)
    {
        const std::size_t base = out.size();
        out.resize(base + celBytes, 0); // 0 is the transparent index

        for (int y = 0; y < celH; ++y)
            for (int x = 0; x < celW; ++x)
            {
                const std::size_t src = static_cast<std::size_t>(y) * celW + x;
                if (src >= c.opaque.size() || c.opaque[src] == 0)
                    continue;
                const std::uint16_t colour =
                    toRgb555(c.rgb[src * 3], c.rgb[src * 3 + 1], c.rgb[src * 3 + 2]);
                out[base + tileOffset(x, y, celW)] = indexOf(colour);
            }
    }

    return out;
}

CursorResult convertCursors(const std::vector<std::uint8_t> &exeBytes, const fs::path &out,
                            std::vector<std::string> &warnings)
{
    CursorResult r;

    const auto source = readPeCursors(exeBytes, warnings);
    if (source.empty())
    {
        r.error = "the executable holds no cursors this converter can read";
        return r;
    }

    // The palette the artwork already uses, across the whole set -- the cursors
    // share one in riven.exe, so this is that palette minus what nothing draws.
    std::vector<std::uint16_t> sourceColours;
    for (const PeCursor &c : source)
        collectColours(c.rgb.data(), c.opaque.data(),
                       static_cast<std::size_t>(c.width) * c.height, sourceColours);

    std::vector<RcurSourceCel> cels;
    cels.reserve(source.size());
    for (const PeCursor &c : source)
    {
        RcurSourceCel cel;
        cel.id = c.groupId;
        downscaleCel(c.rgb.data(), c.opaque.data(), c.width, c.height, kCursorCel,
                     kCursorCel, cel.rgb, cel.opaque);
        snapToSourceColours(cel.rgb, cel.opaque, sourceColours);
        // The hot point scales with the picture. Rounding down keeps it inside
        // the cel for a hot point on the far edge (3007's is at y 15 of 32).
        cel.hotX = c.hotX * kCursorCel / c.width;
        cel.hotY = c.hotY * kCursorCel / c.height;
        cel.drawW = kCursorCel;
        cel.drawH = kCursorCel;
        cels.push_back(std::move(cel));
    }

    const auto bytes = encodeRcur(cels, kCursorCel, kCursorCel, kCursorPaletteBase,
                                  kCursorPaletteMax, warnings);
    std::string err;
    if (!writeFileAtomic(out, bytes, err))
    {
        r.error = err;
        return r;
    }

    r.ok = true;
    r.cels = static_cast<int>(cels.size());
    r.bytes = bytes.size();
    return r;
}

CursorResult convertInventory(const fs::path &extrasMhk, const fs::path &out,
                              std::vector<std::string> &warnings)
{
    CursorResult r;

    // extras.MHK is a Mohawk archive like any other, so it goes through the
    // converter's existing reader -- the only new thing about it is where it
    // came from.
    ArchiveSet set;
    std::vector<std::string> failures;
    set.openAll({extrasMhk}, failures);
    for (const std::string &f : failures)
        warnings.push_back(f);
    if (set.empty())
    {
        r.error = extrasMhk.filename().string() + " is not a readable Mohawk archive";
        return r;
    }

    std::vector<RcurSourceCel> cels;
    for (const std::uint16_t id : {kInvTrapBook, kInvAtrusJournal, kInvCathJournal})
    {
        const Bitmap image = set.readBitmap(id);
        if (!image.valid() || image.rgb() == nullptr)
        {
            warnings.push_back("inventory image " + std::to_string(id)
                               + " could not be decoded");
            continue;
        }

        // Fit inside the cel, never enlarge: these are 24x36, 18x24 and 22x12,
        // and blowing the small one up to fill the cel would make the three
        // books different scales from each other.
        const int w = image.width();
        const int h = image.height();
        int drawW = w, drawH = h;
        if (drawW > kInvCelW || drawH > kInvCelH)
        {
            const double scale =
                std::min(static_cast<double>(kInvCelW) / w, static_cast<double>(kInvCelH) / h);
            drawW = std::max(1, static_cast<int>(w * scale));
            drawH = std::max(1, static_cast<int>(h * scale));
        }

        // The art is opaque throughout: Riven fills the strip black behind it
        // (riven_inventory.cpp:82-88) and the DS band is the 3D clear colour,
        // also black, so an opaque rectangle is pixel-identical to keying black
        // out -- and cannot punch holes in the middle of a dark book.
        std::vector<std::uint8_t> scaledRgb, scaledOpaque;
        {
            std::vector<std::uint8_t> allOpaque(static_cast<std::size_t>(w) * h, 255);
            downscaleCel(image.rgb(), allOpaque.data(), w, h, drawW, drawH, scaledRgb,
                         scaledOpaque);

            std::vector<std::uint16_t> sourceColours;
            collectColours(image.rgb(), nullptr, static_cast<std::size_t>(w) * h,
                           sourceColours);
            snapToSourceColours(scaledRgb, scaledOpaque, sourceColours);
        }

        RcurSourceCel cel;
        cel.id = id;
        cel.hotX = 0;
        cel.hotY = 0;
        cel.drawW = drawW;
        cel.drawH = drawH;
        cel.rgb.assign(static_cast<std::size_t>(kInvCelW) * kInvCelH * 3, 0);
        cel.opaque.assign(static_cast<std::size_t>(kInvCelW) * kInvCelH, 0);

        const int offX = (kInvCelW - drawW) / 2;
        const int offY = (kInvCelH - drawH) / 2;
        for (int y = 0; y < drawH; ++y)
            for (int x = 0; x < drawW; ++x)
            {
                const std::size_t src = static_cast<std::size_t>(y) * drawW + x;
                const std::size_t dst =
                    static_cast<std::size_t>(offY + y) * kInvCelW + (offX + x);
                cel.opaque[dst] = 255;
                for (int i = 0; i < 3; ++i)
                    cel.rgb[dst * 3 + i] = scaledRgb[src * 3 + i];
            }

        cels.push_back(std::move(cel));
    }

    if (cels.empty())
    {
        r.error = "no inventory images could be read";
        return r;
    }

    const auto bytes = encodeRcur(cels, kInvCelW, kInvCelH, kExtrasPaletteBase,
                                  kExtrasPaletteMax, warnings);
    std::string err;
    if (!writeFileAtomic(out, bytes, err))
    {
        r.error = err;
        return r;
    }

    r.ok = true;
    r.cels = static_cast<int>(cels.size());
    r.bytes = bytes.size();
    return r;
}

} // namespace riven
