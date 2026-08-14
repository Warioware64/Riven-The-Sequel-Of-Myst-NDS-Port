#include "SaveGame.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include <yas/mem_streams.hpp>
#include <yas/binary_iarchive.hpp>
#include <yas/binary_oarchive.hpp>
#include <yas/std_types.hpp>

using namespace rivendata;

namespace rivenrt
{
namespace SaveGame
{
namespace
{
    /// The same pairing StackFile.cpp uses, and for the same reason: the file
    /// carries its own header, so yas's is turned off.
    constexpr std::size_t kYasFlags = yas::mem | yas::binary | yas::no_header;

    constexpr std::uint32_t kMagic = 0x56415352u; ///< 'RSAV' little-endian
    constexpr std::size_t kHeaderBytes = 4 + 2 + 1 + 2 + 8 + 4 + 4;

    /// Biggest payload worth believing. A save is a few hundred variables and
    /// twenty-two zip destinations -- a couple of kilobytes -- so this is three
    /// orders of magnitude of headroom and still small enough that a garbage
    /// length cannot make the reader allocate its way out of memory before any
    /// of the other checks get a look at it.
    constexpr std::uint32_t kMaxPayload = 1u << 20;

    /// CRC-32 (the ordinary reflected polynomial, 0xEDB88320), computed a
    /// nibble at a time against a 16-entry table.
    ///
    /// Nibbles rather than the usual 256-entry byte table because this runs
    /// twice per save on a machine with 4 MB: 64 bytes of table against 1 KB,
    /// for double the passes over a payload that is measured in kilobytes and
    /// is about to be handed to an SD card anyway.
    std::uint32_t crc32(const std::uint8_t *data, std::size_t bytes)
    {
        static constexpr std::uint32_t kTable[16] = {
            0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u,
            0x4DB26158u, 0x5005713Cu, 0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
            0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
        std::uint32_t crc = 0xFFFFFFFFu;
        for (std::size_t i = 0; i < bytes; ++i)
        {
            crc ^= data[i];
            crc = (crc >> 4) ^ kTable[crc & 0x0F];
            crc = (crc >> 4) ^ kTable[crc & 0x0F];
        }
        return ~crc;
    }

    /// Where slotPath() builds from. Empty until setDirectory, which is what
    /// makes every function here a no-op on a machine with no card.
    std::string g_dir;

    // Field order is written and read explicitly, by hand, in little-endian.
    // No packed struct and no blitting: the header is the one part of this file
    // that a future build has to be able to read no matter what its compiler
    // decided about padding.
    void put16(std::uint8_t *p, std::uint16_t v)
    {
        p[0] = static_cast<std::uint8_t>(v);
        p[1] = static_cast<std::uint8_t>(v >> 8);
    }
    void put32(std::uint8_t *p, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
            p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void put64(std::uint8_t *p, std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
            p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    std::uint16_t get16(const std::uint8_t *p)
    {
        return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
    }
    std::uint32_t get32(const std::uint8_t *p)
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<std::uint32_t>(p[i]) << (8 * i);
        return v;
    }
    std::uint64_t get64(const std::uint8_t *p)
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
        return v;
    }

    /// Does the header name somewhere a game could actually be?
    ///
    /// The only content check available -- the payload behind it carries no
    /// length and no checksum -- but it is a real one: card 0 is the "no card"
    /// sentinel and StackId::None the "no stack" one, so either means the bytes
    /// are not a save whatever else they are.
    bool plausible(std::uint8_t stackId, std::uint16_t cardId)
    {
        if (cardId == 0)
            return false;
        return stackId > static_cast<std::uint8_t>(StackId::None)
               && stackId <= static_cast<std::uint8_t>(StackId::Aspit);
    }

    /// Read at most `max` bytes of a file. Returns false only when the file will
    /// not open at all; a short file is a successful read of fewer bytes, which
    /// is what lets readSlotInfo tell "truncated" from "absent".
    bool readAtMost(const std::string &path, std::size_t max, std::vector<std::uint8_t> &out)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return false;
        out.resize(max);
        const std::size_t got = std::fread(out.data(), 1, max, f);
        std::fclose(f);
        out.resize(got);
        return true;
    }
} // namespace

void setDirectory(const std::string &dir)
{
    g_dir = dir;
}

bool validSlot(int slot)
{
    return slot >= 1 && slot <= kSlotCount;
}

std::string slotPath(int slot)
{
    if (g_dir.empty() || !validSlot(slot))
        return std::string();
    return g_dir + "slot" + std::to_string(slot) + ".sav";
}

SlotInfo readSlotInfo(int slot)
{
    SlotInfo info;
    const std::string path = slotPath(slot);
    if (path.empty())
        return info;

    std::vector<std::uint8_t> head;
    if (!readAtMost(path, kHeaderBytes, head))
        return info; // no file: neither used nor damaged, which is "empty"

    // From here a file exists, so every failure below is DAMAGED and not empty.
    // That distinction is the whole reason this function has two flags.
    if (head.size() != kHeaderBytes || get32(head.data()) != kMagic
        || get16(head.data() + 4) != kVersion)
    {
        info.damaged = true;
        return info;
    }

    const std::uint8_t stackId = head[6];
    const std::uint16_t cardId = get16(head.data() + 7);
    if (!plausible(stackId, cardId))
    {
        info.damaged = true;
        return info;
    }

    info.used = true;
    info.stackId = stackId;
    info.cardId = cardId;
    info.when = get64(head.data() + 9);
    return info;
}

std::string slotLabel(int slot, const SlotInfo &info)
{
    std::string row = std::to_string(slot) + ".  ";
    if (info.damaged)
        return row + "- damaged -";
    if (!info.used)
        return row + "- empty -";

    row += displayName(static_cast<StackId>(info.stackId));

    // No clock, no date. The DS RTC can be unset, and an unset one would print
    // as a day in 1970 -- which reads as a real date and would be believed.
    if (info.when != 0)
    {
        const std::time_t t = static_cast<std::time_t>(info.when);
        if (const std::tm *lt = std::localtime(&t))
        {
            char stamp[32];
            std::snprintf(stamp, sizeof(stamp), "  %02d/%02d %02d:%02d", lt->tm_mday,
                          lt->tm_mon + 1, lt->tm_hour, lt->tm_min);
            row += stamp;
        }
    }
    return row;
}

bool writeSlot(int slot, const SaveState &state)
{
    const std::string path = slotPath(slot);
    if (path.empty())
        return false;
    if (!plausible(state.stackId, state.cardId))
        return false; // refuse to write a save that readSlot would refuse to read

    // Serialized to memory first, because the file starts with a raw header and
    // a yas output stream owns the whole of what it writes to.
    yas::mem_ostream os;
    yas::binary_oarchive<yas::mem_ostream, kYasFlags> oa(os);
    oa &state;
    const yas::intrusive_buffer payload = os.get_intrusive_buffer();
    if (payload.data == nullptr || payload.size == 0)
        return false;

    if (payload.size > kMaxPayload)
        return false; // readSlot would refuse it, so do not write it

    const std::uint8_t *bytes = reinterpret_cast<const std::uint8_t *>(payload.data);
    std::uint8_t head[kHeaderBytes] = {};
    put32(head, kMagic);
    put16(head + 4, kVersion);
    head[6] = state.stackId;
    put16(head + 7, state.cardId);
    put64(head + 9, static_cast<std::uint64_t>(std::time(nullptr)));
    put32(head + 17, static_cast<std::uint32_t>(payload.size));
    put32(head + 21, crc32(bytes, payload.size));

    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    bool ok = std::fwrite(head, 1, sizeof(head), f) == sizeof(head)
              && std::fwrite(payload.data, 1, payload.size, f) == payload.size;
    // The close is part of the write, not cleanup after it. fwrite only fills a
    // buffer; on a full or pulled card it is fclose that reports the flush
    // failing, and ignoring it would return success over a file missing its
    // tail -- exactly the half-save the cleanup below exists to prevent.
    if (std::fclose(f) != 0)
        ok = false;

    if (!ok)
    {
        // A full card leaves a header with half a payload behind it, which
        // would list as a usable save and fail on load. Never leave half a save.
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

bool readSlot(int slot, SaveState &state)
{
    const std::string path = slotPath(slot);
    if (path.empty())
        return false;

    // EVERY CHECK BELOW HAPPENS BEFORE yas::load, and that ordering is the whole
    // point. yas trusts the counts inside its own payload absolutely: a
    // truncated or corrupted one makes it read past the end of the buffer and
    // throw, which on an ARM9 built -fno-exceptions is a dead console rather
    // than a refused load. Nothing may be deserialized until the bytes have been
    // proved to be the bytes that were written.
    std::vector<std::uint8_t> blob;
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return false;
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        // 1. A file that is all header and no payload is not a save. Checked
        //    before the allocation so a negative or absurd ftell cannot size it.
        if (size <= static_cast<long>(kHeaderBytes)
            || size > static_cast<long>(kHeaderBytes + kMaxPayload))
        {
            std::fclose(f);
            return false;
        }
        blob.resize(static_cast<std::size_t>(size));
        const std::size_t got = std::fread(blob.data(), 1, blob.size(), f);
        std::fclose(f);
        if (got != blob.size())
            return false;
    }

    // 2. Magic and version. yas was told no_header, so this is the only thing
    //    keeping an older layout from being read into these fields.
    if (get32(blob.data()) != kMagic || get16(blob.data() + 4) != kVersion)
        return false;

    const std::uint8_t headStack = blob[6];
    const std::uint16_t headCard = get16(blob.data() + 7);
    // 3. The header names nowhere.
    if (!plausible(headStack, headCard))
        return false;

    // 4. The payload is exactly as long as the header says. This is what catches
    //    a card pulled mid-write, which is the common way a save goes bad.
    const std::size_t have = blob.size() - kHeaderBytes;
    if (get32(blob.data() + 17) != have)
        return false;

    // 5. And it is byte-for-byte what was written. A short file is caught above;
    //    this catches the rest -- a bad sector, a card yanked and re-seated, a
    //    file edited on a PC -- and it is the check that makes "damaged" a
    //    verdict this reader can actually reach instead of a crash.
    const std::uint8_t *payload = blob.data() + kHeaderBytes;
    if (get32(blob.data() + 21) != crc32(payload, have))
        return false;

    state = SaveState{};
    yas::mem_istream is(payload, have);
    yas::binary_iarchive<yas::mem_istream, kYasFlags> ia(is);
    ia &state;

    // 6. Last, check what came out agrees with what the header advertised.
    //    Cheap, and it is the reason stackId and cardId are stored twice: it
    //    catches a payload that is intact and self-consistent but belongs to a
    //    different save than the header in front of it.
    if (state.stackId != headStack || state.cardId != headCard)
        return false;

    return true;
}

bool removeSlot(int slot)
{
    const std::string path = slotPath(slot);
    if (path.empty())
        return false;
    std::error_code ec;
    return std::filesystem::remove(path, ec);
}

} // namespace SaveGame
} // namespace rivenrt
