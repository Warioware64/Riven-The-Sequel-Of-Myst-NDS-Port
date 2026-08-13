#pragma once

// Reading a .rvid off the card: the header, the frame index, and one frame at a
// time into a single buffer.
//
// Kept apart from RvidPlayer so that the file layout is described in exactly one
// place, and free of <nds.h> so tests/test_rvid_arm9.cpp can compile it for the
// host -- on hardware nothing can check that a movie was read back the way it was
// written. shared/RivenVideo.hpp is the format; this is only the reading of it.
//
// One buffer, sized from RvidHeader::largestFrameBytes at open time and never
// resized. That field exists for this: growing a buffer mid-movie on a DS leaves
// a heap fragment that never comes back, and Riven plays a lot of movies.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RivenVideo.hpp"

namespace rivenrt
{

/// One frame as it comes off the card: the header's fields, and pointers into
/// the reader's buffer for the two payloads.
struct RvidFrameData
{
    const std::uint8_t *audio = nullptr; ///< IMA block, or null when there is none
    std::size_t audioBytes = 0;
    /// The picture, width*height ARGB1555 texels -- or null when `repeat` is set
    /// and the frame carries no picture of its own.
    const std::uint8_t *video = nullptr;
    std::size_t videoBytes = 0;
    /// This frame repeats the one before it. How a movie whose audio outlasts its
    /// video carries the rest of its soundtrack.
    bool repeat = false;
    std::uint32_t index = 0; ///< which frame this is
};

class RvidFile
{
public:
    ~RvidFile() { close(); }

    RvidFile() = default;
    RvidFile(const RvidFile &) = delete;
    RvidFile &operator=(const RvidFile &) = delete;

    /// Open and validate. False (with error() set) on a missing file, a bad
    /// magic, a version this build does not read, or a header that does not
    /// describe a playable movie.
    bool open(const std::string &path);
    void close();

    bool isOpen() const { return file_ != nullptr; }

    const rivendata::RvidHeader &header() const { return header_; }
    rivendata::VideoProfile profile() const
    {
        return static_cast<rivendata::VideoProfile>(header_.profile);
    }
    bool hasAudio() const { return (header_.flags & rivendata::kVideoHasAudio) != 0; }
    std::uint32_t frameCount() const { return header_.frameCount; }

    /// Bytes one picture takes. Every frame that has one is this size.
    std::uint32_t frameBytes() const
    {
        return rivendata::rvidFrameBytes(header_.width, header_.height);
    }

    /// Frame index currently positioned at -- the one readNext() will return.
    std::uint32_t position() const { return next_; }
    bool atEnd() const { return next_ >= header_.frameCount; }

    /// Read the frame at the current position and advance. False at the end of
    /// the movie or on a read error (error() then says which).
    ///
    /// The returned pointers are into this object's buffer and are valid only
    /// until the next call.
    bool readNext(RvidFrameData &out) { return readNextInto(out, nullptr, 0); }

    /// The same, but the picture lands in `videoDst` rather than in this object's
    /// buffer -- so the player reads a fullscreen frame straight into the buffer
    /// it is about to show. For a 256-wide movie the file's rows and the
    /// background's rows are the same rows.
    ///
    /// `videoDst` may be null, in which case the picture goes to the internal
    /// buffer as usual. `out.video` points wherever it ended up, except when a
    /// sink is installed (see setVideoSink), where it is null.
    bool readNextInto(RvidFrameData &out, void *videoDst, std::size_t videoDstBytes);

    /// How a picture reaches a destination that std::fread must not touch.
    ///
    /// The destination is now VRAM, and VRAM silently DISCARDS 8-bit writes.
    /// std::fread bottoms out in picolibc's memcpy over the FAT cache, and
    /// neither that memcpy's tail handling nor the cache's alignment is
    /// contractual, so a byte store is entirely possible and would drop pixels
    /// with no error anywhere. With a sink installed, readNextInto() reads the
    /// picture into its own buffer in chunks and hands each one over instead,
    /// leaving the halfword-safe copy to the caller (tonccpy on the ARM9).
    ///
    /// A function pointer rather than a virtual or a std::function on purpose:
    /// this file is compiled for the HOST by tools/riven-convert/tests, so it must
    /// stay free of <nds.h> and of anything that would drag it in.
    using VideoSink = void (*)(void *ctx, std::size_t dstOffset, const void *src,
                               std::size_t bytes);
    void setVideoSink(VideoSink fn, void *ctx)
    {
        sink_ = fn;
        sinkCtx_ = ctx;
    }

    /// Chunk the sink is fed. 8192 is a whole number of 512-byte rows for a
    /// 256-wide movie and a multiple of four for everything else.
    static constexpr std::size_t kSinkChunkBytes = 8192;

    /// Move to `frame` so readNext() returns it. Riven restarts movies constantly
    /// -- a lever animation plays from frame 0 every time the lever is touched --
    /// and raw frames predict from nothing, so any frame is a valid entry point and
    /// this is exact rather than "the keyframe at or before".
    ///
    /// A frame with the repeat flag carries no picture, so seeking to one shows
    /// whatever was on screen; the player only ever seeks to 0 or forwards.
    ///
    /// Returns the frame reached, or -1 if the file has no usable index.
    std::int32_t seekToFrame(std::uint32_t frame);

    bool rewind() { return seekToFrame(0) == 0; }

    const char *error() const { return error_; }

private:
    std::FILE *file_ = nullptr;
    rivendata::RvidHeader header_{};
    std::vector<rivendata::RvidFrameEntry> index_;
    /// sizeof(RvidFrameHeader) + largestFrameBytes worth of scratch, allocated
    /// once. `largestFrameBytes` already counts the frame header.
    std::vector<std::uint8_t> buffer_;
    VideoSink sink_ = nullptr;
    void *sinkCtx_ = nullptr;
    std::uint32_t next_ = 0;
    const char *error_ = "";
    /// Backing store for the one error that has to carry numbers: a file written
    /// by a different converter. "Not an RVID this build reads" is indistinguishable
    /// from a missing movie to whoever is holding the console, and the two have
    /// completely different fixes, so this one says which version it found.
    char versionError_[64] = {};
};

} // namespace rivenrt
