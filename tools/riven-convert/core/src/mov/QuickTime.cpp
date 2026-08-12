#include "riven/QuickTime.hpp"

#include <algorithm>
#include <map>

namespace riven
{
namespace
{
    // An atom is size(u32) type(4cc) payload. size == 1 means a 64-bit size
    // follows the type; size == 0 means "to the end of the enclosing atom".
    // Riven uses neither, but a parser that assumes so walks off the end of a
    // file that does.
    struct Atom
    {
        std::string type;
        std::size_t start = 0; ///< first payload byte
        std::size_t end = 0;   ///< one past the last payload byte
        bool ok = false;
    };

    Atom readAtom(ResourceReader &r, std::size_t containerEnd)
    {
        Atom a;
        const std::size_t header = r.pos();
        if (header + 8 > containerEnd)
            return a;

        std::uint64_t size = r.u32();
        a.type = r.tag();
        if (!r.ok())
            return a;

        if (size == 1)
        {
            const std::uint64_t hi = r.u32();
            const std::uint64_t lo = r.u32();
            size = (hi << 32) | lo;
            if (!r.ok())
                return a;
        }
        else if (size == 0)
        {
            size = containerEnd - header;
        }

        if (size < r.pos() - header)
            return a; // shorter than its own header
        a.start = r.pos();
        a.end = header + static_cast<std::size_t>(size);
        if (a.end > containerEnd)
            a.end = containerEnd; // truncated: salvage what is there
        a.ok = true;
        return a;
    }

    /// Find `type` among the atoms of the container [start, end).
    Atom findAtom(ResourceReader &r, std::size_t start, std::size_t end, const char *type)
    {
        r.seek(start);
        while (r.ok() && r.pos() + 8 <= end)
        {
            const Atom a = readAtom(r, end);
            if (!a.ok)
                break;
            if (a.type == type)
                return a;
            r.seek(a.end);
        }
        return {};
    }

    void readMdhd(ResourceReader &r, const Atom &a, MovieTrack &t)
    {
        r.seek(a.start);
        const std::uint32_t versionFlags = r.u32();
        const bool v1 = (versionFlags >> 24) == 1;
        if (v1)
        {
            r.skip(16); // created, modified, both 64-bit
            t.timescale = r.u32();
            const std::uint64_t hi = r.u32();
            const std::uint64_t lo = r.u32();
            t.duration = (hi << 32) | lo;
        }
        else
        {
            r.skip(8);
            t.timescale = r.u32();
            t.duration = r.u32();
        }
    }

    MovieTrack::Kind readHdlr(ResourceReader &r, const Atom &a)
    {
        r.seek(a.start);
        r.u32();                          // version and flags
        const std::string component = r.tag(); // 'mhlr' for a media handler
        const std::string subtype = r.tag();
        (void)component;
        if (subtype == "vide")
            return MovieTrack::Kind::Video;
        if (subtype == "soun")
            return MovieTrack::Kind::Audio;
        return MovieTrack::Kind::Other;
    }

    void readStsd(ResourceReader &r, const Atom &a, MovieTrack &t)
    {
        r.seek(a.start);
        r.u32(); // version and flags
        const std::uint32_t entries = r.u32();
        if (entries == 0 || !r.ok())
            return;

        // Only the first sample description is read: Riven's movies do not
        // switch codec part way through, and if one ever did, the report
        // saying what it starts as is still the useful answer.
        r.u32();               // entry size
        t.codec = r.tag();
        r.skip(6);             // reserved
        r.u16();               // data reference index

        if (t.kind == MovieTrack::Kind::Video)
        {
            r.skip(2 + 2 + 4); // version, revision, vendor
            r.skip(4 + 4);     // temporal and spatial quality
            t.width = r.u16();
            t.height = r.u16();
            r.skip(4 + 4);     // horizontal and vertical resolution
            r.skip(4);         // data size
            r.u16();           // frame count per sample
            r.skip(32);        // compressor name, Pascal string
            t.depth = r.u16();
        }
        else if (t.kind == MovieTrack::Kind::Audio)
        {
            r.skip(2 + 2 + 4); // version, revision, vendor
            t.channels = r.u16();
            t.bitsPerSample = r.u16();
            r.u16();           // compression id
            r.u16();           // packet size
            // 16.16 fixed point, and the fraction is always zero in practice.
            t.sampleRate = static_cast<int>(r.u32() >> 16);
        }
    }

