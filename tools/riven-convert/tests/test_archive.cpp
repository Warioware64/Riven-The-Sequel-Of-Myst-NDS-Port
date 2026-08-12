// Reads a real Riven install through libvaht and the converter's own parsers,
// and checks the results against counts measured from the archives.
//
// Skips (and passes) when RIVEN_TEST_DATA is unset or does not point at a
// readable install, so CI without game data still goes green. Point it at the
// folder holding Data/ and All/:
//
//     RIVEN_TEST_DATA=/path/to/riven ctest --test-dir build/convert
//
// The expectations below were read off a French 5-CD install. Card and resource
// counts are release-independent (the localisations differ in the byte content
// of tBMP/tWAV, not in how many cards Boiler Island has), so a mismatch means
// either the parser regressed or the source tree is not what it claims.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "riven/Layout.hpp"
#include "riven/MovieList.hpp"
#include "riven/WaterEffect.hpp"

extern "C" {
#include <vaht/vaht.h>
}

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const char *what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }

    /// Expected resource counts per stack, for the archives that carry cards.
    struct Expect
    {
        rivendata::StackId stack;
        const char *archive; ///< filename, matched case-insensitively
        int cards;
        int tbmp;
        int tmov;
        int sfxe;
    };

    const Expect kExpect[] = {
        {rivendata::StackId::Aspit, "a_data.mhk",  8,  74,   1,   0},
        {rivendata::StackId::Bspit, "b_data.mhk",  498, 819, 195, 81},
        {rivendata::StackId::Jspit, "j_data1.mhk", 400, 693, 197, 77},
        {rivendata::StackId::Jspit, "j_data2.mhk", 414, 667, 159, 134},
        {rivendata::StackId::Pspit, "p_data.mhk",  47,  69,  46,  26},
        {rivendata::StackId::Rspit, "r_data.mhk",  37,  50,  28,  3},
    };

    int countType(vaht_archive *a, const char *type)
    {
        vaht_resource **rs = vaht_resources_open(a, type);
        if (rs == nullptr)
            return 0;
        int n = 0;
        while (rs[n] != nullptr)
            ++n;
        vaht_resources_close(rs);
        return n;
    }

    std::vector<std::uint8_t> readResource(vaht_archive *a, const char *type,
                                           std::uint16_t id)
    {
        std::vector<std::uint8_t> buf;
        vaht_resource *r = vaht_resource_open(a, type, id);
        if (r == nullptr)
            return buf;
        buf.resize(vaht_resource_size(r));
        if (!buf.empty())
            vaht_resource_read(r, static_cast<std::uint32_t>(buf.size()), buf.data());
        vaht_resource_close(r);
        return buf;
    }

    std::string lower(std::string s)
    {
        for (auto &c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    /// True when a resource is entirely zero bytes -- the signature of a
    /// download or disc rip that did not finish. No real Riven resource is
    /// all zeroes (every one of them starts with a magic word, a count or a
    /// dimension), so this is a safe test for "the source is damaged here"
    /// rather than "the parser is wrong here".
    bool looksDamaged(const std::vector<std::uint8_t> &buf)
    {
        if (buf.empty())
            return true;
        for (const auto b : buf)
            if (b != 0)
                return false;
        return true;
    }
} // namespace

int main()
{
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("archive: skipped (set RIVEN_TEST_DATA to a Riven install)\n");
        return 0;
    }

    const riven::Source src = riven::detectSource(dataEnv);
    if (!src.valid())
    {
        std::printf("archive: skipped (no Riven data under '%s')\n", dataEnv);
        return 0;
    }

    std::printf("archive: %s, %zu stack(s) present\n", riven::toString(src.layout),
                src.stacks.size());

    // Detection invariants that hold for any install.
    for (const auto &s : src.stacks)
    {
        check(s.usable(), "a listed stack has at least one data archive");
        for (const auto &p : s.dataArchives)
            check(riven::looksLikeMohawk(p), "every listed archive is a Mohawk file");
    }

    // Per-archive resource counts, for whichever expected archives are present.
    for (const auto &e : kExpect)
    {
        const riven::StackSource *ss = src.find(e.stack);
        if (ss == nullptr)
            continue;

        std::string found;
        for (const auto &p : ss->dataArchives)
            if (lower(p.filename().string()) == e.archive)
                found = p.string();
        if (found.empty())
            continue;

        vaht_archive *a = vaht_archive_open(found.c_str());
        if (a == nullptr)
        {
            std::fprintf(stderr, "FAIL: could not open %s\n", found.c_str());
            ++g_failures;
            continue;
        }

        char msg[256];
        auto expectCount = [&](const char *type, int want) {
            const int got = countType(a, type);
            std::snprintf(msg, sizeof(msg), "%s %s: expected %d, got %d",
                          e.archive, type, want, got);
            check(got == want, msg);
        };
        expectCount("CARD", e.cards);
        expectCount("tBMP", e.tbmp);
        expectCount("tMOV", e.tmov);
        if (e.sfxe > 0)
            expectCount("SFXE", e.sfxe);

        // Every card carries the same six lists, so these must match CARD.
        expectCount("PLST", e.cards);
        expectCount("BLST", e.cards);
        expectCount("HSPT", e.cards);
        expectCount("MLST", e.cards);
        expectCount("SLST", e.cards);
        expectCount("FLST", e.cards);

        // Parse every MLST and FLST in the archive: these are ours, not
        // libvaht's, so they get exercised on the whole corpus rather than a
        // sample. A parse that silently returns nothing would show up as a
        // movie-less archive, which no Riven stack is.
        int mlstRecords = 0, flstRecords = 0, mlstFailures = 0;
        vaht_resource **mlsts = vaht_resources_open(a, "MLST");
        for (int i = 0; mlsts != nullptr && mlsts[i] != nullptr; ++i)
        {
            const auto buf = readResource(a, "MLST", vaht_resource_id(mlsts[i]));
            riven::ResourceReader r(buf);
            const auto recs = riven::parseMlst(r);
            mlstRecords += static_cast<int>(recs.size());
            for (const auto &m : recs)
            {
                // rate is documented as always 1 and highBoundTime as 0xFFFF;
                // if that stops holding, the field order is wrong.
                if (m.rate != 1 || m.highBoundTime != 0xFFFF)
                    ++mlstFailures;
            }
        }
        if (mlsts != nullptr)
            vaht_resources_close(mlsts);

        vaht_resource **flsts = vaht_resources_open(a, "FLST");
        for (int i = 0; flsts != nullptr && flsts[i] != nullptr; ++i)
        {
            const auto buf = readResource(a, "FLST", vaht_resource_id(flsts[i]));
            riven::ResourceReader r(buf);
            flstRecords += static_cast<int>(riven::parseFlst(r).size());
        }
        if (flsts != nullptr)
            vaht_resources_close(flsts);

        std::snprintf(msg, sizeof(msg), "%s: MLST parsed %d records", e.archive,
                      mlstRecords);
        check(mlstRecords > 0, msg);
        std::snprintf(msg, sizeof(msg),
                      "%s: %d MLST records have an unexpected rate/highBoundTime",
                      e.archive, mlstFailures);
        check(mlstFailures == 0, msg);

        // FLST references SFXE, so a stack with water effects must have FLST
        // records pointing at them.
        if (e.sfxe > 0)
        {
            std::snprintf(msg, sizeof(msg), "%s: FLST parsed %d records", e.archive,
                          flstRecords);
            check(flstRecords > 0, msg);
        }

        // Every SFXE must parse, and its frames must reference real copies.
        //
        // Damaged source data is separated from parser bugs here, because they
        // are not the same finding and conflating them makes this test lie in
        // both directions. An interrupted copy of the game leaves runs of zero
        // bytes inside otherwise valid archives -- p_Data.MHK in one 5-CD set
        // has five entirely zeroed SFXE and a sixth that turns to zeros
        // partway through frame 25. Refusing to parse those is CORRECT
        // behaviour, so they are counted and reported, not failed. A resource
        // with real bytes that fails to parse is a bug and does fail.
        int sfxeOk = 0, sfxeBad = 0, sfxeDamaged = 0, sfxePartial = 0;
        vaht_resource **fx = vaht_resources_open(a, "SFXE");
        for (int i = 0; fx != nullptr && fx[i] != nullptr; ++i)
        {
            const std::uint16_t id = vaht_resource_id(fx[i]);
            const auto buf = readResource(a, "SFXE", id);
            riven::ResourceReader r(buf);
            const riven::SfxeParse parsed = riven::parseSfxe(r, id);

            if (!parsed.ok())
            {
                if (looksDamaged(buf))
                    ++sfxeDamaged;
                else
                {
                    std::fprintf(stderr,
                                 "  %s SFXE %u: has data but yielded no frames\n",
                                 e.archive, id);
                    ++sfxeBad;
                }
                continue;
            }

            // Frames must index real copies -- an off-by-one here would show up
            // on device as a water effect reading past its own buffer.
            bool good = true;
            for (const auto &f : parsed.effect->frames)
                if (f.firstCopy + f.copyCount > parsed.effect->copies.size())
                    good = false;
            if (!good)
            {
                ++sfxeBad;
                continue;
            }

            ++sfxeOk;
            if (!parsed.complete())
            {
                ++sfxePartial;
                std::printf("               SFXE %u: salvaged %zu of %d frames "
                            "(source damaged)\n",
                            id, parsed.effect->frames.size(), parsed.framesClaimed);
            }
        }
        if (fx != nullptr)
            vaht_resources_close(fx);

        if (e.sfxe > 0)
        {
            std::snprintf(msg, sizeof(msg),
                          "%s: %d SFXE with real data failed to parse",
                          e.archive, sfxeBad);
            check(sfxeBad == 0, msg);
            std::snprintf(msg, sizeof(msg), "%s: %d/%d SFXE accounted for",
                          e.archive, sfxeOk + sfxeDamaged, e.sfxe);
            check(sfxeOk + sfxeDamaged == e.sfxe, msg);
        }
        (void)sfxePartial;

        std::printf("  %-12s cards=%d tBMP=%d tMOV=%d SFXE=%d "
                    "(MLST recs=%d, FLST recs=%d)\n",
                    e.archive, e.cards, e.tbmp, e.tmov, e.sfxe, mlstRecords,
                    flstRecords);
        if (sfxeDamaged > 0)
            std::printf("               NOTE: %d SFXE are zero-filled in this "
                        "copy of the game -- the source data is damaged.\n",
                        sfxeDamaged);

        vaht_archive_close(a);
    }

    std::printf("archive: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
