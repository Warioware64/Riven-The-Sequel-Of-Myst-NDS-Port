#pragma once

// EVERYTHING THAT REACHES THE TOP SCREEN GOES THROUGH HERE. Modelled on the
// Myst port's DebugLog, with the differences this port's hardware layout and
// its own history force.
//
// WHAT IT IS FOR. Which card am I on, what did it try to load, and did the file
// it wanted exist. Every asset here is a separate file on an SD card, so "the
// game drew the wrong thing" is usually a question about a PATH, and the trace
// is what shows them.
//
// THREE DESTINATIONS, because on a DS they are not interchangeable: the
// top-screen console, which is the only one a player on hardware can read and
// which scrolls; nocashMessage, free and unlimited under an emulator and
// invisible everywhere else; and <data>/debug.log, the only one that survives
// the machine being switched off.
//
// THE TOP SCREEN IS A PICTURE, AND THAT IS WHAT DECIDES THE RULES BELOW.
//
// This file used to argue the opposite, and the reversal is the point. warn()
// was deliberately UNGATED, on the reasoning that a failure is the one thing
// worth spending a row on whether or not anyone asked for a trace: it answers
// "why did nothing happen". That was written when the top screen was a bare
// console with nothing behind it, and it cost nothing.
//
// It costs a great deal now. TopBg puts Riven's own AUTORUN.BMP behind the
// console and gives it palette entries 0..239, leaving 240..255 to libnds --
// entry 255 being the white the console prints in. So a glyph is not text over
// a picture, it is an opaque white block ON the picture, with no blending to
// soften it. And the ungated set was never one line: 28 warn() sites and 8
// note() sites, of which the ones that fire on a PERFECTLY GOOD card dominate --
// a card summary per move, an unimplemented external command per click, a line
// every time the player presses X on a card that has no zoom twin. The splash
// was unreadable within a minute of ordinary play, and none of it was addressed
// to the player.
//
// So the console is now the debug setting's, and nothing else's:
//
//   log()     the asset trace          console: debug only
//   note()    cards, saves, notes      console: debug only
//   warn()    a failure                console: debug only
//   notice()  startup only             console: ALWAYS
//   status()  one transient line       console: ALWAYS, and clears itself
//
// nocashMessage is ungated on every one of them, which is why nothing is lost
// rather than merely hidden: a developer on an emulator still sees every warn()
// with debug mode off, and debug.log still gets them when it is on.
//
// notice() AND status() ARE THE TWO THINGS A PLAYER IS ACTUALLY OWED. A notice
// says the conversion on the card is incomplete -- there is no video/, there is
// no menu font -- which is a thing to go and fix, said once, at startup, and
// wiped by settleNotices() once the game begins so it cannot sit on the splash
// forever. A status is the answer to a button the player just pressed, and it
// expires on its own for the same reason.
//
// Without status() the quieting above would have broken the notebook: taking a
// note is silent hardware, and its only feedback was a note() line.
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
// THE SCREENSHOT AND THE DUMP ARE ON BARE L AND BARE R, in debug mode, and on
// SELECT+L and SELECT+R otherwise.
//
// They began on the chords alone, on the argument that SELECT is already this
// port's developer chord (Engine::processInput) and the shoulder buttons are
// otherwise the notebook's. Both halves of that turned out to be wrong for the
// case they exist to serve:
//
//   * a chord is two hands on a machine already being held in two hands, and
//     the frame worth capturing has usually gone by the time the second finger
//     arrives; and
//   * the chord was read by Engine::processInput, which only the CARD loop
//     runs -- so a cutscene, a screen transition and a slider drag were each a
//     stretch of the game that could not be captured at all. Those are exactly
//     the stretches a rendering bug lives in.
//
// So in debug mode they move to a single button each, polled from every loop
// the engine has (pollHotkeys). This takes L away from the notebook, which is
// deliberate: debug mode already replaces the top screen with a trace, and a
// mode that exists to look at the game closely can afford to spend the button
// that exists to write in it.

#include <cstdint>
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

