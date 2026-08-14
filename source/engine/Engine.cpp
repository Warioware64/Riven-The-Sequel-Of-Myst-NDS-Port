#include "Engine.hpp"

#include <cstdio>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "MainMenu.hpp"
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
// Water
// ---------------------------------------------------------------------------

void Engine::activateFlst(std::uint16_t index)
{
    if (card_ == nullptr)
        return;
    for (const FlstRec &f : card_->flst)
    {
        if (f.index != index)
            continue;
        water_.load(global.sfxeDir() + stackName(stack_.id) + "/"
                    + std::to_string(f.sfxeId) + ".rsfx");
        return;
    }
    // Not a failure, and not warn(): the original does nothing here too
    // (riven_card.cpp:914-922 just falls out of the loop), and most cards that
    // carry an FLST at all carry an empty one -- 1751 of the 2307 in a retail
    // copy -- so an ungated line would sit on the console for the whole game.
    // The effect that WAS running keeps running, which is also the original's
    // behaviour: only a match reschedules.
    DebugLog::log("card %u: no water effect %u", cardId_, index);
}

void Engine::setFliesEffect(int count, bool fireflies)
{
    flies_.start(count, fireflies);
}

void Engine::pumpEffects()
{
    // Nothing to draw over when the card is not the thing on screen. The zoom
    // viewer and a fullscreen movie both own the buffers outright, and writing
    // into the card surface underneath them would only dirty rows that get
    // published the moment they hand it back.
    //
    // Ahead of both effects, unlike the water test below it: this is about
    // whether the card surface may be touched at all.
    if (mode_ != Mode::Card || bgs.movieTakeover())
        return;

    // Through the overlay channel, not markRowMask: an effect is on top of the
    // card exactly the way a LITE movie's frame is, and the next screen update
    // has to be able to take it off again. That is what the original does --
    // updateScreen re-copies the effect surface from the untouched one
    // (riven_graphics.cpp:391) and the effect's next tick draws it back.
    //
    // The player's Water setting reaches here as Riven's own variable, which is
    // what the original tests too (riven_graphics.cpp:773). So turning water off
    // in the settings screen stops the ripple where it stands, and turning it
    // back on starts it again -- no reload, no card re-entry.
    //
    // AND IT GATES THE WATER ONLY. The original's updateEffects tests
    // waterenabled on the water branch and nothing on the flies branch
    // (riven_graphics.cpp:772-780); hoisting it to the top of this function
    // would make a setting about ripples silently switch off every firefly in
    // the jungle.
    if (water_.active() && vars_.get(VarId::WaterEnabled) != 0)
        surface_.noteOverlayRows(water_.update(surface_));

    if (flies_.active())
        surface_.noteOverlayRows(flies_.update(surface_));
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

    const std::string path = picPath(tbmpId);
    std::string err;
    if (!surface_.drawPicture(path, rect, err))
    {
        DebugLog::warn("card %u: picture %u: %s", cardId_, tbmpId, err.c_str());
        return;
    }
    // The destination rect as well as the file: a picture that loaded and went
    // somewhere unexpected looks exactly like one that did not load.
    DebugLog::log("  pic %u -> %d,%d %dx%d", tbmpId, rect.left, rect.top,
                  rect.right - rect.left, rect.bottom - rect.top);
}

