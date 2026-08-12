#pragma once

// MLST (movie list) and FLST (water-effect list) parsers.
//
// These two, plus SFXE (WaterEffect.hpp), are the Riven resources libvaht does
// not implement -- everything else the converter needs (CARD, NAME, PLST, BLST,
// HSPT, SLST, RMAP, scripts, tBMP, tWAV) comes from it.
//
// Layouts: riven_card.cpp:1121-1155 (MLST) and :894-912 (FLST).

#include <cstdint>
#include <vector>

#include "RivenData.hpp"
#include "riven/ResourceReader.hpp"

namespace riven
{

/// Parse an MLST resource. 22 bytes per record after a uint16 count.
/// Returns an empty vector (and leaves `r` failed) on a truncated resource.
std::vector<rivendata::MovieRec> parseMlst(ResourceReader &r);

/// Parse an FLST resource. 6 bytes per record after a uint16 count.
std::vector<rivendata::FlstRec> parseFlst(ResourceReader &r);

} // namespace riven
