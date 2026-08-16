#include "RvidPlayer.hpp"

#include <cstring>

#include "DebugLog.hpp"
#include "audio/RivenAudio.hpp"
#include "render/CardSurface.hpp"
#include "tonccpy.h"

using namespace rivendata;

namespace rivenrt
{
namespace
{
    /// Rows per dirty band when compositing a LITE overlay. Taken from the card
    /// surface rather than restated: the mask this returns is fed straight into
    /// its markRowMask, so a mismatch would upload the wrong rows.
    constexpr int kRowBand = CardSurface::kRowBlock;

    /// Exactly one player may hold the maxmod stream. maxmod allows one, and the
    /// movie is what wants it; a second movie playing at the same time (which
    /// happens on cards with several overlays) plays silent, which is what the
    /// original does with everything past its own mixer's limits too.
    const RvidPlayer *g_audioOwner = nullptr;

    /// Where a FULL movie's picture is being placed this frame. Filled in per
    /// frame because the destination alternates between two buffers.
    struct FullBlit
    {
        Texel *dst = nullptr; ///< the buffer's origin, NOT the picture's
        int srcW = 0;
        int x0 = 0;
        int y0 = 0;
    };

    /// RvidFile's video sink: the halfword-safe half of the read.
    ///
    /// The destination is VRAM, which discards byte writes, so the file reader
    /// hands the picture over in chunks instead of freading into it
    /// (RvidFile::setVideoSink). Chunks do not have to land on row boundaries --
    /// a FULL movie is 256 wide and 8192 bytes is eight whole rows, but the split
    /// is done properly so a narrower one would work too.
    void fullSink(void *ctx, std::size_t off, const void *src, std::size_t bytes)
    {
        const FullBlit &b = *static_cast<const FullBlit *>(ctx);
        const auto *p = static_cast<const std::uint8_t *>(src);
        const std::size_t rowBytes = static_cast<std::size_t>(b.srcW) * sizeof(Texel);
        if (b.dst == nullptr || rowBytes == 0)
            return;

        while (bytes > 0)
        {
            const std::size_t row = off / rowBytes;
            const std::size_t col = (off % rowBytes) / sizeof(Texel);
            std::size_t n = rowBytes - col * sizeof(Texel); // to the end of this row
            if (n > bytes)
                n = bytes;
            tonccpy(b.dst + static_cast<std::size_t>(b.y0 + row) * kViewW + b.x0 + col,
                    p, n);
            p += n;
            off += n;
            bytes -= n;
        }
    }

