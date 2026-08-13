#pragma once

// The bottom screen: two 16-bit bitmap backgrounds, three buffers, and the
// transition state machine that slides one over the other.
//
// This replaces the 3D engine entirely for the card view. The old renderer drew
// the card as three textured quads and a fullscreen movie as two 256 KB NEA
// materials, which meant every pixel reached the screen through the TEXTURE bus:
// a bank had to be unmapped from the 3D engine to be written, the 3D engine
// starts rendering 48 scanlines before line 0, and so the whole upload had to fit
// in the ~22 scanlines between VCOUNT 192 and 214. It did not. A card change went
// up over two or three frames as a visible top-to-bottom wipe, and a fullscreen
// movie paid an 84 KB CPU copy every frame on top of 256 KB of RAM shadows.
//
// A background bank has none of those properties. The 2D engine reads it per
// scanline and it is never unmapped, so it can be written at any point in the
// frame; the only rule is to write the buffer that is NOT on screen and flip.
// rivendata::Texel is already the hardware's direct-colour word (RivenImage.hpp:
// bit 15 = opaque), so a frame is copied, never converted.
//
// LAYOUT. Main engine, video mode 5, BG2 and BG3 as BgType_Bmp16 /
// BgSize_B16_256x256 -- 128 KB each, which is exactly one VRAM bank:
//
//     buffer 0 = bank A @ 0x06000000, bitmap base 0
//     buffer 1 = bank B @ 0x06020000, bitmap base 8
//     buffer 2 = bank D @ 0x06040000, bitmap base 16
//
// Two layers, three buffers. The third is what makes a fullscreen movie free to
// leave: the card's visible image is parked in a buffer the movie never touches,
// so the handover back is a rebind rather than another 84 KB upload.
//
// LETTERBOX. Both layers rest at bgSetScroll(id, 0, -kViewOffsetY), so bitmap
// row r is card row r -- the buffers have the same layout as CardSurface's RAM
// picture and no upload path needs an offset term. Screen rows outside the
// picture sample outside the background, and with the display-area-overflow bit
// clear that reads as transparent, so the backdrop (black) shows through. Rows
// kViewH..255 are filled with transparent ONCE at create() and never written
// again; that is what makes a vertical pan reveal the outgoing card underneath
// instead of garbage. Sprites are unaffected, which matters because the
// inventory strip lives at y=176, inside the lower band.

#include <cstdint>

#include "RivenData.hpp"
#include "RivenImage.hpp"
#include "RivenTransition.hpp"
#include "global_header.hpp"

namespace rivenrt
{

class BgSurface
{
public:
    /// One buffer per VRAM bank; a 256x256 16-bit bitmap is exactly 128 KB.
    static constexpr int kBuffers = 3;
    static constexpr int kBufW = 256;
    static constexpr int kBufH = 256;

    /// Frames a pan takes. kRivenTransitionModeFastest is 300 ms
    /// (riven_graphics.cpp:505-508), and the DS runs at 59.83 Hz.
    static constexpr int kPanFrames = 18;
    /// Steps a dissolve takes. BLDALPHA's EVA/EVB are 5-bit and saturate at 16,
    /// so 16 is the finest the hardware blend gets; ScummVM's own count at this
    /// setting is 8, and the extra steps are free here.
    static constexpr int kBlendSteps = 16;

    /// Claim banks A, B and D, bring up BG2/BG3 and clear all three buffers.
    /// The video mode must already be 5 -- see Global::Init, and the note on
    /// bgInit's mode latching there.
    bool create();
    void destroy();
    bool exists() const { return bgId_[0] >= 0; }

    /// The buffer's pixels, straight into VRAM. Writes must be 16- or 32-bit
    /// (tonccpy, dmaCopy); VRAM discards byte stores.
    static rivendata::Texel *pixels(int buf)
    {
        return reinterpret_cast<rivendata::Texel *>(BG_GFX + buf * (0x20000 / 2));
    }

    /// The buffer on screen, and the one it is safe to write.
    int frontBuffer() const { return bound_[front_]; }
    int backBuffer() const { return bound_[front_ ^ 1]; }

    /// Copy the 8-row blocks named in `mask` out of a kViewW x kViewH RAM
    /// picture into `buf`. Runs of adjacent blocks are copied in one go.
    void uploadRows(int buf, const rivendata::Texel *ram, std::uint32_t mask);

    /// Show the back buffer at the next vblank. Idempotent within a frame.
    void requestFlip() { flipPending_ = true; }
    bool flipPending() const { return flipPending_; }

    /// Hand the two buffers that are NOT currently on screen to a fullscreen
    /// movie. The visible card survives untouched in frontBuffer(), so
    /// endMovieTakeover() is a rebind rather than a re-upload.
    void beginMovieTakeover();
    /// Put the parked card back on screen. Returns the buffer whose contents are
    /// now stale, so the caller can mark it for a lazy rebuild.
    int endMovieTakeover();
    bool movieTakeover() const { return keep_ >= 0; }

    /// Start sliding/blending the back buffer (the new card) over the front one.
    /// Both must already hold their finished pictures.
    void beginTransition(rivendata::Transition t);
    bool transitionActive() const { return frames_ > 0; }

    /// The ONLY function that writes a video register. Call once per frame from
    /// the vblank window: it steps a running transition, applies a pending flip
    /// and then commits everything with bgUpdate().
    void vblank();

private:
    void applyPriorities();
    void bind(int layer, int buf);
    /// The buffer neither layer is showing.
    int spareBuffer() const;

    int bgId_[2] = {-1, -1};    ///< BG2, BG3
    int bound_[2] = {0, 1};     ///< buffer shown by each layer
    int front_ = 0;             ///< which of the two layers is on top
    bool flipPending_ = false;

    /// While a fullscreen movie runs, the buffer holding the card underneath.
    /// -1 when there is no movie. The movie is kept off it by rebinding any
    /// layer that lands on it back to the spare (see vblank()).
    int keep_ = -1;

    rivendata::Transition type_ = rivendata::Transition::None;
    int frames_ = 0;
    int frame_ = 0;
    /// A pan ends by swapping which layer is on top; a blend swapped them up
    /// front, because the hardware blends the TOP layer into the one beneath and
    /// the incoming card has to be the top one.
    bool flipAtEnd_ = false;
};

extern BgSurface bgs;

} // namespace rivenrt
