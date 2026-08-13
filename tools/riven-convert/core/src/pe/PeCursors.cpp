#include "riven/PeCursors.hpp"

#include <algorithm>
#include <map>

namespace riven
{
namespace
{
    constexpr std::uint16_t kRtCursor = 1;
    constexpr std::uint16_t kRtGroupCursor = 12;

    /// A bounds-checked cursor over the image, same shape as the installer's.
    /// A PE file is untrusted input here -- it arrives out of a 1997 installer
    /// archive -- so every read is checked and the parse is written straight
    /// through.
    class Cursor
    {
    public:
        explicit Cursor(const std::vector<std::uint8_t> &b) : b_(b) {}

        void seek(std::size_t p) { pos_ = p; }
        std::size_t tell() const { return pos_; }
        void skip(std::size_t n) { pos_ += n; }
        bool bad() const { return bad_; }
        bool has(std::size_t n) const { return pos_ + n <= b_.size(); }

        std::uint8_t u8()
        {
            if (!has(1))
            {
                bad_ = true;
                return 0;
            }
            return b_[pos_++];
        }
        std::uint16_t u16()
        {
            const std::uint32_t a = u8(), c = u8();
            return static_cast<std::uint16_t>(a | (c << 8));
        }
        std::uint32_t u32()
        {
            const std::uint32_t a = u16(), c = u16();
            return a | (c << 16);
        }

    private:
        const std::vector<std::uint8_t> &b_;
        std::size_t pos_ = 0;
        bool bad_ = false;
    };

    struct Section
    {
        std::uint32_t virtualAddress = 0;
        std::uint32_t rawPointer = 0;
        std::uint32_t rawSize = 0;
    };

    /// One leaf of the resource tree: where the bytes are and how many.
    struct ResourceData
    {
        std::uint32_t offset = 0; ///< already converted to a file offset
        std::uint32_t size = 0;
    };

    /// Walk one level of an IMAGE_RESOURCE_DIRECTORY, returning
    /// (id, offsetToData) for every ID entry. Named entries are skipped: Riven
    /// uses none for cursors, and a name is a pointer into a string table this
    /// reader has no reason to follow.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> readDirectory(Cursor &c)
    {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
        c.skip(12); // characteristics, timestamp, major/minor version
        const std::uint16_t named = c.u16();
        const std::uint16_t ids = c.u16();
        c.skip(static_cast<std::size_t>(named) * 8);
        for (std::uint16_t i = 0; i < ids && !c.bad(); ++i)
        {
            const std::uint32_t id = c.u32();
            const std::uint32_t offset = c.u32();
            out.emplace_back(id, offset);
        }
        return out;
    }

    /// The three-level tree is type -> id -> language. Riven's cursors are all
    /// one language (1033), so the first leaf under each id is taken; a release
    /// with per-language cursors would need a preference order, and nothing
    /// suggests one exists.
    std::map<std::uint32_t, ResourceData> collectType(const std::vector<std::uint8_t> &bytes,
                                                      std::size_t resourceBase,
                                                      const Section &rsrc,
                                                      std::uint16_t type)
    {
        std::map<std::uint32_t, ResourceData> out;

        Cursor root(bytes);
        root.seek(resourceBase);
        for (const auto &[typeId, typeOffset] : readDirectory(root))
        {
            if (typeId != type || (typeOffset & 0x80000000u) == 0)
                continue; // not our type, or a leaf where a directory must be

            Cursor idLevel(bytes);
            idLevel.seek(resourceBase + (typeOffset & 0x7FFFFFFFu));
            for (const auto &[resId, resOffset] : readDirectory(idLevel))
            {
                if ((resOffset & 0x80000000u) == 0)
                    continue;

                Cursor langLevel(bytes);
                langLevel.seek(resourceBase + (resOffset & 0x7FFFFFFFu));
                const auto langs = readDirectory(langLevel);
                if (langs.empty() || (langs.front().second & 0x80000000u) != 0)
                    continue;

                Cursor leaf(bytes);
                leaf.seek(resourceBase + langs.front().second);
                const std::uint32_t rva = leaf.u32();
                const std::uint32_t size = leaf.u32();
                if (leaf.bad() || rva < rsrc.virtualAddress)
                    continue;

                ResourceData d;
                d.offset = rva - rsrc.virtualAddress + rsrc.rawPointer;
                d.size = size;
                if (static_cast<std::size_t>(d.offset) + d.size <= bytes.size())
                    out[resId] = d;
            }
        }
        return out;
    }

