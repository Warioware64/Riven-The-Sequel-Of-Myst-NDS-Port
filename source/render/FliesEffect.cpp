#include "render/FliesEffect.hpp"

#include <cmath>
#include <cstdlib>

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Engine frames per animation frame. The original's period is 66 ms
    /// (riven_graphics.cpp:891); at 60 Hz that is four.
    constexpr int kPeriod = 4;

    /// The area the flight model works in -- Riven's own card, not the DS view
    /// (riven_graphics.cpp:883). Every speed and every threshold below is in
    /// these pixels.
    constexpr int kGameW = kCardW;
    constexpr int kGameH = kCardH;

    constexpr float kPi = 3.14159265358979323846f;

    /// ScummVM's getRandomNumberRng(min, max): INCLUSIVE at both ends. It is
    /// getRandomNumber(max - min) + min, and getRandomNumber(n) itself returns
    /// 0..n (common/random.cpp:47-57) -- so an exclusive version here would
    /// silently narrow every range in the flight model.
    int randomBetween(int lo, int hi) { return lo + std::rand() % (hi - lo + 1); }

    /// RivenGraphics::colorBlending (riven_graphics.cpp:1229-1238), in the DS's
    /// five bits per channel rather than eight. Same weights, same truncation.
    inline Texel blend(Texel t, int fr, int fg, int fb, int alpha)
    {
        int r = t & 31;
        int g = (t >> 5) & 31;
        int b = (t >> 10) & 31;
        r = (32 * r + alpha * (fr - r)) / 32;
        g = (32 * g + alpha * (fg - g)) / 32;
        b = (32 * b + alpha * (fb - b)) / 32;
        return static_cast<Texel>(0x8000 | (b << 10) | (g << 5) | r);
    }

    inline std::uint32_t rowMask(int y, int h)
    {
        std::uint32_t mask = 0;
        for (int row = y; row < y + h; ++row)
        {
            if (row < 0 || row >= kViewH)
                continue;
            mask |= 1u << (row / CardSurface::kRowBlock);
        }
        return mask;
    }
} // namespace

// riven_graphics.cpp:848-861 and :863-876. The colours are the original's
// color32 fields unpacked: 8447718 is 0x80E6E6, and colorBlending reads byte 0
// as red (riven_graphics.cpp:1231-1233), so that is 230,230,128 -- a pale warm
// green. 661528 is 0x0A1818, so the flies are 24,24,10: a dark speck, which is
// what an unlit firefly falls back to as well.
const FliesEffect::Params FliesEffect::kFireflies = {
    true, true, true, true, 3.0f, 0.7f, 40, 2.0f, 1.0f, 230, 230, 128, 30, 10};
const FliesEffect::Params FliesEffect::kFlies = {
    false, false, false, true, 8.0f, 3.0f, 80, 3.0f, 1.0f, 24, 24, 10, 30, 10};

void FliesEffect::start(int count, bool fireflies)
{
    if (count <= 0)
    {
        clear();
        return;
    }
    if (count > kMaxFlies)
        count = kMaxFlies;

    p_ = fireflies ? &kFireflies : &kFlies;
    count_ = count;
    ticks_ = 0;
    gen_ = 0;
    for (int i = 0; i < count_; ++i)
    {
        fly_[i] = Fly{};
        initFlyRandomPosition(i);
    }
}

void FliesEffect::clear()
{
    count_ = 0;
    p_ = nullptr;
    ticks_ = 0;
}

/// riven_graphics.cpp:908-917. Anywhere in the card, but never in the top
/// quarter -- that is sky and canopy, and the flight model pulls them down out
/// of it anyway.
void FliesEffect::initFlyRandomPosition(int index)
{
    const int posX = std::rand() % (kGameW - 4 + 1);
    int posY = std::rand() % (kGameH - 4 + 1);
    if (posY < 100)
        posY = 100;
    initFlyAtPosition(index, posX, posY, 15);
}

/// riven_graphics.cpp:923-939. posZFloat is deliberately NOT reset, matching the
/// original: a respawned fly inherits the depth it was drifting at, and posZ is
/// overwritten from it on the very next step anyway.
void FliesEffect::initFlyAtPosition(int index, int posX, int posY, int posZ)
{
    Fly &fly = fly_[index];

    fly.posX = posX;
    fly.posXFloat = static_cast<float>(posX);
    fly.posY = posY;
    fly.posYFloat = static_cast<float>(posY);
    fly.posZ = posZ;
    fly.light = true;

    fly.framesTillLightSwitch =
        randomBetween(p_->minFramesLit, p_->minFramesLit + p_->maxLightDuration);

    fly.hasBlur = false;
    fly.directionAngleRad = randomBetween(0, 300) / 100.0f;
    fly.directionAngleRadZ = randomBetween(0, 300) / 100.0f;
    fly.speed = randomBetween(0, 100) / 100.0f;
}

