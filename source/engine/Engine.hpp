#pragma once

// The engine: the loaded stack, the current card, and the frame loop.
//
// Structured after ScummVM's, because that is the specification for how Riven
// behaves and because keeping the shapes recognisable is what lets a bug be
// tracked down by reading riven.cpp next to this. In particular:
//
//   changeToStack / changeToCard   riven.cpp:377-436
//   entering a card                riven_card.cpp:632-650
//   the frame loop                 riven.cpp:206-235
//
// What differs, and why:
//
//   * The DS has a touch screen, so there is no hover. A hotspot's
//     MouseEnter / MouseInside / MouseLeave fire around the point being touched
//     while the stylus is down; MouseDown and MouseUp are direct. Nothing in the
//     original depends on hover happening without a click except the cursor
//     shape, which a stylus does not have.
//   * Card art, sound and movies come off the SD card one file at a time rather
//     than out of an open Mohawk archive, so a card change is a handful of reads
//     and there is no resource cache to clear.
//   * There is no scheduled screen transition. The variable is set to the
//     original's own "fastest" and the update is applied whole
//     (CardSurface::flush), which is what the setting means.

#include <cstdint>
#include <string>
#include <vector>

#include "DebugLog.hpp"
#include "RivenData.hpp"
#include "SaveGame.hpp"
#include "audio/RivenAudio.hpp"
#include "engine/Vars.hpp"
#include "render/CardSurface.hpp"
#include "render/Cursor.hpp"
#include "render/FliesEffect.hpp"
#include "render/Inventory.hpp"
#include "render/WaterEffect.hpp"
#include "render/ZoomView.hpp"
#include "rvid/RvidPlayer.hpp"

namespace rivenrt
{

/// Playback slots a card's MLST records can address. Riven's "code" field is a
/// small integer; ScummVM keeps a slot per distinct value it sees, and no card
/// in the shipped data uses more than a handful.
inline constexpr int kMovieSlots = 8;

class Engine
{
public:
    /// Load the boot stack and enter its first card: aspit card 1, the main menu
    /// (riven.cpp:195-196). False (with error() set) if the data on the card
    /// cannot be read at all.
    ///
    /// With `restore`, go straight to the saved card instead. Not "boot, then
    /// load": aspit card 1 is Riven's own menu and entering it starts the
    /// attract sequence, so a player who picked Load game would watch the intro
    /// begin and then be yanked out of it. A restore that fails falls through to
    /// the normal boot, which is a new game rather than a black screen.
    bool boot(const SaveGame::SaveState *restore = nullptr);

    /// One pass of the run loop. Called with the main thread parked between
    /// vblanks, so it must not block.
    void frame();

    /// True once boot() has loaded a stack, so there is a game to come back to.
    /// The settings screen asks, because it is reachable from both sides of it:
    /// from the port's menu before there is one, and from Riven's own Options
    /// button once there is.
    bool booted() const { return booted_; }

    /// True once something has asked the game to stop.
    bool quitRequested() const { return quit_; }
    void requestQuit() { quit_ = true; }

    // --- navigation ---------------------------------------------------------

    bool changeToStack(rivendata::StackId id);
    bool changeToCard(std::uint16_t cardId);

    /// Follow a ChangeStack command.
    ///
    /// `cardIsLocal` is RivenStackChangeCommand's `_byStackCardId`
    /// (riven_scripts.cpp:920-926, :934-957) and the distinction is real: the
    /// opcode names its destination by RMAP GLOBAL id, which has to be resolved
    /// against the target stack's own table (riven_stack.cpp:138-150), but a
    /// couple of places in the C++ name a card in the destination stack
    /// directly -- the ending's return to aspit card 1 is one. Guessing which a
    /// number is would silently land on the wrong card.
    ///
    /// DEFERRED while a script is running, and it has to be. A command list is a
    /// std::vector living inside the loaded Stack, and loading the next stack
    /// destroys it -- so replacing the stack from inside a running script would
    /// pull the commands out from under the interpreter. ScummVM survives the same
    /// situation because its scripts are reference-counted objects; here the
    /// change is held until the outermost script returns.
    bool changeToStackAndCard(rivendata::StackId id, std::uint32_t cardId,
                              bool cardIsLocal = false);
    /// Re-enter the current card. Opcode 19.
    void refreshCard();

    // --- what the script interpreter needs ----------------------------------

    Vars &vars() { return vars_; }
    const rivendata::Stack &stack() const { return stack_; }
    const rivendata::Card *card() const { return card_; }
    CardSurface &surface() { return surface_; }

    /// Resolve a script's variable index against this stack's NAME 4 list.
    ///
    /// The mapping was done by the converter (Stack::variableIds), so this is a
    /// bounds check and a subscript -- no string is built. VarId::Unknown covers
    /// both "the index is off the end of the list" and "the name is not one
    /// RivenVars.hpp knows"; Vars treats it as a slot that goes nowhere, so a
    /// caller does not have to special-case it.
    rivendata::VarId variableId(std::uint16_t index) const;
    std::string nameFromList(int list, std::uint16_t index) const;
    /// Index of `name` in a list, or -1. ScummVM's getIdFromName.
    std::int32_t idFromName(int list, const std::string &name) const;

    /// Hotspot lookups, by the id BLST records and opcodes 9/10 use, and by
    /// name for the external commands.
    const rivendata::Hotspot *hotspotByBlstId(std::uint16_t blstId) const;
    const rivendata::Hotspot *hotspotByName(const std::string &name) const;
    void enableHotspot(std::uint16_t blstId, bool enabled);
    void enableHotspotByIndex(std::size_t index, bool enabled);
    bool hotspotEnabled(std::size_t index) const;
    std::size_t hotspotIndexOf(const rivendata::Hotspot *h) const;
    /// The hotspot whose script is running, for opcode 45 (zip mode).
    const rivendata::Hotspot *currentHotspot() const { return currentHotspot_; }

