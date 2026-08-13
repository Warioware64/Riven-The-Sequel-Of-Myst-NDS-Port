#include "RcurFile.hpp"

#include <cstdio>
#include <cstring>

using namespace rivendata;

namespace rivenrt
{

bool RcurFile::load(const std::string &path, std::string &error)
{
    unload();

    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        error = "no sprite set at " + path;
        return false;
    }

    if (std::fread(&header_, 1, sizeof(header_), f) != sizeof(header_)
        || !isRcur(header_))
    {
        std::fclose(f);
        unload();
        error = path + " is not an RCUR this build reads";
        return false;
    }
    if (header_.celWidth == 0 || header_.celHeight == 0 || header_.celCount == 0
        || (header_.celWidth % 8) != 0 || (header_.celHeight % 8) != 0)
    {
        std::fclose(f);
        unload();
        error = "the sprite set's cels are not a usable size";
        return false;
    }
    if (header_.dataBytes
        != rcurCelBytes(header_.celWidth, header_.celHeight) * header_.celCount)
    {
        std::fclose(f);
        unload();
        error = "the sprite set's size does not match its header";
        return false;
    }

    palette_.resize(header_.paletteCount);
    cels_.resize(header_.celCount);
    pixels_.resize(header_.dataBytes);

    const bool ok =
        (palette_.empty()
         || std::fread(palette_.data(), sizeof(std::uint16_t), palette_.size(), f)
                == palette_.size())
        && std::fread(cels_.data(), sizeof(RcurCel), cels_.size(), f) == cels_.size()
        && std::fread(pixels_.data(), 1, pixels_.size(), f) == pixels_.size();
    std::fclose(f);

    if (!ok)
    {
        unload();
        error = path + " is truncated";
        return false;
    }

    error.clear();
    return true;
}

void RcurFile::unload()
{
    header_ = RcurHeader{};
    palette_.clear();
    cels_.clear();
    pixels_.clear();
}

const RcurCel *RcurFile::find(std::uint16_t id) const
{
    // Linear over at most nineteen entries, called when the shape changes rather
    // than per frame. An index would cost more than it saves.
    for (const RcurCel &c : cels_)
        if (c.id == id)
            return &c;
    return nullptr;
}

const std::uint8_t *RcurFile::pixels(const RcurCel &cel) const
{
    const std::size_t index = static_cast<std::size_t>(&cel - cels_.data());
    if (index >= cels_.size())
        return nullptr;
    return pixels_.data() + index * celBytes();
}

} // namespace rivenrt