/// riven_graphics.cpp:971-1065, transcribed. The one omission is the blur: its
/// distance is a single ORIGINAL pixel (riven_graphics.cpp:1000-1001), which is
/// 0.42 of a DS one, so the trailing blob would always land on the fly's own
/// pixel. The hasBlur flag is still maintained, because the bright-background
/// reaction reads it (riven_graphics.cpp:1213-1214).
void FliesEffect::updateFlyPosition(int index)
{
    Fly &fly = fly_[index];

    if (fly.directionAngleRad > 2.0f * kPi)
        fly.directionAngleRad -= 2.0f * kPi;
    if (fly.directionAngleRad < 0.0f)
        fly.directionAngleRad += 2.0f * kPi;
    if (fly.directionAngleRadZ > 2.0f * kPi)
        fly.directionAngleRadZ -= 2.0f * kPi;
    if (fly.directionAngleRadZ < 0.0f)
        fly.directionAngleRadZ += 2.0f * kPi;

    fly.posXFloat += std::cos(fly.directionAngleRad) * fly.speed;
    fly.posYFloat += std::sin(fly.directionAngleRad) * fly.speed;
    fly.posX = static_cast<int>(fly.posXFloat);
    fly.posY = static_cast<int>(fly.posYFloat);
    selectAlphaMap(fly.posXFloat - fly.posX >= 0.5f, fly.posYFloat - fly.posY >= 0.5f,
                   &fly.alphaMap, &fly.width, &fly.height);
    fly.posZFloat += std::cos(fly.directionAngleRadZ) * (fly.speed / 2.0f);
    fly.posZ = static_cast<int>(fly.posZFloat);

    if (p_->canBlur && fly.speed > p_->blurSpeedTreshold)
        fly.hasBlur = true;

    if (fly.posY >= 100)
    {
        int maxAngularSpeed = p_->maxAcceleration;
        if (fly.posZ > 15)
            maxAngularSpeed /= 2;
        const int angularSpeed = randomBetween(-maxAngularSpeed, maxAngularSpeed);
        fly.directionAngleRad += angularSpeed / 100.0f;
    }
    else
    {
        // Too high in the screen: turn them back down.
        const int angularSpeed = randomBetween(0, 50);
        if (fly.directionAngleRad >= kPi / 2.0f && fly.directionAngleRad <= 3.0f * kPi / 2.0f)
            fly.directionAngleRad -= angularSpeed / 100.0f;
        else
            fly.directionAngleRad += angularSpeed / 100.0f;
        if (fly.posY < 1)
            initFlyRandomPosition(index);
    }

    if (fly.posZ >= 0)
    {
        // How far the fly is from the nearest edge, in tenths, capped -- which is
        // how the original keeps them from going deep near the frame.
        int distanceToScreenEdge;
        if (fly.posX / 10 >= (kGameW - fly.posX) / 10)
            distanceToScreenEdge = (kGameW - fly.posX) / 10;
        else
            distanceToScreenEdge = fly.posX / 10;
        if (distanceToScreenEdge > (kGameH - fly.posY) / 10)
            distanceToScreenEdge = (kGameH - fly.posY) / 10;
        if (distanceToScreenEdge > 30)
            distanceToScreenEdge = 30;

        if (fly.posZ <= distanceToScreenEdge)
            fly.directionAngleRadZ +=
                randomBetween(-p_->maxAcceleration, p_->maxAcceleration) / 100.0f;
        else
        {
            fly.posZ = distanceToScreenEdge;
            fly.directionAngleRadZ += kPi;
        }
    }
    else
    {
        fly.posZ = 0;
        fly.directionAngleRadZ += kPi;
    }

    const float minSpeed = p_->minSpeed - fly.posZ / 40.0f;
    const float maxSpeed = p_->maxSpeed - fly.posZ / 20.0f;
    fly.speed += randomBetween(-p_->maxAcceleration, p_->maxAcceleration) / 100.0f;
    if (fly.speed > maxSpeed)
        fly.speed -= randomBetween(0, 50) / 100.0f;
    if (fly.speed < minSpeed)
        fly.speed += randomBetween(0, 50) / 100.0f;
}