    /// Where a hotspot IS, which is not always where the card data says.
    ///
    /// RivenHotspot::setRect (riven_stack.h). One puzzle in Riven moves a
    /// hotspot: the marble grid, whose six marbles are hotspots that follow the
    /// marble around the board (TSpit::setMarbleHotspots, tspit.cpp:298-309).
    ///
    /// A parallel array beside hotspotEnabled_ rather than a mutable card,
    /// because `card_` points into the stack file's own data -- shared by every
    /// card, const, and reloaded from the SD card -- and because the override
    /// has to die with the card anyway. resetCardState refills it from the data.
    ///
    /// EVERY reader of a hotspot's rectangle goes through this. A hit test that
    /// still read `h.rect` would leave a marble drawn in one place and clicked
    /// in another.
    const rivendata::Rect &hotspotRect(std::size_t index) const;
    const rivendata::Rect &hotspotRect(const rivendata::Hotspot *h) const
    {
        return hotspotRect(hotspotIndexOf(h));
    }
    void setHotspotRect(std::size_t index, const rivendata::Rect &r);

    // --- zip mode -----------------------------------------------------------
    //
    // Riven's fast travel: a card that names a "zip destination" is remembered
    // as it is entered, and a hotspot flagged kHotspotZip somewhere else on the
    // same island lights up once a card of the same NAME has been seen -- one
    // click instead of walking back. Off by default; settings.zipMode is what
    // VarId::AZip carries.
    //
    // ScummVM's _zipModeData (riven.cpp:732-748) is stack-BLIND and stores a
    // bare local card id, which is only safe because no two stacks happen to
    // reuse a card name. Storing the stack too costs eight bytes an entry and
    // removes the coincidence from the argument; zip travel is within an island
    // in the game anyway, so restricting the match to the current stack changes
    // nothing a player can see.
    struct ZipDest
    {
        rivendata::StackId stack = rivendata::StackId::None;
        std::uint16_t cardId = 0;
        std::string name;
    };

    /// Forget every zip destination AND restart the clock. New game
    /// (riven.cpp:679, and MohawkEngine::setTotalPlayTime(0) behind it).
    ///
    /// The two go together because they are the state a new game has to drop
    /// that Vars::startNewGame cannot reach: zip destinations are visited-card
    /// history rather than a variable, and the clock is the engine's own. Every
    /// caller wants both -- the New Game button and the ending -- so they are
    /// one call rather than two that can be written apart.
    void clearZipDests()
    {
        zipDests_.clear();
        frames_ = 0;
    }

    /// The card a zip hotspot called `name` leads to in the current stack, or
    /// -1. Public because opcode 45 is the other half of this.
    std::int32_t zipDestFor(const std::string &name) const;

    // --- the notebook -------------------------------------------------------
    //
    // Both defined out of line in render/NoteView.cpp, so the page reaches the
    // card it is a picture of and the frame pump that keeps audio alive while it
    // is up, without either becoming public API. BookNotes.hpp is the format.

    /// L: put a picture of what is on screen into the notebook.
    void captureNote();

    /// Take a note if L has just been pressed. The player's half of
    /// DebugLog::pollHotkeys, and CALL IT FROM EVERY LOOP for the same reason:
    /// this used to hang off processInput(), which only the card loop runs, so
    /// a cutscene, a transition and a slider drag were each a stretch of the
    /// game no note could be taken of. Those are stretches worth writing down --
    /// Gehn's speech and a marble being dragged onto a square are both things
    /// the player is being asked to remember.
    ///
    /// Reads the key register directly and keeps its own edge state, so it
    /// neither needs nor disturbs a caller's scanKeys(). Does nothing in debug
    /// mode, which owns L outright.
    void pollNoteHotkey();

    /// Y: page through the notebook, and scribble on it.
    void runNotebook();

    // --- saved games --------------------------------------------------------

    /// Snapshot the game. Cheap: a map copy and a short vector, because almost
    /// all of Riven's state is the variable map (SaveGame.hpp says why this is
    /// so much smaller than the Myst port's equivalent).
    SaveGame::SaveState buildSaveState() const;

    /// THE single restore path -- the boot menu, the in-game menu and the debug
    /// console all come through here, so there is one ordering to get right
    /// rather than three to keep in step.
    ///
    /// False without moving if the save names a stack this card does not carry:
    /// staying where we are is recoverable and a half-loaded game is not.
    ///
    /// DEFERRED while a script is running, for exactly the reason
    /// changeToStackAndGlobalCard is: this replaces the loaded Stack, and the
    /// command list the interpreter is walking lives inside it. Riven's own
    /// Restore button is an external command, so that is not a corner case --
    /// it is the ordinary route in. True then means ACCEPTED, not arrived.
    bool restoreFrom(const SaveGame::SaveState &s);

    // --- pictures, sound, movies -------------------------------------------