void Engine::activatePlst(std::uint16_t index)
{
    activatedPlst_ = true;
    if (card_ == nullptr)
        return;
    for (const PictureRec &p : card_->plst)
        if (p.index == index)
        {
            // A PLST record that covers the whole card is the card's picture,
            // and that is the one the zoom viewer opens. The rest are overlays
            // -- a lever in one position, a button lit -- and zooming into a
            // 39x76 twin of one would show nothing the card view does not.
            if (p.rect.left <= 0 && p.rect.top <= 0 && p.rect.right >= kCardW
                && p.rect.bottom >= kCardH)
            {
                cardPicture_ = p.id;
            }
            drawBitmap(p.id, p.rect);
            return;
        }
    DebugLog::warn("card %u: no PLST record %u", cardId_, index);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

void Engine::stopAllAmbient()
{
    for (int i = 0; i < ambientCount_; ++i)
        if (ambientSlots_[i] >= 0)
            RivenAudio::stopSound(ambientSlots_[i]);
    ambientCount_ = 0;
    // stopAllSLST clears it too (riven_sound.cpp:116), and it has to: an SLST
    // re-activated after opcode 12, opcode 37 or a stack change would otherwise
    // recognise its own bed, decide it was still sounding and start nothing --
    // leaving the card silent for as long as the player stayed on it.
    mainAmbientId_ = -1;
}

/// One layer of an SLST, started. The mix is the record's:
/// globalVolume scales every layer, 0..256 (RivenData.hpp:397).
void Engine::startAmbientLayer(const SoundRec &rec, std::size_t i)
{
    if (ambientCount_ >= RivenAudio::kSoundSlots)
    {
        DebugLog::warn("card %u: SLST has more layers than channels", cardId_);
        return;
    }
    const int global256 = rec.globalVolume == 0 ? 256 : rec.globalVolume;
    const int vol = i < rec.volumes.size() ? rec.volumes[i] : 255;
    const int bal = i < rec.balances.size() ? rec.balances[i] : 0;
    const int slot = RivenAudio::playSound(soundPath(rec.soundIds[i]),
                                           vol * global256 / 256, bal, rec.loop != 0);
    DebugLog::log("  slst %u vol %d bal %d %s -> slot %d", rec.soundIds[i],
                  vol * global256 / 256, bal, rec.loop != 0 ? "loop" : "once", slot);
    // Recorded EVEN WHEN IT FAILED, as -1. The layer exists in the record either
    // way, and this list is indexed by layer -- dropping the failures would slide
    // the later layers down and the next activation of this bed would start one
    // of them all over again.
    ambientSlots_[ambientCount_++] = slot;
    if (slot < 0)
        // A sound is a file on the card like everything else, and a missing one
        // used to be visible only inside the trace -- so a card that had gone
        // quiet said nothing at all with the trace off.
        DebugLog::warn("card %u: ambient %u will not play", cardId_, rec.soundIds[i]);
}

/// RivenSoundManager::playSLST (riven_sound.cpp:79-113).
///
/// The whole of this is one test: is the new record's FIRST sound the one
/// already sounding? If it is, this is the same ambient bed continuing and
/// nothing may be stopped or restarted -- only layers the new record adds are
/// started, and the mix is adjusted underneath the sounds that carry on.
///
/// It is not a micro-optimisation. A layer here is a whole resident buffer read
/// off the SD card (RivenAudio::playSound), so a needless restart is both a gap
/// in the music and a stall during card entry -- and the shipped game has only
/// 47 distinct beds across its 2137 SLST-1 records, so the great majority of
/// card changes, and every opcode-19 refresh, are this case.
void Engine::playSlst(const SoundRec &rec)
{
    if (rec.soundIds.empty())
        return; // riven_sound.cpp:80-82

    const bool sameBed = static_cast<std::int32_t>(rec.soundIds[0]) == mainAmbientId_;
    if (!sameBed)
    {
        // A different bed. stopAllAmbient() zeroes ambientCount_, which is what
        // lets the loop below start from it and serve both branches.
        stopAllAmbient();
        mainAmbientId_ = rec.soundIds[0];
    }
    else
    {
        DebugLog::log("  slst %u already sounding: kept", rec.soundIds[0]);
    }

    // addAmbientSounds: GROWS the list and never shrinks it. A record with fewer
    // layers than the one before leaves the extras playing, which reads like a
    // bug and is what the original does.
    for (std::size_t i = static_cast<std::size_t>(ambientCount_); i < rec.soundIds.size();
         ++i)
        startAmbientLayer(rec, i);

    // setTargetVolumes: the layers that carried over keep their sound and take
    // the new record's mix. Nothing to do on the other branch -- they were just
    // started at exactly this volume.
    if (!sameBed)
        return;
    const int global256 = rec.globalVolume == 0 ? 256 : rec.globalVolume;
    for (int i = 0; i < ambientCount_ && static_cast<std::size_t>(i) < rec.volumes.size();
         ++i)
    {
        if (ambientSlots_[i] < 0)
            continue; // a layer whose file would not load; it still holds its place
        const std::size_t at = static_cast<std::size_t>(i);
        const int bal = at < rec.balances.size() ? rec.balances[at] : 0;
        RivenAudio::setSoundVolume(ambientSlots_[i], rec.volumes[at] * global256 / 256,
                                   bal);
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
    // The same gap activatePlst reports for a picture, and it was silent here.
    // ScummVM treats it as fatal (RivenCard::getSound errors out,
    // riven_card.cpp:792-800); a line is enough, since the only consequence is a
    // card that stays quiet.
    DebugLog::warn("card %u: no SLST record %u", cardId_, index);
}

void Engine::playEffect(std::uint16_t twavId, int volume)
{
    // One effect at a time, matching ScummVM's single effect handle: the original
    // depends on a door sound being cut short by the click of the next move.
    stopEffects();
    effectSlot_ = RivenAudio::playSound(soundPath(twavId), volume == 0 ? 255 : volume, 0,
                                        false);
    DebugLog::log("  sfx %u vol %d -> slot %d", twavId, volume == 0 ? 255 : volume,
                  effectSlot_);
    if (effectSlot_ < 0)
        DebugLog::warn("card %u: sound %u will not play", cardId_, twavId);
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

    DebugLog::warn("card %u: more than %d movies at once", cardId_, kMovieSlots);
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
    DebugLog::warn("card %u: no MLST record %u", cardId_, index);
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
        DebugLog::warn("movie %s/%u: %s", stackName(stack_.id), ms.movieId,
                       ms.player.error());
        return false;
    }
    ms.player.setVolume(ms.volume);
    ms.open = true;
    DebugLog::log("  mov %u %dx%d %lu frames at %d,%d %s", ms.movieId, ms.player.width(),
                  ms.player.height(),
                  static_cast<unsigned long>(ms.player.frameCount()), ms.left, ms.top,
                  ms.loop ? "loop" : "once");
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
    // Opcode 28 is ScummVM's disable(), and disable() bakes: the frame it was
    // showing becomes part of the card. gspit's pins are exactly this -- play,
    // disable, return, and the baked frame is where the pins now are.
    if (!enabled && movies_[slot].open)
    {
        bakeOverlay(movies_[slot]);
        movies_[slot].player.stop();
    }
}

void Engine::disableAllMovies()
{
    for (std::int32_t i = 0; i < kMovieSlots; ++i)
    {
        movies_[i].enabled = false;
        if (movies_[i].open)
        {
            bakeOverlay(movies_[i]); // opcode 29, same disable() as above
            movies_[i].player.stop();
        }
    }
}

void Engine::playMovie(std::uint16_t code, bool blocking)
{
    const std::int32_t slot = slotForCode(code);
    if (slot < 0)
    {
        // Opcode 32/33 for a code no MLST record on this card ever activated.
        // Silent before; worth a line now that there is somewhere to put it.
        DebugLog::warn("card %u: no movie activated as %u", cardId_, code);
        return;
    }
    playMovieSlot(slot, blocking);
}

void Engine::playMovieRange(std::uint16_t code, std::uint32_t startMs, std::uint32_t endMs)
{
    const std::int32_t slot = slotForCode(code);
    if (slot < 0)
    {
        DebugLog::warn("card %u: no movie activated as %u", cardId_, code);
        return;
    }
    if (!ensureSlotOpen(slot))
        return;

    // The movie's own rate, not an assumed one: 15/1 on every Riven movie
    // measured so far, but the converter stores what it found (RivenVideo.hpp).
    const RvidPlayer &p = movies_[slot].player;
    const std::uint32_t num = p.fpsNum();
    const std::uint32_t den = p.fpsDen();
    if (num == 0 || den == 0)
    {
        playMovieSlot(slot, true);
        return;
    }
    const auto frameAt = [num, den](std::uint32_t ms) {
        return static_cast<std::uint32_t>(static_cast<std::uint64_t>(ms) * num
                                          / (1000ull * den));
    };
    playMovieSlot(slot, true, frameAt(startMs), frameAt(endMs));
}

void Engine::playMovieSlot(std::int32_t slot, bool blocking, std::uint32_t startFrame,
                           std::uint32_t stopFrame)
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

    if (!ms.player.play(loop, startFrame, stopFrame))
    {
        DebugLog::warn("card %u: movie %u will not play: %s", cardId_, ms.movieId,
                    ms.player.error());
        if (full)
            surface_.invalidate(bgs.endMovieTakeover());
        return;
    }

    // Whether the play ran to the movie's own end, which is the only thing the
    // bake below turns on -- and only playMovieSlot knows it.
    const bool whole = startFrame == 0 && stopFrame == 0xFFFFFFFFu;
    if (blocking)
        playMovieBlocking(slot, whole);
}

