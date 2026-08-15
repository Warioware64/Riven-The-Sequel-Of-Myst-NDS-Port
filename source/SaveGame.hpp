#pragma once

// Saved games: five slots on the card, written by the player and by nothing
// else. Modelled on the Myst port's SaveGame, including the file layout, for
// the reason its header gives -- the format is the part that has to survive a
// rebuild, and a slot written by one build has to be listable by the next.
//
// A SAVE IS THE ENGINE'S PERSISTENT STATE, NOT A MEMORY IMAGE. Everything a
// card re-derives when it is entered is deliberately absent: the hotspot enable
// flags, the loaded picture, the running movies, the ambient slots, the
// composited surface. Entering a card rebuilds all of it (Engine::enterCard),
// so saving it would only create a second, staler copy that could disagree.
//
// AND IT IS TINY, WHICH IS THE ONE PLACE THIS DIVERGES FROM MYST. Myst's
// SaveState needed about sixty fields because its getVar() routed the imager,
// the observatory, the page inventory and the tower away from the flat map and
// computed them from dedicated structs -- so a loader there had to restore the
// STRUCTS, and writing the computed var ids back into the map would have been
// silently ignored. Riven has no such routing. Vars::vars_ genuinely is the
// state, so this file saves a map, a stack and a card id and stops.
//
// THREE THINGS ARE LEFT OUT ON PURPOSE, each because it was checked rather than
// assumed:
//
//   * No card stack. Myst carried a std::vector<uint16_t> for its opcodes 17
//     and 18. Riven's "go back" is two ordinary variables, ReturnStackId and
//     ReturnCardId (Inventory::click, Externals::backFromItem), so it rides
//     inside the variable map for free.
//   * No ambient bed. mainAmbientId_ is only the "is this the same bed still
//     sounding" latch (Engine::playSlst), and Riven's card entry activates the
//     default SLST 1 unconditionally -- so a load that stops all ambient first
//     gets the right music back from the card itself. Myst had to re-prime its
//     background because most of ITS cards' sound action is "continue", which
//     would have left a directly-entered card silent.
//   * No settings. AZip and WaterEnabled live in the variable map because
//     Riven's own scripts read them (Settings::apply), but they are the
//     PLAYER'S, not the save's. Engine::restoreFrom calls settings.apply()
//     after restoring so a slot saved with the water off cannot turn it off
//     again for someone who has since turned it on.
//
// A RAW HEADER IN FRONT OF A YAS PAYLOAD. Keeping the header raw is what lets
// the menu list five slots with five short reads instead of five full
// deserializations.
//
//   offset size field
//     0     4   magic 'RSAV'
//     4     2   format version
//     6     1   stackId          } duplicated in the payload -- see readSlot
//     7     2   cardId           }
//     9     8   when, unix seconds, 0 when the clock is unset
//    17     4   payloadBytes
//    21     4   payloadCrc, CRC-32 of exactly those bytes
//    25    ..   yas payload (SaveState::serialize)
//
// THE LENGTH AND THE CHECKSUM ARE NOT BELT AND BRACES. THEY ARE THE ONLY THING
// KEEPING A BAD SLOT FROM CRASHING THE MACHINE. A yas binary archive is not
// self-describing: it carries no length of its own, and a serialized string or
// vector begins with a count that yas trusts absolutely. Hand it a truncated or
// corrupted payload and it reads a count of nine million, runs off the end of
// the buffer and THROWS -- and the ARM9 is built -fno-exceptions, so that throw
// is std::terminate and a dead console, not a failed load.
//
// So nothing here may reach yas::load until the bytes have been proved to be
// the bytes that were written. StackFile.cpp makes the same argument about the
// card data and solves it the same way, with a length in a raw header; a save
// needs the checksum on top because a stack file is written once by the
// converter on a PC, while a save is written by a handheld console onto a
// removable card that can be pulled mid-write and read back on a bad sector.
// "Damaged" has to be a verdict this code can actually reach.
//
// DELIBERATELY FREE OF <nds.h>, like RcurFile.hpp and RvidFile.cpp and for the
// same reason: the host test compiles this and checks that a slot written here
// reads back as the same game, and that four kinds of damaged file are each
// refused. On hardware nothing can check that. It is also why the saves
// directory is handed in through setDirectory() rather than read out of the
// Global singleton, which is an <nds.h> translation unit.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "RivenData.hpp"
#include "RivenVars.hpp"