/// An EVENT rather than an asset: the card that just settled, a slot saved, a
/// note taken. One line per thing the player did, not per file it touched.
///
/// The distinction from log() is rate, and it survives even though both are now
/// gated the same way: a trace is read by scrolling back through it, and a
/// screenful of asset lines between two card summaries is what makes that
/// impossible. Anything that would print more often than the player moves
/// belongs in log().
void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// A failure: a movie that is not on the card, a card a script asked for that
/// does not exist, a sound the budget would not take.
///
/// Gated on the console now, and the long note at the top of this file is the
/// argument for why it stopped being the exception. It still reaches
/// nocashMessage unconditionally, so it is invisible on hardware and free under
/// an emulator -- the failures did not stop being reported, they stopped being
/// painted onto the player's splash screen.
void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// STARTUP ONLY, and never gated: something about this card is incomplete and
/// the player has to go and fix it. No video/, no cursors/, no menu font.
///
/// The one class of message worth interrupting the picture for, because it is
/// the only one the player can act on and there is nowhere else to say it.
/// Bounded by being startup-only and by settleNotices() wiping it, so unlike the
/// old ungated warn() it cannot accumulate.
void notice(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// How many notices have been emitted this run.
int noticeCount();

/// Give the notices a moment to be read, then clear the console.
///
/// Called once, after the engine is up. Dwells only if there was something to
/// read -- a complete conversion pays nothing -- because the last notices come
/// from inside Engine::boot(), after the menu the earlier ones were readable
/// behind. A no-op with the trace on: there the console is the point.
void settleNotices();

/// Write one row of the console without disturbing the scrolling text above it.
///
/// THIS EXISTS BECAUSE consoleSetWindow ZEROES THE CURSOR (libnds console.c:
/// consoleSetWindow sets cursorX and cursorY to 0 along with the rectangle).
/// So the obvious "narrow the window, print, widen it again" loses the position
/// the scrolling output had reached, and the next line printed lands back at the
/// top of the screen and overwrites it. Both callers here got that wrong: the
/// debug console re-aimed its window before every line, so its whole output --
/// help, hotspots, vars -- was written on top of itself in row 0.
///
/// Saves the window rectangle AND the cursor, writes the row, puts both back.
/// `text` is padded to the width and clipped one column short of it: filling the
/// final cell of a row wraps the console, and in a one-row window a wrap scrolls
/// the line that was just drawn straight off it.
void consoleRow(int row, const char *text);

/// One transient line at the bottom of the top screen, for an answer the player
/// is owed to a button they just pressed. Never gated; clears itself.
///
/// An empty string clears it now. See Engine::setStatus, which is the only
/// caller and which existed for a long time without rendering anything.
void status(const char *text);

/// Tick the status line down and blank it when it expires. Called from both
/// Engine::frame and Engine::idleFrame, so a message raised by a modal screen
/// or during a blocking movie still goes away on its own.
void pumpStatus();

/// Write what is on the bottom screen to <data>/shotNNN.bmp.
///
/// The BgSurface buffer rather than the card's RAM picture, so it captures
/// whatever actually owns the screen -- a movie frame, the zoom viewer, a menu
/// -- and not just the card the engine believes is up.
void screenshot();

/// Write every mapped VRAM bank to <data>/vramNNN/bank<X>.bin.
void vramDump();

/// Take a screenshot (L) or a VRAM dump (R) if one has just been asked for.
/// Does nothing at all when debug mode is off.
///
/// CALL IT FROM EVERY LOOP. That is the point of it: the engine has three --
/// the card loop, the idle loop a blocking movie spins, and the interactive
/// loop a drag spins -- and only the first of them ever ran the input handler,
/// so a cutscene, a transition and a slider drag were each a stretch of the
/// game that could not be photographed. Those are the stretches worth
/// photographing.
///
/// Reads the key register directly and keeps its own edge state, so it neither
/// needs nor disturbs a caller's scanKeys(). The body says why that matters.
void pollHotkeys();

/// The ten face and shoulder buttons, straight out of REG_KEYINPUT and already
/// inverted -- a set bit is a button that is DOWN.
///
/// Here rather than in a corner of DebugLog.cpp because Engine::pollNoteHotkey
/// now needs the same thing for the same reason, and the reason is the whole
/// point: every loop in the engine calls one of these pollers, and they
/// disagree about who calls scanKeys(). A second scanKeys() in one frame
/// computes its edge against the held mask the FIRST one stored, so it reports
/// nothing pressed and eats the caller's own B/START. Reading the register is
/// two instructions and cannot interfere with anything.
///
/// A poller pairs this with an edge latch OF ITS OWN. Sharing one latch between
/// two pollers would mean whichever ran first consumed the press.
std::uint32_t rawKeys();

/// Hold the hotkeys off for as long as a screen that needs L and R ITSELF is up.
///
/// The notebook is the one such screen: L and R are its page buttons, and it
/// spins Engine::idleFrame -- which is exactly where pollHotkeys was put so that
/// cutscenes could be captured. Without this, paging through notes in debug mode
/// would take a screenshot per page back and a 600 KB VRAM dump per page
/// forward.
///
/// Counted and scope-bound, the same shape and for the same reason as
/// Engine::CursorHide: a hand-written on/off pair leaks the held state on an
/// early return, and these loops have several.
struct HotkeyHold
{
    HotkeyHold();
    ~HotkeyHold();
    HotkeyHold(const HotkeyHold &) = delete;
    HotkeyHold &operator=(const HotkeyHold &) = delete;
};

/// Is a HotkeyHold alive?
///
/// For Engine::pollNoteHotkey, which polls the SAME button from the SAME loops
/// and therefore needs suppressing in exactly the same places -- the notebook
/// pages with L, and a note taken while paging would be a note of the notebook.
/// A reader on this counter rather than a second one of its own: two counters
/// would be two things to remember to hold, and the one that got forgotten
/// would be forgotten at the one moment it mattered.
bool hotkeysHeld();

} // namespace DebugLog
} // namespace rivenrt