/// Draw every overlay that is still playing back onto the card picture.
///
/// The counterpart to CardSurface::refreshFromClean, which does not know what is
/// playing and restores card-wide bands. An overlay that has no new frame is
/// asked for the one it is holding (RvidPlayer::refreshPicture), because at 15
/// fps "no new frame" lasts four published frames and the hole would be seen.
void Engine::recompositeOverlays()
{
    if (!surface_.exists())
        return;
    for (MovieSlot &m : movies_)
    {
        if (!m.open || !m.enabled || !m.player.isPlaying()
            || m.player.profile() != VideoProfile::Lite)
            continue;
        m.player.refreshPicture();
        surface_.noteOverlayRows(m.player.compositeInto(surface_.texels()));
    }
}

/// Make a LITE overlay's last frame part of the card, the way ScummVM's
/// RivenVideo::disable does (riven_video.cpp:288-301).
///
/// Called where the original calls disable(), and nowhere else: what does NOT
/// bake is what a screen update is free to wipe.
void Engine::bakeOverlay(MovieSlot &m)
{
    if (!surface_.exists() || !m.open || m.player.profile() != VideoProfile::Lite)
        return;
    surface_.bakeRect(m.player.viewX(), m.player.viewY(), m.player.width(),
                      m.player.height());
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
    // The clock advances here as well as in frame(), because this is the loop a
    // blocking movie spins: a timer counted only in frame() would stand still
    // for the length of a cutscene and then fire the moment one ended. The two
    // loops are mutually exclusive, so nothing is counted twice.
    ++frames_;
    flushUploads();

    RivenAudio::pump();
    pumpMovies();
    pumpEffects();
    // Here as well as in frame(), so a "note taken" raised by the notebook or
    // during a blocking movie still expires -- both spin this loop and never
    // reach the other one.
    DebugLog::pumpStatus();
}

