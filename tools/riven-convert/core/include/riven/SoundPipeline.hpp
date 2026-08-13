#pragma once

// Turning one Riven tWAV into the sound/<stack>/<id>.rsnd file the DS reads.
//
// The container and the reasoning behind it are in shared/RivenSound.hpp. This
// header is the converter side: parse the tWAV, get mono PCM or (in the common
// case) the original nibbles, and emit the file.
//
// tWAV is parsed here rather than through libvaht's vaht_wav, for three reasons
// in increasing order of weight:
//
//   1. vaht_wav_open rejects everything that is not ADPCM (vaht_wav.c:201-206),
//      so raw PCM and the DVD release's MPEG-2 Layer II are unreachable.
//   2. It discards the loop fields and the ADPC chunk.
//   3. Its Data-header struct is only correct when libvaht is built with
//      -fpack-struct=1, which its autotools build passes (src/Makefile.am:8) and
//      our CMake build does not. The header is 20 bytes on disc and 24 in
//      memory, so every field after sampleRate would be read from the wrong
//      offset.
//
// That is the same call CardParse.hpp made: libvaht is kept for the two hard
// jobs -- the MHWK container and the tBMP unpacker -- and the rest is walked
// with ResourceReader.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "RivenSound.hpp"
#include "riven/Archive.hpp"
#include "riven/ResourceReader.hpp"

namespace riven
{

/// tWAV Data-chunk encoding field. Riven ships 1 on the 5-CD release and a mix
/// of 1 and 2 on the DVD/GOG release (sound.cpp:241-242).
enum class TwavEncoding : std::uint16_t
{
    Pcm = 0,
    Adpcm = 1,   ///< Intel DVI ADPCM, one continuous stream
    Mp2 = 2,     ///< MPEG-2 Layer II elementary stream
};

/// What the tWAV's Data chunk says, plus where its payload is.
struct TwavInfo
{
    bool ok = false;
    std::string error;

    TwavEncoding encoding = TwavEncoding::Pcm;
    std::uint16_t rawEncoding = 0; ///< as written, so an unknown value can be reported
    std::uint16_t sampleRate = 0;
    std::uint32_t sampleCount = 0; ///< frames, per the header (not always exact)
    std::uint8_t bitsPerSample = 0;
    std::uint8_t channels = 0;
    std::uint16_t loop = 0;
    std::uint32_t loopStart = 0;
    std::uint32_t loopEnd = 0;

    std::size_t dataOffset = 0; ///< payload offset within the resource
    std::size_t dataBytes = 0;  ///< payload bytes to the end of the resource

    std::size_t adpcOffset = 0; ///< ADPC chunk payload, 0 when there is none
    std::size_t adpcBytes = 0;
};

/// Walk the nested MHWK/WAVE form and read the Data chunk header.
/// Never throws; a malformed resource comes back with ok == false.
TwavInfo parseTwav(ResourceReader &r);

// --- IMA ADPCM -------------------------------------------------------------

/// Decoder state. {0, 0} is where every Mohawk stream starts.
struct ImaState
{
    std::int16_t predictor = 0;
    std::uint8_t index = 0;
};

/// Which nibble of a byte holds the earlier sample in Riven's data.
///
/// This is not settled in the wild. ScummVM's DVI decoder takes the HIGH nibble
/// first (adpcm.cpp:224-232); libvaht (vaht_wav.c:55-60) and the Myst port's
/// Python decoder (myst_sound.py:68) take the low one. They cannot both be
/// right, so here is the measurement, made over every mono ADPCM tWAV in a
/// 5-CD install and repeated by test_sound.
///
/// Audio has no DC component, and an IMA stream decoded with its nibbles
/// transposed accumulates one: each nibble is applied to a predictor its step
/// size was not chosen for, and the error integrates. Decoded HIGH-first,
/// |mean| / RMS comes out between 0.001 and 0.004 on all 42 sounds. Decoded
/// low-first, between 0.59 and 0.98 -- the waveform is the same shape riding
/// on an offset of twenty thousand. ScummVM is right.
///
/// The reason the other two get away with it is that Riven's sounds are
/// overwhelmingly STEREO (270 of 312 in that install, 149 of its 152 MB), and
/// a stereo stream alternates channels nibble by nibble -- so there the order
/// only decides which channel is which, and both decode cleanly. It is audible
/// on the 42 mono effects and nowhere else.
inline constexpr bool kMohawkHighNibbleFirst = true;

/// One ADPC entry: the state a decoder should be in at `frame`, as the game's
/// own encoder recorded it (sound.cpp:146-160).
///
/// Worth knowing before reaching for these as a seek table: in Riven they are
/// useless for that. EVERY tWAV carries exactly one entry, always {frame 0,
/// predictor 64, index 0} -- a fixed template, not a per-file measurement.
/// That is why the converter builds its own table by decoding. (The predictor
/// of 64 is ignored too: it is a constant DC offset of 0.2% of full scale, and
/// both ScummVM and libvaht start from zero.)
struct TwavAdpcPoint
{
    std::uint32_t frame = 0;
    ImaState state[2];
};

/// Parse the ADPC chunk `info` located. `r` may be positioned anywhere; this
/// seeks. Empty when there is no chunk or it does not parse.
std::vector<TwavAdpcPoint> parseAdpc(ResourceReader &r, const TwavInfo &info);

/// Decode a continuous IMA stream to interleaved PCM16.
///
/// `frames` is the number of sample frames to produce, capped at what the data
/// holds. Stereo streams alternate channels nibble by nibble, each with its own
/// decoder state, which is how Mohawk stores them (vaht_wav.c:63).
///
/// When `seek` is non-null the decoder state is appended to it every
/// `seekInterval` frames, starting with the state at frame 0 -- one pass
/// produces both the audio and the table. Only meaningful for mono, which is
/// the only case whose nibbles survive into the output file.
std::vector<std::int16_t> decodeIma(const std::uint8_t *data, std::size_t bytes,
                                    std::size_t frames, int channels, bool highNibbleFirst,
                                    ImaState state = {},
                                    std::vector<rivendata::RsndSeekPoint> *seek = nullptr,
                                    unsigned seekInterval = 0);

/// Encode mono PCM16 as a continuous low-nibble-first IMA stream, starting from
/// state {0, 0}. Used for the PCM and MP2 sources; the ADPCM source is copied
/// through instead.
std::vector<std::uint8_t> encodeIma(const std::int16_t *pcm, std::size_t count);

/// The same encoder, in pieces. RVID interleaves one block of audio into every
/// video frame and each block carries the state it starts from, so a player
/// that drops a frame -- or seeks to a keyframe -- can pick the audio up again
/// without decoding from the beginning. The encoding itself stays continuous:
/// resetting the predictor every frame would put a click at 15 Hz through the
/// whole game.
class ImaStream
{
public:
    /// State at the start of the next block, for that block's header.
    ImaState state() const { return state_; }

