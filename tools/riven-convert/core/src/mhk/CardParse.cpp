#include "riven/CardParse.hpp"

namespace riven
{
namespace
{
    using namespace rivendata;

    // Riven's deepest shipped switch nesting is 2 or 3. This guard exists so a
    // corrupt resource cannot drive the parser into unbounded recursion --
    // damaged archives are the norm, not the exception, with this game.
    constexpr int kMaxScriptDepth = 16;

    // No shipped handler comes close to this. It bounds the damage a bad count
    // field can do before the reader's own bounds checks catch up.
    constexpr std::size_t kMaxCommandsPerScript = 4096;
    constexpr std::size_t kMaxBranches = 256;

    std::vector<Command> parseCommands(ResourceReader &r, int depth);

    /// One command. Returns false when the stream ran out or the encoding was
    /// not something we can step over safely -- the caller then stops reading
    /// this script rather than guessing where the next command starts.
    bool parseCommand(ResourceReader &r, int depth, Command &out)
    {
        out = Command{};
        out.opcode = r.u16();
        if (!r.ok())
            return false;

        // Opcode 8 is the only encoding that is not "argc then argc uint16s".
        // riven_scripts.cpp:843-867. Its argc is always 2 (the variable and the
        // branch count), and each branch carries a nested script.
        if (out.opcode == static_cast<std::uint16_t>(Op::Switch))
        {
            const std::uint16_t argc = r.u16();
            if (!r.ok() || argc != 2)
                return false;

            out.switchVar = r.u16();
            const std::uint16_t branchCount = r.u16();
            if (!r.ok() || branchCount > kMaxBranches)
                return false;

            if (depth >= kMaxScriptDepth)
                return false;

            out.branches.reserve(branchCount);
            for (std::uint16_t i = 0; i < branchCount; ++i)
            {
                SwitchBranch b;
                b.value = r.u16();
                if (!r.ok())
                    return false;
                b.commands = parseCommands(r, depth + 1);
                out.branches.push_back(std::move(b));
            }
            return true;
        }

        // Everything else, INCLUDING opcode 27 (changeStack). ScummVM gives 27
        // its own reader because it interprets the payload as a uint16 stack id
        // plus a uint32 card id, but on the wire that is exactly three uint16
        // arguments, so the generic path steps over it correctly. The schema's
        // changeStackNameId/changeStackGlobalCardId accessors recombine them.
        const std::uint16_t argc = r.u16();
        if (!r.ok())
            return false;
        if (r.remaining() < static_cast<std::size_t>(argc) * 2)
            return false;

        out.args.reserve(argc);
        for (std::uint16_t i = 0; i < argc; ++i)
            out.args.push_back(r.u16());
        return r.ok();
    }

    std::vector<Command> parseCommands(ResourceReader &r, int depth)
    {
        std::vector<Command> commands;

        const std::uint16_t count = r.u16();
        if (!r.ok() || count > kMaxCommandsPerScript)
            return commands;

        commands.reserve(count);
        for (std::uint16_t i = 0; i < count; ++i)
        {
            Command c;
            if (!parseCommand(r, depth, c))
                break; // keep what parsed; the caller reports the shortfall
            commands.push_back(std::move(c));
        }
        return commands;
    }
} // namespace

std::vector<rivendata::Handler> parseScriptList(ResourceReader &r)
{
    std::vector<Handler> handlers;

    const std::uint16_t count = r.u16();
    if (!r.ok() || count > kMaxCommandsPerScript)
        return handlers;

    handlers.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        Handler h;
        h.event = r.u16();
        if (!r.ok())
            break;
        h.commands = parseCommands(r, 0);
        handlers.push_back(std::move(h));
    }
    return handlers;
}

bool parseCard(ResourceReader &r, rivendata::Card &out)
{
    out.nameIndex = r.i16();
    out.zipModePlace = r.u16();
    if (!r.ok())
        return false;
    out.scripts = parseScriptList(r);
    return true;
}

std::vector<rivendata::PictureRec> parsePlst(ResourceReader &r)
{
    std::vector<PictureRec> out;

    const std::uint16_t count = r.u16();
    if (!r.ok() || r.remaining() < static_cast<std::size_t>(count) * 12)
        return out;

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        PictureRec p;
        p.index = r.u16();
        p.id = r.u16();
        p.rect.left = r.i16();
        p.rect.top = r.i16();
        p.rect.right = r.i16();
        p.rect.bottom = r.i16();
        out.push_back(p);
    }
    return out;
}

