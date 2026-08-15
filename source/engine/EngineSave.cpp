// Turning the engine into a save file and back.
//
// Out of line in its own translation unit, the way the Myst port keeps its
// per-stack puzzle code and its modal screens out of the main engine file, and
// for the same two reasons: Engine.cpp is long enough already, and a member
// defined here still reaches every private the restore has to put back without
// widening one accessor for the sake of it.
//
// The shape of a save, and the argument for what it leaves out, is in
// SaveGame.hpp. This file is only the two halves that touch the engine.

#include "Engine.hpp"

#include <cstdio>

#include "DebugLog.hpp"
#include "Global.hpp"
#include "SaveGame.hpp"
#include "Settings.hpp"
#include "data/StackFile.hpp"

using namespace rivendata;

namespace rivenrt
{

SaveGame::SaveState Engine::buildSaveState() const
{
    SaveGame::SaveState s;
    s.stackId = static_cast<std::uint8_t>(stack_.id);
    s.cardId = cardId_;

    // The variable map, widened from the enum key to a raw integer. RivenVars.hpp
    // makes the enum's order a contract -- appended to, never renumbered -- so
    // the integer is a stable name for the variable across builds, which the
    // enumerator itself is not (it is a C++ identifier and could be renamed).
    s.vars.reserve(vars_.all().size());
    for (const auto &[id, value] : vars_.all())
        s.vars.emplace(static_cast<std::uint16_t>(id), value);

    s.zipDests.reserve(zipDests_.size());
    for (const ZipDest &z : zipDests_)
        s.zipDests.push_back({static_cast<std::uint8_t>(z.stack), z.cardId, z.name});

    // The frame counter, because three of Riven's timers keep their deadline as
    // a reading of it rather than as a duration. See SaveState::clock.
    s.clock = frames_;

    return s;
}

bool Engine::restoreFrom(const SaveGame::SaveState &s)
{
    const StackId target = static_cast<StackId>(s.stackId);

    // 0. Not from inside a script.
    //
    // changeToStack does `stack_ = std::move(loaded)`, which frees the
    // std::vector<Command> the interpreter is walking -- so a load asked for
    // while a script is running is a use-after-free, not a glitch. And this is
    // the ORDINARY route: Riven's own Restore button is the xarestoregame
    // external command, which runs inside a hotspot's script.
    //
    // Held rather than refused, because a refusal would make that button do
    // nothing. applyDeferredNavigation runs it the moment the outermost script
    // returns, which is the same mechanism changeToStackAndCard uses and
    // for exactly the same reason (Engine.hpp:76-85).
    if (scriptDepth_ > 0)
    {
        haveRestore_ = true;
        pendingRestore_ = s;
        return true;
    }

    // 1. Refuse before touching anything.
    //
    // changeToStack tears the running game down BEFORE it discovers it cannot
    // read the file it wants, and leaves the engine with no card and no stack --
    // recoverable only by quitting. A save naming a stack this conversion does
    // not carry is exactly that case, and it is cheap to see coming.
    const std::string path = global.stacksDir() + stackFileName(target);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        DebugLog::warn("load: %s is not on this card", stackName(target));
        return false;
    }

    // 2. Stop everything the outgoing game is running.
    //
    // changeToStack does this too, but only when the stack actually changes --
    // it early-returns on a save from the island the player is already standing
    // on, which is the common case and the one that would otherwise carry a
    // cutscene and an ambient bed across the load.
    disableAllMovies();
    stopAllAmbient();
    stopEffects();

    // 3. Drop the latches a load can land in the middle of.
    //
    // A save is never written mid-drag, but a LOAD can be asked for while one is
    // armed: the in-game menu opens over a live card, and the stylus that opened
    // it left pressedHotspot_ set. Without this the first release after the load
    // would run a MouseUp script belonging to a hotspot on a card that is gone.
    insideHotspot_ = -1;
    pressedHotspot_ = -1;
    currentHotspot_ = nullptr;
    mode_ = Mode::Card;
    scheduledTransition_ = Transition::None;
    queued_.clear();
    haveStackChange_ = false;

    // The inventory strip's forced-hidden flag is script-owned state that is not
    // a Riven variable, so step 4 cannot restore it and nothing else clears it.
    // Only xadisablemenuintro sets it, only on aspit, and only for the length of
    // the opening cutscene -- so loading while that is on screen would otherwise
    // carry a hidden strip into a game that has no way to put it back.
    // Reset rather than saved: false is right on every card the player can save
    // from, and the intro is not one of them.
    inventory_.setForcedHidden(false);

    // The outgoing card is dropped HERE, before the variables are replaced, and
    // that ordering is the whole point of this line.
    //
    // changeToStack and changeToCard both call leaveCard(), which runs the
    // outgoing card's CardLeave script -- and a leave script writes variables.
    // Run after step 4 it would be writing into the RESTORED map: a script
    // belonging to the game being thrown away, quietly editing the game being
    // loaded. leaveCard() is a no-op with no card, so this skips it, which is
    // also the right answer on its own -- the game that card belonged to is not
    // being left, it is being discarded.
    card_ = nullptr;

    // 4. The state itself.
    vars_.clear();
    for (const auto &[raw, value] : s.vars)
    {
        // Anything this build does not know a name for lands in the Unknown slot
        // rather than being dropped. Vars treats that slot as real but
        // unobservable, so a save from a LATER build loads and simply does not
        // carry the variables this one has never heard of.
        const VarId id = raw < static_cast<std::uint16_t>(VarId::kCount)
                             ? static_cast<VarId>(raw)
                             : VarId::Unknown;
        vars_.set(id, value);
    }

    zipDests_.clear();
    zipDests_.reserve(s.zipDests.size());
    for (const auto &z : s.zipDests)
        zipDests_.push_back({static_cast<StackId>(z.stackId), z.cardId, z.name});

    // The clock, BEFORE step 6 -- the incoming card's own scripts can arm a
    // timer on the way in, and a timer armed against the outgoing session's
    // frame count would fire immediately or never (SaveState::clock).
    frames_ = s.clock;

    // 5. The player's settings win over the save's copy of them.
    //
    // AZip and WaterEnabled are in the variable map because Riven's scripts read
    // them there (Vars::startNewGame), so step 4 has just overwritten both with
    // whatever they were when the slot was written. This puts the player's
    // current preferences back on top -- a game saved with the water off must
    // not turn it off again for someone who has since turned it on.
    settings.apply();

    // 6. Go there.
    if (!changeToStack(target))
        return false;
    if (!changeToCard(s.cardId))
    {
        // The stack loaded but the card in it is not there -- a save from a
        // build whose RMAP differed, or a corrupt-but-checksum-valid id.
        //
        // The old game is already gone by this point and cannot be put back, so
        // returning false here would leave card_ null: a black screen with no
        // hotspots and no way out, which is the one outcome this function's
        // header promises to avoid. Landing on the stack's first card is
        // somewhere the player can walk out of.
        DebugLog::warn("load: %s has no card %u; entering its first card instead",
                       stackName(target), static_cast<unsigned>(s.cardId));
        if (stack_.cards.empty() || !changeToCard(stack_.cards.front().id))
            return false;
    }

    DebugLog::log("LOAD stack=%s card=%u vars=%zu zips=%zu clock=%lu", stackName(target),
                  static_cast<unsigned>(s.cardId), s.vars.size(), s.zipDests.size(),
                  static_cast<unsigned long>(s.clock));
    return true;
}

} // namespace rivenrt
