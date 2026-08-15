#pragma once

// The six fire marbles, out of extras.MHK.
//
// Temple Island's marble grid is the one puzzle in Riven whose pieces are not
// in a stack. TSpit::xdrawmarbles asks the graphics layer for extras tBMPs 200
// to 205 (tspit.cpp:324-342), which are six 8x8 images of a coloured marble;
// every other picture the game draws comes out of the island it is on.
//
// They are written as ONE strip rather than six files. The ARM9 redraws all six
// each time a marble is put down, and drawPictureSections was written so that a
// batch like that costs one open and one decode of the SD card instead of six.
//
// (The small marbles -- the ones on the waffle, 4x2 -- are NOT here. Those are
// pre-scaled inside tspit itself, named tsmallred and up, and the ARM9 reaches
// them by name like any other card resource. ScummVM notes the same split:
// "the original seems to scale the marble images from extras.mhk, but we're
// using the pre-scaled images in the stack".)

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace riven
{

/// tBMP ids of the large marbles in extras.MHK, in the order the variables and
/// the hotspots use: red, orange, yellow, green, blue, violet.
inline constexpr int kMarbleFirstId = 200;
inline constexpr int kMarbleCount = 6;

struct MarbleResult
{
    bool ok = false;
    std::string error;
    std::size_t bytes = 0;
    /// How wide and tall one marble came out. Reported because the ARM9 cuts
    /// the strip on this pitch and there is no header on the far end saying so.
    int cellW = 0;
    int cellH = 0;
};

/// extras.MHK -> extras/marbles.rpic, a single row of six marbles.
///
/// Anything that decodes to nothing is skipped and reported through `warnings`;
/// the strip is still written, with a hole where the marble would be, so that
/// five working marbles are not lost to one bad one.
MarbleResult convertMarbles(const std::filesystem::path &extrasMhk,
                            const std::filesystem::path &out,
                            std::vector<std::string> &warnings);

} // namespace riven
