#pragma once

// SFXE (water effect) parser -- the third resource libvaht does not implement.
// Layout and frame-script semantics: riven_graphics.cpp:414-483.

#include <cstdint>
#include <optional>

#include "RivenSfxe.hpp"
#include "riven/ResourceReader.hpp"

namespace riven
{

/// What parseSfxe made of a resource.
struct SfxeParse
{
    /// The effect, when at least one frame parsed. Empty otherwise.
    std::optional<rivendata::SfxeEffect> effect;
    /// Frames the resource claimed that could not be read. Non-zero means the
    /// source data is damaged; the effect is still usable, just shorter.
    int framesDropped = 0;
    /// Total frames the header claimed.
    int framesClaimed = 0;

    bool ok() const { return effect.has_value(); }
    bool complete() const { return ok() && framesDropped == 0; }
};

/// Parse an SFXE resource into a flattened effect.
///
/// Damaged resources are salvaged rather than rejected. Riven copies in the
/// wild are frequently incomplete -- a 5-CD French set has SFXE resources that
/// are valid for 25 frames and then turn into zero bytes mid-script -- and a
/// water ripple that animates over 25 of its 30 frames is worth far more than
/// no ripple at all. What is NOT acceptable is losing that quietly, so the
/// caller gets the drop count and is expected to report it.
///
/// `effect` is empty only when the header itself is unusable: wrong magic, no
/// frames claimed, or an unreadable offset table.
SfxeParse parseSfxe(ResourceReader &r, std::uint16_t id);

} // namespace riven
