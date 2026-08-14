#pragma once

// Riven's water, on the DS.
//
// An SFXE is not image data. It is a short looping script of "copy this run of
// pixels from the untouched card to this row of the screen", which is how the
// original ripples a still without storing a single extra frame of it
// (riven_graphics.cpp:452-495). The converter has already flattened the on-disc
// opcodes into plain copy records -- see RivenSfxe.hpp for the format and why.
//
// Two things this class is responsible for that the schema is not:
//
//   * THE SCALE. Every other rectangle in the port is scaled where it is used,
//     because a rectangle survives that. A 392-row copy script does not: at the
//     DS's 165 rows, 2.4 original rows land on the same one. So each copy is
//     converted to DS space ONCE, here, when the effect is loaded -- the replay
//     is then pure memcpy, with no divides and nothing to interpret, which is
//     what it has to be at 15 frames a second.
//
//   * THE OVERDRAW. Because of that collapse, the heaviest effects (full-card
//     water on tspit and jspit) write over twice the view's own texel count in
//     one frame. That is not wrong -- every copy reads the untouched plane, so
//     the last writer wins either way, exactly as it does in the original -- but
//     it is wasteful, and it is the first thing to look at if a water card
//     stutters. Merging each frame's runs per DS row at load time would bound a
//     frame to the view's size by construction.
//
// The effect is deliberately not undone here. ScummVM's updateScreen re-copies
// the effect surface from the untouched one (riven_graphics.cpp:391), so a
// screen update ERASES the ripple and the next tick draws it again -- which is
// exactly the contract CardSurface::noteOverlayRows / refreshFromClean already
// implements for movie overlays. Water goes through the same door.

#include <cstdint>
#include <string>
#include <vector>

#include "render/CardSurface.hpp"

namespace rivenrt
{

class WaterEffect
{
public:
    /// Load sfxe/<stack>/<id>.rsfx and start it at its first frame, replacing
    /// whatever was loaded. False, with nothing loaded and a line saying why, if
    /// the file is missing or does not hold a usable effect.
    bool load(const std::string &path);

    /// Forget the effect. Card changes do this: an effect belongs to the card
    /// that asked for it (riven_card.cpp:57).
    void clear();

    bool active() const { return !frames_.empty(); }

    /// One engine frame. Draws the next animation frame when the effect's own
    /// clock comes round, and does nothing on the frames in between.
    ///
    /// Returns the row blocks it wrote, in CardSurface::noteOverlayRows' form --
    /// zero when it drew nothing, so the caller can hand the result straight on.
    std::uint32_t update(CardSurface &surface);

private:
    /// One copy, already in DS pixels and already clipped to the view. Five
    /// fields in six bytes: the coordinates cannot exceed 255 once scaled, and
    /// only the width can reach 256.
    struct Copy
    {
        std::uint8_t dstX = 0;
        std::uint8_t dstY = 0;
        std::uint8_t srcX = 0;
        std::uint8_t srcY = 0;
        std::uint16_t width = 0;
    };

    /// A slice of `copies_`, plus the row blocks it lands in -- worked out at
    /// load time because it is the same every time the frame comes round.
    struct Frame
    {
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint32_t rowMask = 0;
    };

    std::vector<Copy> copies_;
    std::vector<Frame> frames_;
    std::size_t cur_ = 0;
    /// Engine frames per animation frame, and how many have gone by. Riven's
    /// effects are all 15 fps against the DS's 60, so this is 4.
    int period_ = 4;
    int ticks_ = 0;
};

} // namespace rivenrt
