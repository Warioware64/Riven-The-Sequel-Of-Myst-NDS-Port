#include "ImageFile.hpp"

#include "global_header.hpp" // <nds.h> for decompress()

#include <cstdio>
#include <cstring>

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Read the rest of an open file and expand it into `dst`, which must
    /// already be `expected` bytes long.
    ///
    /// The BIOS check is the point of this. decompress() writes as many bytes as
    /// the payload's 4-byte header says it will and does not stop at the end of
    /// a buffer, so a file whose header disagrees with the size the caller
    /// allocated has to be rejected BEFORE it is handed over -- otherwise a
    /// truncated download is not a failed load, it is memory corruption.
    bool readPayload(std::FILE *f, ImageCompression compression, std::uint32_t dataBytes,
                     void *dst, std::size_t expected, std::string &error)
    {
        if (compression == ImageCompression::None)
        {
            if (dataBytes != expected)
            {
                error = "picture's payload is not the size its header claims";
                return false;
            }
            if (std::fread(dst, 1, expected, f) != expected)
            {
                error = "picture is truncated";
                return false;
            }
            return true;
        }

        if (compression != ImageCompression::Lz77)
        {
            error = "picture uses a compression this build does not know";
            return false;
        }

        std::vector<std::uint8_t> payload(dataBytes);
        if (std::fread(payload.data(), 1, payload.size(), f) != payload.size()
            || payload.size() < 4)
        {
            error = "picture is truncated";
            return false;
        }

        // The BIOS LZ77 header: (uncompressedSize << 8) | 0x10.
        std::uint32_t bios = 0;
        std::memcpy(&bios, payload.data(), sizeof(bios));
        if ((bios & 0xFF) != 0x10 || (bios >> 8) != expected)
        {
            error = "picture's compressed payload does not describe its own size";
            return false;
        }
        decompress(payload.data(), dst, LZ77);
        return true;
    }
} // namespace

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
    // Only that it is not zero, which would place the picture nowhere at all.
    // There is no upper bound on purpose -- see the note by kMaxRpicW.
    if (hdr.srcWidth == 0 || hdr.srcHeight == 0)
    {
        std::fclose(f);
        error = "picture does not say how big it is on the card";
        return false;
    }

    const std::size_t pixels = static_cast<std::size_t>(hdr.width) * hdr.height;
    const std::size_t pixelBytes = pixels * sizeof(Texel);

    out.width = hdr.width;
    out.height = hdr.height;
    out.srcWidth = hdr.srcWidth;
    out.srcHeight = hdr.srcHeight;
    out.texels.resize(pixels);

    const bool ok =
        readPayload(f, static_cast<ImageCompression>(hdr.compression), hdr.dataBytes,
                    out.texels.data(), pixelBytes, error);
    std::fclose(f);
    if (ok)
        error.clear();
    return ok;
}

bool loadRpizImage(const std::string &path, RpizImage &out, std::string &error)
{
    out = RpizImage{};

    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        error = "cannot open " + path;
        return false;
    }

    RpizHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr) || !isRpiz(hdr))
    {
        std::fclose(f);
        error = path + " is not an RPIZ this build reads";
        return false;
    }
    if (hdr.width == 0 || hdr.height == 0 || hdr.width > kMaxRpizW
        || hdr.height > kMaxRpizH)
    {
        std::fclose(f);
        error = "picture's dimensions are not a Riven picture's";
        return false;
    }

    // The palette comes before the indices and is a fixed 512 bytes, so it is
    // read straight into place: the file holds RGB555 halfwords with bit 15 set,
    // which is what a Texel is.
    if (std::fread(out.palette, 1, kPaletteBytes, f) != kPaletteBytes)
    {
        std::fclose(f);
        error = "picture has no palette";
        return false;
    }

    const std::size_t plane = static_cast<std::size_t>(hdr.width) * hdr.height;
    out.width = hdr.width;
    out.height = hdr.height;
    out.indices.resize(plane);

    const bool ok = readPayload(f, static_cast<ImageCompression>(hdr.compression),
                                hdr.dataBytes, out.indices.data(), plane, error);
    std::fclose(f);
    if (ok)
        error.clear();
    return ok;
}

} // namespace rivenrt