    /// Opcode 39: draw a PLST record. Opcode 1 (DrawBitmap) also lands here,
    /// because on the DS a tBMP id is a file and PLST is the only thing that
    /// names one.
    void activatePlst(std::uint16_t index);
    void drawBitmap(std::uint16_t tbmpId, const rivendata::Rect &rect);
    /// Several pieces of one tBMP, decoded once. See
    /// CardSurface::drawPictureSections -- the dome's slider strip is why it
    /// is a batch and not twenty-five calls.
    void drawBitmapSections(std::uint16_t tbmpId, const CardSurface::Section *sections,
                            std::size_t count);
    /// The same, for a picture out of `extras/` -- which belongs to the game
    /// rather than to a stack and so has no tBMP id here. `name` is the file's
    /// stem: "marbles" reads extras/marbles.rpic.
    ///
    /// RivenGraphics::drawExtrasImage (riven_graphics.cpp:657-669). Riven keeps
    /// three things outside the stacks: the inventory art, the marbles and the
    /// credits, and the marble grid is the only one a script reaches.
    void drawExtrasSections(const char *name, const CardSurface::Section *sections,
                            std::size_t count);
    /// True once a script has drawn a picture this card entry, so the default
    /// load script knows not to (riven_card.cpp:690-694).
    bool activatedPlst() const { return activatedPlst_; }
    bool activatedSlst() const { return activatedSlst_; }

    // --- the zoom viewer ----------------------------------------------------
    //
    // The engine has exactly one mode otherwise: a card is up. This is the
    // second, and it is deliberately a flag on the frame loop rather than a
    // state machine -- there are two states, and a machine for two states is a
    // machine that has to be read before either can be understood.

    enum class Mode
    {
        Card,
        Zoom,
    };

    /// Open the zoom viewer on the card's own picture, or close it. Refused,
    /// with a line saying why, while a movie or a transition owns the screen
    /// and when the card was converted without its zoom twin.
    void toggleZoom();

    /// The tBMP the last full-card PLST record drew, which is the picture the
    /// zoom viewer opens. Zero until a card has drawn one.
    std::uint16_t cardPicture() const { return cardPicture_; }

    /// The card the engine is on, and its RMAP global id -- the pair the
    /// inventory has to record before it links away, so a journal's back
    /// hotspot can bring the player back to it (riven_inventory.cpp:120-123).
    std::uint16_t cardId() const { return cardId_; }
    std::uint32_t globalCardId(std::uint16_t local) const
    {
        return local < stack_.rmap.size() ? stack_.rmap[local] : 0;
    }

    /// True exactly while a fullscreen movie owns the background buffers, which
    /// is what "a cutscene is playing" means here.
    bool fullscreenMoviePlaying() const;

    /// Opcode 13 (riven_scripts.cpp:600). No longer a no-op: the port has a
    /// cursor now, and a script that shapes it is shaping something real.
    void setCursor(std::uint16_t id) { cursor_.setShape(id); }
    Inventory &inventory() { return inventory_; }
    /// For the screens that take the display away from the card view and have
    /// to put the pointer away with it.
    Cursor &cursor() { return cursor_; }

    /// Put the pointer away for the length of a blocking sequence.
    /// RivenVideo::playBlocking brackets its whole wait with hideCursor() and
    /// showCursor() (riven_video.cpp:216 and :268), for any blocking video --
    /// a fullscreen cutscene and a small overlay alike.
    ///
    /// Counted and scope-bound, which is the sibling Myst port's convention
    /// (MystEngine::CursorHide) and for its reason: a hand-written hide/show
    /// pair leaks the hidden state on an early return, and playMovieBlocking
    /// has one. The guard only moves the counter -- flushUploads re-derives the
    /// sprite's visibility from it every frame.
    ///
    /// The two bodies are out of line only because clang does not extend a
    /// class's complete-class context into its nested classes the way the
    /// standard and gcc do, and cursorSuppress_ is declared below.
    struct CursorHide
    {
        Engine &eng;
        explicit CursorHide(Engine &e);
        ~CursorHide();
        CursorHide(const CursorHide &) = delete;
        CursorHide &operator=(const CursorHide &) = delete;
    };

    /// Where the pointer is, in DS screen pixels. Moved by the stylus and by the
    /// D-pad alike (processInput), and read by the inventory strip, which shows
    /// itself only while it is being pointed at.
    int pointerX() const { return pointerX_; }
    int pointerY() const { return pointerY_; }

    /// Opcode 44 (riven_scripts.cpp:750). Start the card's water effect: the
    /// FLST record with this index names an SFXE, and that is the ripple.
    /// RivenCard::activateWaterEffect (riven_card.cpp:914-922).
    void activateFlst(std::uint16_t index);

    /// RivenGraphics::setFliesEffect (riven_graphics.cpp:754-757), behind
    /// xflies. `count` is 1..5 and `fireflies` picks which of the two creatures
    /// it is. Replaces whatever was flying.
    void setFliesEffect(int count, bool fireflies);

    void activateSlst(std::uint16_t index);
    /// RivenCard::overrideSound (riven_card.cpp:802-804). Give the SLST record
    /// at `slot` the sounds of the one at `withSlot`, for as long as this card
    /// is up.
    ///
    /// POSITIONS in the card's list, zero-based -- NOT the `index` a record
    /// carries and not what activateSlst takes, which are one-based. The body
    /// says why that distinction is the whole of this function.
    ///
    /// bspit's sound plug is the only caller in the game: which of three
    /// ambients the crater's first record names depends on two variables, and
    /// the card's script has no way to say so other than this.
    void overrideCardSound(std::uint16_t slot, std::uint16_t withSlot);
    void playSlst(const rivendata::SoundRec &rec);
    void stopAllAmbient();
    /// Start layer `i` of `rec` on a free slot. Shared by the two paths through
    /// playSlst so the mix and the reporting cannot drift apart.
    void startAmbientLayer(const rivendata::SoundRec &rec, std::size_t i);
    void playEffect(std::uint16_t twavId, int volume);

    /// Play the effect Riven names rather than numbers.
    /// RivenSoundManager::playCardSound (riven_sound.cpp:73-77): the resource is
    /// "<this card's id>_<name>_1" among the stack's tWAVs. This is how the
    /// journals get their page turn, the telescope its clunk and the dome its
    /// tick -- none of which any script names by id.
    void playCardSound(const std::string &name, int volume = 255);

