#pragma once

// Playing a .rvid on the DS: the file, the soundtrack and the picture, tied
// together by a clock.
//
// There is no decoder. A frame in the file IS the texels the DS samples, so
// playback is a read into the right place and nothing else -- which is the whole
// argument for storing them that way (docs/video.md). What is left is the part
// that was always the interesting half: where the picture goes, and when.
//
//   FULL -- fullscreen cutscenes, the intro among them. A frame goes STRAIGHT
//           into the BgSurface buffer that is not on screen, through a small
//           staging chunk, and the pair is flipped at the next vblank. There is
//           no RAM shadow, no texture, and no upload window: a background bank is
//           read by the 2D engine per scanline and is never unmapped, so the
//           buffer that is not being scanned out can be written at any point in
//           the frame. (The previous design held two 256 KB NEA materials, which
//           cost 256 KB of VRAM, another 256 KB of main RAM for their shadows and
//           an 84 KB CPU copy every frame.)
//
//   LITE -- the card overlays, which is most of Riven's movies. Read into one
//           scratch buffer and composited into the card's own RAM picture at the
//           MLST position, so that the card and the overlay reach the screen as
//           one image.
//
// The clock is the soundtrack when there is one. streamSamplesPlayed() counts only
// samples the hardware really took, so a player that cannot keep up stalls the
// clock instead of letting the picture run away from the sound. A silent movie is
// paced by vblanks instead.
//
// What "cannot keep up" means now is only ever the card: a fullscreen frame is
// 84480 bytes and 15 fps is 1.21 MB/s. Frames are dropped by SKIPPING them, which
// raw frames make free -- there is nothing to decode first, so a late player seeks
// past what it missed and reads only what it will show.

#include <cstdint>
#include <string>
#include <vector>

#include "RivenData.hpp"
#include "RivenImage.hpp"
#include "global_header.hpp"
#include "render/BgSurface.hpp"
#include "rvid/RvidFile.hpp"

namespace rivenrt
{

class RvidPlayer
{
public:
    ~RvidPlayer() { close(); }

    RvidPlayer() = default;
    RvidPlayer(const RvidPlayer &) = delete;
    RvidPlayer &operator=(const RvidPlayer &) = delete;

    /// Open a movie. `cardX`/`cardY` are the MLST blit position in Riven's
    /// original 608x392 coordinates and are ignored for a FULL movie, which
    /// always fills the view.
    bool open(const std::string &path, int cardX = 0, int cardY = 0);
    void close();
    bool isOpen() const { return file_.isOpen(); }

    /// Where a FULL movie's frames go. Must be set before play() for a
    /// fullscreen movie; ignored for LITE, which composites into the card
    /// picture instead. The player writes whichever buffer is not on screen and
    /// asks for a flip, so it never has to know which one that is.
    void setSurface(BgSurface *bg) { bg_ = bg; }

    /// Start (or restart) playback from frame 0, taking the audio stream if the
    /// movie has a soundtrack and nothing else already holds it.
    bool play(bool loop);

    /// Stop, releasing the audio stream but keeping the file open so the next
    /// play() does not have to re-read the header. Riven restarts the same movie
    /// constantly.
    void stop();

    bool isPlaying() const { return playing_; }
    bool finished() const { return finished_; }

    /// Read as far as the clock allows. Call once per frame.
    void pump();

    /// True once, each time a FULL movie has a new picture waiting in the back
    /// buffer. The caller turns that into BgSurface::requestFlip(); the picture
    /// itself is already in VRAM, written by pump() outside any timing window.
    bool takeFlip();

    /// Composite a LITE movie's current picture into `dst`, a kViewW x kViewH
    /// ARGB1555 buffer, and return the 8-row bands it touched so the caller can
    /// upload only those. Zero when nothing changed.
    std::uint32_t compositeInto(rivendata::Texel *dst);

    rivendata::VideoProfile profile() const { return file_.profile(); }
    int width() const { return file_.header().width; }
    int height() const { return file_.header().height; }

    int viewX() const { return viewX_; }
    int viewY() const { return viewY_; }

    /// Move an overlay without reopening it. MLST is re-read on every card, and a
    /// record that keeps its movie but changes its blit position is common
    /// (Engine::activateMlst only closes the slot when the movie itself differs),
    /// so the position cannot be something only open() knows. Same coordinates as
    /// open(); ignored for a FULL movie, which is centred in the view.
    void setPosition(int cardX, int cardY);

    void setVolume(int volume); ///< 0..256, the stream gain while this owns it

    const char *error() const { return error_; }

    /// How far ahead of the clock the reader is allowed to run. One frame: with
    /// nothing to decode there is no work to get ahead with, and the read-ahead
    /// exists only so the audio ring is never empty when the interrupt asks.
    static constexpr std::uint32_t kReadAheadFrames = 2;

    /// Vblanks the soundtrack may go without advancing before the player stops
    /// believing in it. Half a second: long enough that a slow card stalling the
    /// ring is not mistaken for a dead stream (that stall is the whole point of
    /// clocking on real samples), short enough that a cutscene which is never
    /// going to start does not hold the game.
    static constexpr std::uint32_t kClockWatchdogVbl = 30;

private:
    bool readNextFrame();
    std::uint32_t clockFrame() const;
    void releaseAudio();

    RvidFile file_;

    // FULL presentation
    BgSurface *bg_ = nullptr;
    bool swapPending_ = false;

    /// LITE scratch: one picture, allocated at open() from the file's own frame
    /// size. The largest overlay in the game is 79860 bytes.
    std::vector<rivendata::Texel> scratch_;

    int viewX_ = 0;
    int viewY_ = 0;

    bool playing_ = false;
    bool finished_ = false;
    bool loop_ = false;
    bool ownsAudio_ = false;
    bool havePicture_ = false;
    bool liteDirty_ = false;
    int volume_ = 256;

    /// Index of the frame currently held, or -1 before the first.
    std::int32_t shown_ = -1;
    /// Vblank counter for a movie with no soundtrack to be paced by -- and for
    /// one whose soundtrack turned out not to be playing. Counts from play()
    /// either way, so the fallback picks up where the sound should have been.
    std::uint32_t vbl_ = 0;

    /// The soundtrack was opened but is not advancing, so the vblank clock has
    /// taken over. Latched: a stream that comes back later would jump the picture.
    bool clockStalled_ = false;
    std::uint32_t lastSamples_ = 0;
    std::uint32_t silentVbl_ = 0;

    const char *error_ = "";
};

} // namespace rivenrt
