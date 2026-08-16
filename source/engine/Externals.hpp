#pragma once

// The handful of things every stack's external commands need.
//
// ScummVM puts these on RivenStack, the base class the eight island files
// inherit (riven_stack.cpp), which is exactly what they are: not one island's
// logic but the vocabulary all of them are written in. The port has free
// functions instead of a base class -- the externals are free functions -- so
// this is that base class's protected section, and nothing else belongs in it.
//
// Each of these was file-local in Externals.cpp or ExternalsJspit.cpp until a
// second island wanted it. That is the bar for adding to this header: a second
// caller, not a guess that there will be one.

#include <cstddef>
#include <cstdint>
#include <string>

#include "RivenTransition.hpp"
#include "RivenVars.hpp"

namespace rivenrt
{

class Engine;

/// RivenStack::getComboDigit (riven_stack.cpp:197-200). A combination is stored
/// as a decimal number whose digits are the buttons in order, so digit 0 of
/// 51234 is 5. Six powers for five digits: the divisor of digit n is
/// powers[n+1].
std::uint32_t comboDigit(std::uint32_t combo, std::uint32_t digit);

/// ScummVM's getRandomNumberRng: INCLUSIVE at both ends (common/random.cpp:56 --
/// getRandomNumber(max - min) + min, and getRandomNumber(n) itself returns
/// 0..n). Getting this wrong makes a range one short at the top, which for the
/// Ytram trap would be invisible and for a movie picker would silently never
/// play one of them.
int randomBetween(int lo, int hi);

/// The MLST "code" of a record addressed by its INDEX.
///
/// The two are different numbers and ScummVM conflates them in several places
/// -- it hands the same value to RivenCard::playMovie, which takes an index,
/// and to openSlot, which takes a code. On the cards where it does that they
/// happen to agree; that is not a coincidence worth relying on, so every port
/// call site that starts from an index goes through this.
std::uint16_t codeForMlstIndex(Engine &engine, std::uint16_t index);

/// Turn the pages of a book for as long as the player holds the button.
///
/// RivenStack::keepTurningPages / pageTurn / waitForPageTurnSound
/// (riven_stack.cpp:395-419) and the loop in ASpit::xaatrusbooknextpage that
/// uses all three. Two books in the game work this way -- Atrus's journal on
/// aspit and Gehn's lab journal on bspit -- and they differ only in the page
/// variable, how many pages there are and what else a page has to draw.
///
/// `delta` is +1 or -1. `firstPage`/`lastPage` are inclusive and it stops dead
/// at either end rather than wrapping. `onPage`, when given, runs after the
/// page's picture is drawn and before the wait -- it is how bspit gets the dome
/// combination onto page 14.
///
/// ONE PAGE PER CALL, unlike ScummVM's, which keeps turning while the mouse is
/// held. The body says why; the short version is that a stylus tap outlasts a
/// page turn and there is no second button to tell the two apart.
///
/// `transition` is scheduled BEFORE the draw, because that is how Riven's
/// transitions work: opcode 18 records a request that the next completed screen
/// update honours. Pass Transition::None for a book that does not want one --
/// Atrus's journal is drawn without one in this port and always has been.
void turnBookPages(Engine &engine, rivendata::VarId pageVar, int delta,
                   std::uint32_t firstPage, std::uint32_t lastPage,
                   rivendata::Transition transition,
                   void (*onPage)(Engine &, std::uint32_t) = nullptr);

/// Riven's page-turn sound: one of two recordings, picked at random so a run of
/// pages does not tick identically (riven_stack.cpp:404-412).
void playPageTurnSound(Engine &engine);

/// RivenStack::runEndGame (riven_stack.cpp:208-215). Play the ending on movie
/// `code`, roll the credits, throw the game state away and put the player back
/// on the menu.
///
/// Three of Riven's islands can end it -- tspit's fissure, ospit's trap book
/// and rspit's -- and the differences between them are all in choosing WHICH
/// movie and how long to wait afterwards, which is the caller's half. This is
/// the part that is the same.
///
/// `delayMs` is the pause between the ending's video running out and the first
/// credits image (riven_stack.cpp:244). It is per-ending and the numbers are
/// ScummVM's own, because they were measured against these videos: the office
/// endings wait eight to twelve seconds on a held last frame, and Tay's waits
/// one and a half.
///
/// `frameCountOverride` is carried and NOT honoured. It exists for the Polish
/// release alone, whose ending videos keep running white frames after the
/// picture ends, so ScummVM stops the video early rather than at endOfVideo
/// (riven_stack.cpp:227-236). This port has no language flag to test it
/// against, and applying it to a non-Polish video would cut the ending short --
/// so it is stored where a language check can find it later rather than dropped
/// and having to be researched again.
///
/// `alreadyPlaying` is for the one ending that is not started here: ospit's
/// third refusal, where Gehn shoots the player part-way through the speech
/// movie that xbookclick was watching. ScummVM reaches that one by calling
/// runCredits directly rather than runEndGame (ospit.cpp:143-146), because
/// starting the movie again would rewind it to the beginning of the scene.
void runEndGame(Engine &engine, std::uint16_t movieCode, std::uint32_t delayMs,
                std::uint32_t frameCountOverride = 0, bool alreadyPlaying = false);

/// Debug-only interception of the ending tail, for the console's `endings`
/// command (DebugConsole.cpp).
///
/// Riven has eight endings and no route to any of them shorter than a
/// playthrough, so the only way to check that the branch logic still picks the
/// right one is to fire each branch and look at what it chose. Armed, runEndGame
/// records its arguments and skips the video and the credits -- everything that
/// costs minutes -- while still running the state reset and the return to the
/// menu, which is the half that can actually be wrong.
///
/// A plain struct with a file-scope instance rather than a callback: there is
/// one console and one engine, the recording is four integers, and the cost when
/// disarmed has to be a single predictable branch on a path that is already
/// about to play a video.
struct EndingProbe
{
    bool armed = false;
    bool fired = false;
    /// tspit's fissure alone: WHICH of its four MLST records was chosen. The
    /// movie code cannot stand in for it -- codeForMlstIndex maps index to code
    /// through the card, and on the fissure card they are not the same numbers.
    std::uint16_t mlstIndex = 0;
    std::uint16_t movieCode = 0;
    std::uint32_t delayMs = 0;
    std::uint32_t frameCountOverride = 0;
    bool alreadyPlaying = false;
    /// The tail, READ BACK off the engine rather than asserted on the way past:
    /// `reset` is the deciding variables all being back at their defaults, and
    /// `returned` is the engine standing on aspit card 1. The second is only
    /// meaningful because the console runs outside the interpreter, where
    /// runEndGame's stack change is immediate rather than deferred.
    bool reset = false;
    bool returned = false;
};
extern EndingProbe endingProbe;

// --- the per-stack tables --------------------------------------------------
//
// One per island. Each returns true when it handled `key`, so the dispatcher in
// Externals.cpp can fall through to the next and finally to "not implemented".
// `key` is already case-folded and `args` is the command's own argument window.
//
// Declared together here rather than in a header each. Three headers that said
// the same sentence three times was the shape until bspit made it three; the
// bodies are still one file per island, which is the split that matters.
//
// Seven of the eight stacks are here. aspit is the eighth and is not: its
// commands are the menu and the books rather than a place, so they stayed in
// the dispatcher with the shared vocabulary above.

bool runJspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);
bool runTspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);
bool runBspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);
bool runGspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);
bool runOspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);
bool runPspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);
bool runRspitExternal(Engine &engine, const std::string &key, const std::uint16_t *args,
                      std::size_t argCount);

/// JSpit::installCardTimer (jspit.cpp:785-802). The four sunner cards are the
/// only ones in jspit that want a card timer.
void installJspitCardTimer(Engine &engine);

/// PSpit::installCardTimer (pspit.cpp:142-150). One card: the top of the prison
/// elevator, where Catherine paces in her cage.
///
/// These two are the whole of the override in the game -- every other stack
/// falls through to nothing (riven_stack.cpp:272). The other timers Riven has
/// are armed by a command rather than by arriving somewhere: bspit's Ytram
/// trap, gspit's Catherine viewer and rspit's rebel window.
void installPspitCardTimer(Engine &engine);

} // namespace rivenrt