    /// Total samples and total duration from the time-to-sample table.
    void readStts(ResourceReader &r, const Atom &a, MovieTrack &t)
    {
        r.seek(a.start);
        r.u32(); // version and flags
        const std::uint32_t entries = r.u32();

        std::uint32_t samples = 0;
        for (std::uint32_t i = 0; i < entries && r.ok(); ++i)
        {
            const std::uint32_t count = r.u32();
            r.u32(); // duration of each
            samples += count;
        }
        if (samples > 0)
            t.sampleCount = samples;
    }

    /// The raw tables, kept only while a track's sample list is built.
    struct Tables
    {
        struct Stsc
        {
            std::uint32_t firstChunk = 0; ///< 0-based; the file stores 1-based
            std::uint32_t samplesPerChunk = 0;
        };
        struct Stts
        {
            std::uint32_t count = 0;
            std::uint32_t duration = 0;
        };

        std::vector<Stsc> stsc;
        std::vector<Stts> stts;
        std::vector<std::uint32_t> chunkOffsets;
        std::vector<std::uint32_t> sampleSizes; ///< empty when uniformSize != 0
        std::uint32_t uniformSize = 0;
        std::uint32_t sampleCount = 0;
        std::vector<std::uint32_t> syncSamples; ///< 0-based; empty means all
        bool hasStss = false;
    };

    void readStscTable(ResourceReader &r, const Atom &a, Tables &tb)
    {
        r.seek(a.start);
        r.u32();
        const std::uint32_t entries = r.u32();
        tb.stsc.reserve(entries);
        for (std::uint32_t i = 0; i < entries && r.ok(); ++i)
        {
            Tables::Stsc e;
            const std::uint32_t first = r.u32();
            e.firstChunk = first > 0 ? first - 1 : 0; // the file is 1-based
            e.samplesPerChunk = r.u32();
            r.u32(); // sample description id -- Riven never has a second entry
            tb.stsc.push_back(e);
        }
    }

    void readSttsTable(ResourceReader &r, const Atom &a, Tables &tb)
    {
        r.seek(a.start);
        r.u32();
        const std::uint32_t entries = r.u32();
        tb.stts.reserve(entries);
        for (std::uint32_t i = 0; i < entries && r.ok(); ++i)
        {
            Tables::Stts e;
            e.count = r.u32();
            e.duration = r.u32();
            tb.stts.push_back(e);
        }
    }

    void readStcoTable(ResourceReader &r, const Atom &a, Tables &tb)
    {
        r.seek(a.start);
        r.u32();
        const std::uint32_t entries = r.u32();
        tb.chunkOffsets.reserve(entries);
        for (std::uint32_t i = 0; i < entries && r.ok(); ++i)
            tb.chunkOffsets.push_back(r.u32());
    }

    void readStszTable(ResourceReader &r, const Atom &a, Tables &tb)
    {
        r.seek(a.start);
        r.u32();
        tb.uniformSize = r.u32();
        tb.sampleCount = r.u32();
        // A non-zero uniform size means there is NO table: every sample is that
        // many bytes (quicktime.cpp:696-697).
        if (tb.uniformSize != 0)
            return;
        tb.sampleSizes.reserve(tb.sampleCount);
        for (std::uint32_t i = 0; i < tb.sampleCount && r.ok(); ++i)
            tb.sampleSizes.push_back(r.u32());
    }

    void readStssTable(ResourceReader &r, const Atom &a, Tables &tb)
    {
        r.seek(a.start);
        r.u32();
        const std::uint32_t entries = r.u32();
        tb.hasStss = true;
        tb.syncSamples.reserve(entries);
        for (std::uint32_t i = 0; i < entries && r.ok(); ++i)
        {
            const std::uint32_t s = r.u32();
            tb.syncSamples.push_back(s > 0 ? s - 1 : 0); // 1-based on disc
        }
    }

    /// Samples per chunk `c`: the last stsc entry whose firstChunk <= c. The
    /// table is run-length and the final entry covers every remaining chunk,
    /// so the chunk count comes from stco and never from stsc.
    std::uint32_t samplesInChunk(const Tables &tb, std::uint32_t chunk)
    {
        std::uint32_t n = 0;
        for (const auto &e : tb.stsc)
        {
            if (e.firstChunk > chunk)
                break;
            n = e.samplesPerChunk;
        }
        return n;
    }