    /// Decode the colour plane of a cursor into RGB, whatever its depth.
    /// Returns false for a depth this does not handle -- the caller names it.
    bool readPixels(const std::vector<std::uint8_t> &bytes, std::size_t pixelStart,
                    std::size_t paletteStart, int paletteEntries, int bitCount, int width,
                    int height, std::vector<std::uint8_t> &rgb)
    {
        // Rows are padded to a 4-byte boundary, and stored bottom-up.
        const std::size_t stride =
            ((static_cast<std::size_t>(width) * bitCount + 31) / 32) * 4;
        if (pixelStart + stride * static_cast<std::size_t>(height) > bytes.size())
            return false;

        rgb.assign(static_cast<std::size_t>(width) * height * 3, 0);

        const auto paletteRgb = [&](int index, std::uint8_t *dst) {
            // Palette entries are BGRA quads.
            const std::size_t at = paletteStart + static_cast<std::size_t>(index) * 4;
            if (index >= paletteEntries || at + 3 > bytes.size())
            {
                dst[0] = dst[1] = dst[2] = 0;
                return;
            }
            dst[0] = bytes[at + 2];
            dst[1] = bytes[at + 1];
            dst[2] = bytes[at + 0];
        };

        for (int y = 0; y < height; ++y)
        {
            const std::size_t row = pixelStart + stride * static_cast<std::size_t>(height - 1 - y);
            std::uint8_t *dst = rgb.data() + static_cast<std::size_t>(y) * width * 3;

            for (int x = 0; x < width; ++x, dst += 3)
            {
                switch (bitCount)
                {
                case 1:
                {
                    const std::uint8_t byte = bytes[row + static_cast<std::size_t>(x) / 8];
                    paletteRgb((byte >> (7 - (x & 7))) & 1, dst);
                    break;
                }
                case 4:
                {
                    const std::uint8_t byte = bytes[row + static_cast<std::size_t>(x) / 2];
                    paletteRgb((x & 1) ? (byte & 0x0F) : (byte >> 4), dst);
                    break;
                }
                case 8:
                    paletteRgb(bytes[row + static_cast<std::size_t>(x)], dst);
                    break;
                case 24:
                {
                    const std::size_t at = row + static_cast<std::size_t>(x) * 3;
                    dst[0] = bytes[at + 2];
                    dst[1] = bytes[at + 1];
                    dst[2] = bytes[at + 0];
                    break;
                }
                case 32:
                {
                    const std::size_t at = row + static_cast<std::size_t>(x) * 4;
                    dst[0] = bytes[at + 2];
                    dst[1] = bytes[at + 1];
                    dst[2] = bytes[at + 0];
                    break;
                }
                default:
                    return false;
                }
            }
        }
        return true;
    }
} // namespace

std::vector<PeCursor> readPeCursors(const std::vector<std::uint8_t> &bytes,
                                    std::vector<std::string> &warnings)
{
    std::vector<PeCursor> out;

    Cursor c(bytes);
    if (bytes.size() < 0x40 || bytes[0] != 'M' || bytes[1] != 'Z')
    {
        warnings.emplace_back("the executable is not a Windows PE image");
        return out;
    }

    c.seek(0x3C);
    const std::uint32_t peOffset = c.u32();
    c.seek(peOffset);
    if (c.u32() != 0x00004550u) // "PE\0\0"
    {
        warnings.emplace_back("the executable has no PE header");
        return out;
    }

    c.skip(2); // machine
    const std::uint16_t sectionCount = c.u16();
    c.skip(12); // timestamp, symbol table pointer, symbol count
    const std::uint16_t optionalSize = c.u16();
    c.skip(2); // characteristics

    // The section table follows the optional header, wherever it ends.
    c.seek(peOffset + 24 + optionalSize);
    Section rsrc;
    bool haveRsrc = false;
    for (std::uint16_t i = 0; i < sectionCount && !c.bad(); ++i)
    {
        char name[9] = {};
        for (int j = 0; j < 8; ++j)
            name[j] = static_cast<char>(c.u8());
        c.skip(4); // virtual size
        const std::uint32_t va = c.u32();
        const std::uint32_t rawSize = c.u32();
        const std::uint32_t rawPtr = c.u32();
        c.skip(16); // relocations, line numbers, characteristics

        if (std::string(name) == ".rsrc")
        {
            rsrc.virtualAddress = va;
            rsrc.rawPointer = rawPtr;
            rsrc.rawSize = rawSize;
            haveRsrc = true;
        }
    }
    if (!haveRsrc || c.bad())
    {
        warnings.emplace_back("the executable has no resource section");
        return out;
    }

    const std::size_t resourceBase = rsrc.rawPointer;
    const auto groups = collectType(bytes, resourceBase, rsrc, kRtGroupCursor);
    const auto images = collectType(bytes, resourceBase, rsrc, kRtCursor);
    if (groups.empty())
    {
        warnings.emplace_back("the executable has no cursor resources");
        return out;
    }

    for (const auto &[groupId, group] : groups)
    {
        Cursor g(bytes);
        g.seek(group.offset);
        g.skip(2); // reserved
        const std::uint16_t kind = g.u16();
        const std::uint16_t count = g.u16();
        if (g.bad() || kind != 2 || count == 0)
            continue; // kind 2 is "cursor"; 1 would be an icon group

        // A group can list several sizes. Riven's list one each, and if one ever
        // listed more, the first is the one its own manager would take.
        g.skip(8); // width, height, planes, bitCount -- all restated in the image
        g.skip(4); // bytesInRes
        const std::uint16_t imageId = g.u16();
        if (g.bad())
            continue;

        const auto it = images.find(imageId);
        if (it == images.end())
        {
            warnings.push_back("cursor " + std::to_string(groupId)
                               + ": its image resource is missing");
            continue;
        }

        Cursor im(bytes);
        im.seek(it->second.offset);
        const std::uint16_t hotX = im.u16();
        const std::uint16_t hotY = im.u16();

        // BITMAPINFOHEADER
        const std::uint32_t headerSize = im.u32();
        const std::int32_t bmWidth = static_cast<std::int32_t>(im.u32());
        const std::int32_t bmHeight = static_cast<std::int32_t>(im.u32());
        im.skip(2); // planes
        const std::uint16_t bitCount = im.u16();
        const std::uint32_t compression = im.u32();
        im.skip(12); // image size, x/y pixels per metre
        const std::uint32_t coloursUsed = im.u32();
        im.skip(4); // colours important
        if (im.bad() || headerSize < 40 || bmWidth <= 0 || bmHeight <= 0)
        {
            warnings.push_back("cursor " + std::to_string(groupId)
                               + ": its bitmap header does not describe a picture");
            continue;
        }
        if (compression != 0)
        {
            warnings.push_back("cursor " + std::to_string(groupId)
                               + ": compressed bitmaps are not read by this converter");
            continue;
        }

        const int width = bmWidth;
        const int height = bmHeight / 2; // the AND mask is stacked underneath
        if (height <= 0)
        {
            warnings.push_back("cursor " + std::to_string(groupId) + ": it has no mask");
            continue;
        }

        int paletteEntries = 0;
        if (bitCount <= 8)
            paletteEntries = coloursUsed != 0 ? static_cast<int>(coloursUsed) : (1 << bitCount);

        const std::size_t paletteStart = it->second.offset + 4 + headerSize;
        const std::size_t pixelStart =
            paletteStart + static_cast<std::size_t>(paletteEntries) * 4;

        PeCursor cur;
        cur.groupId = static_cast<std::uint16_t>(groupId);
        cur.width = width;
        cur.height = height;
        cur.hotX = hotX;
        cur.hotY = hotY;
        cur.bitCount = bitCount;

        if (!readPixels(bytes, pixelStart, paletteStart, paletteEntries, bitCount, width,
                        height, cur.rgb))
        {
            warnings.push_back("cursor " + std::to_string(groupId) + ": "
                               + std::to_string(bitCount)
                               + " bits per pixel is not a depth this converter reads");
            continue;
        }

        // The AND mask: 1 bit per pixel, rows padded to 4 bytes, bottom-up like
        // the colour plane. A set bit means transparent.
        const std::size_t colourStride =
            ((static_cast<std::size_t>(width) * bitCount + 31) / 32) * 4;
        const std::size_t maskStart =
            pixelStart + colourStride * static_cast<std::size_t>(height);
        const std::size_t maskStride = ((static_cast<std::size_t>(width) + 31) / 32) * 4;

        cur.opaque.assign(static_cast<std::size_t>(width) * height, 255);
        if (maskStart + maskStride * static_cast<std::size_t>(height) <= bytes.size())
        {
            for (int y = 0; y < height; ++y)
            {
                const std::size_t row =
                    maskStart + maskStride * static_cast<std::size_t>(height - 1 - y);
                for (int x = 0; x < width; ++x)
                {
                    const std::uint8_t byte = bytes[row + static_cast<std::size_t>(x) / 8];
                    const bool transparent = ((byte >> (7 - (x & 7))) & 1) != 0;
                    cur.opaque[static_cast<std::size_t>(y) * width + x] = transparent ? 0 : 255;
                }
            }
        }
        else
        {
            warnings.push_back("cursor " + std::to_string(groupId)
                               + ": its mask is truncated; treated as fully opaque");
        }

        out.push_back(std::move(cur));
    }

    std::sort(out.begin(), out.end(),
              [](const PeCursor &a, const PeCursor &b) { return a.groupId < b.groupId; });
    return out;
}

} // namespace riven
