#include "BookNotes.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "RivenData.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace BookNotes
{
namespace
{
    constexpr std::uint32_t kNoteMagic = 0x544F4E52u;   ///< 'RNOT' little-endian
    constexpr std::uint32_t kStrokeMagic = 0x4B54534Cu; ///< 'LSTK' little-endian
    constexpr std::size_t kNoteHeaderBytes = 32;
    constexpr std::size_t kStrokeHeaderBytes = 16;

    /// A stroke is a polyline the stylus drew in one go. Two thousand points is
    /// a continuous line about forty screens long; past that the file is not a
    /// note. Bounded because readStrokes allocates from this count, exactly as
    /// readPixels allocates from the width and height.
    constexpr std::uint16_t kMaxStrokePoints = 2000;
    /// Enough for a densely annotated page, and a bound on the same allocation.
    constexpr std::uint16_t kMaxStrokes = 512;

    std::string g_dir;

    // Written and read by hand, little-endian, as in SaveGame.cpp -- no packed
    // struct, so no compiler's padding decision can change the format.
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

    bool validIndex(int index) { return index >= 0 && index < kMaxNotes; }

    std::string lastPath() { return g_dir.empty() ? std::string() : g_dir + "last.dat"; }

    /// Parse and VALIDATE a 32-byte note header.
    ///
    /// Every bound here is guarding an allocation that happens later:
    /// readPixels sizes a buffer from w*h, so a garbage size is an
    /// out-of-memory rather than a failed read. The payload length is
    /// cross-checked against w*h*2 as well, so the two ways of saying how big
    /// the picture is have to agree.
    bool parseNoteHeader(const std::uint8_t *h, NoteInfo &info)
    {
        if (get32(h) != kNoteMagic || get16(h + 4) != kVersion)
            return false;

        const std::uint16_t w = get16(h + 6);
        const std::uint16_t hh = get16(h + 8);
        if (w == 0 || hh == 0 || w > kMaxW || hh > kMaxH)
            return false;
        if (get32(h + 22) != static_cast<std::uint32_t>(w) * hh * 2u)
            return false;

        info.stackId = h[10];
        info.cardId = get16(h + 12);
        info.when = get64(h + 14);
        info.w = w;
        info.h = hh;
        return true;
    }

    bool readHeaderOf(int index, NoteInfo &info)
    {
        const std::string path = notePath(index);
        if (path.empty())
            return false;
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return false;
        std::uint8_t head[kNoteHeaderBytes];
        const std::size_t got = std::fread(head, 1, sizeof(head), f);
        std::fclose(f);
        if (got != sizeof(head))
            return false;
        if (!parseNoteHeader(head, info))
            return false;
        info.index = index;
        return true;
    }
} // namespace

const char *inkName(int index)
{
    static const char *const kNames[kInkCount] = {"red",   "white", "yellow",
                                                  "green", "blue",  "black"};
    return index >= 0 && index < kInkCount ? kNames[index] : "?";
}

void setDirectory(const std::string &dir)
{
    g_dir = dir;
}

std::string notePath(int index)
{
    if (g_dir.empty() || !validIndex(index))
        return std::string();
    char name[24];
    std::snprintf(name, sizeof(name), "note_%03d.rnot", index);
    return g_dir + name;
}

std::string strokePath(int index)
{
    if (g_dir.empty() || !validIndex(index))
        return std::string();
    char name[24];
    std::snprintf(name, sizeof(name), "note_%03d.str", index);
    return g_dir + name;
}

std::vector<NoteInfo> scan()
{
    std::vector<NoteInfo> out;
    if (g_dir.empty())
        return out;
    for (int i = 0; i < kMaxNotes; ++i)
    {
        NoteInfo info;
        if (readHeaderOf(i, info))
            out.push_back(info);
    }
    return out;
}

int capture(const std::uint16_t *pixels, int w, int h, std::uint8_t stackId,
            std::uint16_t cardId)
{
    if (g_dir.empty() || pixels == nullptr)
        return -1;
    if (w <= 0 || h <= 0 || w > kMaxW || h > kMaxH)
        return -1;

    // The first index with no file on it, so a deletion frees a slot rather
    // than leaving a permanent hole in a 24-note notebook.
    int index = -1;
    std::error_code ec;
    for (int i = 0; i < kMaxNotes; ++i)
    {
        if (!std::filesystem::exists(notePath(i), ec))
        {
            index = i;
            break;
        }
    }
    if (index < 0)
        return -1; // full, and the caller says so

    const std::uint32_t bytes = static_cast<std::uint32_t>(w) * h * 2u;
    std::uint8_t head[kNoteHeaderBytes] = {};
    put32(head, kNoteMagic);
    put16(head + 4, kVersion);
    put16(head + 6, static_cast<std::uint16_t>(w));
    put16(head + 8, static_cast<std::uint16_t>(h));
    head[10] = stackId;
    put16(head + 12, cardId);
    put64(head + 14, static_cast<std::uint64_t>(std::time(nullptr)));
    put32(head + 22, bytes);

    const std::string path = notePath(index);
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return -1;
    bool ok = std::fwrite(head, 1, sizeof(head), f) == sizeof(head)
              && std::fwrite(pixels, 1, bytes, f) == bytes;
    // Part of the write: fwrite fills a buffer and it is fclose that reports the
    // flush failing on a full card. See SaveGame::writeSlot.
    if (std::fclose(f) != 0)
        ok = false;
    if (!ok)
    {
        // A full card leaves a header over half a picture, which would list in
        // the browser and open as garbage. Never leave half a note.
        std::filesystem::remove(path, ec);
        return -1;
    }

    // A note is born with no strokes. Any file left at this index by a note that
    // was deleted has to go, or the new capture would open under the old one's
    // scribbles.
    std::filesystem::remove(strokePath(index), ec);
    return index;
}

bool readPixels(int index, std::vector<std::uint16_t> &out, NoteInfo &info)
{
    NoteInfo got;
    if (!readHeaderOf(index, got))
        return false;

    const std::string path = notePath(index);
    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return false;
    if (std::fseek(f, static_cast<long>(kNoteHeaderBytes), SEEK_SET) != 0)
    {
        std::fclose(f);
        return false;
    }

    // Sized from the header, which parseNoteHeader has already bounded to a
    // screen -- so this allocation is at most 96 KB however corrupt the file is.
    const std::size_t count = static_cast<std::size_t>(got.w) * got.h;
    std::vector<std::uint16_t> px(count);
    const std::size_t read = std::fread(px.data(), sizeof(std::uint16_t), count, f);
    std::fclose(f);
    if (read != count)
        return false;

    out = std::move(px);
    info = got;
    return true;
}

std::vector<Stroke> readStrokes(int index)
{
    std::vector<Stroke> out;
    const std::string path = strokePath(index);
    if (path.empty())
        return out;

    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return out; // no strokes is not an error: most notes have none

    std::uint8_t head[kStrokeHeaderBytes];
    if (std::fread(head, 1, sizeof(head), f) != sizeof(head)
        || get32(head) != kStrokeMagic || get16(head + 4) != kVersion)
    {
        std::fclose(f);
        return out;
    }

    const std::uint16_t count = get16(head + 6);
    if (count > kMaxStrokes)
    {
        std::fclose(f);
        return out;
    }

    out.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i)
    {
        std::uint8_t rec[4];
        if (std::fread(rec, 1, sizeof(rec), f) != sizeof(rec))
        {
            // Truncated. Everything read so far is a real stroke and is kept:
            // half a page of scribbles is worth more to the player than none,
            // and writeStrokes will tidy the file up the next time they draw.
            break;
        }
        const std::uint16_t points = get16(rec + 2);
        if (points == 0 || points > kMaxStrokePoints)
            break;

        Stroke s;
        s.color = rec[0] < kInkCount ? rec[0] : 0;
        s.width = rec[1] == 0 ? 1 : rec[1];
        s.x.resize(points);
        s.y.resize(points);
        bool ok = true;
        for (std::uint16_t p = 0; p < points && ok; ++p)
        {
            std::uint8_t xy[2];
            ok = std::fread(xy, 1, sizeof(xy), f) == sizeof(xy);
            s.x[p] = xy[0];
            s.y[p] = xy[1];
        }
        if (!ok)
            break;
        out.push_back(std::move(s));
    }

    std::fclose(f);
    return out;
}