    /// Turn the tables into a flat per-sample list. Riven's movies are small
    /// enough (1225 frames in the longest) that building the whole list up
    /// front is cheaper than the per-sample chunk search it replaces.
    void buildVideoSamples(const Tables &tb, MovieTrack &t, std::size_t resourceSize)
    {
        std::uint32_t sample = 0;
        std::uint32_t time = 0;

        // Durations, expanded from the run-length stts.
        std::size_t sttsIndex = 0;
        std::uint32_t sttsLeft = tb.stts.empty() ? 0 : tb.stts[0].count;

        std::map<std::uint32_t, std::uint32_t> durationHistogram;

        for (std::uint32_t chunk = 0; chunk < tb.chunkOffsets.size(); ++chunk)
        {
            const std::uint32_t inChunk = samplesInChunk(tb, chunk);
            std::size_t offset = tb.chunkOffsets[chunk];

            for (std::uint32_t i = 0; i < inChunk; ++i, ++sample)
            {
                MovieSample s;
                s.offset = offset;
                s.size = tb.uniformSize != 0
                             ? tb.uniformSize
                             : (sample < tb.sampleSizes.size() ? tb.sampleSizes[sample] : 0);
                offset += s.size;

                while (sttsLeft == 0 && sttsIndex + 1 < tb.stts.size())
                {
                    ++sttsIndex;
                    sttsLeft = tb.stts[sttsIndex].count;
                }
                if (sttsIndex < tb.stts.size() && sttsLeft > 0)
                {
                    s.duration = tb.stts[sttsIndex].duration;
                    --sttsLeft;
                }
                s.startTime = time;
                time += s.duration;
                ++durationHistogram[s.duration];

                // An absent stss means every sample is a keyframe -- not none
                // (qt_decoder.cpp:762-770 gets this by returning the frame
                // itself when the table is empty).
                s.keyframe = !tb.hasStss;

                if (s.size == 0 || s.offset + s.size > resourceSize)
                    continue; // outside the resource: drop rather than read wild
                t.samples.push_back(s);
            }
        }

        if (tb.hasStss)
        {
            for (const std::uint32_t k : tb.syncSamples)
                if (k < t.samples.size())
                    t.samples[k].keyframe = true;
        }

        std::uint32_t best = 0;
        for (const auto &[duration, n] : durationHistogram)
            if (n > best)
            {
                best = n;
                t.modalDuration = duration;
            }
        t.rateIsConstant = durationHistogram.size() <= 1;
    }

    /// Audio: one entry per chunk, byte-sized from the sample description
    /// rather than from stsz. Riven's tracks are "old style" -- stts is a
    /// single entry of duration 1, so stsc counts PCM sample frames and the
    /// packet size has to come from the codec (audio/decoders/quicktime.cpp:319-350).
    void buildAudioChunks(const Tables &tb, MovieTrack &t, std::size_t resourceSize,
                          std::uint32_t samplesPerPacket, std::uint32_t bytesPerPacket)
    {
        if (samplesPerPacket == 0 || bytesPerPacket == 0)
            return;

        for (std::uint32_t chunk = 0; chunk < tb.chunkOffsets.size(); ++chunk)
        {
            const std::uint32_t frames = samplesInChunk(tb, chunk);
            if (frames == 0)
                continue;

            MovieSample s;
            s.offset = tb.chunkOffsets[chunk];
            // Round up: a chunk whose frame count is not a whole number of
            // packets still carries the partial packet, and dropping it loses
            // the tail of the track.
            const std::uint64_t packets = (frames + samplesPerPacket - 1) / samplesPerPacket;
            s.size = static_cast<std::uint32_t>(packets * bytesPerPacket);
            if (s.offset >= resourceSize)
                continue;
            if (s.offset + s.size > resourceSize)
                s.size = static_cast<std::uint32_t>(resourceSize - s.offset);
            t.samples.push_back(s);
        }
    }

    void readStsz(ResourceReader &r, const Atom &a, MovieTrack &t)
    {
        r.seek(a.start);
        r.u32(); // version and flags
        const std::uint32_t uniform = r.u32();
        const std::uint32_t entries = r.u32();

        if (uniform != 0)
        {
            t.dataBytes = static_cast<std::uint64_t>(uniform) * entries;
            if (t.sampleCount == 0)
                t.sampleCount = entries;
            return;
        }

        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < entries && r.ok(); ++i)
            total += r.u32();
        t.dataBytes = total;
        if (t.sampleCount == 0)
            t.sampleCount = entries;
    }
} // namespace

const MovieTrack *MovieInfo::video() const
{
    for (const auto &t : tracks)
        if (t.kind == MovieTrack::Kind::Video)
            return &t;
    return nullptr;
}