std::vector<rivendata::BlstRec> parseBlst(ResourceReader &r)
{
    std::vector<BlstRec> out;

    const std::uint16_t count = r.u16();
    if (!r.ok() || r.remaining() < static_cast<std::size_t>(count) * 6)
        return out;

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        BlstRec b;
        b.index = r.u16();
        b.enabled = r.u16();
        b.hotspotId = r.u16();
        out.push_back(b);
    }
    return out;
}

std::vector<rivendata::Hotspot> parseHspt(ResourceReader &r)
{
    std::vector<Hotspot> out;

    const std::uint16_t count = r.u16();
    if (!r.ok())
        return out;
    // Only the fixed part can be bounds-checked up front; each record is
    // followed by a variable-length script list.
    if (r.remaining() < static_cast<std::size_t>(count) * 22)
        return out;

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        Hotspot h;
        h.blstId = r.u16();
        h.nameRes = r.i16();
        // Riven ships records whose rect is nonsense (riven_card.cpp notes
        // tspit 371/377 hotspot 4). They are kept verbatim: the runtime decides
        // what to do with them, and silently repairing data here would hide a
        // difference from the original.
        h.rect.left = r.i16();
        h.rect.top = r.i16();
        h.rect.right = r.i16();
        h.rect.bottom = r.i16();
        h.u0 = r.u16();
        h.cursor = r.u16();
        h.index = r.u16();
        h.transitionOffset = r.i16();
        h.flags = r.u16();
        if (!r.ok())
            break;

        // ScummVM initialises _flags to kFlagEnabled and then ORs in the file
        // value (riven_card.cpp:1263, :1288) -- a hotspot is enabled unless a
        // BLST later disables it.
        h.flags |= kHotspotEnabled;

        h.scripts = parseScriptList(r);
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<rivendata::SoundRec> parseSlst(ResourceReader &r)
{
    std::vector<SoundRec> out;

    const std::uint16_t count = r.u16();
    if (!r.ok())
        return out;

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        SoundRec s;
        s.index = r.u16();
        const std::uint16_t soundCount = r.u16();
        if (!r.ok())
            break;

        // index + soundCount already read; the rest of this record is
        // soundCount ids, five scalars, then three more soundCount-long arrays.
        const std::size_t need = static_cast<std::size_t>(soundCount) * 2 * 4 + 5 * 2;
        if (r.remaining() < need)
            break;

        s.soundIds.reserve(soundCount);
        for (std::uint16_t j = 0; j < soundCount; ++j)
            s.soundIds.push_back(r.u16());

        s.fadeFlags = r.u16();
        s.loop = r.u16();
        s.globalVolume = r.u16();
        s.u0 = r.u16();
        s.suspend = r.u16();

        s.volumes.reserve(soundCount);
        for (std::uint16_t j = 0; j < soundCount; ++j)
            s.volumes.push_back(r.u16());

        s.balances.reserve(soundCount);
        for (std::uint16_t j = 0; j < soundCount; ++j)
            s.balances.push_back(r.i16());

        s.u2.reserve(soundCount);
        for (std::uint16_t j = 0; j < soundCount; ++j)
            s.u2.push_back(r.u16());

        if (!r.ok())
            break;
        out.push_back(std::move(s));
    }
    return out;
}

std::vector<std::uint32_t> parseRmap(ResourceReader &r)
{
    std::vector<std::uint32_t> out;
    const std::size_t count = r.remaining() / 4;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        out.push_back(r.u32());
    return out;
}

rivendata::NameList parseName(ResourceReader &r)
{
    NameList list;

    const std::uint16_t count = r.u16();
    if (!r.ok() || count == 0)
        return list;
    if (r.remaining() < static_cast<std::size_t>(count) * 4)
        return list;

    std::vector<std::uint16_t> offsets(count);
    for (std::uint16_t i = 0; i < count; ++i)
        offsets[i] = r.u16();

    list.sortedIndex.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
        list.sortedIndex.push_back(r.u16());

    if (!r.ok())
        return NameList{};

    // Strings are addressed relative to the end of both tables, not the start
    // of the resource (riven_stack.cpp:444-474).
    const std::size_t stringsBase = r.pos();

    list.names.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        const std::size_t at = stringsBase + offsets[i];
        std::string s;
        ResourceReader sr = r;
        sr.seek(at);
        while (sr.ok() && !sr.atEnd())
        {
            const std::uint8_t c = sr.u8();
            if (c == 0)
                break;
            s.push_back(static_cast<char>(c));
        }
        list.names.push_back(std::move(s));
    }
    return list;
}

} // namespace riven
