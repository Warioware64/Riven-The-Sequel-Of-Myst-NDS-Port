#pragma once

// The credits roll, out of extras.MHK.
//
// Nineteen tBMPs, 302 to 320, and they are the last thing Riven shows: the two
// title cards that fade up after an ending's video, and then seventeen images of
// names that scroll past one row at a time. ScummVM caches all nineteen and
// scrolls them by hand (riven_graphics.cpp:671-726); the ids are its
// kRivenCreditsZeroImage..kRivenCreditsLastImage (riven_graphics.h:54-58).
//
// ONE FILE PER IMAGE, and it is not the shape the marbles use. The two outputs
// are read in opposite ways: the marble strip is drawn six pieces at a time and
// so is worth one open, while the credits are read once each, in order, over
// nearly two minutes. Stacking them would also put the result out of the ARM9's
// reach -- loadRpicImage caps a picture at kMaxRpicH = 1024 rows and decodes the
// whole file into RAM (source/data/ImageFile.hpp), and nineteen stacked images
// are 3648 rows and 1.2 MB. One at a time is 67 KB.
//
// THE SIZE IS NOT THE CARD VIEW'S. Every other picture in the game is scaled by
// 256/608 = 0.421, because that is what turns Riven's screen into the DS's card
// view. The credits are not drawn on a card: they take the whole bottom screen
// with the letterbox off, so the fit that matters is 392 rows into 192 -- 0.49,
// and a 360-wide image comes out 176 wide with room to spare either side. That is
// 16% more picture than the card scale would have given, on the one screen in the
// game that is nothing but small text.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace riven
{

/// tBMP ids of the credits images in extras.MHK. 302 and 303 are the two stills
/// that fade up and hold; 304 to 320 are the scroll.
inline constexpr int kCreditsFirstId = 302;
inline constexpr int kCreditsLastId = 320;
inline constexpr int kCreditsCount = kCreditsLastId - kCreditsFirstId + 1;

/// The first id that scrolls rather than being held. ScummVM's
/// kRivenCreditsSecondImage (riven_graphics.cpp:694 tests `< ` against it).
inline constexpr int kCreditsFirstScrollId = 304;

/// The screen the roll is drawn on: the whole bottom panel, no letterbox.
inline constexpr int kCreditsViewW = 256;
inline constexpr int kCreditsViewH = 192;

/// How big a `w`x`h` credits image comes out on that screen.
///
/// Height first, because the roll's timing is measured in rows and the two
/// stills have to be whole: 392 source rows become 192. Width follows the aspect
/// and is clamped to the screen, so a release whose credits are wider than they
/// are tall loses rows rather than being stretched.
///
/// Public because it IS the geometry argument in the file comment, and because
/// it is the only part of this stage that can be tested without a copy of
/// extras.MHK to hand.
void creditsSize(int w, int h, int &outW, int &outH);

struct CreditsResult
{
    bool ok = false;
    std::string error;
    std::size_t bytes = 0;
    int images = 0;
    /// How big one image came out. Reported because it is the whole geometry
    /// argument above, and a release whose credits are not 360x392 should say so
    /// in the log rather than quietly scroll at the wrong speed.
    int width = 0;
    int height = 0;
};

/// extras.MHK -> extras/credits/302.rpic .. 320.rpic.
///
/// Each image is scaled to `kCreditsViewH` rows, keeping its aspect and clamped
/// to `kCreditsViewW` across. Anything that does not decode is reported through
/// `warnings` and skipped; the rest are still written, because a roll with a hole
/// in it is better than no roll. `ok` is false only when NONE of them decoded.
CreditsResult convertCredits(const std::filesystem::path &extrasMhk,
                             const std::filesystem::path &outDir,
                             std::vector<std::string> &warnings);

} // namespace riven
