#pragma once

// On-card sound format, shared between the converter and the ARM9 runtime.
//
// One file comes out of every Riven tWAV: sound/<stack>/<id>.rsnd, mono
// IMA ADPCM at the source sample rate.
//
// Why ADPCM. The DS decodes IMA ADPCM in hardware (SoundFormat_ADPCM), and the
// 5-CD game is ALREADY Intel DVI ADPCM -- the same algorithm, the same tables,
// a continuous nibble stream with no per-block reseed (sound.cpp:241, and
// libvaht's vaht_wav.c:40-84 is the same decoder written twice). Riven's mono
// effects therefore need no transcode at all: the nibbles are copied through
// bit-exactly. Decoding to PCM16 would quadruple the card footprint of a game
// that already wants gigabytes; PCM8 would double it and sound worse than the
// source.
//
// Why mono. SLST gives every ambient sound its own balance
// (RivenData.hpp:394), and the DS pans in hardware, so a stereo source is
// downmixed rather than stored twice and re-panned. That is most of the game --
// 270 of a 5-CD install's 312 sounds are stereo, and they carry 149 of its
// 152 MB -- so most sounds ARE decoded, downmixed and re-encoded, and the
// halved channel count pays for the one generation of ADPCM loss.
//
// Why a seek table instead of re-blocking. IMA is a continuous chain: to start
// at sample N you must know the decoder state at N. The usual fix is to reset
// the state every few hundred samples (QuickTime's IMA4 does this; so does
// FastVideoDS, at 256 samples per block), but that requires DECODING and
// RE-ENCODING Riven's stream, which is lossy and throws away the free path
// above. Storing the state every kSeekInterval samples in a side table costs
// ~2.6 KB per minute of audio, keeps the payload bit-exact, and gives the
// runtime the same random access. Mohawk itself does this in the tWAV's ADPC
// chunk, which is where the idea comes from -- we rebuild it rather than trust
// it, because a damaged copy of the game may have one and not the other.
//
// Everything is little-endian: the file IS the DS's in-memory layout, so a
// loaded buffer can be reinterpret_cast to the header and the payload handed
// straight to the sound hardware. (Contrast the SOURCE data, which is Mohawk
// and big-endian throughout.)

#include <cstdint>

namespace rivendata
{

/// How the payload after the seek table is stored.
enum class SoundCodec : std::uint8_t
{
    /// 4-bit IMA ADPCM, low nibble first, preceded by the 4-byte DS hardware
    /// state word. This is what the converter emits for every sound.
    ImaAdpcm = 0,
    /// Signed 8-bit PCM. Not currently emitted; the field exists so a future
    /// "uncompressed" option does not need a format version bump.
    Pcm8 = 1,
    /// Signed 16-bit PCM, likewise.
    Pcm16 = 2,
};

/// `flags` bits.
enum : std::uint8_t
{
    /// The source asked for this sound to loop. loopStart/loopEnd are then
    /// meaningful; when they are equal the whole sound loops.
    kSoundLoops = 1 << 0,
};

/// 32-byte header of a .rsnd file.
///
/// Followed by `seekEntries` RsndSeekPoint, then `dataBytes` of payload.
struct RsndHeader
{
    char magic[4];             ///< "RSND"
    std::uint16_t version;     ///< kSoundVersion
    std::uint16_t sampleRate;  ///< Hz, as the DS timer should be programmed
    std::uint32_t sampleCount; ///< frames (mono), decoded
    std::uint32_t dataBytes;   ///< payload bytes following the seek table
    std::uint8_t codec;        ///< SoundCodec
    std::uint8_t channels;     ///< always 1 today
    std::uint8_t flags;        ///< kSoundLoops
    std::uint8_t reserved;
    std::uint32_t loopStart;   ///< frames
    std::uint32_t loopEnd;     ///< frames; == loopStart means "the whole sound"
    std::uint16_t seekInterval;///< frames between seek points, 0 when there is no table
    std::uint16_t seekEntries; ///< number of RsndSeekPoint following the header
};
static_assert(sizeof(RsndHeader) == 32, "RsndHeader must be 32 bytes");

/// IMA decoder state at frame `n * seekInterval`, so playback can start there.
///
/// Entry 0 is always the state at frame 0 and is therefore always
/// {0, 0} -- it is stored anyway so the table is uniform and indexable.
struct RsndSeekPoint
{
    std::int16_t predictor;
    std::uint8_t index;    ///< step table index, 0..88
    std::uint8_t reserved;
};
static_assert(sizeof(RsndSeekPoint) == 4, "RsndSeekPoint must be 4 bytes");

inline constexpr std::uint16_t kSoundVersion = 1;

/// Frames between seek points. 8192 at 22050 Hz is one entry per 0.37 s.
inline constexpr std::uint16_t kSeekInterval = 8192;

/// The DS sound hardware wants an ADPCM sample to begin with this word:
/// the initial predictor in bits 0-15 and the initial step index in bits 16-22.
/// The converter writes it as the first four bytes of an ImaAdpcm payload, so a
/// short effect can be read into RAM and played with no fixup at all.
inline constexpr std::uint32_t makeAdpcmState(std::int16_t predictor, std::uint8_t index)
{
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(predictor))
         | (static_cast<std::uint32_t>(index & 0x7F) << 16);
}

inline constexpr int kAdpcmStateBytes = 4;

/// The IMA ADPCM tables, from the IMA/DVI specification. Shared so the
/// converter's encoder and the ARM9's software decoder (used when streaming a
/// long ambient through maxmod, which cannot take ADPCM) cannot drift apart.
inline constexpr std::int8_t kImaIndexTable[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

inline constexpr std::int16_t kImaStepTable[89] = {
        7,     8,     9,    10,    11,    12,    13,    14,    16,    17,
       19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
       50,    55,    60,    66,    73,    80,    88,    97,   107,   118,
      130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
      337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
      876,   963,  1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
     2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
     5894,  6484,  7132,  7845,  8630,  9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

inline bool isRsnd(const RsndHeader &h)
{
    return h.magic[0] == 'R' && h.magic[1] == 'S' && h.magic[2] == 'N'
        && h.magic[3] == 'D' && h.version == kSoundVersion;
}

} // namespace rivendata
