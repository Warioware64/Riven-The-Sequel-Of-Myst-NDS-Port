#include "riven/Marbles.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "RivenImage.hpp"
#include "riven/Archive.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/ImagePipeline.hpp"

namespace fs = std::filesystem;

namespace riven
{

MarbleResult convertMarbles(const fs::path &extrasMhk, const fs::path &out,
                            std::vector<std::string> &warnings)
{
    MarbleResult r;

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

    // Decode all six first, because the strip's cell size is whatever the
    // marbles turn out to be. They are 8x8 in every release seen, and the ARM9
    // does not assume that -- it cuts on the size reported here -- but a strip
    // has to be one size, so the largest wins and a smaller marble is padded.
    struct Cell
    {
        int w = 0;
        int h = 0;
        std::vector<std::uint8_t> rgb; ///< w*h*3, empty when it did not decode
    };
    Cell cells[kMarbleCount];

    for (int i = 0; i < kMarbleCount; ++i)
    {
        const auto id = static_cast<std::uint16_t>(kMarbleFirstId + i);
        const Bitmap image = set.readBitmap(id);
        if (!image.valid() || image.rgb() == nullptr || image.width() <= 0
            || image.height() <= 0)
        {
            warnings.push_back("marble image " + std::to_string(id)
                               + " could not be decoded");
            continue;
        }
        cells[i].w = image.width();
        cells[i].h = image.height();
        cells[i].rgb.assign(image.rgb(),
                            image.rgb()
                                + static_cast<std::size_t>(image.width()) * image.height() * 3);
    }

    for (const Cell &c : cells)
    {
        r.cellW = std::max(r.cellW, c.w);
        r.cellH = std::max(r.cellH, c.h);
    }
    if (r.cellW <= 0 || r.cellH <= 0)
    {
        r.error = "none of the six marbles (tBMP 200-205) decoded";
        return r;
    }

    // Lay them out left to right, in variable order. Black behind a marble that
    // is missing or smaller than the cell: the grid it is drawn on is dark and
    // the ARM9 draws these opaquely, so black is the least visible hole.
    const int stripW = r.cellW * kMarbleCount;
    std::vector<std::uint8_t> strip(static_cast<std::size_t>(stripW) * r.cellH * 3, 0);
    for (int i = 0; i < kMarbleCount; ++i)
    {
        const Cell &c = cells[i];
        if (c.rgb.empty())
            continue;
        for (int y = 0; y < c.h; ++y)
        {
            const std::uint8_t *src =
                c.rgb.data() + static_cast<std::size_t>(y) * c.w * 3;
            std::uint8_t *dst = strip.data()
                                + (static_cast<std::size_t>(y) * stripW
                                   + static_cast<std::size_t>(i) * r.cellW)
                                      * 3;
            std::copy(src, src + static_cast<std::size_t>(c.w) * 3, dst);
        }
    }

    // No downscale: a marble is already smaller than the card view's own scale
    // takes it to, and the destination rect on the ARM9 does the shrinking.
    // downscaleToTexels at 1:1 is still the right call rather than a hand-rolled
    // pack -- it is where the gamma-correct ARGB1555 conversion lives.
    const auto texels = downscaleToTexels(strip.data(), stripW, r.cellH, stripW, r.cellH);
    const auto bytes = encodeRpic(texels, stripW, r.cellH);
    if (!writeFileAtomic(out, bytes, r.error))
        return r;

    r.bytes = bytes.size();
    r.ok = true;
    return r;
}

} // namespace riven
