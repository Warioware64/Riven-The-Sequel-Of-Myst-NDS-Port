#include "riven/MovieList.hpp"

namespace riven
{

std::vector<rivendata::MovieRec> parseMlst(ResourceReader &r)
{
    std::vector<rivendata::MovieRec> out;

    const std::uint16_t count = r.u16();
    if (!r.ok())
        return out;

    // 22 bytes per record. Checking up front turns a corrupt count into an
    // empty list rather than 65535 zero-filled records.
    if (r.remaining() < static_cast<std::size_t>(count) * 22)
        return out;

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        rivendata::MovieRec m;
        m.index = r.u16();
        m.movieId = r.u16();
        m.slot = r.u16();
        m.left = r.i16();
        m.top = r.i16();
        m.lowBoundTime = r.u16();
        m.startTime = r.u16();
        m.highBoundTime = r.u16();
        m.loop = r.u16();
        m.volume = r.u16();
        m.rate = r.u16();
        out.push_back(m);
    }
    return out;
}

std::vector<rivendata::FlstRec> parseFlst(ResourceReader &r)
{
    std::vector<rivendata::FlstRec> out;

    const std::uint16_t count = r.u16();
    if (!r.ok())
        return out;

    if (r.remaining() < static_cast<std::size_t>(count) * 6)
        return out;

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        rivendata::FlstRec f;
        f.index = r.u16();
        f.sfxeId = r.u16();
        f.u0 = r.u16();
        out.push_back(f);
    }
    return out;
}

} // namespace riven
