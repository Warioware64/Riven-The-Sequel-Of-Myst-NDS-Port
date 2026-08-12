// Exercises the card-list parsers over every card of every readable archive.
//
// These parsers exist because libvaht's API drops fields the DS needs, so
// nothing else validates them. Running the whole corpus rather than a sample is
// affordable (a few seconds) and is the only way to catch a field-order mistake
// that happens to look plausible on card 1.
//
// Skips (and passes) without RIVEN_TEST_DATA so CI stays green without game
// data.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "riven/Archive.hpp"
#include "riven/CardParse.hpp"
#include "riven/Layout.hpp"

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const std::string &what)
    {
        ++g_checks;
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
            ++g_failures;
        }
    }

    struct Tally
    {
        int cards = 0;
        int damaged = 0;
        int handlers = 0;
        int commands = 0;
        int switches = 0;
        int changeStacks = 0;
        int hotspots = 0;
        int pictures = 0;
        int sounds = 0;
        int deepestSwitch = 0;
    };

    void walkCommands(const std::vector<rivendata::Command> &cmds, Tally &t, int depth)
    {
        for (const auto &c : cmds)
        {
            ++t.commands;
            if (c.opcode == static_cast<std::uint16_t>(rivendata::Op::Switch))
            {
                ++t.switches;
                if (depth + 1 > t.deepestSwitch)
                    t.deepestSwitch = depth + 1;
                for (const auto &b : c.branches)
                    walkCommands(b.commands, t, depth + 1);
            }
            else if (c.opcode == static_cast<std::uint16_t>(rivendata::Op::ChangeStack))
            {
                ++t.changeStacks;
            }
        }
    }
} // namespace

