// The save-file reader and writer the DS uses, compiled for the host.
//
// Not a converter test. It compiles source/SaveGame.cpp -- which is free of
// <nds.h> so that it can be -- writes a slot, reads it back, and checks that
// what came out is the game that went in. On hardware nothing can check that,
// and a save is the one file in this port whose loss the player notices.
//
// The claim it is really here to defend is that A DAMAGED SLOT IS REFUSED
// RATHER THAN BELIEVED. yas binary archives are not self-describing and carry
// no length and no checksum, so nothing inside the payload can report that it
// was cut short -- yas fills a short read with zeros and returns. Every guard
// against that lives in SaveGame.cpp's own header handling, and the four ways
// a slot can be wrong (short file, wrong magic, wrong version, header
// disagreeing with the payload) are each checked here because the failure mode
// otherwise is a save that loads into a game which is subtly not the one that
// was saved.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "SaveGame.hpp"

namespace fs = std::filesystem;
using namespace rivenrt;

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

    /// A game with something in every field, so a member dropped from
    /// SaveState::serialize cannot look like a pass.
    SaveGame::SaveState makeState()
    {
        SaveGame::SaveState s;
        s.stackId = static_cast<std::uint8_t>(rivendata::StackId::Jspit);
        s.cardId = 0x1234;
        // Spread across the enum's range rather than bunched at the bottom: the
        // key is a raw integer and a narrowing bug would only show above 255.
        s.vars.emplace(1, 1u);
        s.vars.emplace(77, 0xDEADBEEFu);
        s.vars.emplace(181, 0xFFFFFFFFu);
        s.zipDests.push_back({static_cast<std::uint8_t>(rivendata::StackId::Tspit), 155,
                              "tem_dome"});
        s.zipDests.push_back({static_cast<std::uint8_t>(rivendata::StackId::Bspit), 42,
                              "boiler"});
        // Big enough that a truncation to 16 bits would show: this is a frame
        // counter and a long session reaches six figures.
        s.clock = 0x00BADA55u;
        return s;
    }

    bool sameState(const SaveGame::SaveState &a, const SaveGame::SaveState &b)
    {
        if (a.stackId != b.stackId || a.cardId != b.cardId)
            return false;
        if (a.clock != b.clock)
            return false;
        if (a.vars != b.vars)
            return false;
        if (a.zipDests.size() != b.zipDests.size())
            return false;
        for (std::size_t i = 0; i < a.zipDests.size(); ++i)
        {
            if (a.zipDests[i].stackId != b.zipDests[i].stackId
                || a.zipDests[i].cardId != b.zipDests[i].cardId
                || a.zipDests[i].name != b.zipDests[i].name)
                return false;
        }
        return true;
    }

    std::vector<std::uint8_t> readFile(const std::string &path)
    {
        std::vector<std::uint8_t> out;
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (f == nullptr)
            return out;
        std::fseek(f, 0, SEEK_END);
        out.resize(static_cast<std::size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(out.data(), 1, out.size(), f) != out.size())
            out.clear();
        std::fclose(f);
        return out;
    }

    void writeFile(const std::string &path, const std::vector<std::uint8_t> &bytes)
    {
        std::FILE *f = std::fopen(path.c_str(), "wb");
        if (f == nullptr)
            return;
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
    }
} // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "riven_save_arm9_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    // The trailing separator is part of the contract: slotPath concatenates.
    SaveGame::setDirectory(dir.string() + "/");

    // --- an untouched directory lists as five empty slots ------------------
    for (int i = 1; i <= SaveGame::kSlotCount; ++i)
    {
        const SaveGame::SlotInfo info = SaveGame::readSlotInfo(i);
        check(!info.used && !info.damaged, "an absent slot is neither used nor damaged");
        check(SaveGame::slotLabel(i, info).find("empty") != std::string::npos,
              "an absent slot says empty");
    }

    // --- round trip ---------------------------------------------------------
    const SaveGame::SaveState original = makeState();
    check(SaveGame::writeSlot(2, original), "writeSlot succeeds");

    SaveGame::SaveState loaded;
    check(SaveGame::readSlot(2, loaded), "readSlot succeeds");
    check(sameState(original, loaded), "the game that came out is the one that went in");

    const SaveGame::SlotInfo info = SaveGame::readSlotInfo(2);
    check(info.used && !info.damaged, "a good slot lists as used");
    check(info.stackId == original.stackId && info.cardId == original.cardId,
          "the header names the same place as the payload");
    // displayName, not the on-disc stack name: this row is read by a player.
    check(SaveGame::slotLabel(2, info).find("Jungle Island") != std::string::npos,
          "the row names the island");

    // Slots do not leak into each other.
    check(!SaveGame::readSlotInfo(1).used, "writing slot 2 leaves slot 1 empty");

    const std::string path = SaveGame::slotPath(2);
    const std::vector<std::uint8_t> good = readFile(path);
    constexpr std::size_t kHeaderBytes = 25;
    check(good.size() > kHeaderBytes, "the file is a header plus a payload");

    // --- the ways a slot can be wrong ---------------------------------------
    //
    // Every one of these must come back as false rather than as a throw. The
    // ARM9 is built -fno-exceptions, so a yas exception there is std::terminate
    // -- which means a test that CRASHES here is reporting the same bug a test
    // that fails here is, and neither is a save that merely failed to load.

    // 1. Truncated to the header alone. This is the card-pulled-mid-write case.
    writeFile(path, std::vector<std::uint8_t>(good.begin(), good.begin() + kHeaderBytes));
    check(!SaveGame::readSlot(2, loaded), "a payload-less file is refused");

    // 2. Wrong magic.
    {
        std::vector<std::uint8_t> bad = good;
        bad[0] ^= 0xFF;
        writeFile(path, bad);
        check(!SaveGame::readSlot(2, loaded), "a bad magic is refused");
        check(SaveGame::readSlotInfo(2).damaged, "a bad magic lists as DAMAGED, not empty");
    }

    // 3. Wrong version. The only guard there is against an older layout being
    //    deserialized into today's fields, because yas was told no_header.
    {
        std::vector<std::uint8_t> bad = good;
        bad[4] = static_cast<std::uint8_t>(bad[4] + 1);
        writeFile(path, bad);
        check(!SaveGame::readSlot(2, loaded), "a version bump is refused");
        check(SaveGame::readSlotInfo(2).damaged, "a version bump lists as damaged");
    }

    // 4. An intact header over a truncated payload. The header still parses, so
    //    readSlotInfo is happy and the MENU offers the slot; only readSlot can
    //    tell, and only because the length in the header disagrees.
    //
    //    This is the case that first crashed this test rather than failing it:
    //    the length check used to happen AFTER yas::load, and yas ran off the
    //    end of the buffer and threw before anything could compare anything.
    {
        writeFile(path, std::vector<std::uint8_t>(good.begin(), good.end() - 1));
        check(SaveGame::readSlotInfo(2).used, "a truncated tail still has a good header");
        check(!SaveGame::readSlot(2, loaded), "a truncated payload is refused, not parsed");
    }

    // 5. A payload of the right LENGTH with the wrong bytes in it. The length
    //    check cannot see this one; only the checksum can. Every byte is tried,
    //    because a serialized container's count field is the dangerous one and
    //    it is only a few bytes wide -- flipping a bit in the count is what
    //    makes yas allocate nine million elements and throw.
    for (std::size_t i = kHeaderBytes; i < good.size(); ++i)
    {
        std::vector<std::uint8_t> bad = good;
        bad[i] ^= 0xFF;
        writeFile(path, bad);
        if (SaveGame::readSlot(2, loaded))
        {
            check(false, "a corrupted payload byte was accepted");
            break;
        }
    }
    ++g_checks;

    // 6. A header whose length field is a lie in the other direction: it claims
    //    far more payload than the file holds. Nothing may allocate from it.
    {
        std::vector<std::uint8_t> bad = good;
        bad[17] = 0xFF;
        bad[18] = 0xFF;
        bad[19] = 0xFF;
        bad[20] = 0x7F;
        writeFile(path, bad);
        check(!SaveGame::readSlot(2, loaded), "an absurd payload length is refused");
    }

    // --- a slot with nowhere in it -----------------------------------------
    //
    // Card 0 is the "no card" sentinel and StackId::None the "no stack" one, so
    // either means the bytes are not a save whatever else they are. Refused on
    // the way OUT as well as in, so we cannot write a file we would not read.
    {
        SaveGame::SaveState nowhere = makeState();
        nowhere.cardId = 0;
        check(!SaveGame::writeSlot(3, nowhere), "a save naming card 0 is refused");
        nowhere = makeState();
        nowhere.stackId = static_cast<std::uint8_t>(rivendata::StackId::None);
        check(!SaveGame::writeSlot(3, nowhere), "a save naming no stack is refused");
        check(!fs::exists(SaveGame::slotPath(3)), "a refused save leaves no file behind");
    }

    // --- overwriting a damaged slot repairs it ------------------------------
    check(SaveGame::readSlotInfo(2).used, "slot 2 is still occupied by the truncated file");
    check(SaveGame::writeSlot(2, original), "a damaged slot can be saved over");
    check(SaveGame::readSlot(2, loaded) && sameState(original, loaded),
          "saving over a damaged slot repairs it");

    // --- slot bounds --------------------------------------------------------
    check(!SaveGame::validSlot(0) && !SaveGame::validSlot(SaveGame::kSlotCount + 1),
          "slots are 1-based and bounded");
    check(!SaveGame::readSlot(0, loaded), "slot 0 does not read");
    check(!SaveGame::readSlot(SaveGame::kSlotCount + 1, loaded), "slot 6 does not read");

    // --- removal ------------------------------------------------------------
    check(SaveGame::removeSlot(2), "a slot can be removed");
    check(!SaveGame::readSlotInfo(2).used && !SaveGame::readSlotInfo(2).damaged,
          "a removed slot is empty again");
    check(!SaveGame::removeSlot(2), "removing an absent slot reports nothing removed");

    fs::remove_all(dir, ec);

    std::printf("%s: %d checks, %d failures\n", g_failures == 0 ? "PASS" : "FAIL", g_checks,
                g_failures);
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
