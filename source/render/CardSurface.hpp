#pragma once

// The card picture on the bottom screen: a RAM copy of what should be on it,
// and the dirty bookkeeping that gets the changed parts into a BgSurface buffer.
//
// Drawing goes through a RAM buffer rather than straight to VRAM because Riven
// composites. A card is a still, plus whatever pictures its scripts draw over it
// (opcode 1 and 39), plus any LITE movie overlays; only the result should ever
// reach the screen. That RAM buffer is also what opcodes 20 and 21
// (BeginScreenUpdate / ApplyScreenUpdate) mean on this hardware: mark rows dirty,
// then publish them.
//
// The RAM picture is 256x165 and a BgSurface buffer is 256x256 with the picture
// in its first 165 rows, so a row here is the same row there and publishing is a
// straight copy at the same offset.
//
// DIRTY MASKS ARE PER BUFFER. The display is double buffered, so the buffer being
// written is two frames behind the RAM picture, not one: publishing only the rows
// that changed since the LAST publish would leave the older changes missing from
// that buffer for good. One mask per buffer is the whole fix.

#include <cstdint>
#include <string>

#include "RivenData.hpp"
#include "RivenImage.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"

namespace rivenrt
{

class CardSurface
{
public:
    /// Rows per dirty unit. Eight matches the video decoder's block height, so a
    /// LITE overlay's dirty blocks map onto these one for one.
    static constexpr int kRowBlock = 8;
    static constexpr int kRowBlocks = (rivendata::kViewH + kRowBlock - 1) / kRowBlock; // 21
    static constexpr std::uint32_t kAllDirty = (1u << kRowBlocks) - 1u;

    /// Allocate the RAM picture. False if there is no room for it.
    bool create();

    /// Release everything.
    void destroy();

    /// True while there is a RAM picture.
    bool exists() const { return texels_ != nullptr; }

    /// The RAM picture as it should LOOK -- the card plus whatever movie
    /// overlays are live. This is what publish() sends.
    ///
    /// Engine::pumpMovies is its one legal writer from outside this class, and
    /// only for overlay frames: it must say so with noteOverlayRows() rather
    /// than markRowMask(), or the pixels it wrote can never be taken back off.
    /// Everything else draws through drawPicture()/clear().
    rivendata::Texel *texels() { return texels_; }
    const rivendata::Texel *texels() const { return texels_; }

    /// The card ALONE, without the overlays -- see `clean_` below.
    ///
    /// The water effect is its one reader, and reading it rather than texels_ is
    /// what keeps a ripple from feeding on itself: every SFXE copy takes its
    /// pixels from the undisturbed card, so a frame's result depends only on the
    /// still, never on the frame before it (riven_graphics.cpp:465-479, which
    /// reads _mainScreen and writes the visible screen).
    const rivendata::Texel *clean() const { return clean_; }

    /// What a draw did, for a caller that has to repeat it somewhere else.
    ///
    /// The zoom viewer is the one such caller: it draws the same overlays onto a
    /// 608x392 picture out of the FULL-resolution twin of the same tBMP, and to
    /// do that it needs two things only this class saw. Where the picture landed
    /// on the card, which for drawPicture() is the file's own size and not the
    /// rectangle it was given; and the two widths, because a section's source
    /// rectangle is in the .rpic's pixels and the twin's are the source tBMP's --
    /// the same numbers only while the converter left the picture alone, which is
    /// every overlay and no full-card still.
    struct Placed
    {
        rivendata::Rect card{}; ///< card coordinates, right/bottom edge clipped
        int fileW = 0;          ///< the .rpic's own width
        int fileH = 0;
        int sourceW = 0; ///< the source tBMP's size, which is the .rpiz twin's
        int sourceH = 0;
    };

    /// Draw a .rpic at `cardRect`'s TOP-LEFT CORNER, which is a PLST rectangle in
    /// Riven's original 608x392 coordinates. Only the corner: how big the picture
    /// is on the card is the picture's own business, and it says so in its header
    /// (RpicImage::srcWidth/srcHeight). The original ignores the rectangle's other
    /// two edges the same way, and 96 of Riven's PLST records disagree with the
    /// bitmap they name, so this is a difference you can see.
    ///
    /// Marks the rows it touched dirty. Fills `placed` when it is not null.
    bool drawPicture(const std::string &path, const rivendata::Rect &cardRect,
                     std::string &error, Placed *placed = nullptr);

    /// One piece of a picture: a rectangle of the FILE's own pixels, and where
    /// on the card (608x392) it goes.
    struct Section
    {
        rivendata::Rect src; ///< in the .rpic's pixels
        rivendata::Rect dst; ///< in Riven's card coordinates
    };

    /// Draw several sections of ONE picture, loading and decoding it once.
    ///
    /// The batch is the point. The dome's slider strip is drawn twenty-five
    /// pieces at a time, once per slot, and resetting the sliders redraws it
    /// about twenty times in a row -- five hundred opens and five hundred
    /// decodes of the same file if each piece went through drawPicture(). The
    /// file is on an SD card.
    bool drawPictureSections(const std::string &path, const Section *sections,
                             std::size_t count, std::string &error,
                             Placed *placed = nullptr);