int main()
{
    const char *dataEnv = std::getenv("RIVEN_TEST_DATA");
    if (dataEnv == nullptr || dataEnv[0] == '\0')
    {
        std::printf("cards: skipped (set RIVEN_TEST_DATA to a Riven install)\n");
        return 0;
    }

    const riven::Source src = riven::detectSource(dataEnv);
    if (!src.valid())
    {
        std::printf("cards: skipped (no Riven data under '%s')\n", dataEnv);
        return 0;
    }

    for (const auto &stack : src.stacks)
    {
        riven::ArchiveSet set;
        std::vector<std::string> failures;
        set.openAll(stack.dataArchives, failures);
        check(failures.empty(),
              std::string(rivendata::stackName(stack.id)) + ": every archive opened");
        if (set.empty())
            continue;

        const auto cardIds = set.resourceIds("CARD");
        Tally t;

        // Every card carries the same six lists, keyed by the card's own id.
        // If that stops holding, the whole per-card model is wrong.
        for (const char *type : {"PLST", "BLST", "HSPT", "MLST", "SLST", "FLST"})
        {
            const auto ids = set.resourceIds(type);
            check(ids == cardIds, std::string(rivendata::stackName(stack.id)) + ": "
                                      + type + " ids match CARD ids");
        }

        for (const std::uint16_t id : cardIds)
        {
            const auto cardBytes = set.read("CARD", id);
            if (riven::looksDamaged(cardBytes))
            {
                ++t.damaged;
                continue;
            }

            rivendata::Card card;
            card.id = id;
            riven::ResourceReader cr(cardBytes);
            check(riven::parseCard(cr, card),
                  std::string(rivendata::stackName(stack.id)) + " card "
                      + std::to_string(id) + ": CARD parses");
            ++t.cards;
            t.handlers += static_cast<int>(card.scripts.size());
            for (const auto &h : card.scripts)
                walkCommands(h.commands, t, 0);

            // Each resource is kept in a named local: ResourceReader is a view,
            // and binding it to the temporary returned by read() dangles.
            const auto plstBytes = set.read("PLST", id);
            riven::ResourceReader pr(plstBytes);
            card.plst = riven::parsePlst(pr);

            const auto blstBytes = set.read("BLST", id);
            riven::ResourceReader br(blstBytes);
            card.blst = riven::parseBlst(br);

            const auto hsptBytes = set.read("HSPT", id);
            riven::ResourceReader hr(hsptBytes);
            card.hotspots = riven::parseHspt(hr);

            const auto slstBytes = set.read("SLST", id);
            riven::ResourceReader sr(slstBytes);
            card.slst = riven::parseSlst(sr);

            t.pictures += static_cast<int>(card.plst.size());
            t.hotspots += static_cast<int>(card.hotspots.size());
            t.sounds += static_cast<int>(card.slst.size());
            for (const auto &h : card.hotspots)
                for (const auto &hh : h.scripts)
                    walkCommands(hh.commands, t, 0);

            // The index fields are what activatePLST/activateBLST/activateSLST
            // select on, and the runtime will look them up by value. Confirm
            // they really are the 1-based sequence the opcodes assume -- this
            // is load-bearing and was previously only an assumption.
            for (std::size_t i = 0; i < card.plst.size(); ++i)
                check(card.plst[i].index == i + 1,
                      std::string(rivendata::stackName(stack.id)) + " card "
                          + std::to_string(id) + ": PLST index is 1-based sequential");
            for (std::size_t i = 0; i < card.blst.size(); ++i)
                check(card.blst[i].index == i + 1,
                      std::string(rivendata::stackName(stack.id)) + " card "
                          + std::to_string(id) + ": BLST index is 1-based sequential");

            // Every PLST record must name a tBMP that exists, or the card draws
            // nothing on the DS.
            for (const auto &p : card.plst)
                check(set.find("tBMP", p.id) != nullptr,
                      std::string(rivendata::stackName(stack.id)) + " card "
                          + std::to_string(id) + ": PLST references a real tBMP "
                          + std::to_string(p.id));

            // A BLST record addresses a hotspot by its blstId.
            for (const auto &b : card.blst)
            {
                bool found = false;
                for (const auto &h : card.hotspots)
                    if (h.blstId == b.hotspotId)
                        found = true;
                check(found || card.hotspots.empty(),
                      std::string(rivendata::stackName(stack.id)) + " card "
                          + std::to_string(id) + ": BLST hotspot "
                          + std::to_string(b.hotspotId) + " exists in HSPT");
            }

            // SLST parallel arrays must all be the same length, or the runtime
            // reads a volume for a sound that is not there.
            for (const auto &s : card.slst)
                check(s.volumes.size() == s.soundIds.size()
                          && s.balances.size() == s.soundIds.size()
                          && s.u2.size() == s.soundIds.size(),
                      std::string(rivendata::stackName(stack.id)) + " card "
                          + std::to_string(id) + ": SLST arrays are parallel");
        }

        // RMAP and NAME are per-stack, not per-card.
        const auto rmapIds = set.resourceIds("RMAP");
        if (!rmapIds.empty())
        {
            const auto rmapBytes = set.read("RMAP", rmapIds.front());
            riven::ResourceReader rr(rmapBytes);
            const auto rmap = riven::parseRmap(rr);
            check(!rmap.empty(),
                  std::string(rivendata::stackName(stack.id)) + ": RMAP is non-empty");
        }

        int namesRead = 0;
        for (const std::uint16_t nid : set.resourceIds("NAME"))
        {
            const auto nameBytes = set.read("NAME", nid);
            riven::ResourceReader nr(nameBytes);
            const auto list = riven::parseName(nr);
            if (list.names.empty())
                continue;
            ++namesRead;
            check(list.sortedIndex.size() == list.names.size(),
                  std::string(rivendata::stackName(stack.id)) + " NAME "
                      + std::to_string(nid) + ": sort index matches name count");
            // The sort index addresses names, so every entry must be in range.
            for (const auto ix : list.sortedIndex)
                check(ix < list.names.size(),
                      std::string(rivendata::stackName(stack.id)) + " NAME "
                          + std::to_string(nid) + ": sort index is in range");
        }

        std::printf("  %-6s cards=%d handlers=%d commands=%d (switch=%d depth=%d "
                    "changeStack=%d) hotspots=%d plst=%d slst=%d name-lists=%d",
                    rivendata::stackName(stack.id), t.cards, t.handlers, t.commands,
                    t.switches, t.deepestSwitch, t.changeStacks, t.hotspots,
                    t.pictures, t.sounds, namesRead);
        if (t.damaged > 0)
            std::printf("  [%d damaged CARD skipped]", t.damaged);
        std::printf("\n");

        check(t.cards > 0,
              std::string(rivendata::stackName(stack.id)) + ": parsed at least one card");
        check(t.commands > 0,
              std::string(rivendata::stackName(stack.id)) + ": scripts contain commands");
    }

    std::printf("cards: %d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
