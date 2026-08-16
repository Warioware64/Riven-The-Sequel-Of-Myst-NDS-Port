#include "riven/Credits.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "RivenImage.hpp"
#include "riven/Archive.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/ImagePipeline.hpp"

namespace fs = std::filesystem;

namespace riven
{
void creditsSize(int w, int h, int &outW, int &outH)
{
    if (w <= 0 || h <= 0)
    {
        outW = 0;
        outH = 0;
        return;
    }
    outH = std::min(kCreditsViewH, h);
    outW = static_cast<int>((static_cast<long long>(w) * outH + h / 2) / h);
    if (outW > kCreditsViewW)
    {
        outW = kCreditsViewW;
        outH = static_cast<int>((static_cast<long long>(h) * outW + w / 2) / w);
    }
    outW = std::max(1, outW);
    outH = std::max(1, outH);
}

CreditsResult convertCredits(const fs::path &extrasMhk, const fs::path &outDir,
                             std::vector<std::string> &warnings)
{
    CreditsResult r;

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

    std::error_code ec;
    fs::create_directories(outDir, ec);

    // One at a time, and nothing is held between iterations: this is the only
    // stage whose source images are 715 KB each and whose outputs are never
    // compared with one another.
    for (int id = kCreditsFirstId; id <= kCreditsLastId; ++id)
    {
        const Bitmap image = set.readBitmap(static_cast<std::uint16_t>(id));
        if (!image.valid() || image.rgb() == nullptr || image.width() <= 0
            || image.height() <= 0)
        {
            warnings.push_back("credits image " + std::to_string(id)
                               + " could not be decoded");
            continue;
        }

        int dstW = 0;
        int dstH = 0;
        creditsSize(image.width(), image.height(), dstW, dstH);

        const auto texels =
            downscaleToTexels(image.rgb(), image.width(), image.height(), dstW, dstH);

        // srcWidth/srcHeight are the tBMP's own size, as everywhere else. The
        // runtime does not place these by a PLST rectangle -- it centres them --
        // but the header's contract is "how big this is in Riven's coordinates"
        // and writing the resampled size there would be a lie that a later
        // reader could act on.
        const auto bytes =
            encodeRpic(texels, dstW, dstH, image.width(), image.height());

        const fs::path out = outDir / (std::to_string(id) + ".rpic");
        std::string error;
        if (!writeFileAtomic(out, bytes, error))
        {
            warnings.push_back(error);
            continue;
        }

        r.bytes += bytes.size();
        ++r.images;
        // The last one wins, and they are all the same size in every release
        // seen. Reported rather than asserted -- see CreditsResult::width.
        r.width = dstW;
        r.height = dstH;
    }

    if (r.images == 0)
    {
        r.error = "none of the nineteen credits images (tBMP 302-320) decoded";
        return r;
    }

    r.ok = true;
    return r;
}

} // namespace riven