    /// Fill the picture with black. Marks everything dirty.
    void clear();

    /// Note that rows [y, y + height) have changed.
    void markRows(int y, int height);
    /// Note that the block rows in `mask` have changed -- the form a LITE movie's
    /// composite returns.
    void markRowMask(std::uint32_t mask);
    void markAll();

    // --- movie overlays -----------------------------------------------------

    /// markRowMask, and remember that a MOVIE put those rows there.
    ///
    /// That is the whole difference between an overlay and a drawing: a drawing
    /// is the card, an overlay is on top of it for as long as it plays.
    void noteOverlayRows(std::uint32_t mask);

    /// Take the overlays back off: every row an overlay has written since the
    /// last refresh goes back to what the card's own drawing put there, and is
    /// marked for publishing.
    ///
    /// RivenGraphics::updateScreen (riven_graphics.cpp:383-401), which repaints
    /// the whole card from _mainScreen at every completed screen update and is
    /// what makes an overlay's last frame disappear in the original. Cheap when
    /// nothing composited, which is most updates.
    ///
    /// The caller must re-composite everything still playing straight after --
    /// see Engine::recompositeOverlays. A restored row is card-wide and may be
    /// shared with an overlay that has no new frame to draw.
    void refreshFromClean();

    /// Bumped every time something rewrites the card underneath the overlays --
    /// a drawing, a clear, a refresh, a bake.
    ///
    /// For an effect that has to put back what it drew over, and so keeps a copy
    /// of the pixels it covered: those pixels are only worth putting back if
    /// nothing else has redrawn that part of the card since. FliesEffect is the
    /// one caller. Water does not need it -- it re-reads clean() every tick and
    /// so cannot hold a stale copy of anything.
    std::uint32_t generation() const { return generation_; }

    /// Make an overlay's rect part of the card, so the next refresh keeps it.
    ///
    /// RivenVideo::disable (riven_video.cpp:288-301), and it is load-bearing:
    /// gspit's pins play a movie, disable it and return without redrawing
    /// anything, so the baked frame IS the puzzle's state (gspit.cpp:58-82).
    void bakeRect(int x, int y, int w, int h);

    /// Take a background buffer's pixels to BE the card, both planes of it.
    ///
    /// bakeRect for a fullscreen movie, and it has to read the buffer back
    /// because that is the only copy: a FULL movie's frames go straight into
    /// VRAM and never pass through this surface (RvidPlayer.hpp). ScummVM reaches
    /// the same state by copying the video's rect into _mainScreen when the movie
    /// is disabled (riven_video.cpp:288-301), which is what lets gspit's sub
    /// elevator play its ride through the closed doors the previous movie left
    /// rather than through the still's open ones.
    ///
    /// `buf` is left clean and every other buffer invalidated: `buf` is the one
    /// already showing these pixels, and its ping-pong partner is showing an
    /// older frame of the same movie and so genuinely needs the whole card.
    void adoptBuffer(int buf);

    bool anyDirty(int buf) const { return dirty_[buf] != 0; }

    /// Everything in `buf` is stale, so the next publish must send all of it.
    /// Used when a fullscreen movie hands a buffer back.
    void invalidate(int buf) { dirty_[buf] = kAllDirty; }

    /// The same for every buffer, for a screen that drew over more than the one
    /// handed back.
    ///
    /// A movie writes one buffer at a time and endMovieTakeover names it, so
    /// invalidate(stale) is enough there. The port's own screens are not so
    /// tidy: the zoom viewer draws each buffer as it becomes the back one and
    /// so has written TWO by the time it is left, and the settings screen the
    /// same. Which of them is the spare afterwards depends on how many times
    /// the screen happened to flip, and reasoning about that per caller is how
    /// a buffer gets missed.
    void invalidateAll()
    {
        for (std::uint32_t &d : dirty_)
            d = kAllDirty;
    }

    /// Copy this buffer's outstanding rows into it. Unlike the texture path this
    /// replaces, there is no upload window and no partial write: the buffer is
    /// not the one being scanned out, so the whole 84 KB can go at once whenever
    /// the caller likes.
    void publish(BgSurface &bg, int buf);

private:
    /// What the screen should show: the card plus the overlays that are live.
    rivendata::Texel *texels_ = nullptr;
    /// The card ALONE, as its own drawing left it -- ScummVM's _mainScreen. The
    /// second plane is what makes an overlay undoable: a movie composites into
    /// texels_ only, so the still it covers is still here to be put back.
    rivendata::Texel *clean_ = nullptr;
    /// Bands an overlay has written into texels_ since the last refresh.
    std::uint32_t overlayRows_ = 0;
    /// How many times the card underneath has been rewritten. See generation().
    std::uint32_t generation_ = 0;
    std::uint32_t dirty_[BgSurface::kBuffers] = {};
};

} // namespace rivenrt
