#include "Engine.hpp"

#include <cstdio>

#include "Global.hpp"
#include "Settings.hpp"
#include "audio/RivenAudio.hpp"
#include "data/StackFile.hpp"
#include "engine/Script.hpp"
#include "global_header.hpp"

using namespace rivendata;

namespace rivenrt
{

Engine engine;

namespace
{
    /// aspit card 1 is the main menu, and the card the game boots into
    /// (riven.cpp:194-196).
    constexpr StackId kBootStack = StackId::Aspit;
    constexpr std::uint16_t kBootCard = 1;

    /// A rect the data itself does not mean. tspit 371 and 377 carry hotspots
    /// with inverted rects (riven_card.cpp notes them); testing a point against
    /// one would either never hit or hit the whole card.
    bool rectUsable(const Rect &r) { return r.right > r.left && r.bottom > r.top; }

    bool rectContains(const Rect &r, int x, int y)
    {
        return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }
} // namespace

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

std::string Engine::picPath(std::uint16_t tbmpId) const
{
    return global.picsDir() + stackName(stack_.id) + "/" + std::to_string(tbmpId) + ".rpic";
}

std::string Engine::soundPath(std::uint16_t twavId) const
{
    return global.soundDir() + stackName(stack_.id) + "/" + std::to_string(twavId) + ".rsnd";
}

std::string Engine::moviePath(std::uint16_t tmovId) const
{
    return global.videoDir() + stackName(stack_.id) + "/" + std::to_string(tmovId) + ".rvid";
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

std::string Engine::nameFromList(int list, std::uint16_t index) const
{
    if (list < 0 || list >= kNameListCount)
        return std::string();
    const NameList &nl = stack_.names[static_cast<std::size_t>(list)];
    if (index >= nl.names.size())
        return std::string();
    return nl.names[index];
}

rivendata::VarId Engine::variableId(std::uint16_t index) const
{
    if (index >= stack_.variableIds.size())
        return rivendata::VarId::Unknown;
    return stack_.variableIds[index];
}

std::int32_t Engine::idFromName(int list, const std::string &name) const
{
    if (list < 0 || list >= kNameListCount)
        return -1;
    const NameList &nl = stack_.names[static_cast<std::size_t>(list)];
    const std::string want = Vars::normalise(name);
    for (std::size_t i = 0; i < nl.names.size(); ++i)
        if (Vars::normalise(nl.names[i]) == want)
            return static_cast<std::int32_t>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// Hotspots
// ---------------------------------------------------------------------------

const Hotspot *Engine::hotspotByBlstId(std::uint16_t blstId) const
{
    if (card_ == nullptr)
        return nullptr;
    for (const Hotspot &h : card_->hotspots)
        if (h.blstId == blstId)
            return &h;
    return nullptr;
}

const Hotspot *Engine::hotspotByName(const std::string &name) const
{
    if (card_ == nullptr)
        return nullptr;
    const std::int32_t nameId = idFromName(kHotspotNames, name);
    if (nameId < 0)
        return nullptr;
    for (const Hotspot &h : card_->hotspots)
        if (h.nameRes == nameId)
            return &h;
    return nullptr;
}

std::size_t Engine::hotspotIndexOf(const Hotspot *h) const
{
    if (card_ == nullptr || h == nullptr)
        return static_cast<std::size_t>(-1);
    const Hotspot *first = card_->hotspots.data();
    if (h < first || h >= first + card_->hotspots.size())
        return static_cast<std::size_t>(-1);
    return static_cast<std::size_t>(h - first);
}

bool Engine::hotspotEnabled(std::size_t index) const
{
    return index < hotspotEnabled_.size() && hotspotEnabled_[index];
}

void Engine::enableHotspotByIndex(std::size_t index, bool enabled)
{
    if (index < hotspotEnabled_.size())
        hotspotEnabled_[index] = enabled;
}

void Engine::enableHotspot(std::uint16_t blstId, bool enabled)
{
    enableHotspotByIndex(hotspotIndexOf(hotspotByBlstId(blstId)), enabled);
}

// ---------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------

void Engine::drawBitmap(std::uint16_t tbmpId, const Rect &rect)
{
    if (!surface_.exists())
        return;

    std::string err;
    if (!surface_.drawPicture(picPath(tbmpId), rect, err))
        std::printf("card %u: picture %u: %s\n", cardId_, tbmpId, err.c_str());
}

void Engine::activatePlst(std::uint16_t index)
{
    activatedPlst_ = true;
    if (card_ == nullptr)
        return;
    for (const PictureRec &p : card_->plst)
        if (p.index == index)
        {
            drawBitmap(p.id, p.rect);
            return;
        }
    std::printf("card %u: no PLST record %u\n", cardId_, index);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void Engine::stopAllAmbient()
{
    for (int i = 0; i < ambientCount_; ++i)
        RivenAudio::stopSound(ambientSlots_[i]);
    ambientCount_ = 0;
}

void Engine::playSlst(const SoundRec &rec)
{
    stopAllAmbient();

    // globalVolume scales every layer, 0..256 (RivenData.hpp:397).
    const int global256 = rec.globalVolume == 0 ? 256 : rec.globalVolume;
    for (std::size_t i = 0; i < rec.soundIds.size(); ++i)
    {
        if (ambientCount_ >= RivenAudio::kSoundSlots)
        {
            std::printf("card %u: SLST has more layers than there are channels\n", cardId_);
            break;
        }
        const int vol = i < rec.volumes.size() ? rec.volumes[i] : 255;
        const int bal = i < rec.balances.size() ? rec.balances[i] : 0;
        const int slot = RivenAudio::playSound(soundPath(rec.soundIds[i]),
                                               vol * global256 / 256, bal, rec.loop != 0);
        if (slot >= 0)
            ambientSlots_[ambientCount_++] = slot;
    }
}

void Engine::activateSlst(std::uint16_t index)
{
    activatedSlst_ = true;
    if (card_ == nullptr)
        return;
    for (const SoundRec &s : card_->slst)
        if (s.index == index)
        {
            playSlst(s);
            return;
        }
}

void Engine::playEffect(std::uint16_t twavId, int volume)
{
    // One effect at a time, matching ScummVM's single effect handle: the original
    // depends on a door sound being cut short by the click of the next move.
    stopEffects();
    effectSlot_ = RivenAudio::playSound(soundPath(twavId), volume == 0 ? 255 : volume, 0,
                                        false);
}

void Engine::stopEffects()
{
    if (effectSlot_ >= 0)
    {
        RivenAudio::stopSound(effectSlot_);
        effectSlot_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Movies
// ---------------------------------------------------------------------------

std::int32_t Engine::slotForCode(std::uint16_t code) const
{
    for (std::int32_t i = 0; i < kMovieSlots; ++i)
        if (movies_[i].assigned && movies_[i].code == code)
            return i;
    return -1;
}

/// MLST codes are arbitrary 16-bit values -- ScummVM keys a hash map on them
/// (riven_scripts.cpp:773-779) -- so they are mapped onto the fixed slots here
/// rather than used as indices. Using them as indices meant any code >= 8 was
/// folded onto slot 0 by activateMlst and then rejected as out of range by
/// playMovie, so the record clobbered slot 0 and never played.
std::int32_t Engine::claimSlotForCode(std::uint16_t code)
{
    const std::int32_t existing = slotForCode(code);
    if (existing >= 0)
        return existing;

    for (std::int32_t i = 0; i < kMovieSlots; ++i)
        if (!movies_[i].assigned)
        {
            movies_[i].code = code;
            return i;
        }

    // More distinct codes on one card than there are slots. Take one that is not
    // on screen rather than dropping the record; the alternative is a movie that
    // silently never plays, which is the bug this function exists to fix.
    for (std::int32_t i = 0; i < kMovieSlots; ++i)
        if (!movies_[i].player.isPlaying())
        {
            closeSlot(i);
            movies_[i].code = code;
            return i;
        }

    std::printf("card %u: more than %d movies at once\n", cardId_, kMovieSlots);
    return -1;
}

void Engine::activateMlst(std::uint16_t index, bool andPlay)
{
    if (card_ == nullptr)
        return;

    for (const MovieRec &m : card_->mlst)
    {
        if (m.index != index)
            continue;

        // The record is remembered; the FILE is not opened. See MovieSlot on why:
        // a card can activate far more movies than it plays, and a fullscreen
        // one's planes are 258 KB.
        const std::int32_t slot = claimSlotForCode(m.slot);
        if (slot < 0)
            return;
        MovieSlot &ms = movies_[slot];
        if (ms.open && ms.movieId != m.movieId)
            closeSlot(slot);

        ms.movieId = m.movieId;
        ms.left = m.left;
        ms.top = m.top;
        ms.loop = m.loop != 0;
        // MLST is where position, looping and volume come from, never the movie
        // file (riven_scripts.cpp:773-779).
        ms.volume = m.volume == 0 ? 256 : m.volume;
        ms.assigned = true;
        ms.enabled = true;
        if (ms.open)
        {
            ms.player.setVolume(ms.volume);
            // The slot was kept because the movie is the same one, but the record
            // that brought it back can put it somewhere else.
            ms.player.setPosition(ms.left, ms.top);
        }

        if (andPlay)
            playMovieSlot(slot, false);
        return;
    }
    std::printf("card %u: no MLST record %u\n", cardId_, index);
}

bool Engine::ensureSlotOpen(std::int32_t slot)
{
    if (slot < 0 || slot >= kMovieSlots)
        return false;
    MovieSlot &ms = movies_[slot];
    if (!ms.assigned)
        return false;
    if (ms.open)
        return true;

    const std::string path = moviePath(ms.movieId);
    if (!ms.player.open(path, ms.left, ms.top))
    {
        // Named by stack and id rather than by full path: the console is 32
        // columns wide and the drive prefix is the part nobody needs.
        std::printf("movie %s/%u: %s\n", stackName(stack_.id), ms.movieId,
                    ms.player.error());
        return false;
    }
    ms.player.setVolume(ms.volume);
    ms.open = true;
    return true;
}

void Engine::closeSlot(std::int32_t slot)
{
    if (slot < 0 || slot >= kMovieSlots)
        return;
    MovieSlot &ms = movies_[slot];
    ms.player.close();
    ms.open = false;
}

void Engine::enableMovie(std::uint16_t code, bool enabled)
{
    const std::int32_t slot = enabled ? claimSlotForCode(code) : slotForCode(code);
    if (slot < 0)
        return;
    movies_[slot].enabled = enabled;
    if (!enabled && movies_[slot].open)
        movies_[slot].player.stop();
}

void Engine::disableAllMovies()
{
    for (std::int32_t i = 0; i < kMovieSlots; ++i)
    {
        movies_[i].enabled = false;
        if (movies_[i].open)
            movies_[i].player.stop();
    }
}

void Engine::playMovie(std::uint16_t code, bool blocking)
{
    const std::int32_t slot = slotForCode(code);
    if (slot < 0)
    {
        // Opcode 32/33 for a code no MLST record on this card ever activated.
        // Silent before; worth a line now that there is somewhere to put it.
        std::printf("card %u: no movie activated as %u\n", cardId_, code);
        return;
    }
    playMovieSlot(slot, blocking);
}

void Engine::playMovieSlot(std::int32_t slot, bool blocking)
{
    if (!ensureSlotOpen(slot))
        return;
    MovieSlot &ms = movies_[slot];

    // A blocking movie never loops -- it would never return (opcode 32 sets
    // looping off explicitly, riven_scripts.cpp:669-675).
    const bool loop = ms.loop && !blocking;

    const bool full = ms.player.profile() == VideoProfile::Full;
    if (full)
    {
        // Only one fullscreen movie can be on screen, so only one needs to be
        // open. tspit's opening cutscene is the case that makes this matter: it
        // activates four 608x392 movies on one card and plays them one after
        // another, and reopening one is a header read.
        for (std::int32_t i = 0; i < kMovieSlots; ++i)
            if (i != slot && movies_[i].open
                && movies_[i].player.profile() == VideoProfile::Full)
                closeSlot(i);

        // A fullscreen movie takes the two buffers that are NOT on screen and
        // leaves the card's own image parked in the third. That is the whole
        // reason there are three: the card is never torn down, so the handover
        // back at the end is a rebind rather than another 84 KB upload, and the
        // card never has to be re-entered (which would re-run its scripts and
        // start this movie over).
        ms.player.setSurface(&bgs);
        bgs.beginMovieTakeover();
    }

    if (!ms.player.play(loop))
    {
        std::printf("card %u: movie %u will not play: %s\n", cardId_, ms.movieId,
                    ms.player.error());
        if (full)
            surface_.invalidate(bgs.endMovieTakeover());
        return;
    }

    if (blocking)
        playMovieBlocking(slot);
}

void Engine::stopMovie(std::uint16_t code)
{
    const std::int32_t slot = slotForCode(code);
    if (slot >= 0 && movies_[slot].open)
        movies_[slot].player.stop();
}

/// One turn of the wait-and-draw loop, in the order the hardware demands: wait
/// for vblank, do the uploads while that window is open, build the display list,
/// and only then spend time decoding.
void Engine::idleFrame()
{
    NEA_WaitForVBL(static_cast<NEA_UpdateFlags>(0));
    flushUploads();

    RivenAudio::pump();
    pumpMovies();
}

void Engine::playMovieBlocking(std::int32_t slot)
{
    if (slot < 0 || slot >= kMovieSlots)
        return;
    MovieSlot &ms = movies_[slot];
    const bool full = ms.player.profile() == VideoProfile::Full;

    // The original blocks the script here, and the game leans on it: a cutscene
    // that returned immediately would have the next command draw over it.
    while (ms.player.isPlaying() && !ms.player.finished() && !quit_)
    {
        idleFrame();

        scanKeys();
        if ((keysDown() & (KEY_B | KEY_START)) != 0)
            break; // let the player out of a long cutscene
    }
    if (full)
        endFullscreenMovie(ms); // stops it too
    else
        ms.player.stop();
}

void Engine::delay(std::uint32_t ms)
{
    // 60 frames a second. The original counts real milliseconds; on the DS the
    // vblank IS the clock, and a script's delay is always a round number of
    // tenths.
    const std::uint32_t frames = (ms * 60 + 999) / 1000;
    for (std::uint32_t i = 0; i < frames && !quit_; ++i)
        idleFrame();
}

// ---------------------------------------------------------------------------
// Scripts
// ---------------------------------------------------------------------------

void Engine::runCommands(const std::vector<Command> &commands, bool queue)
{
    if (commands.empty())
        return;
    if (queue)
    {
        queued_.insert(queued_.end(), commands.begin(), commands.end());
        return;
    }

    // The depth is what makes a deferred stack change safe: `commands` lives
    // inside the loaded Stack, so the stack can only be swapped once no
    // interpreter is walking one.
    ++scriptDepth_;
    runCommandList(*this, commands);
    --scriptDepth_;
    if (scriptDepth_ == 0)
        applyPendingStackChange();
}

void Engine::runHandlers(const std::vector<Handler> &handlers, ScriptEvent event, bool queue)
{
    for (const Handler &h : handlers)
        if (static_cast<ScriptEvent>(h.event) == event)
            runCommands(h.commands, queue);
}

/// RivenGraphics::applyScreenUpdate (riven_graphics.cpp:732-751).
///
/// The nesting is what makes the CardUpdate script fire at the right moment, and
/// that script is load-bearing: aspit card 1 calls xastartupbtnhide from it, and
/// the two journals draw their current page from it. Running it on every
/// applyScreenUpdate rather than only the outermost would draw a book page for
/// each nested update.
void Engine::applyScreenUpdate(bool force)
{
    if (force)
        updateDepth_ = 0;
    else if (updateDepth_ > 0)
        --updateDepth_;

    if (updateDepth_ > 0 || runningUpdate_)
        return;

    runningUpdate_ = true;
    if (card_ != nullptr)
        runHandlers(card_->scripts, ScriptEvent::CardUpdate, false);
    updateDepth_ = 0;
    runningUpdate_ = false;

    // The batch is closed, so the new picture is complete. Without a transition
    // the pixels reach VRAM in the next flushUploads() and appear in one flip;
    // with one, they have to go up NOW so that both buffers hold finished
    // pictures for the slide to move past each other.
    if (scheduledTransition_ != Transition::None)
        runScheduledTransition();
}

void Engine::scheduleTransition(Transition t)
{
    // The player's setting is refused HERE rather than in
    // runScheduledTransition, so that "off" means the request never existed:
    // a transition that was scheduled and then skipped still costs the frame
    // applyScreenUpdate spends asking, and still leaves the flag to clear.
    scheduledTransition_ = settings.transitions ? t : Transition::None;
}

/// ScummVM blocks here too (riven_graphics.cpp:549-609): it spins its own
/// doFrame() until the effect is done, and so does this -- on idleFrame(), the
/// same primitive opcode 14's delay() and a blocking movie already use.
///
/// Blocking is also what swallows input for the duration, because idleFrame()
/// does not call processInput().
void Engine::runScheduledTransition()
{
    const Transition t = scheduledTransition_;
    scheduledTransition_ = Transition::None;

    if (inTransition_ || t == Transition::None)
        return;
    // A fullscreen movie owns both displayed buffers; there is no old/new card
    // pair to slide, and stealing them would stall the cutscene.
    if (fullscreenMoviePlaying() || !bgs.exists() || !surface_.exists())
        return;

    const int back = bgs.backBuffer();
    if (!surface_.anyDirty(back))
        return; // nothing actually changed; a slide would show the same card twice

    inTransition_ = true;
    surface_.publish(bgs, back);
    bgs.beginTransition(t);
    while (bgs.transitionActive() && !quit_)
        idleFrame();
    inTransition_ = false;
}

bool Engine::fullscreenMoviePlaying() const
{
    for (const MovieSlot &m : movies_)
        if (m.open && m.player.isPlaying()
            && m.player.profile() == VideoProfile::Full)
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------

bool Engine::changeToStack(StackId id)
{
    if (booted_ && stack_.id == id)
        return true;

    // The card that is going away gets its leave script first, while its stack is
    // still loaded -- the script lives in the vector that is about to be replaced.
    leaveCard();

    // Everything holding onto the old stack's data goes first: the movie players
    // hold open files under the old stack's directory and the card graph they
    // were opened from is about to be replaced.
    disableAllMovies();
    for (std::int32_t i = 0; i < kMovieSlots; ++i)
    {
        closeSlot(i);
        // The record fields, not the player: RvidPlayer owns a file and buffers
        // and is deliberately not assignable.
        MovieSlot &m = movies_[i];
        m.code = 0;
        m.movieId = 0;
        m.left = 0;
        m.top = 0;
        m.loop = false;
        m.volume = 256;
        m.assigned = false;
        m.enabled = false;
    }
    stopAllAmbient();
    stopEffects();

    card_ = nullptr;
    hotspotEnabled_.clear();

    const std::string path = global.stacksDir() + stackFileName(id);
    Stack loaded;
    std::string err;
    if (!loadStackFile(path, loaded, err))
    {
        error_ = std::string(stackName(id)) + ": " + err;
        std::printf("%s\n", error_.c_str());
        return false;
    }
    stack_ = std::move(loaded);
    booted_ = true;
    return true;
}

bool Engine::changeToCard(std::uint16_t cardId)
{
    const Card *next = stack_.findCard(cardId);
    if (next == nullptr)
    {
        // RMAP lists ids that have no CARD resource, and ScummVM skips those too
        // (riven_stack.cpp:164-177). Reaching one from a script is a bug in our
        // resolution, not in the data, so it is worth saying.
        std::printf("%s: no card %u\n", stackName(stack_.id), cardId);
        return false;
    }

    leaveCard();
    card_ = next;
    cardId_ = cardId;
    resetCardState();
    enterCard();
    return true;
}

bool Engine::changeToStackAndGlobalCard(StackId id, std::uint32_t globalCardId)
{
    if (scriptDepth_ > 0)
    {
        // Held until the interpreter is out of the current stack's memory. See
        // the note on this in Engine.hpp -- the command list being walked lives
        // in the vector loading the next stack would destroy.
        haveStackChange_ = true;
        pendingStack_ = id;
        pendingGlobalCard_ = globalCardId;
        return true;
    }

    if (!changeToStack(id))
        return false;
    const std::int32_t local = stack_.localCardForGlobal(globalCardId);
    if (local < 0)
    {
        std::printf("%s: no card for global id %lu\n", stackName(id),
                    static_cast<unsigned long>(globalCardId));
        return false;
    }
    return changeToCard(static_cast<std::uint16_t>(local));
}

void Engine::applyPendingStackChange()
{
    if (!haveStackChange_)
        return;
    haveStackChange_ = false;
    const StackId id = pendingStack_;
    const std::uint32_t globalCard = pendingGlobalCard_;
    // Anything left queued belonged to the stack that is going away.
    queued_.clear();
    changeToStackAndGlobalCard(id, globalCard);
}

void Engine::resetCardState()
{
    hotspotEnabled_.assign(card_ != nullptr ? card_->hotspots.size() : 0, false);
    if (card_ != nullptr)
        for (std::size_t i = 0; i < card_->hotspots.size(); ++i)
            hotspotEnabled_[i] = (card_->hotspots[i].flags & kHotspotEnabled) != 0;

    insideHotspot_ = -1;
    pressedHotspot_ = -1;
    // Otherwise an opcode-13 shape from the last card is still on screen until
    // the pointer happens to cross a hotspot.
    cursor_.setShape(rivendata::kCursorMain);
    currentHotspot_ = nullptr;
    queued_.clear();
    updateDepth_ = 0;
}

/// RivenCard::leave (riven_card.cpp:1021-1034). The counterpart to enterCard():
/// a card that started a movie or an ambient stops it here, and without this
/// those leak into the next card.
///
/// The guard is not defensive: a leave script can itself change the card, and
/// the change would re-enter this with the same card_ still current.
void Engine::leaveCard()
{
    if (card_ == nullptr || leavingCard_)
        return;
    leavingCard_ = true;
    const Card *const leaving = card_;
    runHandlers(leaving->scripts, ScriptEvent::CardLeave, false);
    leavingCard_ = false;
}

void Engine::refreshCard()
{
    if (card_ != nullptr)
        enterCard();
}

// ---------------------------------------------------------------------------
// Zip mode
// ---------------------------------------------------------------------------

std::int32_t Engine::zipDestFor(const std::string &name) const
{
    if (name.empty())
        return -1;
    for (const ZipDest &z : zipDests_)
        if (z.stack == stack_.id && z.name == name)
            return z.cardId;
    return -1;
}

/// riven_card.cpp:652-670. Two halves that look unrelated and are not: a card
/// records itself on the way in, and every zip hotspot on it is enabled against
/// what has been recorded so far -- so a zip shortcut appears the moment its
/// other end has been seen, which is exactly the affordance.
void Engine::initializeZipMode()
{
    if (card_ == nullptr)
        return;

    if (card_->zipModePlace != 0)
    {
        const std::string name = nameFromList(rivendata::kCardNames, card_->nameIndex);
        // An unnamed zip place cannot be matched by any hotspot, so recording it
        // would only grow the list (riven.cpp:733-734).
        if (!name.empty() && zipDestFor(name) < 0)
            zipDests_.push_back(ZipDest{stack_.id, cardId_, name});
    }

    const bool enabled = vars_.get(rivendata::VarId::AZip) != 0;
    for (std::size_t i = 0; i < card_->hotspots.size(); ++i)
    {
        const Hotspot &h = card_->hotspots[i];
        if ((h.flags & kHotspotZip) == 0)
            continue;
        // With zip mode off, a zip hotspot is off whatever its BLST said --
        // otherwise the shortcut would still be clickable, just invisible.
        const bool reachable =
            enabled && zipDestFor(nameFromList(rivendata::kHotspotNames, h.nameRes)) >= 0;
        enableHotspotByIndex(i, reachable);
    }
}

/// RivenCard::enter (riven_card.cpp:632-650), in order: the card variable, the
/// load script, the default picture if the load script did not draw one, zip
/// mode, the screen update, then the enter script.
void Engine::enterCard()
{
    vars_.at(rivendata::VarId::CurrentStackId) = static_cast<std::uint32_t>(stack_.id);
    vars_.at(rivendata::VarId::CurrentCardId) = cardId_;

    activatedPlst_ = false;
    activatedSlst_ = false;

    // A load script can itself change the card, and then everything below is
    // about the card that is no longer current -- the new one has already had its
    // own enterCard() run from inside.
    const Card *const entered = card_;

    beginScreenUpdate();
    runHandlers(card_->scripts, ScriptEvent::CardLoad, false);
    if (card_ != entered)
        return;

    // The default load script: draw PLST 1 unless the load script already drew
    // something (riven_card.cpp:690-694).
    if (!activatedPlst_ && !card_->plst.empty())
        activatePlst(1);

    // After the load script, before the screen update: the load script may have
    // enabled or disabled hotspots, and zip mode has the last word on the ones
    // it owns (riven_card.cpp:640-643).
    initializeZipMode();

    applyScreenUpdate(true);
    if (card_ != entered)
        return;

    runHandlers(card_->scripts, ScriptEvent::CardEnter, false);
    if (card_ != entered)
        return;

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s card %u", displayName(stack_.id), cardId_);
    status_ = buf;
}

// ---------------------------------------------------------------------------
// The frame loop
// ---------------------------------------------------------------------------

bool Engine::boot()
{
    vars_.startNewGame();
    zipDests_.clear();

    if (!bgs.create())
    {
        error_ = "no VRAM for the card view";
        return false;
    }
    if (!surface_.create())
    {
        error_ = "no memory for the card picture";
        return false;
    }

    // Neither is fatal. A card converted before these stages existed has no
    // cursors/ and no extras/, and the game it ran before is the game it still
    // runs -- just with no pointer and no way into a journal. Both say so on the
    // console rather than stopping the boot.
    cursor_.create();
    inventory_.create();

    if (!changeToStack(kBootStack))
        return false;
    if (!changeToCard(kBootCard))
    {
        error_ = "aspit has no card 1";
        return false;
    }
    return true;
}

void Engine::processInput()
{
    if (card_ == nullptr)
        return;

    scanKeys();
    touchPosition touch;
    touchRead(&touch);

    // SELECT + a direction replays the current card with that transition, and
    // SELECT + A dissolves it. A developer aid: the shipped data only reaches
    // opcode 18 on particular moves, and an effect that cannot be triggered on
    // demand cannot be looked at closely enough to tell right from nearly right.
    // SELECT is otherwise unused, and holding it suppresses the pointer below.
    if ((keysHeld() & KEY_SELECT) != 0)
    {
        const std::uint32_t down = keysDown();
        Transition t = Transition::None;
        if ((down & KEY_LEFT) != 0)
            t = Transition::PanLeft;
        else if ((down & KEY_RIGHT) != 0)
            t = Transition::PanRight;
        else if ((down & KEY_UP) != 0)
            t = Transition::PanUp;
        else if ((down & KEY_DOWN) != 0)
            t = Transition::PanDown;
        else if ((down & KEY_A) != 0)
            t = Transition::Blend;
        if (t != Transition::None)
        {
            // The card has to be redrawn for there to be anything to slide TO,
            // and refreshCard() is what a script's opcode 19 does.
            scheduleTransition(t);
            surface_.markAll();
            refreshCard();
        }
        return;
    }

    // The pointer, not the stylus, is what the game is played with now.
    // Touching moves it; lifting leaves it where it was; the D-pad nudges it and
    // A clicks. That is what gives the port hover -- and hover is the only thing
    // Riven's cursor shapes are for, so without it there is no point reading
    // Hotspot::cursor at all.
    const bool touchHeld = (keysHeld() & KEY_TOUCH) != 0;
    if (touchHeld)
    {
        pointerX_ = touch.px;
        pointerY_ = touch.py;
    }
    else
    {
        const int pad = keysHeld() & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN);
        if (pad == 0)
            padHeld_ = 0;
        else
        {
            // One pixel a frame to start with, four once it is clear the player
            // means to travel: a 256-pixel screen crossed at one pixel a frame
            // takes four seconds, and Riven's hotspots are small enough that the
            // fine speed has to be the default.
            ++padHeld_;
            const int step = padHeld_ > 15 ? 4 : 1;
            if ((pad & KEY_LEFT) != 0)
                pointerX_ -= step;
            if ((pad & KEY_RIGHT) != 0)
                pointerX_ += step;
            if ((pad & KEY_UP) != 0)
                pointerY_ -= step;
            if ((pad & KEY_DOWN) != 0)
                pointerY_ += step;
        }
    }
    pointerX_ = pointerX_ < 0 ? 0 : (pointerX_ >= kScreenW ? kScreenW - 1 : pointerX_);
    pointerY_ = pointerY_ < 0 ? 0 : (pointerY_ >= kScreenH ? kScreenH - 1 : pointerY_);
    cursor_.moveTo(pointerX_, pointerY_);

    // Touch and A are the same click.
    const bool held = touchHeld || (keysHeld() & KEY_A) != 0;
    const bool pressed = (keysDown() & (KEY_TOUCH | KEY_A)) != 0;
    const bool released = (keysUp() & (KEY_TOUCH | KEY_A)) != 0;

    // The inventory band sits below the card view, so it is tested first and in
    // screen coordinates -- toCardY() of a row below the view is a card row that
    // does not exist. A click there leaves the card entirely, so nothing after
    // it may run.
    if (released)
    {
        const std::uint16_t item = inventory_.hitTest(pointerX_, pointerY_);
        if (item != 0)
        {
            pressedHotspot_ = -1;
            insideHotspot_ = -1;
            inventory_.click(*this, item);
            return;
        }
    }

    // Hit-tested every frame, whether or not anything is pressed: that is the
    // hover the pointer exists to provide.
    std::int32_t hit = -1;
    if (pointerY_ >= kViewOffsetY && pointerY_ < kViewOffsetY + kViewH)
    {
        // The pointer is in DS pixels; hotspot rects are in Riven's original
        // 608x392 space and are deliberately never pre-scaled (RivenData.hpp), so
        // the point is converted rather than the rect.
        const int cardX = toCardX(pointerX_);
        const int cardY = toCardY(pointerY_ - kViewOffsetY);
        // The LAST match wins, not the first: this loop deliberately does not
        // break (riven_card.cpp:827-835). Riven layers small controls over a
        // card-wide hotspot and resolves the overlap by file order, so stopping
        // at the first containing rect makes every button that sits on top of a
        // bigger one unreachable -- which is most of the menu and both journals.
        for (std::size_t i = 0; i < card_->hotspots.size(); ++i)
        {
            const Hotspot &h = card_->hotspots[i];
            if (!hotspotEnabled_[i] || !rectUsable(h.rect))
                continue;
            if (rectContains(h.rect, cardX, cardY))
                hit = static_cast<std::int32_t>(i);
        }
    }

    // A hotspot script is the usual way a card changes, and once it has, every
    // index into the old card's hotspots is meaningless -- and the new card has
    // already been given its own insideHotspot_ by resetCardState(). So each
    // dispatch reports whether the card survived it, and the caller stops if it
    // did not.
    const rivendata::Card *const entered = card_;
    const auto run = [&](std::int32_t index, ScriptEvent event) -> bool {
        if (index < 0 || card_ != entered)
            return card_ == entered;
        const Hotspot &h = card_->hotspots[static_cast<std::size_t>(index)];
        currentHotspot_ = &h;
        runHandlers(h.scripts, event, false);
        currentHotspot_ = nullptr;
        return card_ == entered;
    };

    // The release frame is handled first and on its own. libnds clears KEY_TOUCH
    // from keysHeld() on the frame the stylus lifts, so there is no position to
    // hit-test and `hit` is always -1 here; letting the enter/leave
    // reconciliation below run first would set insideHotspot_ to -1 and the
    // MouseUp could never fire. Which hotspot it belongs to is the one the press
    // landed on, so that is latched rather than recomputed
    // (riven_card.cpp:941-951 runs MouseUp where press and release coincide).
    if (released)
    {
        const std::int32_t was = pressedHotspot_ >= 0 ? pressedHotspot_ : insideHotspot_;
        pressedHotspot_ = -1;
        // insideHotspot_ is NOT cleared: the pointer is still wherever it was,
        // so the hotspot is still under it. The next frame's hit != inside test
        // fires the real leave when the pointer actually moves off, which is what
        // makes a drag puzzle -- a slider, a wahrk lever -- behave.
        if (!run(was, ScriptEvent::MouseUp))
            return;
    }

    // riven_card.cpp:1009-1018. Done here rather than in the dispatch below
    // because a hotspot script may change the card, and the shape belongs to the
    // hotspot that was under the pointer before it did.
    if (hit != insideHotspot_)
        cursor_.setShape(hit >= 0 ? card_->hotspots[static_cast<std::size_t>(hit)].cursor
                                  : kCursorMain);

    if (hit != insideHotspot_)
    {
        if (!run(insideHotspot_, ScriptEvent::MouseLeave))
            return;
        insideHotspot_ = hit;
        if (!run(insideHotspot_, ScriptEvent::MouseEnter))
            return;
    }
    else if (hit >= 0 && held && !pressed)
    {
        if (!run(hit, ScriptEvent::MouseInside))
            return;
        if (!run(hit, ScriptEvent::MouseDrag))
            return;
    }

    if (pressed)
    {
        pressedHotspot_ = hit;
        if (!run(hit, ScriptEvent::MouseDown))
            return;
    }
}

void Engine::pumpMovies()
{
    for (MovieSlot &m : movies_)
    {
        if (!m.open || !m.enabled || !m.player.isPlaying())
            continue;
        m.player.pump();
        if (m.player.profile() == VideoProfile::Lite && surface_.exists())
            surface_.markRowMask(m.player.compositeInto(surface_.texels()));

        // A fullscreen movie that has run out has to give the buffers back, and
        // only the blocking path used to do it. Opcode 33 does not block, so a
        // card that starts a fullscreen movie and carries on would otherwise be
        // left showing the movie's last frame for good.
        if (m.player.profile() == VideoProfile::Full && m.player.finished())
            endFullscreenMovie(m);
    }
}

/// Put the card back on screen after a fullscreen movie. The card's own image was
/// parked in a buffer the movie never touched, so this is a rebind and a flip --
/// not an upload, and certainly not a re-entry, which would run the card's
/// scripts again and start the movie over.
void Engine::endFullscreenMovie(MovieSlot &m)
{
    m.player.stop();
    // The buffer handed back holds the movie's last frame rather than the card,
    // so the next publish has to send all of it.
    surface_.invalidate(bgs.endMovieTakeover());
}

/// Everything that has to happen at the top of a frame: publish whatever the
/// scripts changed, then let BgSurface commit the flip, the transition step and
/// the sprites.
///
/// The old version of this had to fit an 84 KB texture upload into the ~22
/// scanlines where a texture bank could safely leave the texture bus. There is no
/// such window any more -- a background bank is never unmapped -- so what is left
/// here is ordering, not timing.
void Engine::flushUploads()
{
    for (MovieSlot &m : movies_)
        if (m.open && m.player.isPlaying() && m.player.takeFlip())
            bgs.requestFlip();

    // Not while a screen update is open. Opcodes 20 and 21 bracket a batch of
    // drawing that is only meant to appear whole, and a script inside that
    // bracket can reach here through opcode 14's delay() or a blocking movie --
    // both of which spin idleFrame(), which publishes unconditionally.
    //
    // Nor during a transition: both buffers are then holding finished pictures
    // that are being slid past each other, and writing either of them mid-slide
    // would tear the effect.
    if (updateDepth_ == 0 && !bgs.transitionActive() && !bgs.movieTakeover())
    {
        const int back = bgs.backBuffer();
        if (surface_.anyDirty(back))
        {
            surface_.publish(bgs, back);
            bgs.requestFlip();
        }
    }

    // The one place video registers are written, and the reason it belongs here:
    // the caller has just come back from NEA_WaitForVBL.
    bgs.vblank();

    // OAM is written by DMA, so the sprites belong in the same window. Their
    // positions were decided by processInput() late in the previous frame, so the
    // pointer is one frame behind the stylus -- 16 ms, and the cost of the loop
    // order that keeps everything else where it must be.
    cursor_.flush();
    inventory_.flush();
}

void Engine::frame()
{
    // Order is the hardware's, not the logic's: the caller has just returned from
    // NEA_WaitForVBL, so the flip and the sprite writes go first, and only then
    // the decoding and the scripts -- which are what fill the buffers the NEXT
    // frame will publish.
    flushUploads();

    RivenAudio::pump();
    pumpMovies();

    processInput();

    if (!queued_.empty() && !runningQueued_)
    {
        runningQueued_ = true;
        std::vector<Command> run = std::move(queued_);
        queued_.clear();
        runCommands(run, false);
        runningQueued_ = false;
    }

    // CardFrame runs once per frame while the card is up (ScriptEvent 8).
    if (card_ != nullptr)
        runHandlers(card_->scripts, ScriptEvent::CardFrame, false);

    // After the scripts, so a card that has just granted a book shows it on the
    // same frame rather than the next.
    inventory_.update(*this);
}

} // namespace rivenrt