namespace
{
    /// Whether a blocking play that ran to the movie's own end bakes its last
    /// frame into the card, the way ScummVM's playBlocking() with no end time
    /// does -- it finishes with disable() (riven_video.cpp:264-268), and
    /// disable() bakes.
    ///
    /// A CONVENTION, not a requirement, which is why it is a switch. The bake
    /// that is load-bearing is the explicit one on opcodes 28 and 29: gspit's
    /// pins play a RANGED segment and then disable it by hand
    /// (gspit.cpp:58-82), and a ranged play never reaches here with `whole`.
    /// Nothing is known to depend on THIS one, and it is what keeps a blocking
    /// movie's last frame on a card that never redraws -- the telescope button
    /// on tspit 137 is exactly that, and needs an explicit refreshCard()
    /// because of it (Externals.cpp). Set false and that class of leftover
    /// disappears at the next screen update instead; the risk is a card that
    /// was relying on the frame the way the pins do.
    constexpr bool kBakeBlockingMovies = true;
} // namespace

Engine::CursorHide::CursorHide(Engine &e) : eng(e)
{
    ++eng.cursorSuppress_;
}

Engine::CursorHide::~CursorHide()
{
    if (--eng.cursorSuppress_ < 0)
        eng.cursorSuppress_ = 0;
}

void Engine::playMovieBlocking(std::int32_t slot, bool whole)
{
    if (slot < 0 || slot >= kMovieSlots)
        return;
    MovieSlot &ms = movies_[slot];
    const bool full = ms.player.profile() == VideoProfile::Full;

    // No pointer over a video. riven_video.cpp:216 and :268 bracket the whole of
    // playBlocking this way, and it is every blocking movie -- a fullscreen
    // cutscene and a small overlay alike, which is why this is here and not on
    // the fullscreen test below. Scoped rather than a hide/show pair because the
    // `full` branch returns early.
    CursorHide hideCursor{*this};

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
    {
        endFullscreenMovie(ms); // stops it too
        return;
    }
    if (whole && kBakeBlockingMovies)
        bakeOverlay(ms);
    ms.player.stop();
}

bool Engine::movieEnded(std::uint16_t code) const
{
    const std::int32_t slot = slotForCode(code);
    if (slot < 0 || !movies_[slot].open)
        return true;
    const RvidPlayer &p = movies_[slot].player;
    return !p.isPlaying() || p.finished();
}

std::uint32_t Engine::movieDurationMs(std::uint16_t code) const
{
    const std::int32_t slot = slotForCode(code);
    if (slot < 0 || !movies_[slot].open)
        return 0;
    const RvidPlayer &p = movies_[slot].player;
    const std::uint32_t num = p.fpsNum();
    const std::uint32_t den = p.fpsDen();
    if (num == 0)
        return 0;
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(p.frameCount()) * 1000ull
                                      * den / num);
}

bool Engine::playMovieUntilClick(std::uint16_t code)
{
    const std::int32_t slot = slotForCode(code);
    if (slot < 0)
    {
        // NOT a warning, unlike playMovie's. ScummVM's openSlot on a code no
        // MLST record claimed hands back an empty handle whose endOfVideo() is
        // already true (riven_video.cpp:309-322), so the original falls straight
        // out of this loop -- and the sunners timers poll often enough that a
        // line here would be a line every few seconds.
        return false;
    }

    // seek(0) + enable() + play(), which is what openSlot's caller does before
    // this loop: playMovieSlot always restarts from the first frame.
    playMovieSlot(slot, false);
    MovieSlot &ms = movies_[slot];

    // Same as playMovieBlocking: no pointer over a video (riven_video.cpp:216).
    CursorHide hideCursor{*this};

    // RivenStack::mouseForceUp (riven_stack.cpp:318-321), and it is load-bearing
    // here: the click that walked the player onto this card is very likely still
    // held when its CardEnter script reaches this, and without the latch the
    // alert would be cut short by the press that caused it.
    bool armed = false;
    bool clicked = false;
    while (ms.player.isPlaying() && !ms.player.finished() && !quit_)
    {
        idleFrame();

        scanKeys();
        if (!armed)
            armed = (keysHeld() & (KEY_TOUCH | KEY_A)) == 0;
        else if ((keysDown() & (KEY_TOUCH | KEY_A)) != 0)
        {
            clicked = true;
            break;
        }
        // The cutscene skip, as everywhere else. It counts as the click: the
        // player asked to get on with it, and the sunners react either way.
        if ((keysDown() & (KEY_B | KEY_START)) != 0)
        {
            clicked = true;
            break;
        }
    }

    // Every movie this is used on is a small overlay -- the sunners are a corner
    // of the lagoon, not a cutscene -- but a fullscreen one took two buffers on
    // the way in and has to give them back, and stop() alone does not.
    if (ms.player.profile() == VideoProfile::Full)
    {
        endFullscreenMovie(ms);
        return clicked;
    }

    // stop(), NEVER bakeOverlay(). ScummVM's sunnersPlayVideo ends in stop()
    // (jspit.cpp:558) and only playBlocking ends in disable() -- so the last
    // frame here stays an overlay and the next screen update takes it off, which
    // is what puts the sunners back where the still has them.
    ms.player.stop();
    return clicked;
}

// ---------------------------------------------------------------------------
// The card timer
// ---------------------------------------------------------------------------