    FullBlit g_fullBlit;
} // namespace

bool RvidPlayer::open(const std::string &path, int cardX, int cardY)
{
    close();

    if (!file_.open(path))
    {
        error_ = file_.error();
        return false;
    }

    const RvidHeader &h = file_.header();

    if (file_.profile() == VideoProfile::Full)
    {
        // A fullscreen movie is written straight into a background buffer, so
        // the only requirement is that the picture fits the card view. Every FULL
        // movie in the game is exactly 256x165, which is the view itself.
        if (h.width > kViewW || h.height > kViewH)
        {
            error_ = "fullscreen movie is larger than the card view";
            close();
            return false;
        }
        viewX_ = (kViewW - h.width) / 2;
        viewY_ = (kViewH - h.height) / 2;
    }
    else
    {
        setPosition(cardX, cardY);

        scratch_.assign(file_.frameBytes() / sizeof(Texel), 0);
        if (scratch_.empty())
        {
            error_ = "overlay movie has no picture";
            close();
            return false;
        }
    }

    error_ = "";
    return true;
}

void RvidPlayer::close()
{
    stop();
    file_.close();
    scratch_.clear();
    scratch_.shrink_to_fit();
    swapPending_ = false;
    havePicture_ = false;
    liteDirty_ = false;
    shown_ = -1;
    finished_ = false;
}

void RvidPlayer::setPosition(int cardX, int cardY)
{
    if (!file_.isOpen() || file_.profile() == VideoProfile::Full)
        return;

    // MLST positions are in Riven's original coordinates and are scaled here,
    // never by the converter -- the same reason the hotspot rects are not
    // pre-scaled (RivenData.hpp).
    const int x = toDsX(cardX);
    const int y = toDsY(cardY);
    if (x == viewX_ && y == viewY_)
        return;

    viewX_ = x;
    viewY_ = y;
    // A picture already up belongs to the old rect. Compositing it again puts it
    // where it now goes; what it left behind is the card's own, and the card is
    // redrawn whenever a record moves.
    liteDirty_ = havePicture_;
}

void RvidPlayer::releaseAudio()
{
    if (!ownsAudio_)
        return;
    RivenAudio::streamClose();
    g_audioOwner = nullptr;
    ownsAudio_ = false;
}

bool RvidPlayer::play(bool loop, std::uint32_t startFrame, std::uint32_t stopFrame)
{
    if (!file_.isOpen())
        return false;

    stop();

    const std::uint32_t last = file_.frameCount() > 0 ? file_.frameCount() - 1 : 0;
    startFrame_ = startFrame > last ? last : startFrame;
    stopFrame_ = stopFrame > last ? last : stopFrame;
    if (stopFrame_ < startFrame_)
        stopFrame_ = startFrame_;

    const bool full = file_.profile() == VideoProfile::Full;
    if (full)
    {
        if (bg_ == nullptr || !bg_->exists())
        {
            error_ = "no background surface for a fullscreen movie";
            return false;
        }
        // Only when the movie does not fill the view: the frames themselves never
        // touch the border, so it has to be black before the first one lands.
        // Every FULL movie in the game is exactly the view, so this is the
        // correctness case, not the common one.
        if (width() != kViewW || height() != kViewH)
            for (int b = 0; b < BgSurface::kBuffers; ++b)
                toncset16(BgSurface::pixels(b), static_cast<Texel>(0x8000),
                          static_cast<std::size_t>(kViewW) * kViewH);
    }

    // Unconditionally, so replaying a slot as LITE takes the sink back off. Only
    // FULL writes into VRAM and so only FULL needs one; an overlay reads into its
    // own scratch, and a sink left installed would swallow that read whole.
    file_.setVideoSink(full ? &fullSink : nullptr, full ? &g_fullBlit : nullptr);

    if (file_.seekToFrame(startFrame_) < 0)
    {
        error_ = file_.error();
        return false;
    }
    shown_ = -1;
    havePicture_ = false;
    liteDirty_ = false;
    finished_ = false;
    videoDisabled_ = false;
    // A segment cannot loop: readNextFrame's loop path rewinds to frame 0, which
    // is outside the range. Nothing asks for both -- the only caller that gives a
    // range is the telescope, which blocks on one stroke.
    loop_ = loop && startFrame_ == 0 && stopFrame_ == last;
    vbl_ = 0;
    clockStalled_ = false;
    lastSamples_ = 0;
    silentVbl_ = 0;

    if (file_.hasAudio() && g_audioOwner == nullptr)
    {
        const int rate = file_.header().audioRate;
        if (RivenAudio::streamOpen(rate))
        {
            RivenAudio::streamSetVolume(volume_);
            g_audioOwner = this;
            ownsAudio_ = true;
        }
    }

    playing_ = true;

    // Prime: get the first picture up and some audio into the ring before the
    // clock starts consuming it.
    for (std::uint32_t i = 0; i <= kReadAheadFrames && !finished_; ++i)
        if (!readNextFrame())
            break;

    return true;
}

void RvidPlayer::stop()
{
    playing_ = false;
    releaseAudio();
}

void RvidPlayer::setVolume(int volume)
{
    volume_ = volume < 0 ? 0 : (volume > 256 ? 256 : volume);
    if (ownsAudio_)
        RivenAudio::streamSetVolume(volume_);
}

std::uint32_t RvidPlayer::clockFrame() const
{
    const RvidHeader &h = file_.header();
    // Both clocks measure from play(), so both are offset by where play() began.
    // Zero for a whole movie, which is nearly all of them.
    if (ownsAudio_ && !clockStalled_ && h.audioRate > 0)
    {
        // Which frame the soundtrack has reached. Only samples the hardware
        // really took are counted, so a player that falls behind stalls this
        // rather than being run away from.
        const std::uint64_t played = RivenAudio::streamSamplesPlayed();
        return startFrame_
               + static_cast<std::uint32_t>(
                   played * h.fpsNum
                   / (static_cast<std::uint64_t>(h.fpsDen) * h.audioRate));
    }
    // Silent movie: the DS refreshes at ~60 Hz.
    const std::uint32_t vblPerFrame =
        h.fpsNum > 0 ? (60u * h.fpsDen + h.fpsNum / 2) / h.fpsNum : 4u;
    return startFrame_ + vbl_ / (vblPerFrame > 0 ? vblPerFrame : 1);
}

bool RvidPlayer::readNextFrame()
{
    // Past the end of what this playback owns. stopFrame_ is the last frame of
    // the file for a whole movie, so this IS the old atEnd() test there; for a
    // segment it stops early, before the frames belonging to the next stroke.
    if (file_.position() > stopFrame_)
    {
        if (!loop_)
        {
            finished_ = true;
            return false;
        }
        // Back to the top of the range, which for the only movies that loop --
        // whole ones -- is frame 0 (see the loop_ test in play()).
        if (file_.seekToFrame(startFrame_) < 0)
        {
            finished_ = true;
            return false;
        }
        shown_ = -1;
    }

    // The ring has to have room for this frame's audio BEFORE the frame is read,
    // not after. Checking afterwards meant the frame had already been consumed
    // from the file and its picture written, and the early return then threw away
    // the picture AND the samples -- and since the clock is derived from samples
    // the hardware took, every discarded block moved the clock permanently out of
    // step with the file position.
    if (ownsAudio_ && file_.header().audioSamplesPerFrame > 0
        && RivenAudio::streamFree() < file_.header().audioSamplesPerFrame)
        return false; // not an error: show what is held and come back next frame

    const bool full = file_.profile() == VideoProfile::Full;

    // FULL goes straight into the background buffer that is not on screen, via
    // the sink (VRAM cannot take fread's byte stores). LITE goes into the overlay
    // scratch to be composited into the card picture. Either way the picture is
    // never copied twice.
    void *dst = nullptr;
    std::size_t dstBytes = 0;
    if (videoDisabled_)
    {
        // Nowhere, which is what disableVideo() means. A null destination makes
        // readNextInto's `toCaller` false, so the sink is bypassed too and the
        // picture lands in the reader's own buffer to be overwritten by the next
        // frame -- the file still has to be walked, because the audio is
        // interleaved with it and the audio is the point.
    }
    else if (full)
    {
        g_fullBlit.dst = BgSurface::pixels(bg_->backBuffer());
        g_fullBlit.srcW = width();
        g_fullBlit.x0 = viewX_;
        g_fullBlit.y0 = viewY_;
        dst = g_fullBlit.dst; // only has to be non-null; the sink does the placing
        dstBytes = static_cast<std::size_t>(kViewW) * kViewH * sizeof(Texel);
    }
    else
    {
        dst = static_cast<void *>(scratch_.data());
        dstBytes = scratch_.size() * sizeof(Texel);
    }

    RvidFrameData frame;
    if (!file_.readNextInto(frame, dst, dstBytes))
    {
        error_ = file_.error();
        finished_ = true;
        return false;
    }

    // The room was checked above, so this only fails on a block the decoder
    // cannot use -- and the picture is still good, so it is shown either way.
    if (frame.audioBytes > 0 && ownsAudio_)
        RivenAudio::streamPushIma(frame.audio, frame.audioBytes);

    // A repeat frame carries no picture and leaves the last one alone. That is how
    // a movie whose audio outlasts its video carries the rest of its soundtrack.
    //
    // And a disabled one has a picture that went nowhere, so there is nothing to
    // announce: havePicture_ stays as it was, and neither the flip nor the
    // composite is asked for.
    if (!videoDisabled_ && !frame.repeat && frame.videoBytes > 0)
    {
        havePicture_ = true;
        if (full)
            swapPending_ = true;
        else
            liteDirty_ = true;
    }

    shown_ = static_cast<std::int32_t>(frame.index);
    return true;
}

void RvidPlayer::pump()
{
    if (!playing_ || finished_)
        return;

    ++vbl_;

    // The soundtrack is the clock, and nothing in play() can tell whether maxmod
    // actually started pulling -- streamOpen() reports that the stream was set up,
    // not that its callback ever fires. If it does not, samplesPlayed() never
    // moves, the target frame stays 0 forever, and the movie sits on its primed
    // frame with no error and no end: a blocking cutscene would spin here until
    // the player gave up, which is indistinguishable from "no video". So the
    // clock has to prove itself, and fall back to vblanks when it does not.
    if (ownsAudio_ && !clockStalled_)
    {
        const std::uint32_t played = RivenAudio::streamSamplesPlayed();
        if (played != lastSamples_)
        {
            lastSamples_ = played;
            silentVbl_ = 0;
        }
        else if (++silentVbl_ > kClockWatchdogVbl)
        {
            clockStalled_ = true;
            // vbl_ has been counting since play(), so the fallback clock starts
            // where the soundtrack should have been rather than back at frame 0.
            DebugLog::warn("movie soundtrack is not playing: pacing on vblanks");
        }
    }

    const std::uint32_t target = clockFrame();

    // Behind by more than the read-ahead? Skip straight to where the clock is
    // rather than reading pictures nobody will see. This is what raw frames buy:
    // a skipped frame costs one seek, because nothing after it depends on it.
    //
    // Audio is the one thing that cannot be skipped -- it is the clock -- so this
    // only ever runs when the clock is running, which means the ring is being fed.
    // stopFrame_ and not frameCount(): skipping past the end of a segment would
    // seek out of it, and the frames after it belong to somebody else's stroke.
    if (shown_ >= 0 && target > static_cast<std::uint32_t>(shown_) + kReadAheadFrames + 2
        && target <= stopFrame_)
    {
        if (file_.seekToFrame(target) >= 0)
            shown_ = static_cast<std::int32_t>(target) - 1;
    }

    int guard = 8; // never spend an unbounded amount of one frame in here
    while (!finished_ && guard-- > 0
           && (shown_ < 0 || static_cast<std::uint32_t>(shown_) < target + kReadAheadFrames))
    {
        if (ownsAudio_
            && RivenAudio::streamQueued() > (kReadAheadFrames + 1)
                                                * file_.header().audioSamplesPerFrame)
            break;
        if (!readNextFrame())
            break;
    }
}

bool RvidPlayer::takeFlip()
{
    if (!playing_ || file_.profile() != VideoProfile::Full || !swapPending_)
        return false;
    // The picture is already in the back buffer -- pump() wrote it there through
    // the sink, at whatever point in the frame it happened to run. All that is
    // left is to say so.
    swapPending_ = false;
    return true;
}

std::uint32_t RvidPlayer::rowMask() const
{
    if (file_.profile() != VideoProfile::Lite || !havePicture_)
        return 0;

    // The band range, not a row-by-row walk: the picture is a rectangle, so the
    // blocks it lands in are contiguous and the ends are all that has to be
    // clipped. Same clip as compositeInto's, which is what makes the two agree
    // about which bands the overlay owns.
    int y0 = viewY_;
    int y1 = viewY_ + height() - 1;
    if (y0 < 0)
        y0 = 0;
    if (y1 >= kViewH)
        y1 = kViewH - 1;
    if (y1 < y0)
        return 0;

    std::uint32_t mask = 0;
    for (int b = y0 / kRowBand; b <= y1 / kRowBand; ++b)
        mask |= 1u << b;
    return mask;
}

std::uint32_t RvidPlayer::compositeInto(Texel *dst)
{
    if (dst == nullptr || file_.profile() != VideoProfile::Lite || !havePicture_
        || !liteDirty_)
        return 0;

    const int w = width();
    const int h = height();
    std::uint32_t touched = 0;

    for (int y = 0; y < h; ++y)
    {
        const int vy = viewY_ + y;
        if (vy < 0 || vy >= kViewH)
            continue;
        touched |= 1u << (vy / kRowBand);

        const Texel *src = scratch_.data() + static_cast<std::size_t>(y) * w;
        Texel *row = dst + static_cast<std::size_t>(vy) * kViewW;

        // Clip once per row rather than per pixel: an overlay is placed by MLST
        // and can hang off the edge of the view after scaling.
        int x0 = 0;
        int x1 = w;
        if (viewX_ + x0 < 0)
            x0 = -viewX_;
        if (viewX_ + x1 > kViewW)
            x1 = kViewW - viewX_;
        if (x0 >= x1)
            continue;
        std::memcpy(row + viewX_ + x0, src + x0,
                    static_cast<std::size_t>(x1 - x0) * sizeof(Texel));
    }

    liteDirty_ = false;
    return touched;
}

} // namespace rivenrt
