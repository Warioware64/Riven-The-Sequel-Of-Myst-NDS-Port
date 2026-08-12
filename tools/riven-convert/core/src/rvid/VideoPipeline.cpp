#include "riven/VideoPipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "RivenData.hpp"
#include "riven/AtomicWrite.hpp"
#include "riven/ImagePipeline.hpp"
#include "riven/QuickTime.hpp"
#include "riven/Rvid.hpp"
#include "riven/SoundPipeline.hpp"
#include "riven/VideoCodecs.hpp"

namespace fs = std::filesystem;
using namespace rivendata;

namespace riven
{
namespace
{
    /// One I-frame every four seconds. It is the seek granularity and the
    /// recovery point after a dropped frame; Riven restarts movies constantly,
    /// so the index earns its eight bytes an entry.
    constexpr int kGopFrames = 60;

    /// Frames past the end of the video track, added so a movie whose audio
    /// outlasts its picture still carries all of its audio. Twelve of a 5-CD
    /// install's 795 movies need this -- ospit's tMOV 0 has 260 seconds of
    /// dialogue over 82 seconds of video -- and the extra frames cost a few
    /// bytes each because nothing in them changes.
    constexpr int kMaxTailFrames = 20000;

    std::vector<std::int16_t> monoFromMovieAudio(const std::vector<std::uint8_t> &packed,
                                                 int channels)
    {
        const auto pcm = decodeIma4(packed.data(), packed.size(), channels);
        return downmixToMono(pcm, channels);
    }
} // namespace

VideoResult convertMovie(const ArchiveSet &set, std::uint16_t id, const fs::path &out,
                         int quality)
{
    const auto bytes = set.readMovie(id);
    if (bytes.empty())
    {
        // Either the resource is missing or libvaht could not follow its atom
        // tree. Ask for the raw bytes so the failure is reported against real
        // content rather than an empty buffer.
        VideoResult res;
        const auto raw = set.read("tMOV", id);
        res.error = "tMOV " + std::to_string(id)
                  + (raw.empty() ? " could not be read" : " is not a readable QuickTime movie");
        return res;
    }
    return convertMovieBytes(bytes, id, out, quality);
}

VideoResult convertMovieBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                              const fs::path &out, int quality)
{
    VideoResult res;

    if (looksDamaged(bytes))
    {
        res.error = "tMOV " + std::to_string(id) + " is zero-filled in this copy of the game";
        return res;
    }

    const MovieInfo info = probeMovie(bytes, id, true);
    if (!info.ok)
    {
        res.error = "tMOV " + std::to_string(id) + ": " + info.error;
        return res;
    }

    const MovieTrack *video = info.video();
    if (video == nullptr || video->samples.empty())
    {
        res.error = "tMOV " + std::to_string(id) + " has no video track";
        return res;
    }
    if (video->width <= 0 || video->height <= 0)
    {
        res.error = "tMOV " + std::to_string(id) + " has no frame size";
        return res;
    }

    // --- the decoder for the source codec ----------------------------------
    std::unique_ptr<VideoDecoder> source;
    if (video->codec == "cvid")
    {
        source.reset(new CinepakDecoder(video->width, video->height));
    }
    else if (video->codec == "rle ")
    {
        if (!QtRleDecoder::supports(video->depth))
        {
            res.error = "tMOV " + std::to_string(id) + ": " + std::to_string(video->depth)
                      + "-bit QuickTime RLE";
            res.unsupported = true;
            return res;
        }
        source.reset(new QtRleDecoder(video->width, video->height, video->depth, {}));
    }
    else
    {
        res.error = "tMOV " + std::to_string(id) + ": unsupported codec '" + video->codec + "'";
        res.unsupported = true;
        return res;
    }

    // --- geometry ----------------------------------------------------------
    // Everything on the DS is the card scaled by the same ratio, movies
    // included, so an overlay lines up with the still it sits on.
    int dstW = std::max(1, video->width * kScaleNum / kScaleDen);
    int dstH = std::max(1, video->height * kScaleNum / kScaleDen);
    const int padW = (dstW + kVideoBlock - 1) / kVideoBlock * kVideoBlock;
    const int padH = (dstH + kVideoBlock - 1) / kVideoBlock * kVideoBlock;

    RvidSettings settings;
    settings.profile = (video->width >= kCardW && video->height >= kCardH)
                           ? VideoProfile::Full
                           : VideoProfile::Lite;
    settings.quality = quality;
    settings.gopLength = kGopFrames;

    res.profile = settings.profile;
    res.width = dstW;
    res.height = dstH;

    // --- audio -------------------------------------------------------------
    const MovieTrack *audio = info.audio();
    std::vector<std::int16_t> mono;
    int audioRate = 0;
    if (audio != nullptr && !audio->samples.empty() && audio->codec == "ima4")
    {
        std::vector<std::uint8_t> packed;
        packed.reserve(static_cast<std::size_t>(audio->samples.size()) * 1024);
        for (const auto &chunk : audio->samples)
            packed.insert(packed.end(), bytes.begin() + chunk.offset,
                          bytes.begin() + chunk.offset + chunk.size);

        mono = monoFromMovieAudio(packed, audio->channels > 0 ? audio->channels : 1);
        audioRate = audio->sampleRate > 0 ? audio->sampleRate : kTargetRate;

        // The tables can describe more packets than the track's duration
        // actually uses; trim rather than emit the padding.
        if (audio->timescale > 0)
        {
            const std::size_t want = static_cast<std::size_t>(
                static_cast<double>(audio->duration) / audio->timescale * audioRate);
            if (want > 0 && want < mono.size())
                mono.resize(want);
        }
    }
    res.hasAudio = !mono.empty();

    // --- frame rate --------------------------------------------------------
    int fpsNum = 15;
    int fpsDen = 1;
    if (video->timescale > 0 && video->modalDuration > 0)
    {
        fpsNum = static_cast<int>(video->timescale);
        fpsDen = static_cast<int>(video->modalDuration);
    }
    const double fps = fpsDen > 0 ? static_cast<double>(fpsNum) / fpsDen : 15.0;

    // A movie whose audio outlasts its picture gets extra frames that repeat
    // the last one, so the whole soundtrack fits in the container.
    int frameCount = static_cast<int>(video->samples.size());
    if (!mono.empty() && fps > 0.0)
    {
        const int needed = static_cast<int>(
            (static_cast<double>(mono.size()) / audioRate) * fps + 0.999);
        frameCount = std::clamp(std::max(frameCount, needed), frameCount,
                                frameCount + kMaxTailFrames);
    }

    // --- encode ------------------------------------------------------------
    RvidEncoder encoder(padW, padH, settings);
    ImaStream audioStream;

    std::vector<std::uint8_t> frameData;
    std::vector<rivendata::RvidKeyframe> keyframes;
    std::uint32_t largestFrame = 0;
    std::size_t audioPos = 0;

    RvidFrame lastFrame;
    for (int i = 0; i < frameCount; ++i)
    {
        if (i < static_cast<int>(video->samples.size()))
        {
            const auto &s = video->samples[i];
            source->decode(bytes.data() + s.offset, s.size);
            const auto texels =
                downscaleToTexels(source->rgb().data(), video->width, video->height, dstW, dstH);
            lastFrame = texelsToFrame(texels, dstW, dstH);
        }
        if (lastFrame.empty())
        {
            res.error = "tMOV " + std::to_string(id) + ": frame 0 did not decode";
            return res;
        }

        const bool forceIntra = (i % kGopFrames) == 0;
        const RvidEncodedFrame encoded = encoder.encode(lastFrame, forceIntra);

        // Audio for this frame: however many samples the clock says should
        // have been played by the end of it, minus what has gone already.
        std::vector<std::uint8_t> audioBlock;
        if (!mono.empty())
        {
            const std::size_t upTo = static_cast<std::size_t>(
                (static_cast<double>(i + 1) * fpsDen / fpsNum) * audioRate);
            std::size_t take = upTo > audioPos ? upTo - audioPos : 0;
            take = std::min(take, mono.size() - std::min(audioPos, mono.size()));
            take &= ~std::size_t(1); // whole bytes, so blocks stay byte-aligned
            if (take > 0)
            {
                const ImaState start = audioStream.state();
                const auto nibbles = audioStream.encode(mono.data() + audioPos, take);
                const std::uint32_t word = makeAdpcmState(start.predictor, start.index);
                audioBlock.resize(kAdpcmStateBytes + nibbles.size());
                std::memcpy(audioBlock.data(), &word, sizeof(word));
                std::memcpy(audioBlock.data() + kAdpcmStateBytes, nibbles.data(),
                            nibbles.size());
                audioPos += take;
            }
        }

        rivendata::RvidFrameHeader fh{};
        fh.byteCount = static_cast<std::uint32_t>(audioBlock.size() + encoded.bytes.size());
        fh.audioBytes = static_cast<std::uint16_t>(audioBlock.size());
        fh.type = static_cast<std::uint8_t>(encoded.intra ? RvidFrameType::Intra
                                                          : RvidFrameType::Predicted);

        if (encoded.intra)
            keyframes.push_back({static_cast<std::uint32_t>(i), 0}); // offset patched below

        const std::size_t at = frameData.size();
        frameData.resize(at + sizeof(fh) + fh.byteCount);
        std::memcpy(frameData.data() + at, &fh, sizeof(fh));
        std::memcpy(frameData.data() + at + sizeof(fh), audioBlock.data(), audioBlock.size());
        std::memcpy(frameData.data() + at + sizeof(fh) + audioBlock.size(),
                    encoded.bytes.data(), encoded.bytes.size());

        if (encoded.intra)
            keyframes.back().offset = static_cast<std::uint32_t>(at);

        largestFrame = std::max<std::uint32_t>(largestFrame,
                                               static_cast<std::uint32_t>(sizeof(fh) + fh.byteCount));
    }

    // --- assemble ----------------------------------------------------------
    rivendata::RvidHeader hdr{};
    std::memcpy(hdr.magic, "RVID", 4);
    hdr.version = kVideoVersion;
    hdr.profile = static_cast<std::uint8_t>(settings.profile);
    hdr.flags = mono.empty() ? 0 : kVideoHasAudio;
    hdr.width = static_cast<std::uint16_t>(dstW);
    hdr.height = static_cast<std::uint16_t>(dstH);
    hdr.fpsNum = static_cast<std::uint16_t>(fpsNum);
    hdr.fpsDen = static_cast<std::uint16_t>(fpsDen);
    hdr.frameCount = static_cast<std::uint32_t>(frameCount);
    hdr.keyframeCount = static_cast<std::uint32_t>(keyframes.size());
    hdr.audioRate = static_cast<std::uint16_t>(mono.empty() ? 0 : audioRate);
    hdr.audioSamplesPerFrame =
        static_cast<std::uint16_t>(mono.empty() ? 0 : std::lround(audioRate * fpsDen / double(fpsNum)));
    hdr.largestFrameBytes = largestFrame;

    const std::size_t indexBytes = keyframes.size() * sizeof(rivendata::RvidKeyframe);
    const std::size_t base = sizeof(hdr) + indexBytes;
    for (auto &k : keyframes)
        k.offset += static_cast<std::uint32_t>(base);

    std::vector<std::uint8_t> file(base + frameData.size());
    std::memcpy(file.data(), &hdr, sizeof(hdr));
    if (indexBytes > 0)
        std::memcpy(file.data() + sizeof(hdr), keyframes.data(), indexBytes);
    std::memcpy(file.data() + base, frameData.data(), frameData.size());

    if (!writeFileAtomic(out, file, res.error))
        return res;

    res.bytes = file.size();
    res.frames = frameCount;
    res.keyframes = static_cast<int>(keyframes.size());
    res.ok = true;
    return res;
}

} // namespace riven
