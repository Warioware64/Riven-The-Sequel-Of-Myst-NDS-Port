#include "ImageFile.hpp"

#include "global_header.hpp" // <nds.h> for decompress()

#include <cstdio>
#include <cstring>

using namespace rivendata;

namespace rivenrt
{

bool loadRpicImage(const std::string &path, RpicImage &out, std::string &error)
{
    out = RpicImage{};

    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        error = "cannot open " + path;
        return false;
    }

    RpicHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) || !isRpic(hdr))
    {
        std::fclose(f);
        error = path + " is not an RPIC this build reads";
        return false;
    }
    if (hdr.width == 0 || hdr.height == 0 || hdr.width > kMaxRpicW
        || hdr.height > kMaxRpicH)
    {
        std::fclose(f);
        error = "picture's dimensions are not a Riven picture's";
        return false;
    }

    const std::size_t pixels = static_cast<std::size_t>(hdr.width) * hdr.height;
    const std::size_t pixelBytes = pixels * sizeof(Texel);

    out.width = hdr.width;
    out.height = hdr.height;
    out.texels.resize(pixels);

    const auto compression = static_cast<ImageCompression>(hdr.compression);

    if (compression == ImageCompression::None)
    {
        if (hdr.dataBytes != pixelBytes)
        {
            std::fclose(f);
            error = "picture's payload is not the size its header claims";
            return false;
        }
        const std::size_t got = std::fread(out.texels.data(), sizeof(Texel), pixels, f);
        std::fclose(f);
        if (got != pixels)
        {
            error = "picture is truncated";
            return false;
        }
        error.clear();
        return true;
    }

    if (compression != ImageCompression::Lz77)
    {
        std::fclose(f);
        error = "picture uses a compression this build does not know";
        return false;
    }

    std::vector<std::uint8_t> payload(hdr.dataBytes);
    const std::size_t got = std::fread(payload.data(), 1, payload.size(), f);
    std::fclose(f);
    if (got != payload.size() || payload.size() < 4)
    {
        error = "picture is truncated";
        return false;
    }

    // The BIOS LZ77 header: (uncompressedSize << 8) | 0x10. Checked before
    // decompressing, because the BIOS writes as many bytes as it says it will and
    // does not stop at the end of a buffer.
    std::uint32_t bios = 0;
    std::memcpy(&bios, payload.data(), sizeof(bios));
    if ((bios & 0xFF) != 0x10 || (bios >> 8) != pixelBytes)
    {
        error = "picture's compressed payload does not describe its own size";
        return false;
    }
    decompress(payload.data(), out.texels.data(), LZ77);

    error.clear();
    return true;
}

} // namespace rivenrt
