#pragma once

// Riven card-graph data model, shared verbatim between the host converter
// (tools/riven-convert) and the ARM9 runtime.
//
// This header depends ONLY on yas + the standard library. No <nds.h>, no
// Global, nothing device-side -- that is what lets the converter link the exact
// same structs the DS deserializes, and what lets the round-trip tests run on
// the host. Keep it that way.
//
// This is the file that replaces the Myst port's JSON pipeline. There, Python
// wrote nodes/<stack>.json, the DS parsed it with nlohmann on first boot and
// baked it to yasNode/<stack>.bin (MystNode.hpp: bakeStack / bakeAllIfNeeded /
// deleteStackJson). Here the converter yas-serializes straight to
// stacks/<stack>.bin, so there is no JSON on the card, no nlohmann in the ROM
// and no first-boot bake screen.
//
// Field layouts mirror Riven's on-disc resources one-for-one; the reference is
// ScummVM's engines/mohawk/riven_card.cpp and riven_scripts.cpp, cited per
// struct. Everything on disc is big-endian and the DS is little-endian, so the
// converter byte-swaps once at parse time and nothing downstream ever swaps.
//
// COORDINATES: every rect and position in here is in Riven's ORIGINAL 608x392
// card space, never in DS pixels. The runtime scales at hit-test and draw time
// via toDsX()/toDsY() below. This is deliberate: the Myst port pre-scaled rects
// in its converter and had to keep two scale constants (ds_texture.SCALE and
// myst_nodes.NDS_SCALE) in sync by hand, and Riven's slider/drag maths is
// written against original coordinates in ScummVM, so it ports verbatim only if
// the numbers stay original.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <yas/serialize.hpp>
#include <yas/std_types.hpp>

#include "RivenVars.hpp"