/// riven_graphics.cpp:1067-1154, verbatim. Eight maps: two sizes, and within a
/// size the four half-pixel offsets, which is how the original gets a fly to
/// drift smoothly across a pixel boundary instead of snapping.
void FliesEffect::selectAlphaMap(bool horGridOffset, bool vertGridOffset,
                                 const std::uint16_t **alphaMap, int *width,
                                 int *height) const
{
    static const std::uint16_t alpha1[12] = {8, 16, 8, 16, 32, 16, 8, 16, 8, 0, 0, 0};
    static const std::uint16_t alpha2[12] = {4, 12, 12, 4, 8, 24, 24, 8, 4, 12, 12, 4};
    static const std::uint16_t alpha3[12] = {4, 8, 4, 12, 24, 12, 12, 24, 12, 4, 8, 4};
    static const std::uint16_t alpha4[16] = {2, 6, 6, 2, 6, 18, 18, 6,
                                             6, 18, 18, 6, 2, 6, 6, 2};
    static const std::uint16_t alpha5[12] = {4, 8, 4, 8, 32, 8, 4, 8, 4, 0, 0, 0};
    static const std::uint16_t alpha6[12] = {2, 6, 6, 2, 4, 24, 24, 4, 2, 6, 6, 2};
    static const std::uint16_t alpha7[12] = {2, 4, 2, 6, 24, 6, 6, 24, 6, 2, 4, 2};
    static const std::uint16_t alpha8[16] = {1, 3, 3, 1, 3, 18, 18, 3,
                                             3, 18, 18, 3, 1, 3, 3, 1};

    struct AlphaMap
    {
        bool horizontalGridOffset;
        bool verticalGridOffset;
        bool isLarge;
        int width;
        int height;
        const std::uint16_t *pixels;
    };

    static const AlphaMap selector[] = {
        {true, true, true, 4, 4, alpha4},   {true, true, false, 4, 4, alpha8},
        {true, false, true, 4, 3, alpha2},  {true, false, false, 4, 3, alpha6},
        {false, true, true, 3, 4, alpha3},  {false, true, false, 3, 4, alpha7},
        {false, false, true, 3, 3, alpha1}, {false, false, false, 3, 3, alpha5}};

    for (const AlphaMap &m : selector)
    {
        if (m.horizontalGridOffset == horGridOffset && m.verticalGridOffset == vertGridOffset
            && m.isLarge == p_->isLarge)
        {
            *alphaMap = m.pixels;
            *width = m.width;
            *height = m.height;
            return;
        }
    }
    // Unreachable: the eight rows cover every combination of three flags. The
    // original error()s here; there is nowhere to report to on the DS, and a
    // null map would be a crash, so fall back to the smallest.
    *alphaMap = alpha5;
    *width = 3;
    *height = 3;
}