    std::vector<std::uint8_t> encode(const std::int16_t *pcm, std::size_t count);

private:
    ImaState state_;
};

/// Exchange the two nibbles of every byte. Turns a high-nibble-first stream
/// into a low-nibble-first one, which is what the DS hardware wants.
void swapNibbles(std::uint8_t *data, std::size_t bytes);

// --- MPEG-2 Layer II -------------------------------------------------------

/// Decode a Layer I/II/III elementary stream to interleaved PCM16.
/// `channels` and `sampleRate` come back from the first frame decoded.
/// Empty result means the stream did not decode; `error` says why.
std::vector<std::int16_t> decodeMpegAudio(const std::uint8_t *data, std::size_t bytes,
                                          int &channels, int &sampleRate,
                                          std::string &error);

// --- PCM helpers -----------------------------------------------------------

/// Average interleaved channels down to one. A no-op for mono.
std::vector<std::int16_t> downmixToMono(const std::vector<std::int16_t> &pcm, int channels);

/// The same, then give back the level the average threw away.
///
/// (L+R)/2 is only lossless when the two channels are the same signal. The wider
/// they are, the more the average cancels, and Riven's ambient beds are wide:
/// most of the game arrives on the DS several dB below where it started, with
/// nothing downstream able to put it back -- the DS master volume is already at
/// its maximum and the sample is already at its own peak. That is most of why the
/// port is quiet.
///
/// The gain is not a normalisation. It restores the mono peak to the peak of the
/// LOUDEST INPUT CHANNEL and stops there, so a sound is never pushed past where it
/// started and the relative loudness the game was mixed with survives intact: a
/// correlated sound gets a gain of 1 and is returned bit-identical, a fully
/// decorrelated one gets the whole 6 dB back. The 2x ceiling is for the
/// pathological anti-correlated case, where the mono peak is near zero and the
/// ratio is meaningless.
///
/// Mono in, mono out, untouched.
std::vector<std::int16_t> downmixToMonoWithMakeup(const std::vector<std::int16_t> &pcm,
                                                  int channels);

/// Linear-interpolating resample of mono PCM16. Riven's own sounds are left at
/// their native rate; this exists for the DVD release's 44.1 kHz MP2 tracks,
/// which are halved to keep the card footprint and the DS timer sane.
std::vector<std::int16_t> resampleMono(const std::vector<std::int16_t> &pcm, int srcRate,
                                       int dstRate);

/// Rate every decoded-then-re-encoded sound lands on.
inline constexpr int kTargetRate = 22050;

// --- The file --------------------------------------------------------------

/// Everything encodeRsnd needs. Exactly one of `nibbles` (already in DS order,
/// without the state word) or `pcm` is used, per `codec`.
struct RsndSource
{
    rivendata::SoundCodec codec = rivendata::SoundCodec::ImaAdpcm;
    std::uint16_t sampleRate = 0;
    std::uint32_t sampleCount = 0;
    bool loops = false;
    std::uint32_t loopStart = 0;
    std::uint32_t loopEnd = 0;

    std::vector<std::uint8_t> nibbles;
    std::vector<rivendata::RsndSeekPoint> seek;
};

/// Build the complete .rsnd file bytes.
std::vector<std::uint8_t> encodeRsnd(const RsndSource &src);

/// What convertSound produced, for progress reporting and size accounting.
struct SoundResult
{
    bool ok = false;
    std::string error;
    std::size_t bytes = 0;
    /// The resource parsed but its encoding is one we cannot convert. The
    /// caller reports this as a warning naming the id, not as a failure.
    bool unsupported = false;
    /// True when the source was MPEG-2 Layer II, so the run can say how many
    /// of those it converted -- it is the DVD-only path and worth counting.
    bool wasMpeg = false;
};

/// Convert tWAV `id` from `set` and write `out` atomically.
SoundResult convertSound(const ArchiveSet &set, std::uint16_t id,
                         const std::filesystem::path &out);

/// The same, on bytes already pulled out of the archive.
///
/// This split is what lets the sound stage use more than one core: libvaht is not
/// thread-safe, so reading stays on the thread that owns the ArchiveSet and only
/// this function -- which touches nothing shared -- runs on workers. It matters most
/// on the DVD release, where every ambient is an MPEG-2 Layer II decode.
SoundResult convertSoundBytes(const std::vector<std::uint8_t> &bytes, std::uint16_t id,
                              const std::filesystem::path &out);

} // namespace riven