    /// The tBMP this card names `name`, or -1. DomeSpit::buildCardResourceName
    /// (domespit.cpp:231-233); the dome's slider strip is the only user.
    std::int32_t bitmapIdForName(const std::string &name) const;

    void stopEffects();
    /// True while the one effect slot is still sounding. RivenSoundManager's
    /// isEffectPlaying (riven_sound.h:86), for the externals that have to wait
    /// out a sound -- the rebel tunnel's stones are the case that needs it.
    bool isEffectPlaying() const;

    /// One turn of the inner loop, for an external that has to wait: audio,
    /// movies and effects keep running, the screen keeps publishing, and nothing
    /// else happens. ScummVM's externals spin `_vm->doFrame()` for this.
    ///
    /// The pointer does NOT move here -- see pumpInteractiveFrame() for the
    /// loops that need it to.
    void pumpIdleFrame();

    // --- what a drag command needs ------------------------------------------
    //
    // Riven has controls you pull rather than press: the whark elevator's lever,
    // the dome's five sliders. Each is one external command that does not return
    // until the button comes up, and that spins a loop of its own in the
    // meantime. ScummVM can do that because its scripts are queued and
    // RivenStack::onFrame/onMouseUp skip every dispatch while a queued script is
    // running (riven_scripts.cpp:137-147, riven_stack.cpp:277-331) -- the pointer
    // and the button keep moving, and nothing else happens at all.
    //
    // pumpInteractiveFrame() is that, exactly: idleFrame() plus the pointer and
    // the button, and no hit test, no hotspot script, no timer, no queue drain.
    // It must not dispatch, because the loop is running INSIDE the very hotspot
    // script that started it.

    void pumpInteractiveFrame();

    /// riven_stack.cpp:316. False after mouseForceUp() until the player lets go
    /// and presses again.
    bool mouseIsDown() const { return mouseDown_ && !forcedUp_; }

    /// riven_stack.cpp:318-321. "This press is spent" -- the click that started
    /// the command is still held, and the loop about to run would otherwise
    /// count it as the player's answer.
    void mouseForceUp();

    /// The pointer in Riven's 608x392 coordinates, which is the space every
    /// hotspot rect and every threshold in the original is written in.
    int pointerCardX() const;
    int pointerCardY() const;

    /// Where the button went down, in DS screen pixels
    /// (riven_stack.cpp:377). DS pixels rather than card ones on purpose: the
    /// thresholds that read it are compared against a stylus, and a card pixel
    /// is only 0.42 of one.
    int dragStartX() const { return dragStartX_; }
    int dragStartY() const { return dragStartY_; }

    /// The same point in Riven's 608x392 space, for the drags whose thresholds
    /// the original wrote in card pixels rather than as a feel -- bspit's water
    /// valve turns on ten of them (bspit.cpp:432-443), and ten card pixels is
    /// four DS ones. Compare pointerCardX/Y against these and the numbers mean
    /// what they meant when they were written.
    int dragStartCardX() const;
    int dragStartCardY() const;

    // Opcodes 28/31/32/33/34 all name a movie by its MLST "code", never by a
    // slot index, so that is what these take.
    void activateMlst(std::uint16_t index, bool andPlay);
    void enableMovie(std::uint16_t code, bool enabled);
    void disableAllMovies();
    /// RivenVideoManager::closeVideos (riven_video.cpp:151-155). Stop every open
    /// movie WITHOUT baking its last frame -- which is what separates it from
    /// disableAllMovies, opcode 29, however alike the names read. See the body.
    void closeAllMovies();
    void playMovie(std::uint16_t code, bool blocking);
    /// Play the slice of a movie between two times, blocking until it is done.
    ///
    /// For the externals that animate a control one notch at a time out of one
    /// long movie -- the telescope, whose five positions are five slices of the
    /// same file. Times are the original's milliseconds; the frame numbers come
    /// from the movie's own frame rate rather than an assumed one, because the
    /// converter stores it (RvidHeader::fpsNum).
    void playMovieRange(std::uint16_t code, std::uint32_t startMs, std::uint32_t endMs);
    void stopMovie(std::uint16_t code);
    /// Wait `ms`, pumping the loop. Opcode 14 (riven.cpp:661-667).
    void delay(std::uint32_t ms);

    /// Play a movie and let the player cut it short by clicking.
    ///
    /// JSpit::sunnersPlayVideo's wait loop (jspit.cpp:542-567), which is the one
    /// place Riven plays a movie the player is EXPECTED to interrupt: the sunners
    /// bask until you move, and moving is what startles them. True means the
    /// player ended it; false means it ran out.
    ///
    /// Deliberately not playMovieBlocking with an extra key test -- it does not
    /// bake (see the body), and it has to ignore the click that is still held
    /// from the move that got here.
    bool playMovieUntilClick(std::uint16_t code);

    /// Opcode 38. Keep `opcode arg` back until the movie on `code` has been
    /// playing for `delayMs`, and run it when that movie's blocking play ends.
    ///
    /// RivenScriptManager::setStoredMovieOpcode (riven_scripts.cpp:101-106) and
    /// the test at the bottom of RivenVideo::playBlocking (riven_video.cpp:
    /// 255-260). Note where that test is: the original does NOT interrupt the
    /// movie part-way through to run this. It waits for the movie to finish and
    /// then asks whether the movie got as far as the stored time -- so the delay
    /// decides IF the opcode runs, not when. Every one of the 49 shipped uses
    /// delays an activateSLST, which is why the whole mechanism is audible rather
    /// than visible: a cue that arrives at the button press instead of at the end
    /// of the ride.
    ///
    /// There is exactly one of these at a time, and storing a second forgets the
    /// first -- ScummVM's storage is a single member, not a queue.
    void storeMovieOpcode(std::uint16_t code, std::uint32_t delayMs, std::uint16_t opcode,
                          std::uint16_t arg);