namespace rivenrt
{
namespace SaveGame
{

/// Five, like Myst's, and for a player's reason rather than a technical one: it
/// is enough to keep a branch point and still have somewhere to put today's
/// game, and few enough that every slot fits on one menu screen with no paging.
inline constexpr int kSlotCount = 5;

/// Bumped whenever SaveState's layout changes. yas is told no_header, so the
/// archive is not self-describing and THIS is the only thing standing between a
/// new build and an old file: a mismatch is reported as damaged rather than
/// deserialized into the wrong fields.
///
/// Changelog, one line per bump:
///   1: first version -- vars, zip destinations, stack and card.
///   2: the frame clock, for the timers that store a deadline in it.
inline constexpr std::uint16_t kVersion = 2;

/// Everything a running game cannot be put back together without.
struct SaveState
{
    /// FIRST TWO FIELDS, and they stay first. readSlot compares them against
    /// the copies in the raw header once the payload has been decoded -- a last
    /// check, after the length and the checksum, that the thing which came out
    /// is about the same game the header advertised.
    std::uint8_t stackId = 0;
    std::uint16_t cardId = 0; ///< local id, the one Engine::changeToCard takes

    /// The whole of Riven's state. Keyed by the raw VarId integer, which is safe
    /// because RivenVars.hpp makes the enum's order a contract: names are
    /// appended and never renumbered. If that ever stops being true, kVersion
    /// has to move with it.
    std::unordered_map<std::uint16_t, std::uint32_t> vars;

    /// Zip destinations: visited-card history, not a variable, so startNewGame
    /// cannot clear it and it cannot ride inside `vars` either.
    struct ZipDest
    {
        std::uint8_t stackId = 0;
        std::uint16_t cardId = 0;
        std::string name;

        template <class Ar> void serialize(Ar &ar) { ar &stackId &cardId &name; }
    };
    std::vector<ZipDest> zipDests;

    /// Engine::clock(), the free-running frame counter.
    ///
    /// Not a variable either, and not optional. Riven has three timed things --
    /// jspit's sunners, gspit's wharks and bspit's Ytram trap -- and every one
    /// of them stores its deadline as a READING of this clock: `bytramtime` is
    /// `clock() + N`, not `N`. A save that dropped the clock would restore a
    /// deadline in the millions against a counter starting at zero, and the trap
    /// the player set before saving would never go off again. The bug was
    /// already here, latent, before Boiler Island made it reachable.
    std::uint32_t clock = 0;

    template <class Ar> void serialize(Ar &ar)
    {
        // APPEND ONLY, and stackId/cardId stay first: readSlot's last guard
        // compares the decoded pair against the raw header.
        ar &stackId &cardId &vars &zipDests &clock;
    }
};

/// What the menu needs to draw a row, read from the seventeen-byte header
/// alone.
///
/// `damaged` is kept apart from `used` rather than folded into it so the menu
/// can SAY damaged. A slot holding an unreadable file that listed as empty
/// would be overwritten without anyone being asked, and the file might be the
/// only copy of a game the player still wants off the card.
struct SlotInfo
{
    bool used = false;    ///< a header that parses and names a real place
    bool damaged = false; ///< a file is there and is not one of ours
    std::uint8_t stackId = 0;
    std::uint16_t cardId = 0;
    std::uint64_t when = 0;
};

/// Point the slot functions at <data>/saves/. Called once, after FAT is up.
/// Everything below is a no-op until it is, which is what keeps a build with no
/// card from writing slot files into the working directory.
void setDirectory(const std::string &dir);

/// Full path of a slot's file. Slots are 1-based in this whole API.
std::string slotPath(int slot);
bool validSlot(int slot);

/// Read a slot's header. Never fails: a slot with no file comes back with
/// neither flag set, which is the third state the menu needs.
SlotInfo readSlotInfo(int slot);

/// A menu row: "2.  Jungle Island  14/08 19:47", or "- empty -" / "- damaged -".
/// The date is dropped entirely when the clock was unset rather than shown as
/// an epoch time, which would read as a real date from 1970.
std::string slotLabel(int slot, const SlotInfo &info);

/// Serialize to memory, then write header and payload in one pass. False if the
/// card is full, write-protected or absent -- and a partial file is removed
/// rather than left to list as damaged.
bool writeSlot(int slot, const SaveState &state);

/// Four guards deep; see the .cpp. False means do not load this, and the caller
/// should say WHICH failure it was, because "load failed" tells a player
/// nothing they can act on.
bool readSlot(int slot, SaveState &state);

/// Remove a slot's file. False if there was nothing there or it would not go.
bool removeSlot(int slot);

} // namespace SaveGame
} // namespace rivenrt
