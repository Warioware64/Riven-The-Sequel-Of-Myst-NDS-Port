#pragma once

// MLST (movie list) and FLST (water-effect list) parsers.
//
// These two, plus SFXE (WaterEffect.hpp), are the Riven resources libvaht does
// not implement -- everything else the converter needs (CARD, NAME, PLST, BLST,
// HSPT, SLST, RMAP, scripts, tBMP, tWAV) comes from it.
//
// Layouts: riven_card.cpp:1121-1155 (MLST) and :894-912 (FLST).

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "RivenData.hpp"
#include "riven/Archive.hpp"
#include "riven/ResourceReader.hpp"

namespace riven
{

/// Parse an MLST resource. 22 bytes per record after a uint16 count.
/// Returns an empty vector (and leaves `r` failed) on a truncated resource.
std::vector<rivendata::MovieRec> parseMlst(ResourceReader &r);

/// Where a movie is drawn, in Riven's original 608x392 coordinates.
struct MoviePlacement
{
    std::int16_t left = 0;
    std::int16_t top = 0;
};

/// The MLST placement of every movie in a stack, keyed by tMOV id.
///
/// The video stage needs this because a movie's DS size is not a property of
/// the movie alone: an overlay of card-width W drawn at left L covers DS columns
/// [toDsX(L), toDsX(L+W)), which is floor(W*s) wide or one more depending on L.
/// VideoPipeline used to scale the length and lose that pixel.
///
/// ONE position per movie, and that is not an approximation: across the eight
/// stacks of a 5-CD install, all 1055 movies appear at exactly one (left, top).
/// A movie placed twice keeps the first record seen -- cards are walked in id
/// order, so that is stable -- and the count of conflicts is returned so the
/// caller can say something rather than silently picking one.
std::unordered_map<std::uint16_t, MoviePlacement>
collectMoviePlacements(const ArchiveSet &set, int *conflicts = nullptr);

/// Parse an FLST resource. 6 bytes per record after a uint16 count.
std::vector<rivendata::FlstRec> parseFlst(ResourceReader &r);

} // namespace riven