    /// Run a stored opcode if it was waiting on `code` and that movie reached its
    /// time. Clears it either way it fires; leaves it alone otherwise, because
    /// the movie it names may not have been played yet.
    void runStoredMovieOpcode(std::uint16_t code);

    /// Whether the movie on `code` has stopped -- ScummVM's
    /// `!oldVideo || oldVideo->endOfVideo()` (jspit.cpp:578). A code no MLST
    /// record on this card ever claimed counts as ended, because openSlot on one
    /// hands back an empty handle that is already at its end
    /// (riven_video.cpp:309-322).
    bool movieEnded(std::uint16_t code) const;

    /// How long the movie on `code` runs, in milliseconds, or 0 if it is not
    /// open. RivenVideo::getDuration, for the top-stairs timer (jspit.cpp:589).
    std::uint32_t movieDurationMs(std::uint16_t code) const;

    /// How far into the movie on `code` playback has got, in milliseconds, or 0
    /// if it is not open. RivenVideo::getTime (riven_video.cpp:106-113).
    ///
    /// ospit's cage is the only caller: xbookclick waits out two spans of one
    /// long movie -- named to it in QuickTime's 1/600 second units -- and the
    /// player's window to use the trap book is the gap between them.
    std::uint32_t movieTimeMs(std::uint16_t code) const;

    /// Where the movie on `code` has got to, and how long it is, in frames --
    /// both -1/0 when nothing is open on that code. The dome's check needs the
    /// pair together (domespit.cpp:50-64).
    std::int32_t movieCurrentFrame(std::uint16_t code) const;
    std::uint32_t movieFrameCount(std::uint16_t code) const;

    // --- the card timer -----------------------------------------------------
    //
    // RivenStack's one-slot timer (riven_stack.cpp:381-410). A card that wants
    // something to happen while the player stands still installs a proc on the
    // way in; the proc RE-ARMS OR REMOVES ITSELF, exactly as the original's does
    // (riven_stack.cpp:383), which is why checkTimer does not clear it.
    //
    // The clock is the vblank, because the DS has no other one -- Engine::delay
    // already counts frames for the same reason. 2^32 frames is two years.

    using TimerProc = void (*)(Engine &);

    /// `name` is for the trace and the console's `timer` command, and it is
    /// required rather than defaulted for a reason: the six timers in the game
    /// are the six things that happen while the player stands still doing
    /// nothing, so when one of them does not happen there is no click to point
    /// at and no card change to blame. A function pointer says nothing on a
    /// console; a literal that travels with the install says which of the six
    /// is armed. Expected to be a string literal -- it is stored by pointer,
    /// not copied.
    void installTimer(TimerProc proc, std::uint32_t ms, const char *name);
    void removeTimer();
    /// What is armed, or nullptr. For the console.
    const char *timerName() const { return timerProc_ != nullptr ? timerName_ : nullptr; }
    /// Frames until the armed timer is due, or 0 if it is overdue or idle.
    std::uint32_t timerFramesLeft() const
    {
        return (timerProc_ != nullptr && timerDeadline_ > frames_) ? timerDeadline_ - frames_ : 0;
    }
    /// Engine frames since boot. Both loops advance it: a clock that stopped for
    /// the length of a cutscene would let a timer fire the instant one ended.
    std::uint32_t clock() const { return frames_; }
    static std::uint32_t msToFrames(std::uint32_t ms) { return (ms * 60 + 999) / 1000; }

    // --- script queue -------------------------------------------------------

    /// Run `commands` now, or queue them to run at the end of the frame.
    void runCommands(const std::vector<rivendata::Command> &commands, bool queue);
    void runHandlers(const std::vector<rivendata::Handler> &handlers,
                     rivendata::ScriptEvent event, bool queue);
    bool runningQueued() const { return runningQueued_; }

    /// RivenScriptManager::stopAllScripts (riven_scripts.cpp:158-162). Abandon
    /// the rest of every command list currently being walked.
    ///
    /// One caller, and it is the reason this exists: ospit's trap-book ending
    /// runs from a hotspot on the cage card, and by the time xbookclick returns
    /// the player is somewhere else entirely. The commands after it on that
    /// hotspot would put them back in the cage.
    ///
    /// A flag rather than an exception: the interpreter is plain recursion over
    /// vectors that live inside the loaded Stack, and unwinding through it is
    /// exactly what the deferred-navigation machinery exists to avoid. Cleared
    /// when the outermost list returns, so it cannot leak into the next script.
    void stopScripts() { stopScripts_ = true; }
    bool scriptsStopped() const { return stopScripts_; }

    /// Screen-update batching. Opcodes 20 and 21.
    void beginScreenUpdate() { ++updateDepth_; }
    void applyScreenUpdate(bool force = false);

    /// RivenGraphics::enableCardUpdateScript (riven_graphics.cpp:789-791). Turn
    /// off to publish a picture the card's own CardUpdate script would paint
    /// straight over -- the gallows carriage is the one command that needs it,
    /// and it turns it back on two lines later.
    void enableCardUpdateScript(bool enable) { cardUpdateEnabled_ = enable; }

    /// Opcode 18. Ask for the next completed screen update to arrive as a slide
    /// or a dissolve rather than instantly. Riven schedules the transition BEFORE
    /// drawing the card it applies to, so this only records the request; it is
    /// applyScreenUpdate that runs it, once the new picture is finished.
    void scheduleTransition(rivendata::Transition t);

    const std::string &error() const { return error_; }