const MovieTrack *MovieInfo::audio() const
{
    for (const auto &t : tracks)
        if (t.kind == MovieTrack::Kind::Audio)
            return &t;
    return nullptr;
}

MovieInfo probeMovie(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                     bool withSamples)
{
    MovieInfo info;
    info.id = id;
    info.resourceBytes = bytes.size();

    if (bytes.size() < 16)
    {
        info.error = "too short to be a movie";
        return info;
    }

    ResourceReader r(bytes);
    const Atom moov = findAtom(r, 0, bytes.size(), "moov");
    if (!moov.ok)
    {
        info.error = "no moov atom";
        return info;
    }

    // Walk the tracks by hand rather than through findAtom: there are several
    // trak atoms and findAtom returns the first match of a type.
    r.seek(moov.start);
    while (r.ok() && r.pos() + 8 <= moov.end)
    {
        const Atom trak = readAtom(r, moov.end);
        if (!trak.ok)
            break;
        const std::size_t next = trak.end;

        if (trak.type == "trak")
        {
            MovieTrack t;

            const Atom mdia = findAtom(r, trak.start, trak.end, "mdia");
            if (mdia.ok)
            {
                if (const Atom hdlr = findAtom(r, mdia.start, mdia.end, "hdlr"); hdlr.ok)
                    t.kind = readHdlr(r, hdlr);
                if (const Atom mdhd = findAtom(r, mdia.start, mdia.end, "mdhd"); mdhd.ok)
                    readMdhd(r, mdhd, t);

                if (const Atom minf = findAtom(r, mdia.start, mdia.end, "minf"); minf.ok)
                    if (const Atom stbl = findAtom(r, minf.start, minf.end, "stbl"); stbl.ok)
                    {
                        if (const Atom stsd = findAtom(r, stbl.start, stbl.end, "stsd");
                            stsd.ok)
                            readStsd(r, stsd, t);
                        if (const Atom stts = findAtom(r, stbl.start, stbl.end, "stts");
                            stts.ok)
                            readStts(r, stts, t);
                        if (const Atom stsz = findAtom(r, stbl.start, stbl.end, "stsz");
                            stsz.ok)
                            readStsz(r, stsz, t);

                        if (withSamples)
                        {
                            Tables tb;
                            if (const Atom x = findAtom(r, stbl.start, stbl.end, "stsc"); x.ok)
                                readStscTable(r, x, tb);
                            if (const Atom x = findAtom(r, stbl.start, stbl.end, "stts"); x.ok)
                                readSttsTable(r, x, tb);
                            if (const Atom x = findAtom(r, stbl.start, stbl.end, "stco"); x.ok)
                                readStcoTable(r, x, tb);
                            if (const Atom x = findAtom(r, stbl.start, stbl.end, "stsz"); x.ok)
                                readStszTable(r, x, tb);
                            if (const Atom x = findAtom(r, stbl.start, stbl.end, "stss"); x.ok)
                                readStssTable(r, x, tb);

                            if (t.kind == MovieTrack::Kind::Video)
                                buildVideoSamples(tb, t, bytes.size());
                            else if (t.kind == MovieTrack::Kind::Audio)
                                buildAudioChunks(tb, t, bytes.size(), kIma4SamplesPerPacket,
                                                 kIma4PacketBytes
                                                     * static_cast<std::uint32_t>(
                                                         t.channels > 0 ? t.channels : 1));
                        }
                    }
            }

            if (!t.codec.empty())
                info.tracks.push_back(std::move(t));
        }

        r.seek(next);
    }

    if (info.tracks.empty())
    {
        info.error = "moov has no readable tracks";
        return info;
    }

    info.ok = true;
    return info;
}

std::vector<MovieInfo> probeMovies(const ArchiveSet &set)
{
    std::vector<MovieInfo> out;
    for (const std::uint16_t id : set.resourceIds("tMOV"))
    {
        const auto bytes = set.read("tMOV", id);
        if (bytes.empty())
        {
            MovieInfo bad;
            bad.id = id;
            bad.error = "could not be read";
            out.push_back(std::move(bad));
            continue;
        }
        if (looksDamaged(bytes))
        {
            MovieInfo bad;
            bad.id = id;
            bad.resourceBytes = bytes.size();
            bad.error = "zero-filled in this copy of the game";
            out.push_back(std::move(bad));
            continue;
        }
        out.push_back(probeMovie(bytes, id));
    }
    return out;
}

} // namespace riven