void Engine::installTimer(TimerProc proc, std::uint32_t ms)
{
    timerProc_ = proc;
    timerDeadline_ = frames_ + msToFrames(ms);
}

void Engine::removeTimer()
{
    timerProc_ = nullptr;
    timerDeadline_ = 0;
}

void Engine::checkTimer()
{
    if (timerProc_ == nullptr || inTimer_ || frames_ < timerDeadline_)
        return;

    // The proc is NOT cleared first: re-arming or removing itself is its job,
    // and every one of them ends in one or the other (riven_stack.cpp:383).
    const TimerProc proc = timerProc_;
    inTimer_ = true;
    proc(*this);
    inTimer_ = false;
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
        applyDeferredNavigation();
}

void Engine::runHandlers(const std::vector<Handler> &handlers, ScriptEvent event, bool queue)
{
    // `handlers` LIVES INSIDE THE LOADED STACK, and running one of them can
    // replace that stack: the last runCommands to return drops scriptDepth_ to
    // zero, which is where applyDeferredNavigation fires a held stack change or
    // a held load. Both free the vector this loop is walking.
    //
    // So the loop stops the moment the card is not the one it started on, before
    // the iterator is advanced again. Same guard, and the same reason, as the
    // `card_ != entered` checks through enterCard. Reading card_ is safe when
    // handlers is not; it is a member of this class.
    const Card *const entered = card_;
    for (const Handler &h : handlers)
    {
        if (static_cast<ScriptEvent>(h.event) == event)
            runCommands(h.commands, queue);
        if (card_ != entered)
            return;
    }
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

    // Take the movie overlays off the card, then put back the ones still
    // playing. RivenGraphics::updateScreen (riven_graphics.cpp:383-401) repaints
    // the whole card from _mainScreen at exactly this moment, which is what
    // makes a finished overlay disappear in the original instead of staying
    // where it stopped -- an overlay only outlives its movie if something baked
    // it (CardSurface::bakeRect).
    //
    // Both halves, and in this order: refreshFromClean restores whole card-wide
    // bands, so an overlay that shares a band with the one that stopped goes
    // with it and has to be drawn again. Straight away, not next frame:
    // flushUploads() runs BEFORE pumpMovies() in frame() and idleFrame() alike,
    // so leaving it would publish the hole first.
    surface_.refreshFromClean();
    recompositeOverlays();

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

    // A timer is a card's, and a card belongs to a stack: the sunners procs
    // resolve RMAP global ids, and against the wrong stack's table that is a
    // different card entirely. changeToCard does this too, but the boot path and
    // a load reach here without going through one.
    removeTimer();

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
        DebugLog::warn("%s", error_.c_str());
        return false;
    }
    stack_ = std::move(loaded);
    booted_ = true;
    DebugLog::log("STACK %s: %zu cards, %zu card names, %zu vars", stackName(id),
                  stack_.cards.size(), stack_.names[kCardNames].names.size(),
                  stack_.variableIds.size());
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
        DebugLog::warn("%s: no card %u", stackName(stack_.id), cardId);
        return false;
    }

    // Before the leave script, where riven.cpp:630 puts it. A timer belongs to
    // the card that installed it and its proc addresses that card's movie codes;
    // the moment the card is going away it must not be able to fire again.
    removeTimer();

    leaveCard();
    card_ = next;
    cardId_ = cardId;
    resetCardState();

    // enterCard can itself change the card -- a CardEnter external that links
    // away, which is exactly what the sunners alerts do -- and that inner
    // changeToCard has already installed the timer the destination wants. So
    // this only arms one if the card really is still the one we entered.
    const Card *const entered = card_;
    enterCard();
    if (card_ == entered)
        installCardTimer(*this); // riven.cpp:643
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
        DebugLog::warn("%s: no card for global id %lu", stackName(id),
                    static_cast<unsigned long>(globalCardId));
        return false;
    }
    return changeToCard(static_cast<std::uint16_t>(local));
}

