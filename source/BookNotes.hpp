#pragma once

// The notebook: a picture of what the player was looking at, and what they
// wrote on it.
//
// Riven is a game of written-down things -- D'ni numerals on a schoolroom wall,
// the animal that goes with each dome sound, a grid of fire marbles, the colours
// on Gehn's throne -- and the DS has a stylus. So L takes a picture of the view,
// and the notebook lets the player scribble on it. Ported from the Myst port's
// BookNotes, whose header makes the same argument about the same kind of game.
//
// NOTES ARE GLOBAL, NOT PART OF A SAVE SLOT. What the player wrote down is
// knowledge they keep whichever game they load -- the same way a real notebook
// beside a real console does not get rolled back when you reload. It also means
// a note survives the save file being deleted, which is the behaviour anyone who
// has ever written a combination down actually expects.
//
// TWO FILES PER NOTE, because the pixels never change and the scribbles do:
//
//   note_NNN.rnot   32-byte header + ARGB1555 capture.  Written ONCE.
//   note_NNN.str    the strokes.  Rewritten whenever they change.
//   last.dat        which note the browser should open on.
//
// A capture is 256x165x2 = 82 KB and a stroke file is about a kilobyte, so
// keeping them apart is the difference between an 82 KB write every time the
// stylus lifts and a 1 KB one.
//
// STROKES, NOT A PAINTED LAYER. The page the player sees is rebuilt from the
// untouched capture plus the strokes that are still there, which is what makes
// the eraser exact: erasing DROPS strokes, and no amount of pixel bookkeeping
// could undo one stroke where two had already overlapped. Storing 82 KB of
// painted page per note instead of a kilobyte of polylines would also be the
// whole notebook's budget spent on the wrong half.
//
// kMaxNotes IS A HARD CAP AND NOT A RING BUFFER. At ~83 KB each, 24 notes is
// about 2 MB. A ring buffer would silently throw away the oldest page -- which,
// in a game about writing things down, is the page with the combination on it.
// The player is told the notebook is full instead.
//
// DELIBERATELY FREE OF <nds.h>, like SaveGame.hpp and RcurFile.hpp: the host
// test compiles this and checks that a note written here reads back, and that a
// corrupt header is refused rather than believed. That is also why the notes
// directory is handed in through setDirectory() instead of being read out of the
// Global singleton.

#include <cstdint>
#include <string>
#include <vector>

namespace rivenrt
{
namespace BookNotes
{

/// See the header note: a cap, not a ring.
inline constexpr int kMaxNotes = 24;

/// Format version of both files. A note whose version this build does not know
/// is listed as unreadable rather than decoded into the wrong shape.
inline constexpr std::uint16_t kVersion = 1;

/// The widest and tallest capture this reader will believe.
///
/// Load-bearing, and not a tidiness check: readPixels sizes a buffer from the
/// width and height in the header, and w*h*2 of a garbage size is an instant
/// out-of-memory on a 4 MB console. The bound is the screen, because nothing
/// can ever be captured that is bigger than the screen it was captured from.
inline constexpr int kMaxW = 256;
inline constexpr int kMaxH = 192;

/// The ink the player can draw in. ARGB1555, bit 15 set: rivendata::Texel is
/// the hardware word, and these are written straight into the page.
///
/// Stored in a note as an INDEX, never as a colour, so this palette stays
/// tweakable without rewriting anyone's notes.
inline constexpr std::uint16_t kInk[] = {
    static_cast<std::uint16_t>(0x8000 | (0 << 10) | (0 << 5) | 31),   // red
    static_cast<std::uint16_t>(0x8000 | (31 << 10) | (31 << 5) | 31), // white
    static_cast<std::uint16_t>(0x8000 | (0 << 10) | (31 << 5) | 31),  // yellow
    static_cast<std::uint16_t>(0x8000 | (0 << 10) | (28 << 5) | 0),   // green
    static_cast<std::uint16_t>(0x8000 | (31 << 10) | (8 << 5) | 0),   // blue
    static_cast<std::uint16_t>(0x8000 | (0 << 10) | (0 << 5) | 0),    // black
};
inline constexpr int kInkCount = static_cast<int>(sizeof(kInk) / sizeof(kInk[0]));

/// The ink's name, for the toolbar. Never null.
const char *inkName(int index);

/// One freehand line, in page coordinates.
///
/// x and y are parallel arrays of the same length rather than a vector of
/// points: they are bytes, and a std::vector<std::pair<u8,u8>> would pad each
/// point to four. A byte is enough because a page is at most 256x192, which is
/// the same bound kMaxW/kMaxH enforce on the way in.
struct Stroke
{
    std::uint8_t color = 0;
    std::uint8_t width = 2;
    std::vector<std::uint8_t> x;
    std::vector<std::uint8_t> y;

    std::size_t points() const { return x.size() < y.size() ? x.size() : y.size(); }
};

/// What the browser needs to list a note, straight out of its 32-byte header.
struct NoteInfo
{
    int index = 0; ///< the NNN in the filename, not a position in a list
    std::uint8_t stackId = 0;
    std::uint16_t cardId = 0;
    std::uint64_t when = 0;
    std::uint16_t w = 0;
    std::uint16_t h = 0;
};

/// Point the note functions at <data>/notes/. Everything here is a no-op until
/// this is called, which is what keeps a build with no card from writing notes
/// into the working directory.
void setDirectory(const std::string &dir);

std::string notePath(int index);
std::string strokePath(int index);

/// Every readable note, in index order.
///
/// Probes indices 0..kMaxNotes-1 rather than walking the directory: the names
/// are ours, the range is 24 long, and a header is a 32-byte read. A file that
/// exists but will not parse is dropped from the list AND logged by the caller,
/// because a note that silently vanishes looks exactly like a lost write.
std::vector<NoteInfo> scan();

/// Write a new note. Returns its index, or -1 if the notebook is full, the
/// picture is bigger than a screen, or the card would not take it.
///
/// Takes the FIRST FREE index, so indices are reused after a deletion. That is
/// why lastOpened() stores an index and not a position.
int capture(const std::uint16_t *pixels, int w, int h, std::uint8_t stackId,
            std::uint16_t cardId);

/// The capture. False leaves `out` untouched.
bool readPixels(int index, std::vector<std::uint16_t> &out, NoteInfo &info);

/// The strokes, or an empty vector -- which is also what a note that has never
/// been drawn on returns, and the two are deliberately the same thing.
std::vector<Stroke> readStrokes(int index);

/// Replace a note's strokes. An EMPTY vector deletes the file rather than
/// writing an empty one, so a note that has been fully erased is byte-for-byte
/// a note that was never drawn on.
bool writeStrokes(int index, const std::vector<Stroke> &strokes);

/// Delete a note and its strokes.
bool remove(int index);

/// "Temple Island  card 155  14/08 21:03" -- the caption over a page. The date
/// is dropped when the clock was unset rather than shown as a day in 1970.
std::string label(const NoteInfo &info);

/// Which note the browser should open on: an INDEX, not a position, because
/// capture() reuses freed indices and positions shift under deletion while an
/// index always names the same note. -1 for none.
int lastOpened();
void setLastOpened(int index);

} // namespace BookNotes
} // namespace rivenrt
