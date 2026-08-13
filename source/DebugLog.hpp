#pragma once

// Developer debug facility, gated by settings.debugMode. Modelled on the Myst
// port's DebugLog, with the differences its own hardware layout forces.
//
// WHAT IT IS FOR. The port already prints its FAILURES to the top screen -- a
// movie that is not on the card, a card a script asked for that does not exist.
// That answers "why did nothing happen", and it is deliberately all that is
// printed by default, because a line per asset would push those failures off a
// 24-row console before they could be read. What it does not answer is the
// other half: which card am I on, what did it try to load, and did the file it
// wanted exist. Every asset here is a separate file on an SD card, so "the game
// drew the wrong thing" is usually a question about a PATH. This is the mode
// that shows them.
//
// THREE DESTINATIONS, because on a DS they are not interchangeable: the
// top-screen console, which is the only one a player on hardware can read and
// which scrolls; nocashMessage, free and unlimited under an emulator and
// invisible everywhere else; and <data>/debug.log, the only one that survives
// the machine being switched off.
//
// TWO DEPARTURES FROM THE MYST PORT, both because this port's screens are laid
// out differently:
//
//   * It does not stand up a console. Myst's top screen is a picture and debug
//     mode replaces it; here the top screen has ALWAYS been the console
//     (Global::Init), so begin() only hides the picture that was put behind it,
//     which a dense trace is unreadable over.
//   * The VRAM dump remaps nothing. Myst's card art lives in TEXTURE banks,
//     which the CPU cannot address, so each had to be flipped to LCD and back
//     to be read. Nothing here is a texture -- the card view is bitmap
//     backgrounds -- so every mapped bank is already in a CPU-readable window
//     and the dump is a straight copy with no window where the screen is blank.
//
// The screenshot and the dump are on SELECT+L and SELECT+R. Myst moved its to
// console commands because its buttons were taken; SELECT is already this
// port's developer chord (Engine::processInput) and its shoulder buttons are
// free.

#include <string>

namespace rivenrt
{
namespace DebugLog
{

/// Open the log and take the top screen, if settings.debugMode is set. Safe
/// before FAT is up -- it just means no file. Truncates: a trace is of one run,
/// and an appended one would bury the run being debugged under every run before
/// it.
void begin();

/// Flush and close. Called at the end of main() so a normal exit leaves a
/// complete log; a power-off does not, which is what the per-line flush is for.
void end();

/// True when anything at all would be written. Callers that would have to BUILD
/// something to log it -- a path, a formatted name -- should ask first; the
/// logging itself is already free when off.
bool enabled();

/// printf-style, one line, newline added. No-op when disabled.
void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// A failure, which is NOT gated: it reaches the console whether the trace is
/// on or not, and additionally reaches debug.log when it is.
///
/// This exists because the two halves would otherwise disagree about the most
/// important lines. Every failure in this port is a printf, so a trace read on
/// screen shows them; a trace read afterwards out of debug.log would contain
/// every asset that LOADED and no mention of the one that did not, which is the
/// opposite of what the file is for. Routing them through here rather than
/// logging them twice is also what keeps the console to one line per event.
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// SELECT+L: write what is on the bottom screen to <data>/shotNNN.bmp.
///
/// The BgSurface buffer rather than the card's RAM picture, so it captures
/// whatever actually owns the screen -- a movie frame, the zoom viewer, a menu
/// -- and not just the card the engine believes is up.
void screenshot();

/// SELECT+R: write every mapped VRAM bank to <data>/vramNNN/bank<X>.bin.
void vramDump();

} // namespace DebugLog
} // namespace rivenrt