    /// A one-line note for the top screen. Overwritten freely; this is a
    /// developer aid, not a game feature.
    /// One transient line at the bottom of the top screen: the answer to a
    /// button the player just pressed. "note taken", "notebook full".
    ///
    /// This was a setter into a std::string that NOTHING ever read -- six call
    /// sites writing to a field with no renderer. It only looked like it worked
    /// because DebugLog::note printed the same events, and gating that is what
    /// made this have to become real. An empty string clears it now; it also
    /// clears itself after a couple of seconds (DebugLog::status).
    void setStatus(const std::string &s) { DebugLog::status(s.c_str()); }

private:
    /// What an MLST record assigned to a playback slot, and whether the file for
    /// it is currently open.
    ///
    /// The two are separate because a card can activate more MLST records than it
    /// ever plays -- tspit's opening cutscene activates four 608x392 movies on one
    /// card and plays them one at a time -- and a fullscreen movie's plane buffers
    /// are 258 KB. Holding four would be a megabyte of a 4 MB machine spent on
    /// pictures three of them are not showing, so a slot carries the record and
    /// the file is opened when it is actually played.
    struct MovieSlot
    {
        RvidPlayer player;
        /// The MLST "code" this slot is standing in for. Riven's data is free to
        /// use any 16-bit value -- ScummVM keys a hash map on it
        /// (riven_scripts.cpp:773-779) -- so it cannot be an index here.
        std::uint16_t code = 0;
        std::uint16_t movieId = 0;
        std::int16_t left = 0;
        std::int16_t top = 0;
        bool loop = false;
        int volume = 256;
        bool assigned = false; ///< an MLST record has been activated into this slot
        bool enabled = false;  ///< opcodes 28/31
        bool open = false;     ///< the file is open and the planes allocated
    };

    /// The slot standing in for an MLST code, or -1 if no record has claimed it.
    std::int32_t slotForCode(std::uint16_t code) const;
    /// The same, claiming a free slot if the code has none yet.
    std::int32_t claimSlotForCode(std::uint16_t code);

    /// Open the slot's movie if an MLST has assigned one and it is not open yet.
    /// Takes a SLOT INDEX, not an MLST code.
    bool ensureSlotOpen(std::int32_t slot);
    void closeSlot(std::int32_t slot);

    /// Give every playback slot back, because the card that filled them is gone.
    ///
    /// RivenCard::~RivenCard (riven_card.cpp:52-60), whose last three lines are
    /// clearWaterEffect, clearFliesEffect and closeVideos. resetCardState() has
    /// always done the first two; this is the third, and leaving it out is what
    /// let an overlay from one card go on being drawn on the next -- the same
    /// nineteen-by-twenty-five pixels of a jungle tree turning up on three
    /// unrelated cards, because nothing between them ever closed the slot.
    ///
    /// NOT closeAllMovies(), however alike they read: that one leaves the
    /// records ASSIGNED so that bspit's boiler can stop its loop and then play
    /// code 11 without re-activating it (xbchangeboiler). This unassigns, which
    /// is what makes a code the new card never activated behave the way
    /// ScummVM's openSlot on a closed video does -- empty, ended, duration
    /// zero.
    void releaseCardMovies();

    /// Stop a fullscreen movie and give its two background buffers back. Both the
    /// blocking and the non-blocking path end here; the non-blocking one used not
    /// to, which left the bottom screen black for good.
    ///
    /// `bake` picks WHICH image survives, and the two are not interchangeable:
    ///
    ///  - true is ScummVM's disable() -- the movie's last frame becomes the card
    ///    (riven_video.cpp:288-301). Only playBlocking ends that way, and it is
    ///    what a card that chains several movies needs, because each one has to
    ///    start from where the last one stopped.
    ///  - false puts the card's own drawing back, which is where a movie that was
    ///    merely stopped leaves the screen: the next update repaints from
    ///    _mainScreen and the movie is gone.
    ///
    /// Spelled at every call site rather than defaulted, because the callers
    /// genuinely disagree and a default would let the wrong one pass unread.
    void endFullscreenMovie(MovieSlot &m, bool bake);

    /// Run the transition opcode 18 asked for, blocking until it finishes -- the
    /// way ScummVM's own runScheduledTransition does (riven_graphics.cpp:549-609).
    void runScheduledTransition();

    /// playMovie() with the code already resolved to a slot index. The frame
    /// range defaults to the whole movie; RvidPlayer::play clamps it.
    void playMovieSlot(std::int32_t slot, bool blocking, std::uint32_t startFrame = 0,
                       std::uint32_t stopFrame = 0xFFFFFFFFu);
    /// Wait for the movie in slot INDEX `slot` to finish, pumping the loop.
    /// `whole` says the play was not a segment, which is the only case that
    /// bakes its last frame into the card (see kBakeBlockingMovies).
    void playMovieBlocking(std::int32_t slot, bool whole);

    /// Draw every LITE overlay that is still playing back onto the card picture,
    /// after something has written over the rows it sits in --
    /// CardSurface::refreshFromClean, which takes them all off, or a water
    /// effect, which erases whatever share of one it happens to cover.
    ///
    /// `rows` is the band mask that was overwritten; an overlay outside it is
    /// left alone, because putting one back costs a blit and the effects ask
    /// for this four times a second.
    void recompositeOverlays(std::uint32_t rows = CardSurface::kAllDirty);
    /// Make one overlay's last frame part of the card -- ScummVM's disable().
    void bakeOverlay(MovieSlot &m);

    void enterCard();
    /// The one ungated diagnostic: what the card that just settled ended up with.
    void logCardSummary() const;
    /// The card's CardLeave script, run before it stops being the current card.
    void leaveCard();

