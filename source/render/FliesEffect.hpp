#pragma once

// Riven's insects, on the DS.
//
// Two of the game's islands are alive with them: the jungle by night has
// fireflies, the jungle and the gardens by day have flies, and the script asks
// for either with one external command (xflies) whose arguments are a flag and a
// count. There is no art -- the original composites one to five small glowing
// blobs over the finished card every 66 ms, and the whole effect is the motion
// (riven_graphics.cpp:848-1258).
//
// Three things about doing that here rather than at 608x392:
//
//   * THE SIMULATION STAYS IN RIVEN'S COORDINATES. Speeds, accelerations and the
//     "stay below y=100" rule are all in original pixels and all interdependent;
//     rescaling them would be rebalancing the flight model. Only the DRAWING is
//     scaled, at the end, through toDsX/toDsY.
//
//   * A 3x3 ALPHA BLOB COLLAPSES TO 2x2. At this scale a fly is a pixel and a
//     bit, so the eight alpha maps are kept verbatim and summed into a 2x2 as
//     they are drawn -- the sub-pixel map choice still decides how the light
//     leans, which is what keeps the twinkle from looking like a jumping square.
//
//   * IT HAS TO PUT BACK WHAT IT COVERED. This is the one way it differs from
//     WaterEffect: water rewrites the same rows from the untouched card every
//     tick, so it cannot leave a trail, but a fly MOVES. Restoring from
//     CardSurface::clean() would be wrong -- a fly crossing a movie overlay
//     would punch a card-coloured hole in it that nothing repaints until the
//     next screen update, and on a settled card that is never. So each fly keeps
//     the four texels it covered and puts those back, which is right whatever
//     was underneath. The surface's generation counter says when that copy has
//     been invalidated by something redrawing the card.

#include <cstdint>

#include "RivenData.hpp"
#include "render/CardSurface.hpp"

namespace rivenrt
{

class FliesEffect
{
public:
    /// The most the shipped data ever asks for. Every xflies call in the game
    /// passes a count between 1 and 5.
    static constexpr int kMaxFlies = 5;

    /// RivenGraphics::setFliesEffect (riven_graphics.cpp:754-757). Replaces
    /// whatever was running, exactly as the original's delete-and-new does --
    /// which is why a refreshCard, whose card scripts call xflies again, scatters
    /// them afresh.
    void start(int count, bool fireflies);

    /// Forget them. A card change does this: the original destroys the effect
    /// with the card (riven_card.cpp:57-58).
    void clear();

    bool active() const { return count_ > 0; }

    /// One engine frame. Moves and redraws when the effect's own clock comes
    /// round, and does nothing on the three frames in between.
    ///
    /// Returns the row blocks it wrote, in CardSurface::noteOverlayRows' form, so
    /// the caller can hand the result straight on -- a fly is on top of the card
    /// the way a movie's frame is, and a screen update must be able to take it
    /// off.
    std::uint32_t update(CardSurface &surface);

private:
    /// FliesEffectData (riven_graphics.h:158-171), with color32 already unpacked
    /// -- ScummVM stores it as a little-endian byte triple and picks it apart in
    /// colorBlending (riven_graphics.cpp:1231-1233).
    struct Params
    {
        bool lightable;
        bool unlightIfTooBright;
        bool isLarge;
        bool canBlur;
        float maxSpeed;
        float minSpeed;
        int maxAcceleration;
        float blurSpeedTreshold;
        float blurDistance;
        int r, g, b; ///< 0..255, as the original stores them
        int minFramesLit;
        int maxLightDuration;
    };

    /// riven_graphics.cpp:848-876. Members rather than file statics because the
    /// unlit half of the firefly case reads the flies table (see update()).
    static const Params kFireflies;
    static const Params kFlies;

    /// FliesEffectEntry (riven_graphics.h:139-156), plus what the DS needs to
    /// undraw itself.
    struct Fly
    {
        bool light = true;
        int posX = 0, posY = 0, posZ = 0;
        const std::uint16_t *alphaMap = nullptr;
        int width = 0, height = 0;
        int framesTillLightSwitch = 0;
        bool hasBlur = false;
        float posXFloat = 0.0f, posYFloat = 0.0f, posZFloat = 0.0f;
        float directionAngleRad = 0.0f, directionAngleRadZ = 0.0f;
        float speed = 0.0f;

        /// The texels this fly covered last tick, and where. Four at most: the
        /// blob is 2x2 in DS pixels.
        bool hasPrev = false;
        std::uint8_t prevX = 0, prevY = 0, prevW = 0, prevH = 0;
        rivendata::Texel under[4] = {};
    };

    void initFlyRandomPosition(int index);
    void initFlyAtPosition(int index, int posX, int posY, int posZ);
    void updateFlyPosition(int index);
    void selectAlphaMap(bool horGridOffset, bool vertGridOffset, const std::uint16_t **map,
                        int *width, int *height) const;

    Fly fly_[kMaxFlies];
    int count_ = 0;
    const Params *p_ = nullptr;

    /// Engine frames per animation frame, and how many have gone by. The
    /// original runs on a 66 ms period (riven_graphics.cpp:891); at 60 Hz that is
    /// four frames, the same cadence as the water.
    int ticks_ = 0;

    /// The CardSurface generation `under` was sampled from. A drawing, a clear,
    /// a refresh or a bake since then means those texels no longer belong to the
    /// card and must not be written back.
    std::uint32_t gen_ = 0;
};

} // namespace rivenrt