std::uint32_t FliesEffect::update(CardSurface &surface)
{
    if (count_ == 0 || !surface.exists())
        return 0;
    if (++ticks_ < kPeriod)
        return 0;
    ticks_ = 0;

    Texel *const texels = surface.texels();
    std::uint32_t mask = 0;

    // 1. Undraw. Only if nothing has redrawn the card since the copy was taken:
    // if it has, the fly has already been painted over by something with a
    // better claim to those pixels, and writing the old ones back would be
    // stamping a two-frame-old scrap of picture onto the new one.
    const bool stale = surface.generation() != gen_;
    for (int i = 0; i < count_; ++i)
    {
        Fly &fly = fly_[i];
        if (!fly.hasPrev)
            continue;
        if (!stale)
        {
            for (int y = 0; y < fly.prevH; ++y)
                for (int x = 0; x < fly.prevW; ++x)
                    texels[(fly.prevY + y) * kViewW + fly.prevX + x] =
                        fly.under[y * 2 + x];
        }
        mask |= rowMask(fly.prevY, fly.prevH);
        fly.hasPrev = false;
    }

    // 2. Fly. riven_graphics.cpp:951-969.
    for (int i = 0; i < count_; ++i)
    {
        updateFlyPosition(i);

        Fly &fly = fly_[i];
        if (fly.posX < 1 || fly.posX > kGameW - 4 || fly.posY > kGameH - 4)
            initFlyRandomPosition(i);

        if (p_->lightable)
        {
            if (--fly.framesTillLightSwitch <= 0)
            {
                fly.light = !fly.light;
                fly.framesTillLightSwitch =
                    randomBetween(p_->minFramesLit, p_->minFramesLit + p_->maxLightDuration);
                fly.hasBlur = false;
            }
        }
    }

    // 3. Draw, in two passes over the flies rather than one.
    //
    // Sampling and blending are separated because two flies can land on the same
    // pixel. A single pass has the second one saving the first one's BLOB as
    // what it must put back, and next tick it stamps that blob down again -- a
    // smear that outlives both of them. Reading every background pixel before
    // any of them is written is the whole fix, and it makes the bright-background
    // test read the card rather than a neighbour's light as well.
    // riven_graphics.cpp:1156-1227.
    int acc[kMaxFlies][2][2] = {};

    for (int i = 0; i < count_; ++i)
    {
        Fly &fly = fly_[i];
        if (fly.alphaMap == nullptr)
            continue;

        // Collapse the 3x3 or 4x4 alpha map onto the one or two DS pixels it
        // lands on. The depth fade is subtracted and clipped PER SOURCE CELL,
        // before the sum, because that is where the original applies it -- doing
        // it once on the total would fade a near fly and a far one alike.
        const int dsX0 = toDsX(fly.posX);
        const int dsY0 = toDsY(fly.posY);
        if (dsX0 < 0 || dsY0 < 0)
            continue;

        int usedW = 1;
        int usedH = 1;
        for (int y = 0; y < fly.height; ++y)
        {
            for (int x = 0; x < fly.width; ++x)
            {
                int a = static_cast<int>(fly.alphaMap[fly.width * y + x]) - fly.posZ;
                if (a <= 0)
                    continue;
                if (a > 32)
                    a = 32;
                // Two DS pixels at most: three or four original pixels span 1.26
                // to 1.68 of them, so the clamp only fires on the unlucky
                // alignment where a 4-wide map straddles three.
                int col = toDsX(fly.posX + x) - dsX0;
                int row = toDsY(fly.posY + y) - dsY0;
                if (col > 1)
                    col = 1;
                if (row > 1)
                    row = 1;
                acc[i][row][col] += a;
                if (col + 1 > usedW)
                    usedW = col + 1;
                if (row + 1 > usedH)
                    usedH = row + 1;
            }
        }

        // Clip to the view. The simulation's own bounds keep a fly at
        // posX <= 604 and posY <= 388 -- 254 and 163 in DS pixels -- so this only
        // ever trims the second column or row.
        int w = usedW;
        int h = usedH;
        if (dsX0 + w > kViewW)
            w = kViewW - dsX0;
        if (dsY0 + h > kViewH)
            h = kViewH - dsY0;
        if (w <= 0 || h <= 0)
            continue;

        bool hoveringBrightBackground = false;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                const Texel px = texels[(dsY0 + y) * kViewW + dsX0 + x];
                // Saved whether or not this cell ends up drawn: the rect goes
                // back next tick as one piece.
                fly.under[y * 2 + x] = px;

                if (p_->unlightIfTooBright)
                {
                    // 192 in the original's eight bits (riven_graphics.cpp:1175).
                    if ((px & 31) >= 24 || ((px >> 5) & 31) >= 24 || ((px >> 10) & 31) >= 24)
                        hoveringBrightBackground = true;
                }
            }
        }

        fly.hasPrev = true;
        fly.prevX = static_cast<std::uint8_t>(dsX0);
        fly.prevY = static_cast<std::uint8_t>(dsY0);
        fly.prevW = static_cast<std::uint8_t>(w);
        fly.prevH = static_cast<std::uint8_t>(h);
        mask |= rowMask(dsY0, h);

        // Over something bright, a firefly puts its light out and turns aside --
        // which is what keeps them off the sky and out of lit windows
        // (riven_graphics.cpp:1213-1225).
        if (hoveringBrightBackground)
        {
            fly.hasBlur = false;
            if (p_->lightable)
            {
                fly.light = false;
                fly.framesTillLightSwitch =
                    randomBetween(p_->minFramesLit, p_->minFramesLit + p_->maxLightDuration);
            }
            if ((std::rand() & 1) != 0)
                fly.directionAngleRad += kPi / 2.0f;
            else
                fly.directionAngleRad -= kPi / 2.0f;
        }
    }

    for (int i = 0; i < count_; ++i)
    {
        const Fly &fly = fly_[i];
        if (!fly.hasPrev)
            continue;

        // An unlit firefly is not invisible: it is drawn in the FLIES colour,
        // which is the dark speck the light was hiding
        // (riven_graphics.cpp:1161-1164). Read after the light may have been put
        // out above, exactly as the original reads it after its own update.
        const Params &colour = fly.light ? *p_ : kFlies;
        const int fr = colour.r >> 3;
        const int fg = colour.g >> 3;
        const int fb = colour.b >> 3;

        for (int y = 0; y < fly.prevH; ++y)
        {
            for (int x = 0; x < fly.prevW; ++x)
            {
                int a = acc[i][y][x];
                if (a <= 0)
                    continue;
                if (a > 32)
                    a = 32;
                Texel *const px = &texels[(fly.prevY + y) * kViewW + fly.prevX + x];
                *px = blend(*px, fr, fg, fb, a);
            }
        }
    }

    gen_ = surface.generation();
    return mask;
}

} // namespace rivenrt
