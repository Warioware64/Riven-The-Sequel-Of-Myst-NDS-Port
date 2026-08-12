#pragma once

// Parsers for Riven's per-card resources, straight into the shared schema.
//
// libvaht can read most of these, but its API drops fields the DS port needs:
// HSPT loses u0, index (draw order) and transitionOffset; SLST loses index, u0,
// suspend and the u2[] array; PLST and BLST lose index, which is the value
// activatePLST/activateBLST select on. Those are not optional -- half the
// opcodes address records by index, and transitionOffset drives transitions.
//
// So libvaht is kept for the two genuinely hard jobs, the MHWK container and
// the tBMP unpacker, and these fixed-layout records are parsed here. Every
// layout below is cited to ScummVM's engines/mohawk/riven_card.cpp, which is
// the reference implementation, and every field is big-endian.
//
// Coordinates stay in Riven's original 608x392 space -- see the note in
// RivenData.hpp about why the DS scales at draw time instead.

#include <cstdint>
#include <optional>
#include <vector>

#include "RivenData.hpp"
#include "riven/ResourceReader.hpp"

namespace riven
{

/// A script list: `uint16 count` then that many `{uint16 event, <script>}`.
/// Used by CARD and by every HSPT record. riven_scripts.cpp:57-95.
///
/// Returns what it managed to read. A truncated or corrupt list yields the
/// handlers parsed so far rather than nothing, matching the salvage policy the
/// rest of the converter follows on damaged source data.
std::vector<rivendata::Handler> parseScriptList(ResourceReader &r);

/// CARD: `int16 nameIndex; uint16 zipModePlace; <script list>`.
/// riven_card.cpp:62-70. Fills only those three fields; the per-card lists are
/// separate resources sharing the card's id.
bool parseCard(ResourceReader &r, rivendata::Card &out);

/// PLST picture list. riven_card.cpp:700-716, 12 bytes/record.
std::vector<rivendata::PictureRec> parsePlst(ResourceReader &r);

/// BLST hotspot-enable list. riven_card.cpp:867-881, 6 bytes/record.
std::vector<rivendata::BlstRec> parseBlst(ResourceReader &r);

/// HSPT hotspot list. riven_card.cpp:806-820 and :1262-1292.
/// 22 fixed bytes per record, then a script list.
std::vector<rivendata::Hotspot> parseHspt(ResourceReader &r);

/// SLST ambient sound list. riven_card.cpp:735-783.
/// Variable length: four parallel arrays sized by the record's soundCount.
std::vector<rivendata::SoundRec> parseSlst(ResourceReader &r);

/// RMAP: a flat array of uint32 covering the whole resource, indexed by local
/// card id. riven_stack.cpp:125-136.
std::vector<std::uint32_t> parseRmap(ResourceReader &r);

/// NAME: count, string offsets, the original's case-insensitive sort order,
/// then NUL-terminated strings. riven_stack.cpp:444-474.
///
/// The sort order is preserved rather than recomputed so that name lookups on
/// the DS resolve exactly as the original engine's binary search did.
rivendata::NameList parseName(ResourceReader &r);

} // namespace riven