namespace rivendata
{

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using i16 = std::int16_t;
using u32 = std::uint32_t;
using i32 = std::int32_t;

// ---------------------------------------------------------------------------
// Versioning
// ---------------------------------------------------------------------------

/// Bump on ANY change to a struct below, including field order. yas binary
/// archives are not self-describing: without this a converter/ROM mismatch
/// deserializes garbage silently instead of failing.
inline constexpr u16 kSchemaVersion = 2;

/// Fixed-size header written raw ahead of the yas payload in stacks/<stack>.bin.
/// Read it with a plain fread before handing the rest to yas.
struct StackFileHeader
{
    char magic[4];      ///< "RVYN"
    u16 schemaVersion;  ///< kSchemaVersion at write time
    u8 stackId;         ///< StackId, for a cheap sanity check against the filename
    u8 reserved;
    u32 cardCount;      ///< so a loader can size things before deserializing
    u32 payloadBytes;   ///< yas payload length following this header
};
static_assert(sizeof(StackFileHeader) == 16, "StackFileHeader must be 16 bytes");

inline constexpr char kStackMagic[4] = {'R', 'V', 'Y', 'N'};

inline bool headerLooksValid(const StackFileHeader &h)
{
    return h.magic[0] == 'R' && h.magic[1] == 'V' && h.magic[2] == 'Y'
        && h.magic[3] == 'N' && h.schemaVersion == kSchemaVersion;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

/// Riven's card view. The game window is 608x436: this 608x392 picture area
/// plus a 44px inventory strip. riven_graphics.cpp:331-339.
///
/// The strip does NOT live on the top screen, which an earlier note here said it
/// would: the top screen is not a touch screen, and the strip's whole purpose is
/// to be clicked. It lives in the lower letterbox band of the bottom screen --
/// see source/render/Inventory.hpp, which also explains why the icons there are
/// not drawn at the scale everything else uses.
inline constexpr int kCardW = 608;
inline constexpr int kCardH = 392;

/// The DS bottom screen. 392 * 256/608 = 165.05, so the card view is 256x165
/// centred in 256x192 with black bands above and below.
inline constexpr int kScreenW = 256;
inline constexpr int kScreenH = 192;
inline constexpr int kViewW = 256;
inline constexpr int kViewH = 165;

/// THE scale, defined once. Anything that needs it -- the converter's image
/// resampler, the video encoder, hit-testing, MLST movie placement -- reads it
/// from here rather than restating the ratio.
inline constexpr int kScaleNum = kViewW;  // 256
inline constexpr int kScaleDen = kCardW;  // 608

inline constexpr int toDsX(int x) { return x * kScaleNum / kScaleDen; }
inline constexpr int toDsY(int y) { return y * kScaleNum / kScaleDen; }
inline constexpr int toCardX(int x) { return x * kScaleDen / kScaleNum; }
inline constexpr int toCardY(int y) { return y * kScaleDen / kScaleNum; }

static_assert(toDsX(kCardW) == kViewW, "horizontal scale must land exactly on 256");
static_assert(toDsY(kCardH) == kViewH, "vertical scale must agree with kViewH");

/// Vertical offset of the card view inside the 256x192 screen.
inline constexpr int kViewOffsetY = (kScreenH - kViewH) / 2;

// ---------------------------------------------------------------------------
// Stacks
// ---------------------------------------------------------------------------

/// Riven's eight stacks. Values match ScummVM's RivenStackId (riven.h:61-75) so
/// ids can be cross-checked against its sources directly.
///
/// Note: ScummVM's comments on Rspit/Tspit have long been swapped -- rspit is
/// the Tay/Rebel Age and tspit is Temple Island. The names here are the correct
/// ones.
enum class StackId : u8
{
    None = 0,
    Ospit = 1,  ///< Gehn's office, the 233rd Age
    Pspit = 2,  ///< Prison Island
    Rspit = 3,  ///< Tay, the Rebel Age
    Tspit = 4,  ///< Temple Island
    Bspit = 5,  ///< Book-Making / Boiler Island
    Gspit = 6,  ///< Garden Island
    Jspit = 7,  ///< Jungle Island
    Aspit = 8,  ///< Main menu, books, setup
};

inline constexpr int kStackCount = 9; ///< including None at index 0

/// On-disc stack name, and the basename used for every converted file.
inline const char *stackName(StackId s)
{
    switch (s)
    {
    case StackId::Ospit: return "ospit";
    case StackId::Pspit: return "pspit";
    case StackId::Rspit: return "rspit";
    case StackId::Tspit: return "tspit";
    case StackId::Bspit: return "bspit";
    case StackId::Gspit: return "gspit";
    case StackId::Jspit: return "jspit";
    case StackId::Aspit: return "aspit";
    default:             return "none";
    }
}

/// Player-facing island name, for save slots and the top-screen status line.
inline const char *displayName(StackId s)
{
    switch (s)
    {
    case StackId::Ospit: return "233rd Age";
    case StackId::Pspit: return "Prison Island";
    case StackId::Rspit: return "Tay";
    case StackId::Tspit: return "Temple Island";
    case StackId::Bspit: return "Boiler Island";
    case StackId::Gspit: return "Garden Island";
    case StackId::Jspit: return "Jungle Island";
    case StackId::Aspit: return "Riven";
    default:             return "Unknown";
    }
}

inline std::string stackFileName(StackId s)
{
    return std::string(stackName(s)) + ".bin";
}

/// Parse a stack name as it appears in the NAME 5 list.
inline StackId parseStackName(const std::string &s)
{
    for (int i = 1; i < kStackCount; ++i)
    {
        const auto id = static_cast<StackId>(i);
        if (s == stackName(id))
            return id;
    }
    return StackId::None;
}

// ---------------------------------------------------------------------------
// Scripts -- riven_scripts.cpp:57-95
// ---------------------------------------------------------------------------

/// Script/event types. riven_scripts.h:40-53; names at riven_scripts.cpp:243-260.
/// 8 (CardFrame) is present in the data even though ScummVM calls it
/// "CardUnknown" -- do not renumber around it.
enum class ScriptEvent : u16
{
    MouseDown = 0,
    MouseDrag = 1,
    MouseUp = 2,
    MouseEnter = 3,
    MouseInside = 4,
    MouseLeave = 5,
    CardLoad = 6,
    CardLeave = 7,
    CardFrame = 8,
    CardEnter = 9,
    CardUpdate = 10,
};

/// Opcodes. Gaps are opcodes the shipped data never uses (riven_scripts.cpp:428-493
/// marks them "empty" or "not used"); they are listed so the dispatch table can
/// be dense and so an unexpected one is recognisably an unimplemented opcode
/// rather than a parse error.
enum class Op : u16
{
    Empty = 0,
    DrawBitmap = 1,
    SwitchCard = 2,
    PlayScriptSLST = 3,
    PlaySound = 4,
    SetVariable = 7,
    Switch = 8,                ///< irregular encoding, see Command::branches
    EnableHotspot = 9,
    DisableHotspot = 10,
    StopSound = 12,
    ChangeCursor = 13,
    Delay = 14,
    RunExternalCommand = 17,
    Transition = 18,
    RefreshCard = 19,
    BeginScreenUpdate = 20,
    ApplyScreenUpdate = 21,
    IncrementVariable = 24,
    ChangeStack = 27,          ///< irregular encoding, carries a uint32 card id
    DisableMovie = 28,
    DisableAllMovies = 29,
    EnableMovie = 31,
    PlayMovieBlocking = 32,
    PlayMovie = 33,
    StopMovie = 34,
    Unk36 = 36,
    FadeAmbientSounds = 37,
    StoreMovieOpcode = 38,
    ActivatePLST = 39,
    ActivateSLST = 40,
    ActivateMLSTAndPlay = 41,
    ActivateBLST = 43,
    ActivateFLST = 44,
    ZipMode = 45,
    ActivateMLST = 46,
};

struct Command;

/// One arm of an opcode-8 switch. `value` 0xFFFF is the default branch
/// (riven_scripts.cpp:884-908).
struct SwitchBranch
{
    u16 value = 0;
    std::vector<Command> commands;

    template <class Ar> void serialize(Ar &ar) { ar & value & commands; }
};

inline constexpr u16 kSwitchDefaultValue = 0xFFFF;

/// One script command.
///
/// Simple commands carry `opcode` + `args` exactly as encoded. The two
/// irregular forms both keep their wire encoding rather than being unpacked
/// into dedicated fields, so this struct stays small (Riven has tens of
/// thousands of commands per big stack and they are all resident while their
/// stack is loaded):
///
///   - opcode 8 (Switch) puts the control variable in `switchVar` and the arms
///     in `branches`; `args` is empty.
///   - opcode 27 (ChangeStack) is the one command whose payload is not a list
///     of uint16 (riven_scripts.cpp:934-940). Its three words land in `args` as
///     { stackNameId, cardIdHi, cardIdLo }; use the accessors below.
struct Command
{
    u16 opcode = 0;
    u16 switchVar = 0;             ///< opcode 8 only
    std::vector<u16> args;
    std::vector<SwitchBranch> branches; ///< opcode 8 only

    template <class Ar> void serialize(Ar &ar)
    {
        ar & opcode & switchVar & args & branches;
    }
};

/// NAME 5 (stack names) id naming the destination stack of a ChangeStack.
inline u16 changeStackNameId(const Command &c)
{
    return c.args.size() >= 1 ? c.args[0] : 0;
}

/// RMAP global card id of a ChangeStack destination, resolved against the
/// target stack's RMAP by the runtime (riven_stack.cpp:138-150).
inline u32 changeStackGlobalCardId(const Command &c)
{
    if (c.args.size() < 3)
        return 0;
    return (static_cast<u32>(c.args[1]) << 16) | c.args[2];
}

/// A script list entry: the commands run for one event.
struct Handler
{
    u16 event = 0; ///< ScriptEvent
    std::vector<Command> commands;

    template <class Ar> void serialize(Ar &ar) { ar & event & commands; }
};

// ---------------------------------------------------------------------------
// Per-card resource lists
// ---------------------------------------------------------------------------

/// Original-coordinate rectangle (608x392 card space).
struct Rect
{
    i16 left = 0, top = 0, right = 0, bottom = 0;

    constexpr int width() const { return right - left; }
    constexpr int height() const { return bottom - top; }

    template <class Ar> void serialize(Ar &ar) { ar & left & top & right & bottom; }
};

/// PLST -- picture list. riven_card.cpp:700-716, 12 bytes/record on disc.
/// `index` is what opcode 39 (ActivatePLST) selects on.
struct PictureRec
{
    u16 index = 0;
    u16 id = 0; ///< tBMP resource id -> pics/<stack>/<id>.rpic
    Rect rect;

    template <class Ar> void serialize(Ar &ar) { ar & index & id & rect; }
};

/// BLST -- hotspot enable list. riven_card.cpp:867-881, 6 bytes/record.
struct BlstRec
{
    u16 index = 0;
    u16 enabled = 0;
    u16 hotspotId = 0; ///< matches Hotspot::blstId

    template <class Ar> void serialize(Ar &ar) { ar & index & enabled & hotspotId; }
};

/// FLST -- water-effect list. riven_card.cpp:894-912, 6 bytes/record.
struct FlstRec
{
    u16 index = 0;
    u16 sfxeId = 0;
    u16 u0 = 0; ///< always 0 in shipped data

    template <class Ar> void serialize(Ar &ar) { ar & index & sfxeId & u0; }
};

/// MLST -- movie list. riven_card.cpp:1121-1155, 22 bytes/record.
/// Position, looping and volume all come from here, never from the movie file
/// (riven_scripts.cpp:773-779).
struct MovieRec
{
    u16 index = 0;
    u16 movieId = 0;       ///< tMOV resource id -> video/<stack>/<id>.rvid
    u16 slot = 0;          ///< "code": the playback slot opcodes 28/31-34 address
    i16 left = 0, top = 0; ///< blit position, original coordinates
    u16 lowBoundTime = 0;  ///< always 0 in shipped data
    u16 startTime = 0;     ///< always 0
    u16 highBoundTime = 0; ///< always 0xFFFF
    u16 loop = 0;
    u16 volume = 0;
    u16 rate = 0;          ///< always 1

    template <class Ar> void serialize(Ar &ar)
    {
        ar & index & movieId & slot & left & top & lowBoundTime & startTime
            & highBoundTime & loop & volume & rate;
    }
};

/// SLST -- ambient sound list. riven_card.cpp:735-783, riven_sound.h:42-53.
/// The four per-sound arrays are parallel and all `soundIds.size()` long.
struct SoundRec
{
    u16 index = 0;
    std::vector<u16> soundIds; ///< tWAV ids -> sound/<stack>/<id>.rsnd
    std::vector<u16> volumes;
    std::vector<i16> balances; ///< <0 left, 0 centre, >0 right
    std::vector<u16> u2;       ///< always 255 or 256
    u16 fadeFlags = 0;         ///< 1 = fade out previous, 2 = fade in new
    u16 loop = 0;
    u16 globalVolume = 0;      ///< 0..256, applied as vol*global/256
    u16 u0 = 0;                ///< always 0 or 1
    u16 suspend = 0;           ///< always 0 in shipped data

    template <class Ar> void serialize(Ar &ar)
    {
        ar & index & soundIds & volumes & balances & u2 & fadeFlags & loop
            & globalVolume & u0 & suspend;
    }
};

/// HSPT -- one hotspot. riven_card.cpp:806-820 (list), :1262-1292 (record).
/// 22 fixed bytes then a script list.
struct Hotspot
{
    u16 blstId = 0;            ///< the id BLST records and opcodes 9/10 use
    i16 nameRes = -1;          ///< index into NAME 2; -1 = unnamed
    Rect rect;                 ///< invalid rects exist in the data (tspit 371/377)
    u16 u0 = 0;
    u16 cursor = 0;
    u16 index = 0;             ///< draw/priority order within the card
    i16 transitionOffset = -1; ///< -1 = no overlap
    u16 flags = 0;
    std::vector<Handler> scripts;

    template <class Ar> void serialize(Ar &ar)
    {
        ar & blstId & nameRes & rect & u0 & cursor & index & transitionOffset
            & flags & scripts;
    }
};

/// Hotspot::flags bits. riven_card.h:280-283.
inline constexpr u16 kHotspotZip = 1 << 0;
inline constexpr u16 kHotspotEnabled = 1 << 1;

/// One card: the CARD record plus every per-card list, keyed by the same
/// resource id. CARD itself is riven_card.cpp:62-70.
struct Card
{
    u16 id = 0;
    i16 nameIndex = -1;    ///< index into NAME 1; <0 = unnamed
    u16 zipModePlace = 0;  ///< non-zero => registers as a zip destination

    std::vector<Handler> scripts;
    std::vector<PictureRec> plst;
    std::vector<BlstRec> blst;
    std::vector<FlstRec> flst;
    std::vector<MovieRec> mlst;
    std::vector<SoundRec> slst;
    std::vector<Hotspot> hotspots;

    template <class Ar> void serialize(Ar &ar)
    {
        ar & id & nameIndex & zipModePlace & scripts & plst & blst & flst & mlst
            & slst & hotspots;
    }
};

/// One NAME resource. riven_stack.cpp:444-474. `sortedIndex` is the original's
/// case-insensitive binary-search order over `names`; it is kept rather than
/// rebuilt so lookups match the original exactly.
struct NameList
{
    std::vector<std::string> names;
    std::vector<u16> sortedIndex;

    template <class Ar> void serialize(Ar &ar) { ar & names & sortedIndex; }
};

/// The five NAME lists every stack carries. riven_stack.h:39-45.
enum NameListKind
{
    kCardNames = 0,
    kHotspotNames = 1,
    kExternalCommandNames = 2,
    kVariableNames = 3,
    kStackNames = 4,
    kNameListCount = 5,
};

/// One stack: every card, the RMAP global-id table, and the NAME lists.
/// Stacks are loaded and freed one at a time -- jspit is the worst case at ~814
/// cards across j_Data1 and j_Data2.
struct Stack
{
    StackId id = StackId::None;
    std::vector<Card> cards;                    ///< sorted by id
    std::vector<u32> rmap;                      ///< local card id -> global id
    std::array<NameList, kNameListCount> names;

    /// names[kVariableNames] resolved to the global enum, index for index. The
    /// converter fills this so the runtime never has to look at a variable name:
    /// a script's variable index is a subscript here and the result is the key.
    /// Entries the enum does not know are VarId::Unknown (RivenVars.hpp).
    std::vector<VarId> variableIds;

    template <class Ar> void serialize(Ar &ar)
    {
        ar & id & cards & rmap & names & variableIds;
    }

    /// Cards are sorted by id, so this is a binary search. Returns nullptr when
    /// the stack has no card with that id (RMAP lists ids that have no CARD
    /// resource -- riven_stack.cpp:164-177 skips them too).
    const Card *findCard(u16 cardId) const
    {
        std::size_t lo = 0, hi = cards.size();
        while (lo < hi)
        {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (cards[mid].id < cardId)
                lo = mid + 1;
            else
                hi = mid;
        }
        return (lo < cards.size() && cards[lo].id == cardId) ? &cards[lo] : nullptr;
    }

    /// Reverse of the RMAP table: global id -> local card id. Linear, matching
    /// ScummVM's getCardStackId (riven_stack.cpp:138-150). Returns -1 if absent.
    i32 localCardForGlobal(u32 globalId) const
    {
        for (std::size_t i = 0; i < rmap.size(); ++i)
            if (rmap[i] == globalId)
                return static_cast<i32>(i);
        return -1;
    }
};

} // namespace rivendata
