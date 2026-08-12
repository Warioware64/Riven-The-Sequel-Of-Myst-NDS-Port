// Round-trips the shared schema through yas, using the SAME header the ARM9
// compiles (shared/RivenData.hpp) and the SAME archive flags the converter will
// write with.
//
// This is the test that guards the whole design decision behind this port: the
// Myst port shipped JSON and had the DS bake it, so a schema mismatch showed up
// as a parse error on device. Here the converter writes a binary the DS
// memory-loads with no validation of its own beyond the 16-byte header, so a
// silent disagreement between the two sides would corrupt every card. Anything
// that changes a struct in RivenData.hpp should fail here first.

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <yas/mem_streams.hpp>
#include <yas/binary_iarchive.hpp>
#include <yas/binary_oarchive.hpp>

#include "RivenData.hpp"
#include "RivenSfxe.hpp"

namespace
{
    int g_failures = 0;

    void check(bool cond, const char *what)
    {
        if (!cond)
        {
            std::fprintf(stderr, "FAIL: %s\n", what);
            ++g_failures;
        }
    }

    // The flags the converter and the runtime both use. no_header because the
    // file carries StackFileHeader instead, which is versioned and readable
    // without yas.
    constexpr std::size_t kFlags = yas::mem | yas::binary | yas::no_header;

    template <class T> T roundtrip(const T &in)
    {
        yas::mem_ostream os;
        yas::binary_oarchive<yas::mem_ostream, kFlags> oa(os);
        oa & in;

        auto buf = os.get_intrusive_buffer();
        yas::mem_istream is(buf.data, buf.size);
        yas::binary_iarchive<yas::mem_istream, kFlags> ia(is);

        T out{};
        ia & out;
        return out;
    }

    rivendata::Stack makeStack()
    {
        using namespace rivendata;
        Stack s;
        s.id = StackId::Bspit;

        // A card exercising every list and both irregular command encodings.
        Card c;
        c.id = 42;
        c.nameIndex = 7;
        c.zipModePlace = 1;

        c.plst.push_back(PictureRec{1, 1234, Rect{0, 0, 608, 392}});
        c.plst.push_back(PictureRec{2, 1235, Rect{10, 20, 300, 200}});
        c.blst.push_back(BlstRec{1, 1, 9});
        c.flst.push_back(FlstRec{1, 77, 0});
        c.mlst.push_back(MovieRec{1, 5, 2, 100, 50, 0, 0, 0xFFFF, 1, 256, 1});

        SoundRec snd;
        snd.index = 1;
        snd.soundIds = {10, 11};
        snd.volumes = {256, 128};
        snd.balances = {-64, 64};
        snd.u2 = {256, 255};
        snd.fadeFlags = 3;
        snd.globalVolume = 256;
        c.slst.push_back(snd);

        // A plain command.
        Command draw;
        draw.opcode = static_cast<u16>(Op::DrawBitmap);
        draw.args = {1234, 0, 0, 608, 392};

        // opcode 27: the one command whose payload is not a uint16 list.
        Command chg;
        chg.opcode = static_cast<u16>(Op::ChangeStack);
        chg.args = {3, 0x0002, 0x2118}; // stack name id, then a uint32 in halves

        // opcode 8: nested scripts, including the 0xFFFF default arm.
        Command sw;
        sw.opcode = static_cast<u16>(Op::Switch);
        sw.switchVar = 55;
        SwitchBranch b0;
        b0.value = 0;
        b0.commands.push_back(draw);
        SwitchBranch bDef;
        bDef.value = kSwitchDefaultValue;
        bDef.commands.push_back(chg);
        sw.branches = {b0, bDef};

        Handler h;
        h.event = static_cast<u16>(ScriptEvent::CardEnter);
        h.commands = {draw, chg, sw};
        c.scripts.push_back(h);

        Hotspot hs;
        hs.blstId = 9;
        hs.nameRes = -1;
        hs.rect = Rect{100, 100, 200, 200};
        hs.cursor = 3;
        hs.index = 0;
        hs.transitionOffset = -1;
        hs.flags = kHotspotEnabled;
        hs.scripts.push_back(h);
        c.hotspots.push_back(hs);

        s.cards.push_back(c);

        // A second card so findCard's binary search has something to search.
        Card c2;
        c2.id = 100;
        s.cards.push_back(c2);

        s.rmap = {0x1F04, 0x2E76, 0x22118};
        s.names[kCardNames].names = {"bspit_start", "bdome"};
        s.names[kCardNames].sortedIndex = {1, 0};
        s.names[kStackNames].names = {"ospit", "pspit", "rspit"};
        return s;
    }
} // namespace

