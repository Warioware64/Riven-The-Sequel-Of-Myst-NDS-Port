#include "StackFile.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Must match the converter's kYasFlags exactly (Converter.cpp:31). yas's own
    /// header is turned off because the file carries StackFileHeader instead, and
    /// mem|binary is the pairing the host wrote with.
    constexpr std::size_t kYasFlags = yas::mem | yas::binary | yas::no_header;
} // namespace

bool loadStackFile(const std::string &path, Stack &out, std::string &error)
{
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
    {
        error = "cannot open " + path;
        return false;
    }

    StackFileHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr))
    {
        std::fclose(f);
        error = "stack file is shorter than its header";
        return false;
    }
    if (!headerLooksValid(hdr))
    {
        std::fclose(f);
        // headerLooksValid checks the schema version too, which is the whole
        // point of having one: a converter and a ROM built from different
        // revisions must fail here rather than deserialize nonsense.
        error = "stack file is not this build's schema";
        return false;
    }
    if (hdr.payloadBytes == 0)
    {
        std::fclose(f);
        error = "stack file has no payload";
        return false;
    }

    std::vector<std::uint8_t> payload(hdr.payloadBytes);
    const std::size_t got = std::fread(payload.data(), 1, payload.size(), f);
    std::fclose(f);
    if (got != payload.size())
    {
        error = "stack file is truncated";
        return false;
    }

    // yas throws on a malformed archive, and the ARM9 is built -fno-exceptions,
    // so a bad payload has to be caught by the checks above rather than here.
    // That is what the magic, the version and payloadBytes are for.
    yas::mem_istream is(payload.data(), payload.size());
    yas::binary_iarchive<yas::mem_istream, kYasFlags> ia(is);
    ia &out;

    if (out.id != static_cast<StackId>(hdr.stackId))
    {
        error = "stack file's contents do not match its header";
        return false;
    }
    if (out.cards.size() != hdr.cardCount)
    {
        error = "stack file's card count does not match its header";
        return false;
    }

    error.clear();
    return true;
}

} // namespace rivenrt
