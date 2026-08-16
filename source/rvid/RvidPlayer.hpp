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
    bool play(bool loop) { return play(loop, 0, lastFrame()); }

    /// The same for a SEGMENT: start at `startFrame` and finish after
    /// `stopFrame`, both clamped to the file. Riven's telescope is what needs it
    /// -- one press moves the tube a fifth of its travel, and the original plays
    /// the matching slice of one long movie rather than five short ones
    /// (tspit.cpp:xtexterior300_telescopeup).
    ///
    /// Every frame is a valid entry point (RvidFile::seekToFrame), so this is an
    /// exact seek and the clock is offset to match. `loop` is ignored unless the
    /// range is the whole movie: a loop rewinds to frame 0, which a segment does
    /// not own.
    bool play(bool loop, std::uint32_t startFrame, std::uint32_t stopFrame);

    /// Last frame in the file, or 0 for an empty one.
    std::uint32_t lastFrame() const
    {
        return file_.frameCount() > 0 ? file_.frameCount() - 1 : 0;
    }

    /// Stop, releasing the audio stream but keeping the file open so the next
    /// play() does not have to re-read the header. Riven restarts the same movie
    /// constantly.
    void stop();

    bool isPlaying() const { return playing_; }
    bool finished() const { return finished_; }

    /// Whether the PICTURE has run out, even though the movie has not.
    ///
    /// ScummVM's `videoPtr->getCurFrame() >= frameCount - 1` in
    /// RivenStack::runCredits (riven_stack.cpp:236-240), and the difference
    /// between this and finished() is up to three minutes: finished() is the
    /// whole container, soundtrack and all. See RvidFile::pictureFrames for how
    /// the count is arrived at and what it approximates.
    ///
    /// LEADS THE SCREEN by up to kReadAheadFrames, because shown_ is the frame
    /// READ and pump() reads ahead of the clock -- about 130 ms at 15 fps.
    /// Against the delay its one caller then sits through (1.5 to 12 seconds
    /// before the first credits card) that is nothing, and matching
    /// currentFrame() is worth more than the two frames.
    bool pictureEnded() const
    {
        const std::uint32_t last = file_.pictureFrames();
        return finished_ || last == 0
               || (shown_ >= 0 && static_cast<std::uint32_t>(shown_) + 1 >= last);
    }

    /// Turn looping off (or on) on a playback that has already started.
    ///
    /// RivenVideo::setLooping, which RivenStack::runEndGame calls straight after
    /// play() (riven_stack.cpp:210-213). play() decides this from its own
    /// argument; this is for a caller that has to be sure whatever the MLST
    /// record said -- an ending that looped would never let the credits go.
    void setLooping(bool on) { loop_ = on; }

    /// Stop presenting the picture. Keep playing the sound.
    ///
    /// RivenVideo::disable()'s half that matters here: ScummVM calls it the
    /// moment the picture runs out so the credits can have the screen
    /// (riven_stack.cpp:242). In the ordinary case it has nothing to do -- past
    /// the last picture frame every frame is an audio block behind an eight-byte
    /// header and there are no pixels to present -- and it exists for the case
    /// where pictureFrames() guessed short, where without it a frame arriving
    /// mid-credits would be written into the buffer the roll is scrolling.
    ///
    /// Cleared by play(): a slot is reused, and the next movie in it is not the
    /// one that asked to be silent-with-pictures.
    void disableVideo() { videoDisabled_ = true; }

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

    /// Offer the picture already held to the next compositeInto, even though no
    /// new frame has arrived.
    ///
    /// For a destination that was rebuilt underneath a still-playing overlay:
    /// CardSurface::refreshFromClean puts a whole card-wide band back, which
    /// takes any overlay in that band with it, and an overlay running at 15 fps
    /// may have nothing new to say for another four published frames.
    void refreshPicture() { liteDirty_ = havePicture_; }

    /// The 8-row blocks this overlay's picture occupies, in
    /// CardSurface::noteOverlayRows' form. Zero for a FULL movie, or before the
    /// first frame has arrived.
    ///
    /// Unlike compositeInto's return value this does not depend on there being
    /// anything new to draw: it says where the overlay IS, which is what a
    /// caller that has just written over part of the card needs in order to ask
    /// whether it wrote over this one (Engine::pumpEffects).
    std::uint32_t rowMask() const;

    rivendata::VideoProfile profile() const { return file_.profile(); }
    int width() const { return file_.header().width; }
    int height() const { return file_.header().height; }
    std::uint32_t frameCount() const { return file_.frameCount(); }
    /// Frames of PICTURE, which on a movie whose soundtrack outlasts it is fewer
    /// than frameCount(). See RvidFile::pictureFrames.
    std::uint32_t pictureFrames() const { return file_.pictureFrames(); }
    /// The frame currently on screen, or -1 before the first one.
    /// RivenVideo::getCurFrame (riven_video.cpp:101-104), for the dome's
    /// "did you click on the golden frame?" test (domespit.cpp:50-64).
    std::int32_t currentFrame() const { return shown_; }
    /// The movie's frame rate as the converter found it, for a caller that has a
    /// time and needs a frame (Engine::playMovieRange).
    std::uint32_t fpsNum() const { return file_.header().fpsNum; }
    std::uint32_t fpsDen() const { return file_.header().fpsDen; }

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

    /// The slice of the file this playback owns. 0..lastFrame() for a whole
    /// movie, which is everything but the telescope.
    std::uint32_t startFrame_ = 0;
    std::uint32_t stopFrame_ = 0;

    bool playing_ = false;
    bool finished_ = false;
    bool videoDisabled_ = false;
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