    /// Remember the current card as a zip destination if it names itself one,
    /// then enable or disable this card's zip hotspots against what has been
    /// seen. RivenCard::initializeZipMode (riven_card.cpp:652-670).
    void initializeZipMode();
    /// SELECT+START: the debug command prompt, on an on-screen keyboard.
    ///
    /// Debug mode only. The whole body, including the command table, is in
    /// engine/DebugConsole.cpp -- as a member so the commands reach this class's
    /// private state without one accessor being widened for a debug tool.
    void runDebugConsole();
    /// One typed line. True closes the console.
    bool runConsoleCommand(const std::string &line);

    void processInput();
    /// The two halves of processInput() that a drag loop also needs, and the
    /// only ones -- see pumpInteractiveFrame().
    void updatePointer(const touchPosition &touch);
    void updateMouseLatch();
    void pumpMovies();
    /// One engine frame of the card's ambient effects: its water, if it has one
    /// and the player has not turned water off, and its insects.
    /// RivenGraphics::updateEffects (riven_graphics.cpp:772-780).
    void pumpEffects();

    /// A frame's screen work: publish what the scripts changed, flip, step a
    /// transition, write OAM. Called straight after NEA_WaitForVBL because that
    /// is when the video registers may be touched, not because any of the copies
    /// need a window -- none of them do any more.
    void flushUploads();

    /// Apply a stack change that was held until the script stopped running.
    /// Run whatever was held back because a script was running: a queued stack
    /// change, or a load. Called the moment the outermost command list returns.
    void applyDeferredNavigation();

    /// One pass of the wait-and-draw loop that a blocking movie or a script's
    /// delay spins on, so both spin on the same thing.
    void idleFrame();

    /// The card's own scripts, then hotspots, reset for a new card.
    void resetCardState();

    /// Fire the card's timer if it is due. RivenStack::checkTimer
    /// (riven_stack.cpp:387-401).
    void checkTimer();

    std::string picPath(std::uint16_t tbmpId) const;
    std::string soundPath(std::uint16_t twavId) const;
    std::string moviePath(std::uint16_t tmovId) const;

    rivendata::Stack stack_;
    const rivendata::Card *card_ = nullptr;
    std::uint16_t cardId_ = 0;

    Vars vars_;
    CardSurface surface_;
    Cursor cursor_;
    Inventory inventory_;
    /// The card's water, if it activated any. One at a time, like the original:
    /// scheduleWaterEffect replaces whatever was running (riven_graphics.cpp:403).
    WaterEffect water_;
    /// The card's insects, if xflies asked for any. Also one at a time, and also
    /// replaced rather than added to (riven_graphics.cpp:754-757).
    FliesEffect flies_;

    /// The card's timer, if it installed one, and when it comes due -- both in
    /// engine frames. One slot, like the original's (riven_stack.h:213-214).
    TimerProc timerProc_ = nullptr;
    std::uint32_t timerDeadline_ = 0;
    /// What installTimer was told this proc is called. A borrowed string
    /// literal; nothing else would be safe, and nothing else is passed.
    const char *timerName_ = "";
    /// Engine frames since boot, advanced by frame() and idleFrame() alike.
    std::uint32_t frames_ = 0;
    /// True while a timer proc is running. A proc may spin idleFrame() for the
    /// length of a movie, and this is what makes it provable that nothing can
    /// re-enter checkTimer while it does -- the same job as ScummVM running its
    /// procs from inside the queued-script drain (riven_scripts.cpp:984).
    bool inTimer_ = false;

    /// The key register as pollNoteHotkey last saw it. ITS OWN, and not
    /// keysDown(): this is called from frame() and from idleFrame(), which
    /// disagree about who calls scanKeys() -- and a second scanKeys() in one
    /// frame computes its edge against the mask the first one stored, so it
    /// reports nothing pressed and eats the caller's own B/START. The same
    /// reasoning, and the same shape, as DebugLog::pollHotkeys.
    std::uint32_t noteKeys_ = 0;

    /// True while idleFrame() is on the stack. captureNote() pumps a frame
    /// either side of its SD write to keep the audio stream fed, and it is now
    /// reached FROM that loop -- so the pump has to be skipped rather than
    /// re-entered. Left alone, the recursion would be one frame deep and
    /// harmless, but it would also flip buffers underneath a caller that had
    /// just filled one.
    bool inIdleFrame_ = false;

    /// How many CursorHide guards are alive. Read once a frame in flushUploads.
    int cursorSuppress_ = 0;

    /// The pointer, in DS screen pixels. Persistent on purpose: a stylus has no
    /// hover, so a pointer that existed only while the stylus was down could
    /// never show a hotspot's cursor shape -- which is the one thing Riven uses
    /// the cursor for. Touch sets it, lifting leaves it, the D-pad nudges it.
    int pointerX_ = rivendata::kScreenW / 2;
    int pointerY_ = rivendata::kViewOffsetY + rivendata::kViewH / 2;
    /// Frames the D-pad has been held, for the slow-then-fast nudge.
    int padHeld_ = 0;

    /// Whether anything else has been pressed during the current SELECT hold.
    ///
    /// SELECT is the developer chord AND, in debug mode, the command prompt on
    /// its own -- so the two have to be told apart, and the only thing that
    /// separates them is whether a second button ever joined in. Latched while
    /// SELECT is down and read when it comes up, because by then whatever was
    /// pressed with it has usually been let go of as well. See processInput.
    bool selectChorded_ = false;

    /// The button, and where it went down. RivenStack's _mouseIsDown and
    /// _mouseDragStartPosition (riven_stack.h:216-218): kept as members rather
    /// than as locals in processInput() because an external's wait loop reads
    /// them while no dispatch is running.
    bool mouseDown_ = false;
    bool forcedUp_ = false;
    int dragStartX_ = 0;
    int dragStartY_ = 0;

