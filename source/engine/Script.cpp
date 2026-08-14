#include "Script.hpp"

#include <cstdio>

#include "DebugLog.hpp"
#include "engine/Engine.hpp"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Log an opcode this build does not implement, once per opcode. Riven's
    /// data uses about thirty of the fifty numbered opcodes; the rest are
    /// "empty" or "not used" in ScummVM's table, so seeing one at all is news.
    void reportUnimplemented(int opcode)
    {
        static bool seen[64] = {};
        if (opcode < 0 || opcode >= 64)
            return;
        if (seen[opcode])
            return;
        seen[opcode] = true;
        DebugLog::warn("script: opcode %d is not implemented", opcode);
    }

    std::uint16_t arg(const Command &c, std::size_t i)
    {
        return i < c.args.size() ? c.args[i] : 0;
    }

    void runOne(Engine &e, const Command &c);

    /// Opcode 8. The one branching command, and the one with its own encoding:
    /// the control variable is in switchVar and the arms in branches, with
    /// value 0xFFFF the default arm (riven_scripts.cpp:884-908).
    void runSwitch(Engine &e, const Command &c)
    {
        const VarId id = e.variableId(c.switchVar);
        if (id == VarId::Unknown)
            return;
        const std::uint32_t value = e.vars().get(id);

        const SwitchBranch *fallback = nullptr;
        for (const SwitchBranch &b : c.branches)
        {
            if (b.value == kSwitchDefaultValue)
            {
                fallback = &b;
                continue;
            }
            if (b.value == value)
            {
                runCommandList(e, b.commands);
                return;
            }
        }
        if (fallback != nullptr)
            runCommandList(e, fallback->commands);
    }

    /// Opcode 3. An SLST built out of the script's own arguments rather than
    /// taken from the card's list. The layout is positional and unforgiving:
    /// count, ids, then the four scalars, then three parallel per-sound arrays
    /// (riven_scripts.cpp:513-545).
    void runPlayScriptSlst(Engine &e, const Command &c)
    {
        std::size_t at = 0;
        const std::size_t count = arg(c, at++);
        if (count == 0 || c.args.size() < 1 + count * 4 + 5)
            return;

        SoundRec rec;
        rec.index = 0; // the scripts do not set one
        rec.soundIds.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            rec.soundIds[i] = arg(c, at++);
        rec.fadeFlags = arg(c, at++);
        rec.loop = arg(c, at++);
        rec.globalVolume = arg(c, at++);
        rec.u0 = arg(c, at++);
        rec.suspend = arg(c, at++);

        rec.volumes.resize(count);
        rec.balances.resize(count);
        rec.u2.resize(count);
        for (std::size_t i = 0; i < count; ++i)
            rec.volumes[i] = arg(c, at++);
        for (std::size_t i = 0; i < count; ++i)
            rec.balances[i] = static_cast<std::int16_t>(arg(c, at++));
        for (std::size_t i = 0; i < count; ++i)
            rec.u2[i] = arg(c, at++);

        e.playSlst(rec);
    }

    /// Opcode 17. The command name is a NAME 2 index and the argument count is
    /// explicit, so the arguments are a window into args rather than all of it.
    void runExternal(Engine &e, const Command &c)
    {
        const std::uint16_t nameIndex = arg(c, 0);
        const std::size_t count = arg(c, 1);
        const std::uint16_t *first =
            c.args.size() > 2 && count > 0 ? c.args.data() + 2 : nullptr;
        const std::size_t have = c.args.size() > 2 ? c.args.size() - 2 : 0;
        runExternalCommand(e, nameIndex, first, count < have ? count : have);
    }

    /// Opcode 27. The one command whose payload is not a list of uint16: three
    /// words holding a stack name id and a 32-bit RMAP global card id
    /// (riven_scripts.cpp:934-940).
    void runChangeStack(Engine &e, const Command &c)
    {
        const std::uint16_t nameId = changeStackNameId(c);
        const std::uint32_t globalCard = changeStackGlobalCardId(c);
        const std::string name = e.nameFromList(kStackNames, nameId);
        const StackId target = parseStackName(name);
        if (target == StackId::None)
        {
            DebugLog::warn("script: ChangeStack to unknown stack name id %u",
                        static_cast<unsigned>(nameId));
            return;
        }
        e.changeToStackAndGlobalCard(target, globalCard);
    }

    void runOne(Engine &e, const Command &c)
    {
        switch (static_cast<Op>(c.opcode))
        {
        case Op::Empty:
            break;

        // 1: draw a tBMP. With fewer than five arguments the picture covers the
        // whole card, ignoring the rest (riven_scripts.cpp:500-506).
        case Op::DrawBitmap:
        {
            Rect r;
            if (c.args.size() < 5)
            {
                r.left = 0;
                r.top = 0;
                r.right = kCardW;
                r.bottom = kCardH;
            }
            else
            {
                r.left = static_cast<std::int16_t>(arg(c, 1));
                r.top = static_cast<std::int16_t>(arg(c, 2));
                r.right = static_cast<std::int16_t>(arg(c, 3));
                r.bottom = static_cast<std::int16_t>(arg(c, 4));
            }
            e.drawBitmap(arg(c, 0), r);
            break;
        }

        case Op::SwitchCard: // 2
            e.changeToCard(arg(c, 0));
            break;

        case Op::PlayScriptSLST: // 3
            runPlayScriptSlst(e, c);
            break;

        // 4: play a local tWAV (id, volume, playOnDraw). The third argument is
        // ScummVM's playOnDraw, which only matters to its own draw scheduling.
        case Op::PlaySound: // 4
            e.playEffect(arg(c, 0), arg(c, 1));
            break;

        case Op::SetVariable: // 7
        {
            const VarId id = e.variableId(arg(c, 0));
            if (id != VarId::Unknown)
                e.vars().at(id) = arg(c, 1);
            break;
        }

        case Op::Switch: // 8
            runSwitch(e, c);
            break;

        case Op::EnableHotspot: // 9
            e.enableHotspot(arg(c, 0), true);
            break;

        case Op::DisableHotspot: // 10
            e.enableHotspot(arg(c, 0), false);
            break;

        // 12: a bitfield -- bit 0 the effects, bit 1 the ambient tracks, and no
        // bits at all meaning both (riven_scripts.cpp:575-595).
        //
        // ScummVM carries a WORKAROUND here: the menu's Play/Visit/Start buttons
        // run this AFTER their ChangeStack, so on the original it silences the
        // ambient the new card just started, and ScummVM special-cases the two
        // Temple Island cards to ignore it. It is not needed here, because a stack
        // change from inside a script is deferred to the end of the script
        // (Engine.hpp) -- so this still runs on the stack that is leaving, which is
        // what the command was for.
        case Op::StopSound: // 12
        {
            const std::uint16_t flags = arg(c, 0);
            if ((flags & 2) != 0 || flags == 0)
                e.stopAllAmbient();
            if ((flags & 1) != 0 || flags == 0)
                e.stopEffects();
            break;
        }

        // 13: shape the cursor (riven_scripts.cpp:600). This was a no-op while
        // the port had no cursor; it has Riven's own now, and the id a script
        // names is one of the ids the converter read out of riven.exe.
        case Op::ChangeCursor: // 13
            e.setCursor(arg(c, 0));
            break;

        case Op::Delay: // 14
            if (arg(c, 0) > 0)
                e.delay(arg(c, 0));
            break;

        case Op::RunExternalCommand: // 17
            runExternal(e, c);
            break;

        // 18: schedule a transition (riven_scripts.cpp:619-628). Argument 0 is
        // the type; 1-4 are an optional rectangle, which is ignored -- the only
        // effects it would restrict are the wipes, and ScummVM notes that none
        // of those appear in the shipped data.
        case Op::Transition: // 18
            if (!c.args.empty())
                e.scheduleTransition(static_cast<Transition>(
                    static_cast<std::int16_t>(arg(c, 0))));
            break;

        case Op::RefreshCard: // 19
            e.refreshCard();
            break;

        case Op::BeginScreenUpdate: // 20
            e.beginScreenUpdate();
            break;

        case Op::ApplyScreenUpdate: // 21
            e.applyScreenUpdate();
            break;

        case Op::IncrementVariable: // 24
        {
            const VarId id = e.variableId(arg(c, 0));
            if (id != VarId::Unknown)
                e.vars().at(id) += arg(c, 1);
            break;
        }

        case Op::ChangeStack: // 27
            runChangeStack(e, c);
            break;

        case Op::DisableMovie: // 28
            e.enableMovie(arg(c, 0), false);
            break;

        case Op::DisableAllMovies: // 29
            e.disableAllMovies();
            break;

        case Op::EnableMovie: // 31
            e.enableMovie(arg(c, 0), true);
            break;

        case Op::PlayMovieBlocking: // 32
            e.enableMovie(arg(c, 0), true);
            e.playMovie(arg(c, 0), true);
            break;

        case Op::PlayMovie: // 33
            e.enableMovie(arg(c, 0), true);
            e.playMovie(arg(c, 0), false);
            break;

        case Op::StopMovie: // 34
            e.stopMovie(arg(c, 0));
            break;

        // 36: unknown in ScummVM too, and a no-op there (riven_scripts.cpp:691).
        case Op::Unk36: // 36
            break;

        // 37: like StopSound's ambient half, but fading. Nothing here fades yet,
        // so it stops -- which is what the player hears either way on a card
        // change, only more abruptly.
        case Op::FadeAmbientSounds: // 37
            e.stopAllAmbient();
            break;

        // 38: delay an opcode until a movie reaches a time. Every use in the
        // shipped game delays an activateSLST (riven_scripts.cpp:700-715), so the
        // ambient it was waiting to start is started now instead. The difference
        // is a sound cue arriving early rather than a card that never gets one.
        case Op::StoreMovieOpcode: // 38
            if (static_cast<Op>(arg(c, 3)) == Op::ActivateSLST)
                e.activateSlst(arg(c, 4));
            break;

        case Op::ActivatePLST: // 39
            e.activatePlst(arg(c, 0));
            break;

        case Op::ActivateSLST: // 40
            e.activateSlst(arg(c, 0));
            break;

        case Op::ActivateMLSTAndPlay: // 41
            e.activateMlst(arg(c, 0), true);
            break;

        case Op::ActivateBLST: // 43
        {
            // The BLST record names a hotspot and whether it should be on.
            const Card *card = e.card();
            if (card == nullptr)
                break;
            for (const BlstRec &b : card->blst)
                if (b.index == arg(c, 0))
                {
                    e.enableHotspot(b.hotspotId, b.enabled == 1);
                    break;
                }
            break;
        }

        // 44: pick the card's water effect, by FLST index
        // (riven_scripts.cpp:750).
        case Op::ActivateFLST: // 44
            e.activateFlst(arg(c, 0));
            break;

        // 45: zip mode -- jump to the previously visited card whose NAME
        // matches this hotspot's (riven_scripts.cpp:753-765). The hotspot is
        // only clickable at all when initializeZipMode found that match, so the
        // lookup failing here means the card changed underneath the script;
        // doing nothing is then the right answer rather than a fallback.
        case Op::ZipMode: // 45
        {
            const Hotspot *const h = e.currentHotspot();
            if (h == nullptr)
                break;
            const std::int32_t dest =
                e.zipDestFor(e.nameFromList(rivendata::kHotspotNames, h->nameRes));
            if (dest >= 0)
                e.changeToCard(static_cast<std::uint16_t>(dest));
            break;
        }

        case Op::ActivateMLST: // 46
            e.activateMlst(arg(c, 0), false);
            break;

        default:
            reportUnimplemented(c.opcode);
            break;
        }
    }
} // namespace

void runCommandList(Engine &e, const std::vector<Command> &commands)
{
    for (const Command &c : commands)
    {
        runOne(e, c);
        // A command that changed the card has invalidated everything after it:
        // ScummVM's script objects are reference-counted and the rest of the list
        // is simply dropped. Bailing here is the same thing without the
        // bookkeeping.
        if (e.quitRequested())
            return;
    }
}

} // namespace rivenrt