bool writeStrokes(int index, const std::vector<Stroke> &strokes)
{
    const std::string path = strokePath(index);
    if (path.empty())
        return false;

    std::error_code ec;
    if (strokes.empty())
    {
        // Not an empty file: a fully erased note and a never-drawn-on note are
        // the same thing and must look the same on the card.
        std::filesystem::remove(path, ec);
        return true;
    }

    // Decided ONCE, and then both the header and the file body are written from
    // this list. Counting in one loop and writing in another let the two
    // disagree about which strokes were skipped -- a page with more than
    // kMaxStrokes on it wrote a header saying 512 followed by every stroke there
    // was, and readStrokes would then walk off the end of what the header
    // described.
    std::vector<const Stroke *> keep;
    std::size_t payload = 0;
    for (const Stroke &s : strokes)
    {
        if (s.points() == 0 || s.points() > kMaxStrokePoints)
            continue;
        if (keep.size() == kMaxStrokes)
            break;
        keep.push_back(&s);
        payload += 4 + s.points() * 2;
    }
    if (keep.empty())
    {
        std::filesystem::remove(path, ec);
        return true;
    }

    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;

    std::uint8_t head[kStrokeHeaderBytes] = {};
    put32(head, kStrokeMagic);
    put16(head + 4, kVersion);
    put16(head + 6, static_cast<std::uint16_t>(keep.size()));
    put32(head + 8, static_cast<std::uint32_t>(payload));
    bool ok = std::fwrite(head, 1, sizeof(head), f) == sizeof(head);

    for (const Stroke *s : keep)
    {
        if (!ok)
            break;
        const std::size_t points = s->points();
        std::uint8_t rec[4] = {s->color, s->width, 0, 0};
        put16(rec + 2, static_cast<std::uint16_t>(points));
        ok = std::fwrite(rec, 1, sizeof(rec), f) == sizeof(rec);
        for (std::size_t p = 0; p < points && ok; ++p)
        {
            const std::uint8_t xy[2] = {s->x[p], s->y[p]};
            ok = std::fwrite(xy, 1, sizeof(xy), f) == sizeof(xy);
        }
    }
    if (std::fclose(f) != 0)
        ok = false;

    if (!ok)
    {
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

bool remove(int index)
{
    const std::string path = notePath(index);
    if (path.empty())
        return false;
    std::error_code ec;
    const bool gone = std::filesystem::remove(path, ec);
    std::filesystem::remove(strokePath(index), ec);

    // Forget it if it was the remembered one. A dangling index would otherwise
    // reopen the browser on whatever note lands at that slot next, which is a
    // capture the player has not taken yet.
    if (gone && lastOpened() == index)
    {
        const std::string lp = lastPath();
        if (!lp.empty())
            std::filesystem::remove(lp, ec);
        setLastOpened(-1);
    }
    return gone;
}

std::string label(const NoteInfo &info)
{
    std::string row = displayName(static_cast<StackId>(info.stackId));
    row += "  card " + std::to_string(info.cardId);
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

namespace
{
    /// -2 = never read from the card, -1 = none, >=0 = a note index.
    ///
    /// Three values and not two because "nobody has opened a note" and "the file
    /// has not been looked at yet" are different states, and collapsing them
    /// would make the lazy read happen on every call.
    int g_lastOpened = -2;
} // namespace

int lastOpened()
{
    if (g_lastOpened == -2)
    {
        g_lastOpened = -1;
        const std::string path = lastPath();
        if (!path.empty())
        {
            if (std::FILE *f = std::fopen(path.c_str(), "rb"))
            {
                int v = -1;
                if (std::fscanf(f, "%d", &v) == 1 && validIndex(v))
                    g_lastOpened = v;
                std::fclose(f);
            }
        }
    }
    return g_lastOpened;
}

void setLastOpened(int index)
{
    if (index == -1)
    {
        g_lastOpened = -1;
        return;
    }
    // Written only on a real change, so paging through the notebook costs one
    // small write per note actually opened rather than one per frame.
    if (!validIndex(index) || index == lastOpened())
        return;
    g_lastOpened = index;
    const std::string path = lastPath();
    if (path.empty())
        return;
    if (std::FILE *f = std::fopen(path.c_str(), "wb"))
    {
        std::fprintf(f, "%d", index);
        std::fclose(f);
    }
}

} // namespace BookNotes
} // namespace rivenrt