    /// Whether a completed screen update runs the card's CardUpdate script.
    /// See enableCardUpdateScript(); true everywhere except inside one command.
    bool cardUpdateEnabled_ = true;

    /// The transition opcode 18 asked for, and the guard that keeps
    /// runScheduledTransition out of itself -- it spins idleFrame(), which can
    /// reach applyScreenUpdate again through a LITE movie.
    rivendata::Transition scheduledTransition_ = rivendata::Transition::None;
    bool inTransition_ = false;

    /// Parallel to card_->hotspots: whether each is currently enabled. Kept
    /// beside the data rather than in it, because the card graph is const --
    /// it is the converter's output, shared by every visit to the card.
    std::vector<bool> hotspotEnabled_;
    /// Parallel to card_->hotspots as well: where each one is. Starts as a copy
    /// of the data's own rects and is only ever different on the marble grid.
    /// See hotspotRect().
    std::vector<rivendata::Rect> hotspotRect_;
    /// SLST records this card has had its sounds replaced on, as zero-based
    /// {slot, take the sounds of this slot}. Empty on every card but bspit's
    /// crater. See overrideCardSound().
    std::vector<std::pair<std::uint16_t, std::uint16_t>> soundOverride_;
    const rivendata::Hotspot *currentHotspot_ = nullptr;
    Mode mode_ = Mode::Card;
    /// The tBMP of the last PLST record that covered the whole card view. Only
    /// full-card pictures are recorded: the overlays a script draws on top are
    /// PLST records too, and zooming into a 39x76 button would be nothing.
    std::uint16_t cardPicture_ = 0;
    /// Every zip destination visited this game. Small: 22 cards in the whole of
    /// Riven carry a non-zero zipModePlace.
    std::vector<ZipDest> zipDests_;
    /// Which hotspot the stylus is inside, for the enter/leave pair.
    std::int32_t insideHotspot_ = -1;
    /// Which hotspot the press landed on, latched until the stylus lifts. The
    /// release frame has no touch position to hit-test -- libnds has already
    /// cleared KEY_TOUCH from keysHeld() -- so this is the only thing that knows
    /// whose MouseUp script to run.
    std::int32_t pressedHotspot_ = -1;
    /// The inventory book the press landed on, latched for the same reason and
    /// used for one more: the release must land on it too, or a drag that ends
    /// in the strip would open a journal. Engine::processInput.
    std::uint16_t pressedInvItem_ = 0;

    MovieSlot movies_[kMovieSlots];

    /// Opcode 38's one held-back command. See storeMovieOpcode().
    ///
    /// A code and not a slot index: the code is what the script named and what
    /// opcode 32 will name, and a slot is a thing this engine allocates -- the
    /// two only agree by accident.
    struct StoredMovieOpcode
    {
        bool set = false;
        std::uint16_t code = 0;
        std::uint32_t delayMs = 0;
        std::uint16_t opcode = 0;
        std::uint16_t arg = 0;
    };
    StoredMovieOpcode storedMovieOpcode_;

    /// The current SLST's layers: one entry per layer IN THE RECORD, holding the
    /// RivenAudio::playSound slot, or -1 for a layer that did not start.
    ///
    /// Parallel to the record's layers and not packed to the ones that sound,
    /// because playSlst grows this list rather than rebuilding it: a packed array
    /// would make a failed layer shift every later one down, so the next
    /// activation of the same bed would start an already-sounding layer a second
    /// time and the mix would be applied to the wrong slots.
    int ambientSlots_[RivenAudio::kSoundSlots] = {};
    int ambientCount_ = 0;
    /// The first sound id of the SLST now sounding, or -1. ScummVM's
    /// _mainAmbientSoundId (riven_sound.cpp:38), and the one value that decides
    /// whether an SLST is this bed CONTINUING or a different one starting --
    /// which is the whole of playSlst's behaviour. Only 47 distinct beds exist
    /// across the 2137 SLST-1 records in the shipped game, so most card changes
    /// are a continuation and restarting one is both audible and an SD read.
    std::int32_t mainAmbientId_ = -1;
    int effectSlot_ = -1;

    std::vector<rivendata::Command> queued_;
    bool runningQueued_ = false;
    /// Set by stopScripts(), cleared when the outermost command list returns.
    bool stopScripts_ = false;
    /// How deep the interpreter is. Non-zero means a command list is being walked
    /// out of the loaded stack's memory, so the stack must not be replaced.
    int scriptDepth_ = 0;
    bool haveStackChange_ = false;
    rivendata::StackId pendingStack_ = rivendata::StackId::None;
    std::uint32_t pendingCard_ = 0;
    /// Whether pendingCard_ is a local card id or an RMAP global one. See
    /// changeToStackAndCard -- the two are different numbering and there is no
    /// telling them apart by value.
    bool pendingCardIsLocal_ = false;
    /// A load asked for from inside a script, held until the interpreter is out
    /// of the stack it is about to replace. See restoreFrom.
    bool haveRestore_ = false;
    SaveGame::SaveState pendingRestore_;
    int updateDepth_ = 0;
    /// Guards the CardUpdate script against re-entering itself: it draws, and
    /// drawing is what ends a screen update (riven_graphics.cpp:740).
    bool runningUpdate_ = false;
    /// Guards the CardLeave script against re-entering itself: a leave script may
    /// change the card, which is another leave.
    bool leavingCard_ = false;
    bool activatedPlst_ = false;
    bool activatedSlst_ = false;
    bool quit_ = false;
    bool booted_ = false;

    std::string error_;
};

extern Engine engine;

} // namespace rivenrt