void Engine::applyDeferredNavigation()
{
    // A load first: it replaces the whole game, so a stack change queued by the
    // script that asked for it is about to be meaningless.
    if (haveRestore_)
    {
        haveRestore_ = false;
        haveStackChange_ = false;
        queued_.clear();
        const SaveGame::SaveState state = pendingRestore_;
        pendingRestore_ = SaveGame::SaveState{};
        restoreFrom(state);
        return;
    }

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
    // The new card has not drawn its picture yet, and offering the old card's
    // zoom twin would be showing the player somewhere they have left.
    cardPicture_ = 0;
    // Otherwise an opcode-13 shape from the last card is still on screen until
    // the pointer happens to cross a hotspot.
    cursor_.setShape(rivendata::kCursorMain);
    // An effect belongs to the card that asked for it, and the original throws
    // it away with the card (riven_card.cpp:57-58, which clears both). Without
    // this a lagoon's ripple would keep running over the next room's walls, and
    // the jungle's fireflies would follow the player indoors.
    water_.clear();
    flies_.clear();
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
// The zoom viewer
// ---------------------------------------------------------------------------

void Engine::toggleZoom()
{
    if (mode_ == Mode::Zoom)
    {
        zoomView.close();
        mode_ = Mode::Card;
        // The viewer draws on all 192 rows, which reaches past the card view
        // into the rows BgSurface fills transparent once and never again. Give
        // every buffer it could have written back the way create() left them,
        // or a later vertical pan slides zoom pixels through the view
        // (BgSurface::resetBuffer). Before endMovieTakeover, which is what still
        // knows which buffer holds the parked card.
        for (int b = 0; b < BgSurface::kBuffers; ++b)
            if (b != bgs.parkedBuffer())
                bgs.resetBuffer(b);
        bgs.setLetterbox(true);

        // The card was parked untouched while the viewer had the screen, so
        // this is a rebind. Every buffer that is not the parked one holds zoom
        // pixels, not just the one endMovieTakeover names, so none of them may
        // be trusted (CardSurface::invalidateAll).
        (void)bgs.endMovieTakeover();
        surface_.invalidateAll();
        applyScreenUpdate(true);
        cursor_.setVisible(true);
        inventory_.setSuppressed(false);
        setStatus("");
        return;
    }

    // Refused rather than queued. All three of these mean something else owns
    // the buffers, and the viewer's whole screen-handling is "take the two the
    // card is not using" -- which is exactly what they are already doing.
    if (fullscreenMoviePlaying() || bgs.transitionActive() || bgs.movieTakeover())
    {
        DebugLog::warn("zoom: not while the screen is busy");
        return;
    }
    if (cardPicture_ == 0)
    {
        DebugLog::warn("zoom: this card has no picture of its own");
        return;
    }

    const std::string path = global.picsHiDir() + stackName(stack_.id) + "/"
                             + std::to_string(cardPicture_) + ".rpiz";
    if (!zoomView.open(path))
    {
        // open() has already said which of the two it was: no pics_hi/ at all
        // (converted with --no-hires) or no twin for this one picture.
        return;
    }

    mode_ = Mode::Zoom;
    cursor_.setVisible(false);
    inventory_.setSuppressed(true);
    // 31 columns is what the status row has (DebugLog::consoleRow clips to it),
    // and this used to be forty characters of advice that nothing rendered.
    setStatus("zoom: pan, L notes, B leaves");
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

    // The card's shape before any of its scripts run, which is what makes a
    // trace readable: everything logged after this line belongs to this card,
    // and the counts say what it had to work with. nameFromList builds a
    // std::string, so it is behind the gate rather than in the argument list.
    if (DebugLog::enabled())
    {
        const std::string name = nameFromList(kCardNames, card_->nameIndex);
        DebugLog::log("CARD %s/%u \"%s\" plst=%zu hspt=%zu slst=%zu mlst=%zu flst=%zu%s",
                      stackName(stack_.id), cardId_, name.c_str(), card_->plst.size(),
                      card_->hotspots.size(), card_->slst.size(), card_->mlst.size(),
                      card_->flst.size(), card_->zipModePlace != 0 ? " zip" : "");
    }

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

    // And its other half, which was missing: SLST 1 unless a script already
    // activated one (riven_card.cpp:694). Without it activatedSlst_ was written
    // and never read, and a card with no opcode 40 of its own simply inherited
    // whatever bed was already playing -- which sounded right often enough to
    // hide that it was luck.
    //
    // Only safe because playSlst now recognises a bed that is already sounding:
    // this fires on EVERY card entry, and enterCard is also what opcode 19
    // re-runs, so without that test this line would restart the music on every
    // card change and every lever pull.
    //
    // The emptiness guard is RivenCard::playSound's `index <= _soundList.size()`
    // (riven_card.cpp:698-703) and it is what keeps the 170 cards with an empty
    // SLST quiet. Every one of the other 2137 carries a record indexed 1.
    if (!activatedSlst_ && !card_->slst.empty())
        activateSlst(1);

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

    // No status line here. This used to set one -- "Temple Island card 155" --
    // into a field nothing rendered, which was harmless while it went nowhere.
    // Now that setStatus actually draws, a line per card entry would flash on
    // every single move the player makes, which is the noise this whole change
    // exists to remove. The status line is for answering a button press.
    logCardSummary();
}

/// One line per card, on the console whether or not the trace is on.
///
/// WHAT THE CARD ENDED UP WITH, which is why it is here at the bottom of
/// enterCard rather than next to the trace's own CARD header at the top. The
/// header is the card's shape before any of its scripts run -- how many records
/// it HAS -- and it is the right thing to open a trace with, because everything
/// logged after it belongs to this card. It is the wrong thing to answer "the
/// game drew nothing" with: what matters there is which picture is actually up,
/// how many movies actually opened, how many hotspots are actually clickable and
/// whether the ambience actually started. Those are known only once the load and
/// enter scripts have run, and every early return above this point is a card
/// that handed over to another one, whose own summary follows.
///
/// Ungated because it is one line and the player caused it -- see DebugLog::note.
void Engine::logCardSummary() const
{
    if (card_ == nullptr)
        return;

    // Both counts read "what there is / what the card has to work with", which is
    // the comparison that answers the question. Movie slots outlive a card on
    // purpose -- activateMlst keeps a slot whose movie has not changed -- so the
    // open count is players held right now and not this card's own, which is
    // exactly why the MLST count is beside it.
    std::size_t movies = 0;
    for (const MovieSlot &m : movies_)
        if (m.open)
            ++movies;

    std::size_t hotspots = 0;
    for (std::size_t i = 0; i < hotspotEnabled_.size(); ++i)
        if (hotspotEnabled_[i])
            ++hotspots;

    // pic=- and not pic=0: no PLST record covered the whole view, which is a
    // real state (an overlay-only card) and not the id zero.
    char pic[8] = "-";
    if (cardPicture_ != 0)
        std::snprintf(pic, sizeof(pic), "%u", cardPicture_);

    DebugLog::note("CARD %s/%u pic=%s mov=%zu/%zu hs=%zu/%zu snd=%d%s",
                   stackName(stack_.id), cardId_, pic, movies, card_->mlst.size(), hotspots,
                   hotspotEnabled_.size(), ambientCount_,
                   card_->zipModePlace != 0 ? " zip" : "");
}

// ---------------------------------------------------------------------------
// The frame loop
// ---------------------------------------------------------------------------

bool Engine::boot(const SaveGame::SaveState *restore)
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

    // The saved game, if the menu picked one. restoreFrom does its own refusing
    // and says why; falling through to the normal boot is what turns "that slot
    // names an island this conversion does not carry" into a playable new game
    // rather than a dead machine.
    if (restore != nullptr)
    {
        if (restoreFrom(*restore))
            return true;
        // It got far enough to lay the save's variables down before giving up,
        // and those must not survive into the new game the fall-through starts
        // -- aspit card 1 reading a half-finished playthrough's state would be
        // a stranger bug than the failed load that caused it.
        vars_.startNewGame();
        zipDests_.clear();
    }

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

    // X opens and closes the zoom viewer. It is the only free face button --
    // A clicks, B skips a cutscene, and Y is left alone as the one thing a
    // player can press without changing anything.
    if ((keysDown() & KEY_X) != 0)
    {
        toggleZoom();
        return;
    }

    // While the viewer is up it owns the input, and nothing below runs: the
    // hotspots under the pointer belong to a card that is not on screen.
    if (mode_ == Mode::Zoom)
    {
        const std::uint32_t held = keysHeld();
        const std::uint32_t down = keysDown();
        if ((down & (KEY_B | KEY_START)) != 0)
        {
            toggleZoom();
            return;
        }

        // L snapshots the zoomed view into the notebook.
        //
        // Here rather than in the bare-L handler further down, because this
        // branch owns the input while the viewer is up and returns before
        // reaching it. This is the place the feature is most wanted: the player
        // opened the viewer to READ something, and reading something in Riven is
        // usually the prelude to writing it down.
        //
        // Guarded on SELECT because the developer-chord block is also below this
        // branch and so never runs during zoom -- without the guard, SELECT+L
        // meant as a screenshot would take a note instead.
        if ((down & KEY_L) != 0 && (held & KEY_SELECT) == 0)
        {
            captureNote();
            return;
        }

        // Slow for the first quarter second, then fast -- the same shape as the
        // pointer's D-pad nudge below, and for the same reason: a single press
        // should move a pixel and a held one should cross the picture.
        ++padHeld_;
        if ((held & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) == 0)
            padHeld_ = 0;
        const int step = padHeld_ > 15 ? 8 : 2;

        int dx = 0;
        int dy = 0;
        if ((held & KEY_LEFT) != 0)
            dx -= step;
        if ((held & KEY_RIGHT) != 0)
            dx += step;
        if ((held & KEY_UP) != 0)
            dy -= step;
        if ((held & KEY_DOWN) != 0)
            dy += step;

        // Stylus drag: the picture follows the finger, so dragging left pulls
        // the window right. Only while the stylus stays down -- a fresh touch
        // seeds the anchor instead of jumping the view to it.
        if ((held & KEY_TOUCH) != 0)
        {
            if ((down & KEY_TOUCH) == 0)
            {
                dx += pointerX_ - touch.px;
                dy += pointerY_ - touch.py;
            }
            pointerX_ = touch.px;
            pointerY_ = touch.py;
        }

        zoomView.pan(dx, dy);
        return;
    }

    // SELECT + a direction replays the current card with that transition, and
    // SELECT + A dissolves it. A developer aid: the shipped data only reaches
    // opcode 18 on particular moves, and an effect that cannot be triggered on
    // demand cannot be looked at closely enough to tell right from nearly right.
    // SELECT is otherwise unused, and holding it suppresses the pointer below.
    if ((keysHeld() & KEY_SELECT) != 0)
    {
        const std::uint32_t down = keysDown();

        // The shoulder buttons, which nothing else in the port uses. Both write
        // to the card and take long enough to be felt, so they are on the same
        // chord as the transition replay rather than on a bare press.
        if ((down & KEY_L) != 0)
        {
            DebugLog::screenshot();
            return;
        }
        if ((down & KEY_R) != 0)
        {
            DebugLog::vramDump();
            return;
        }

        // The command prompt. On START because the two shoulder buttons are
        // taken above and START is otherwise only a cutscene skip -- and it is
        // the one chord that opens something rather than doing something, which
        // is what the longest reach on the machine ought to be spent on.
        if ((down & KEY_START) != 0)
        {
            runDebugConsole();
            return;
        }

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

    // The notebook, on the two buttons nothing else uses bare.
    //
    // Below the SELECT block on purpose: SELECT+L is the screenshot and SELECT+R
    // the VRAM dump, and the block above returns before this, so the chords keep
    // their meaning and the bare presses get the shoulder buttons and Y.
    if ((keysDown() & KEY_L) != 0)
    {
        captureNote();
        return;
    }
    if ((keysDown() & KEY_Y) != 0)
    {
        runNotebook();
        return;
    }

    // START opens the port's own menu: save, load, settings, resume.
    //
    // Below the SELECT block, so SELECT+START stays available as a chord, and
    // above everything else, so the menu cannot be opened by a press that also
    // lands on a hotspot. Unreachable during a cutscene and from the zoom
    // viewer, both of which return before this -- the viewer above, and a
    // blocking movie because it spins its own loop and never calls this.
    if ((keysDown() & KEY_START) != 0)
    {
        mainMenu.runInGameMenu();
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
        // noteOverlayRows and not markRowMask: these rows are a MOVIE's, and the
        // difference is what lets the next screen update take them back off
        // again (CardSurface::refreshFromClean).
        if (m.player.profile() == VideoProfile::Lite && surface_.exists())
            surface_.noteOverlayRows(m.player.compositeInto(surface_.texels()));

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
    //
    // The two reasons a video puts the pointer away, resolved here because this
    // is the one place per frame that both are known: a blocking play, which is
    // scoped and counted (CursorHide), and a fullscreen movie owning the screen,
    // which is derived -- opcode 33 can start one and let the script carry on,
    // so there is no scope to hang a guard on. Inventory.cpp:151-152 is the same
    // rule for the strip next to it.
    //
    // This runs BEFORE pumpMovies() in frame() and idleFrame() alike, so on the
    // frame a fullscreen movie ends, endFullscreenMovie has not run yet and the
    // pointer stays away one frame longer. 16 ms, and the right side to err on.
    cursor_.setBlocked(cursorSuppress_ > 0 || fullscreenMoviePlaying());
    cursor_.flush();
    inventory_.flush();

    // The zoom viewer publishes through the same door as everything else, and
    // after the card's own publish is skipped above -- movieTakeover() is true
    // while it is up, which is how it took the buffers in the first place.
    //
    // LAST, after everything the window is for, because it is the one upload
    // that does not fit in it. A pan repaints all 49 152 texels of the window
    // through a palette, one halfword store at a time, every frame a direction
    // is held: several milliseconds against a ~4.5 ms blank. Ahead of
    // bgs.vblank() it pushed the priority swap out into active display and the
    // flip tore across the middle of the screen; ahead of the OAM writes it
    // would do the same to the cursor. The pixels themselves never needed the
    // window -- nothing is scanning the buffer they go to -- and the flip this
    // asks for is committed at the next frame's vblank instead.
    if (mode_ == Mode::Zoom)
        zoomView.publish();
}

void Engine::frame()
{
    // Order is the hardware's, not the logic's: the caller has just returned from
    // NEA_WaitForVBL, so the flip and the sprite writes go first, and only then
    // the decoding and the scripts -- which are what fill the buffers the NEXT
    // frame will publish.
    ++frames_; // the clock -- see idleFrame(), which advances the same one
    flushUploads();

    RivenAudio::pump();
    pumpMovies();
    // Next to pumpMovies in BOTH loops, because the original's updateEffects is
    // called from doFrame -- which is what playBlocking spins. So Riven's water
    // keeps moving underneath a blocking movie, and a cutscene over a lagoon
    // does not freeze the lagoon.
    pumpEffects();
    // Before processInput, so a status raised by a button this frame gets its
    // full hold rather than being ticked once on the frame it appeared.
    DebugLog::pumpStatus();

    processInput();

    // Nothing below this belongs to a card that is not on screen. A queued
    // command list would draw into buffers the viewer owns, and a CardFrame
    // handler is an animation nobody can see.
    if (mode_ == Mode::Zoom)
        return;

    // The card's timer, if it is due. RivenStack::onFrame (riven_stack.cpp:326-333):
    // skipped while there are queued scripts, and -- this is the part that
    // matters -- reached only from the OUTER loop. ScummVM gets that by queueing
    // the proc as a script rather than calling it ("so that they don't run when
    // the doFrame method is called from an inner game loop", riven_stack.cpp:387-390);
    // here it falls out of where the call sits, because idleFrame() is the inner
    // loop and does not have this line. Below the Zoom return above, so a proc
    // cannot start a movie under a viewer that owns the buffers.
    if (card_ != nullptr && queued_.empty() && !runningQueued_)
        checkTimer();

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
