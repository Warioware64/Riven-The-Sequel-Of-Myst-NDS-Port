#include "RvidFile.hpp"

#include <cstring>

using namespace rivendata;

namespace rivenrt
{

/// See RvidFile::pictureFrames for what this is and what it costs.
///
/// Backwards, because the answer is at the end and the tail is short: the first
/// frame big enough to hold a picture IS the last picture, and on a movie with
/// no tail at all that is the very first entry looked at.
std::uint32_t RvidFile::derivePictureFrames()
{
    // The last frame's size is the one thing the index cannot give -- there is no
    // entry after it to subtract from -- so the end of the file stands in for it.
    std::uint32_t fileBytes = 0;
    if (std::fseek(file_, 0, SEEK_END) == 0)
    {
        const long end = std::ftell(file_);
        if (end > 0)
            fileBytes = static_cast<std::uint32_t>(end);
    }

    // indexCount is documented as equal to frameCount and the converter writes
    // them that way, but this reader does not require it of a file, so neither
    // does this.
    const std::uint32_t frames = header_.frameCount < index_.size()
                                     ? header_.frameCount
                                     : static_cast<std::uint32_t>(index_.size());
    const std::uint32_t least =
        static_cast<std::uint32_t>(sizeof(RvidFrameHeader)) + frameBytes();

    for (std::uint32_t n = frames; n-- > 0;)
    {
        const std::uint32_t at = index_[n].offset;
        const std::uint32_t next = (n + 1 < frames) ? index_[n + 1].offset : fileBytes;
        if (next <= at)
            break; // an index that does not run forwards answers nothing
        if (next - at >= least)
            return n + 1;
    }

    // No index, an index that reads backwards, or a movie of nothing but repeat
    // frames -- which the converter cannot write, since frame 0 always carries a
    // picture. Whichever it is, the honest answer is the one everything gave
    // before this function existed.
    return header_.frameCount;
}

bool RvidFile::open(const std::string &path)
{
    close();

    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr)
    {
        error_ = "movie is not on the card";
        return false;
    }

    if (std::fread(&header_, 1, sizeof(header_), file_) != sizeof(header_))
    {
        error_ = "movie is shorter than its header";
        close();
        return false;
    }
    // isRvid checks the version too, so a file written by an older converter is
    // rejected here rather than decoded into noise. Which of the two it was
    // matters more than anything else this function reports: a wrong magic means
    // a corrupt file, and a wrong version means the card was converted by an
    // older build and the whole video stage has to be redone with --force, which
    // the converter will not do on its own (its freshness check only compares
    // mtimes against the source archive).
    const bool magicOk = header_.magic[0] == 'R' && header_.magic[1] == 'V'
                         && header_.magic[2] == 'I' && header_.magic[3] == 'D';
    if (magicOk && header_.version != kVideoVersion)
    {
        std::snprintf(versionError_, sizeof(versionError_),
                      "movie is RVID format %u; this build reads %u -- reconvert",
                      static_cast<unsigned>(header_.version),
                      static_cast<unsigned>(kVideoVersion));
        error_ = versionError_;
        close();
        return false;
    }
    if (!isRvid(header_))
    {
        error_ = "not an RVID file this build can read";
        close();
        return false;
    }
    if (header_.width == 0 || header_.height == 0 || header_.frameCount == 0
        || header_.largestFrameBytes < sizeof(RvidFrameHeader))
    {
        error_ = "movie header describes nothing playable";
        close();
        return false;
    }

    if (header_.indexCount > 0)
    {
        index_.resize(header_.indexCount);
        const std::size_t want = index_.size() * sizeof(RvidFrameEntry);
        if (std::fread(index_.data(), 1, want, file_) != want)
        {
            error_ = "movie is truncated inside its frame index";
            close();
            return false;
        }

        // Where the picture stops, before the position is set: this is the one
        // read of the file's end anybody makes, and doing it here means the seek
        // below leaves the handle where the caller expects it either way.
        pictureFrames_ = derivePictureFrames();

        // Seek to the first frame rather than assuming it starts where the
        // index ends. The converter sizes the index from an upper bound on the
        // frame count (ffprobe cannot give an exact one for these movies), so a
        // short movie leaves unused entries between the index and frame 0.
        if (std::fseek(file_, static_cast<long>(index_[0].offset), SEEK_SET) != 0)
        {
            error_ = "movie's index points outside the file";
            close();
            return false;
        }
    }

    // One allocation for the whole movie. largestFrameBytes already includes the
    // 8-byte frame header (VideoPipeline.cpp measures sizeof(fh) + byteCount).
    buffer_.resize(header_.largestFrameBytes);
    next_ = 0;
    error_ = "";
    return true;
}

