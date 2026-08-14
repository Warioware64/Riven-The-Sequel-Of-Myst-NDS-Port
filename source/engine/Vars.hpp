#pragma once

// Riven's variables: one flat global store, keyed by rivendata::VarId.
//
// The scripts reference a variable by an INDEX into the stack's own NAME 4 list,
// so the same number means different variables in different stacks and the name
// is the only stable identity. That identity is resolved on the HOST, though:
// the converter maps each stack's NAME 4 entries onto the shared enum and ships
// the result as Stack::variableIds (shared/RivenVars.hpp, RivenData.hpp), so a
// variable access here is an integer subscript followed by a hash lookup and
// there is no string anywhere on the device.
//
// The defaults are ScummVM's initVars (riven_vars.cpp:300-366). Everything not
// listed there starts at zero, which the map gives for free. Three of them are
// randomised per game -- the telescope, prison and dome combinations -- and are
// randomised here for the same reason: a fixed combination would make a
// walkthrough's numbers work, which is not the game Cyan shipped.

#include <cstdint>
#include <string>
#include <unordered_map>

#include "RivenVars.hpp"

namespace rivenrt
{

class Vars
{
public:
    using VarId = rivendata::VarId;

    /// Clear everything and apply the new-game defaults, including fresh random
    /// combinations. This is startNewGame (riven.cpp:669-683).
    void startNewGame();

    /// The variable's slot, created at zero if it does not exist yet. Scripts
    /// assign through this, so it has to be a reference the way ScummVM's
    /// getStackVar is.
    ///
    /// VarId::Unknown is a real slot rather than a special case: a script that
    /// names a variable RivenVars.hpp does not know reads and writes there, and
    /// so does nothing observable, which is the behaviour we want and is one
    /// branch cheaper than checking.
    std::uint32_t &at(VarId id) { return vars_[id]; }

    std::uint32_t get(VarId id) const
    {
        const auto it = vars_.find(id);
        return it == vars_.end() ? 0u : it->second;
    }

    void set(VarId id, std::uint32_t value) { at(id) = value; }

    /// Every variable a playthrough has touched, for the save file.
    ///
    /// Const, and the only door out. Scripts assign through at(), and a second
    /// mutable handle on the map would leave it unclear which of the two a
    /// change had come through -- so a load goes back in through clear() and
    /// set() rather than by swapping the container underneath.
    const std::unordered_map<VarId, std::uint32_t, rivendata::VarIdHash> &all() const
    {
        return vars_;
    }

    /// Forget everything. What startNewGame does first, and what a load has to
    /// do before it lays a saved map down: a variable the save does not mention
    /// is a variable that was never set, and leaving the outgoing game's copy of
    /// it in place would carry state across a load in the one direction nobody
    /// would think to look.
    void clear() { vars_.clear(); }

    /// ASCII case fold. NOT used by this class any more -- it is kept here
    /// because the external-command dispatch (Externals.cpp) and the hotspot
    /// name lookup (Engine::idFromName) still match NAME-list strings by hand,
    /// and those lists are mixed case ("xaSetupComplete").
    static std::string normalise(const std::string &name);

    std::size_t size() const { return vars_.size(); }

private:
    /// unordered_map with the identity hash: the key is a dense enum well under
    /// the bucket count, so hashing costs nothing and the lookup is one probe.
    /// Only the variables a playthrough actually touches are ever allocated.
    std::unordered_map<VarId, std::uint32_t, rivendata::VarIdHash> vars_;
};

} // namespace rivenrt
