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

    /// Follow a ChangeStack command: resolve the destination's RMAP global id
    /// against the target stack's table (riven_stack.cpp:138-150).
    ///
    /// DEFERRED while a script is running, and it has to be. A command list is a
    /// std::vector living inside the loaded Stack, and loading the next stack
    /// destroys it -- so replacing the stack from inside a running script would
    /// pull the commands out from under the interpreter. ScummVM survives the same
    /// situation because its scripts are reference-counted objects; here the
    /// change is held until the outermost script returns.
    bool changeToStackAndGlobalCard(rivendata::StackId id, std::uint32_t globalCardId);
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

    /// Forget every zip destination. New game (riven.cpp:679).
    void clearZipDests() { zipDests_.clear(); }

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

    void activateSlst(std::uint16_t index);
    void playSlst(const rivendata::SoundRec &rec);
    void stopAllAmbient();
    /// Start layer `i` of `rec` on a free slot. Shared by the two paths through
    /// playSlst so the mix and the reporting cannot drift apart.
    void startAmbientLayer(const rivendata::SoundRec &rec, std::size_t i);
    void playEffect(std::uint16_t twavId, int volume);
    void stopEffects();

    // Opcodes 28/31/32/33/34 all name a movie by its MLST "code", never by a
    // slot index, so that is what these take.
    void activateMlst(std::uint16_t index, bool andPlay);
    void enableMovie(std::uint16_t code, bool enabled);
    void disableAllMovies();
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

    // --- script queue -------------------------------------------------------

    /// Run `commands` now, or queue them to run at the end of the frame.
    void runCommands(const std::vector<rivendata::Command> &commands, bool queue);
    void runHandlers(const std::vector<rivendata::Handler> &handlers,
                     rivendata::ScriptEvent event, bool queue);
    bool runningQueued() const { return runningQueued_; }

    /// Screen-update batching. Opcodes 20 and 21.
    void beginScreenUpdate() { ++updateDepth_; }
    void applyScreenUpdate(bool force = false);

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

    /// Stop a fullscreen movie and put the card back on screen. Both the blocking
    /// and the non-blocking path end here; the non-blocking one used not to,
    /// which left the bottom screen black for good.
    void endFullscreenMovie(MovieSlot &m);

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
    /// after CardSurface::refreshFromClean has taken them all off.
    void recompositeOverlays();
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
    void pumpMovies();
    /// One engine frame of the card's water effect, if it has one and the player
    /// has not turned water off. RivenGraphics::updateEffects
    /// (riven_graphics.cpp:772-780).
    void pumpWater();

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

    /// The transition opcode 18 asked for, and the guard that keeps
    /// runScheduledTransition out of itself -- it spins idleFrame(), which can
    /// reach applyScreenUpdate again through a LITE movie.
    rivendata::Transition scheduledTransition_ = rivendata::Transition::None;
    bool inTransition_ = false;

    /// Parallel to card_->hotspots: whether each is currently enabled. Kept
    /// beside the data rather than in it, because the card graph is const --
    /// it is the converter's output, shared by every visit to the card.
    std::vector<bool> hotspotEnabled_;
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

    MovieSlot movies_[kMovieSlots];
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
    /// How deep the interpreter is. Non-zero means a command list is being walked
    /// out of the loaded stack's memory, so the stack must not be replaced.
    int scriptDepth_ = 0;
    bool haveStackChange_ = false;
    rivendata::StackId pendingStack_ = rivendata::StackId::None;
    std::uint32_t pendingGlobalCard_ = 0;
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