int main()
{
    using namespace rivendata;

    // --- header ------------------------------------------------------------
    {
        StackFileHeader h{};
        std::memcpy(h.magic, kStackMagic, 4);
        h.schemaVersion = kSchemaVersion;
        h.stackId = static_cast<u8>(StackId::Bspit);
        h.cardCount = 2;
        h.payloadBytes = 1234;
        check(headerLooksValid(h), "a well-formed header validates");

        StackFileHeader bad = h;
        bad.schemaVersion = kSchemaVersion + 1;
        check(!headerLooksValid(bad), "a version mismatch is rejected");

        StackFileHeader badMagic = h;
        badMagic.magic[0] = 'X';
        check(!headerLooksValid(badMagic), "a bad magic is rejected");
    }

    // --- geometry ----------------------------------------------------------
    {
        check(toDsX(kCardW) == kViewW, "608 scales to exactly 256");
        check(toDsY(kCardH) == kViewH, "392 scales to 165");
        check(toDsX(0) == 0, "origin is preserved");
        check(kViewOffsetY > 0 && kViewOffsetY * 2 + kViewH <= kScreenH,
              "the view is letterboxed inside 256x192");
    }

    // --- stack round-trip --------------------------------------------------
    {
        const Stack in = makeStack();
        const Stack out = roundtrip(in);

        check(out.id == in.id, "stack id survives");
        check(out.cards.size() == in.cards.size(), "card count survives");
        check(out.rmap == in.rmap, "rmap survives");
        check(out.names[kCardNames].names == in.names[kCardNames].names,
              "name list survives");
        check(out.names[kCardNames].sortedIndex == in.names[kCardNames].sortedIndex,
              "name sort index survives");

        const Card &a = in.cards[0];
        const Card &b = out.cards[0];
        check(b.id == a.id && b.nameIndex == a.nameIndex
                  && b.zipModePlace == a.zipModePlace,
              "card header fields survive");
        check(b.plst.size() == 2 && b.plst[1].id == 1235
                  && b.plst[1].rect.right == 300,
              "PLST survives");
        check(b.blst.size() == 1 && b.blst[0].hotspotId == 9, "BLST survives");
        check(b.flst.size() == 1 && b.flst[0].sfxeId == 77, "FLST survives");
        check(b.mlst.size() == 1 && b.mlst[0].highBoundTime == 0xFFFF
                  && b.mlst[0].left == 100,
              "MLST survives");
        check(b.slst.size() == 1 && b.slst[0].soundIds.size() == 2
                  && b.slst[0].balances[0] == -64,
              "SLST survives, including signed balances");
        check(b.hotspots.size() == 1 && b.hotspots[0].transitionOffset == -1,
              "hotspot survives, including its -1 sentinel");

        // The two irregular command encodings.
        const auto &cmds = b.scripts.at(0).commands;
        check(cmds.size() == 3, "handler command count survives");
        check(cmds[0].args.size() == 5 && cmds[0].args[3] == 608,
              "a simple command's args survive");
        check(changeStackNameId(cmds[1]) == 3,
              "changeStack name id reads back");
        check(changeStackGlobalCardId(cmds[1]) == 0x22118,
              "changeStack recombines its uint32 card id");
        check(cmds[2].switchVar == 55, "switch control variable survives");
        check(cmds[2].branches.size() == 2, "switch arm count survives");
        check(cmds[2].branches[1].value == kSwitchDefaultValue,
              "the default arm keeps its 0xFFFF marker");
        check(cmds[2].branches[0].commands.size() == 1,
              "commands nested inside a switch arm survive");
        check(changeStackGlobalCardId(cmds[2].branches[1].commands[0]) == 0x22118,
              "a changeStack nested inside a switch survives");
    }

    // --- lookups -----------------------------------------------------------
    {
        const Stack s = makeStack();
        check(s.findCard(42) != nullptr && s.findCard(42)->id == 42,
              "findCard finds the first card");
        check(s.findCard(100) != nullptr, "findCard finds the last card");
        check(s.findCard(43) == nullptr, "findCard misses cleanly");
        check(s.localCardForGlobal(0x2E76) == 1, "rmap reverse lookup works");
        check(s.localCardForGlobal(0xDEAD) == -1, "rmap reverse lookup misses cleanly");
    }

    // --- water effects -----------------------------------------------------
    {
        SfxeEffect fx;
        fx.id = 77;
        fx.left = 10;
        fx.top = 20;
        fx.right = 200;
        fx.bottom = 100;
        fx.speed = 15;
        fx.copies.push_back(SfxeCopy{1, 2, 3, 4, 5});
        fx.copies.push_back(SfxeCopy{6, 7, 8, 9, 10});
        fx.frames.push_back(SfxeFrame{0, 2});

        const SfxeEffect out = roundtrip(fx);
        check(out.id == 77 && out.speed == 15, "sfxe header survives");
        check(out.copies.size() == 2 && out.copies[1].rowWidth == 10,
              "sfxe copies survive");
        check(out.frames.size() == 1 && out.frames[0].copyCount == 2,
              "sfxe frames survive");
    }

    if (g_failures == 0)
        std::printf("schema round-trip: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