void RvidFile::close()
{
    if (file_ != nullptr)
    {
        std::fclose(file_);
        file_ = nullptr;
    }
    index_.clear();
    buffer_.clear();
    header_ = RvidHeader{};
    // The sink belongs to the movie that installed it, not to the object. A slot
    // is reused for the next movie -- often one of the other profile -- and a sink
    // left behind would send that movie's picture to the old destination while the
    // caller's buffer stayed untouched: an overlay composited from a buffer nothing
    // ever wrote is a black rectangle the size of the animation.
    sink_ = nullptr;
    sinkCtx_ = nullptr;
    next_ = 0;
    pictureFrames_ = 0;
}

bool RvidFile::readNextInto(RvidFrameData &out, void *videoDst, std::size_t videoDstBytes)
{
    out = RvidFrameData{};
    if (file_ == nullptr)
    {
        error_ = "no movie open";
        return false;
    }
    if (next_ >= header_.frameCount)
        return false; // the end of the movie, not an error

    RvidFrameHeader fh{};
    if (std::fread(&fh, 1, sizeof(fh), file_) != sizeof(fh))
    {
        error_ = "movie is truncated at a frame header";
        return false;
    }
    if (sizeof(fh) + fh.byteCount > buffer_.size() || fh.audioBytes > fh.byteCount)
    {
        // largestFrameBytes is what the one buffer was sized from, so a frame
        // that claims to be bigger is a corrupt file rather than a reason to
        // reallocate.
        error_ = "frame is larger than the header said any frame would be";
        return false;
    }

    // Audio first, then video: the ARM7 needs the samples before the ARM9 needs
    // the picture, so one sequential read serves both in the order they are used.
    const std::size_t videoBytes = fh.byteCount - fh.audioBytes;

    if (fh.audioBytes > 0
        && std::fread(buffer_.data(), 1, fh.audioBytes, file_) != fh.audioBytes)
    {
        error_ = "movie is truncated inside a frame's audio";
        return false;
    }

    std::uint8_t *videoInto = buffer_.data() + fh.audioBytes;
    const bool toCaller = videoBytes > 0 && videoDst != nullptr;
    if (toCaller && videoDstBytes < videoBytes)
    {
        error_ = "the caller's picture buffer is smaller than a frame";
        return false;
    }

    if (toCaller && sink_ != nullptr)
    {
        // Read into our own buffer a chunk at a time and let the sink place it.
        // See setVideoSink: fread must not write the destination directly.
        std::uint8_t *const staging = buffer_.data() + fh.audioBytes;
        for (std::size_t at = 0; at < videoBytes;)
        {
            std::size_t want = videoBytes - at;
            if (want > kSinkChunkBytes)
                want = kSinkChunkBytes;
            if (std::fread(staging, 1, want, file_) != want)
            {
                error_ = "movie is truncated inside a frame's picture";
                return false;
            }
            sink_(sinkCtx_, at, staging, want);
            at += want;
        }
        // Nothing readable was left behind: the picture is wherever the sink put
        // it, and the staging area is about to be reused.
        videoInto = nullptr;
    }
    else
    {
        if (toCaller)
            videoInto = static_cast<std::uint8_t *>(videoDst);
        if (videoBytes > 0 && std::fread(videoInto, 1, videoBytes, file_) != videoBytes)
        {
            error_ = "movie is truncated inside a frame's picture";
            return false;
        }
    }

    out.audioBytes = fh.audioBytes;
    out.audio = fh.audioBytes > 0 ? buffer_.data() : nullptr;
    out.videoBytes = videoBytes;
    out.video = videoInto;
    out.repeat = (fh.flags & kFrameRepeat) != 0;
    out.index = next_;

    // A frame either carries a whole picture or none at all -- there is nothing
    // in between, and a partial one would be read as texels and shown as noise.
    if (out.videoBytes != 0 && out.videoBytes != frameBytes())
    {
        error_ = "frame's picture is not the size the header describes";
        return false;
    }
    if (out.repeat && out.videoBytes != 0)
    {
        error_ = "frame claims to repeat but carries a picture";
        return false;
    }

    ++next_;
    error_ = "";
    return true;
}

std::int32_t RvidFile::seekToFrame(std::uint32_t frame)
{
    if (file_ == nullptr || index_.empty())
    {
        error_ = "movie has no frame index";
        return -1;
    }
    if (frame >= index_.size())
    {
        error_ = "no such frame";
        return -1;
    }

    // One entry per frame, in order, so this is a direct lookup. It still reads
    // the entry's own frame number rather than trusting the position: the index is
    // on a card, and a wrong seek would play the wrong picture in silence.
    const RvidFrameEntry &e = index_[frame];
    if (e.frame != frame)
    {
        error_ = "frame index does not name the frames in order";
        return -1;
    }
    if (std::fseek(file_, static_cast<long>(e.offset), SEEK_SET) != 0)
    {
        error_ = "could not seek in the movie";
        return -1;
    }
    next_ = frame;
    error_ = "";
    return static_cast<std::int32_t>(frame);
}

} // namespace rivenrt
