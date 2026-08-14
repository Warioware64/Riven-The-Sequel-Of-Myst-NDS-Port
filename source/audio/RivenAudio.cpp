#include "RivenAudio.hpp"

#include "global_header.hpp" // NEASound.h + <nds.h> + maxmod

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "RivenSound.hpp"

using namespace rivendata;

namespace
{
    bool g_inited = false;

    // -----------------------------------------------------------------------
    // The movie stream
    // -----------------------------------------------------------------------

    /// PCM16 mono samples. 32768 samples is 1.4 s at 22050 Hz -- generous, but
    /// this is the slack that absorbs a slow SD read or a frame that took longer
    /// to decode than it lasts, and 64 KB against a 4 MB machine is cheap
    /// insurance against a soundtrack with holes in it.
    constexpr std::uint32_t kRingSamples = 32768;

    std::int16_t g_ring[kRingSamples];
    /// Written by the pushing side only, read by both.
    volatile std::uint32_t g_ringIn = 0;
    /// Written by the interrupt only, read by both. Aligned 32-bit access is
    /// atomic on ARM, and there is exactly one producer and one consumer, so
    /// neither side needs to lock the other out.
    volatile std::uint32_t g_ringOut = 0;

    volatile std::uint32_t g_samplesPlayed = 0;
    /// What the movie asked for, and what the interrupt actually applies. Two
    /// values rather than one because the master gain multiplies this and
    /// either can change while a movie is playing: folding them together would
    /// make each new master volume compound with the last.
    int g_streamRequested = 256;
    int g_streamVolume = 256;
    bool g_streamOpen = false;
    int g_streamRate = 0;

    /// The player's master gain as a multiplier, 0..kGainMax with kGainUnity as
    /// 1.0. Settings::apply owns it, through the 0..255 setting byte.
    int g_masterGain = RivenAudio::kGainUnity;

    /// Decoder state carried across a movie's frames. Each RVID audio block
    /// begins with its own state word, so this is reseeded per block rather than
    /// relied on -- but keeping it here costs nothing and documents that the
    /// blocks are a chain in the source even though they are independent here.
    struct ImaState
    {
        std::int16_t predictor = 0;
        std::uint8_t index = 0;
    };

    /// One IMA ADPCM nibble. The tables are the shared ones so the converter's
    /// encoder and this cannot drift apart (RivenSound.hpp).
    inline std::int16_t imaStep(ImaState &s, int nibble)
    {
        const int step = kImaStepTable[s.index];
        int diff = step >> 3;
        if (nibble & 1)
            diff += step >> 2;
        if (nibble & 2)
            diff += step >> 1;
        if (nibble & 4)
            diff += step;
        if (nibble & 8)
            diff = -diff;

        int predictor = s.predictor + diff;
        if (predictor > 32767)
            predictor = 32767;
        else if (predictor < -32768)
            predictor = -32768;
        s.predictor = static_cast<std::int16_t>(predictor);

        int index = s.index + kImaIndexTable[nibble];
        if (index < 0)
            index = 0;
        else if (index > 88)
            index = 88;
        s.index = static_cast<std::uint8_t>(index);

        return s.predictor;
    }

    mm_word streamCallback(mm_word length, mm_addr dest, mm_stream_formats fmt)
    {
        (void)fmt; // always MM_STREAM_16BIT_STEREO; streamOpen is the only opener
        auto *out = static_cast<std::int16_t *>(dest);

        const std::uint32_t in = g_ringIn;
        std::uint32_t out_ = g_ringOut;
        std::uint32_t avail = in - out_; // unsigned wrap is the ring's arithmetic
        if (avail > kRingSamples)
            avail = kRingSamples;

        // The soundtrack is mono and stays mono -- the ring, the decode and the
        // clock all count one sample per frame. What is stereo is the OUTPUT, and
        // only so that maxmod spends two hardware channels on it: it pans them hard
        // left and hard right, where one centred channel would have put half the
        // amplitude in each speaker. Same samples, twice the level. maxmod wants
        // them interleaved and splits them into its two channel buffers itself.
        std::uint32_t give = length < avail ? length : avail;
        for (std::uint32_t i = 0; i < give; ++i)
        {
            std::int32_t s = g_ring[(out_ + i) % kRingSamples];
            if (g_streamVolume != RivenAudio::kGainUnity)
            {
                // The one place in the port with room to make something LOUDER:
                // these samples are mixed in software, where a hardware channel
                // can only be turned down. Above unity it has to saturate --
                // Riven's own soundtracks are mastered close to full scale, so
                // wrapping would be heard as a tear rather than as clipping.
                s = (s * g_streamVolume) >> 8;
                if (s > 32767)
                    s = 32767;
                else if (s < -32768)
                    s = -32768;
            }
            out[i * 2] = static_cast<std::int16_t>(s);
            out[i * 2 + 1] = static_cast<std::int16_t>(s);
        }
        for (std::uint32_t i = give; i < length; ++i)
        {
            out[i * 2] = 0; // an underrun is heard as a gap, not as garbage
            out[i * 2 + 1] = 0;
        }

        g_ringOut = out_ + give;
        // Only real samples advance the clock. Counting the silence too would let
        // the picture walk away from the soundtrack during a stall and never come
        // back; this way presentation waits and the ring catches up.
        g_samplesPlayed += give;
        return length;
    }

    // -----------------------------------------------------------------------
    // .rsnd on hardware channels
    // -----------------------------------------------------------------------

    /// Hardware channels per slot: one hard left, one hard right, both playing the
    /// same buffer. See panScaleFor.
    constexpr int kChannelsPerSlot = 2;

    /// The top channels are reserved from maxmod so libnds can drive them
    /// directly. Without mmLockChannels a soundPlaySampleChannel on a
    /// maxmod-owned channel is simply silent.
    ///
    /// This has to land at 6 or above: maxmod's DS stream does not allocate its
    /// channels, it hardcodes 4 and 5 (maxmod ds/common/stream.c, CHANNEL_LEFT and
    /// CHANNEL_RIGHT), so locking either of them away would leave the movie
    /// soundtrack with nothing to play on.
    constexpr int kFirstSoundChannel = 16 - RivenAudio::kSoundSlots * kChannelsPerSlot;
    static_assert(kFirstSoundChannel >= 6, "the movie stream owns channels 4 and 5");

    /// Bytes held across every slot, against RivenAudio::kSoundBudgetBytes.
    std::size_t g_resident = 0;

    struct SoundSlot
    {
        int channelL = -1;
        int channelR = -1;
        void *data = nullptr;
        std::size_t bytes = 0;
        /// Frames of run loop left before the sound has finished. The DS has no
        /// per-channel "still playing" flag, so this is counted down in pump()
        /// from the sound's own length.
        unsigned framesLeft = 0;
        /// The volume and balance the GAME asked for, before the master gain.
        /// Kept so setMasterVolume can re-derive the channel volumes of a
        /// sound that is already playing; without them, moving the slider
        /// during a game would only be heard at the next card.
        int volume = 255;
        int balance = 0;
        bool looping = false;
        bool active = false;
    };
    SoundSlot g_sounds[RivenAudio::kSoundSlots];

    int clampVolume(int v) { return v < 0 ? 0 : (v > 127 ? 127 : v); }

    /// SLST balance (negative left, positive right) as a pair of 0..127 scales for
    /// the slot's two hard-panned channels.
    ///
    /// The obvious thing -- one channel at libnds panning 64 -- is what made the
    /// whole game quiet. The DS mixes linearly: a channel contributes
    /// `sample * volume * (128 - pan) / 128` to the left and
    /// `sample * volume * pan / 128` to the right, so at pan 64 each speaker gets
    /// HALF of it. Nothing downstream can put that back; the master volume is
    /// already 127 and the sample is already at its own peak. Two channels, one at
    /// pan 0 and one at pan 127, deliver the whole sample to each speaker, which is
    /// the missing 6 dB.
    ///
    /// So this is a balance law and not a pan law: centre is BOTH sides full, and
    /// moving off centre attenuates only the side being moved away from, exactly
    /// the way a stereo balance knob works. A pan law would divide the very
    /// amplitude the second channel exists to recover.
    void panScaleFor(int balance, int &leftScale, int &rightScale)
    {
        if (balance < -127)
            balance = -127;
        else if (balance > 127)
            balance = 127;
        leftScale = balance > 0 ? 127 - balance : 127;
        rightScale = balance < 0 ? 127 + balance : 127;
    }

    /// The slot's two channel volumes for an SLST volume (0..255) and balance.
    ///
    /// The master gain is applied HERE, which is the one place both playSound
    /// and setSoundVolume pass through. Doing it at the call sites instead
    /// would work today and quietly stop working the first time a fourth
    /// caller is added.
    void slotVolumes(int volume, int balance, int &left, int &right)
    {
        // Clamped at unity, because this ends in a hardware channel's volume
        // register: 127 with a divider that only divides, so the sample's own
        // amplitude is the ceiling. The top half of the master range is a boost
        // the movie soundtrack can take and these cannot (RivenAudio.hpp).
        const int gain = g_masterGain > RivenAudio::kGainUnity ? RivenAudio::kGainUnity
                                                              : g_masterGain;
        const int base = clampVolume(volume * gain / RivenAudio::kGainUnity * 127 / 255);
        int leftScale = 127;
        int rightScale = 127;
        panScaleFor(balance, leftScale, rightScale);
        left = base * leftScale / 127;
        right = base * rightScale / 127;
    }
} // namespace

void RivenAudio::initSystem()
{
    if (g_inited)
        return;

    // Streaming only: no soundbank, no maxmod effects. The game's own sounds go
    // to libnds hardware channels because they are already in the format the
    // hardware decodes.
    mm_ds_system sys;
    sys.mod_count = 0;
    sys.samp_count = 0;
    sys.mem_bank = nullptr;
    sys.fifo_channel = FIFO_MAXMOD;
    mmInit(&sys);

    mmLockChannels(((1u << (kSoundSlots * kChannelsPerSlot)) - 1u) << kFirstSoundChannel);
    for (int i = 0; i < kSoundSlots; ++i)
    {
        g_sounds[i].channelL = kFirstSoundChannel + i * kChannelsPerSlot;
        g_sounds[i].channelR = g_sounds[i].channelL + 1;
    }

    NEA_SoundSystemResetPool(1);
    soundEnable();

    g_inited = true;
}

// ---------------------------------------------------------------------------
// The movie stream
// ---------------------------------------------------------------------------

bool RivenAudio::streamOpen(int sampleRate)
{
    if (!g_inited || sampleRate < 1024 || sampleRate > 32768)
        return false;

    streamClose();

    g_ringIn = 0;
    g_ringOut = 0;
    g_samplesPlayed = 0;
    // A fresh stream asks for unity and the master is folded straight back in.
    // Assigning g_streamVolume directly here would drop the player's setting for
    // as long as it took someone to call streamSetVolume -- which today is the
    // very next line in RvidPlayer::play, and tomorrow is whatever a second
    // caller remembers to do.
    g_streamRequested = RivenAudio::kGainUnity;
    g_streamVolume = g_streamRequested * g_masterGain / RivenAudio::kGainUnity;
    g_streamRate = sampleRate;
    std::memset(g_ring, 0, sizeof(g_ring));

    // 2048-sample callback chunks: ~93 ms at 22050 Hz, which is long enough that
    // the interrupt is cheap and short enough that a volume change is heard
    // straight away.
    // STEREO for a mono soundtrack on purpose: it buys the two hard-panned hardware
    // channels that a centred mono stream throws half the amplitude away for. The
    // callback writes each sample twice; nothing upstream of it changes.
    NEA_StreamOpen(static_cast<mm_word>(sampleRate), 2048, streamCallback,
                   MM_STREAM_16BIT_STEREO, MM_TIMER0);
    g_streamOpen = true;
    return true;
}

void RivenAudio::streamClose()
{
    if (!g_streamOpen)
        return;
    NEA_StreamClose();
    g_streamOpen = false;
    g_ringIn = 0;
    g_ringOut = 0;
}

bool RivenAudio::streamIsOpen() { return g_streamOpen; }

std::uint32_t RivenAudio::streamSamplesPlayed() { return g_samplesPlayed; }

std::uint32_t RivenAudio::streamQueued()
{
    const std::uint32_t used = g_ringIn - g_ringOut;
    return used > kRingSamples ? kRingSamples : used;
}

std::uint32_t RivenAudio::streamFree() { return kRingSamples - streamQueued(); }

void RivenAudio::streamSetVolume(int volume)
{
    g_streamRequested = volume < 0 ? 0 : (volume > kGainUnity ? kGainUnity : volume);
    g_streamVolume = g_streamRequested * g_masterGain / kGainUnity;
}

void RivenAudio::setMasterVolume(int volume)
{
    // The setting byte spans the WHOLE gain range, so 127 is about unity and 255
    // is twice it (RivenAudio.hpp). The stream takes all of it; slotVolumes
    // clamps its own copy back to unity because a hardware channel cannot go
    // above the sample it is playing.
    const int v = volume < 0 ? 0 : (volume > 255 ? 255 : volume);
    g_masterGain = v * kGainMax / 255;
    g_streamVolume = g_streamRequested * g_masterGain / kGainUnity;

    // Everything already sounding, so the slider is heard while it moves rather
    // than at the next card. setSoundVolume goes back through slotVolumes, which
    // is where the master gain is applied.
    for (int i = 0; i < kSoundSlots; ++i)
        if (g_sounds[i].active)
            setSoundVolume(i, g_sounds[i].volume, g_sounds[i].balance);
}

bool RivenAudio::streamPushIma(const std::uint8_t *block, std::size_t bytes)
{
    if (!g_streamOpen || block == nullptr
        || bytes <= static_cast<std::size_t>(kAdpcmStateBytes))
        return true; // nothing to do; a silent frame is not a failure

    const std::size_t nibbleBytes = bytes - kAdpcmStateBytes;
    const std::uint32_t samples = static_cast<std::uint32_t>(nibbleBytes) * 2;
    if (samples > streamFree())
        return false; // decoded further ahead than the ring holds

    // The block's own initial state, exactly as makeAdpcmState() packed it: the
    // predictor in bits 0-15 and the step index in bits 16-22. Reading it per
    // block is what makes a movie restartable at any keyframe.
    std::uint32_t word = 0;
    std::memcpy(&word, block, sizeof(word));
    ImaState state;
    state.predictor = static_cast<std::int16_t>(word & 0xFFFF);
    state.index = static_cast<std::uint8_t>((word >> 16) & 0x7F);
    if (state.index > 88)
        state.index = 88;

    const std::uint8_t *nibbles = block + kAdpcmStateBytes;
    std::uint32_t in = g_ringIn;
    for (std::size_t i = 0; i < nibbleBytes; ++i)
    {
        // Low nibble first, as the converter writes them.
        g_ring[in % kRingSamples] = imaStep(state, nibbles[i] & 0x0F);
        ++in;
        g_ring[in % kRingSamples] = imaStep(state, (nibbles[i] >> 4) & 0x0F);
        ++in;
    }
    g_ringIn = in;
    return true;
}

// ---------------------------------------------------------------------------
// .rsnd
// ---------------------------------------------------------------------------

int RivenAudio::playSound(const std::string &path, int volume, int balance, bool loop)
{
    if (!g_inited)
        return -1;

    int slot = -1;
    for (int i = 0; i < kSoundSlots; ++i)
        if (!g_sounds[i].active)
        {
            slot = i;
            break;
        }
    if (slot < 0)
        return -1;

    std::FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr)
        return -1;

    RsndHeader h{};
    if (std::fread(&h, 1, sizeof(h), f) != sizeof(h) || !isRsnd(h) || h.channels != 1
        || h.dataBytes == 0)
    {
        std::fclose(f);
        std::printf("sound: %s is not a playable .rsnd\n", path.c_str());
        return -1;
    }

    const auto codec = static_cast<SoundCodec>(h.codec);
    if (codec != SoundCodec::ImaAdpcm && codec != SoundCodec::Pcm16)
    {
        std::fclose(f);
        std::printf("sound: %s uses a codec this build does not play\n", path.c_str());
        return -1;
    }
    if (codec == SoundCodec::Pcm16 && h.dataBytes != h.sampleCount * 2)
    {
        std::fclose(f);
        std::printf("sound: %s is PCM16 but not two bytes a sample\n", path.c_str());
        return -1;
    }

    if (h.dataBytes > kMaxSoundBytes || g_resident + h.dataBytes > kSoundBudgetBytes)
    {
        // Said out loud rather than skipped quietly: this is a real track that will
        // not be heard, and the log line is the only way to tell that from a card
        // that simply has no sound there. A hardware channel plays a whole resident
        // buffer, so the alternative to a budget is a failed allocation somewhere
        // else, later, with no clue attached.
        std::fclose(f);
        std::printf("sound: %s is %lu KB and %lu KB of the %lu KB budget is in use: silent\n",
                    path.c_str(), static_cast<unsigned long>(h.dataBytes / 1024),
                    static_cast<unsigned long>(g_resident / 1024),
                    static_cast<unsigned long>(kSoundBudgetBytes / 1024));
        return -1;
    }

    // Past the seek table. ADPCM carries one so a caller could start mid-sound,
    // which a channel playing the whole payload has no use for; PCM16 carries none
    // at all.
    if (std::fseek(f, static_cast<long>(h.seekEntries) * sizeof(RsndSeekPoint), SEEK_CUR) != 0)
    {
        std::fclose(f);
        return -1;
    }

    void *data = std::malloc(h.dataBytes);
    if (data == nullptr)
    {
        std::fclose(f);
        return -1;
    }
    const std::size_t got = std::fread(data, 1, h.dataBytes, f);
    std::fclose(f);
    if (got != h.dataBytes)
    {
        std::free(data);
        return -1;
    }

    // The ARM7 reads this buffer directly, so it has to be out of the ARM9's
    // cache before the channel starts.
    DC_FlushRange(data, h.dataBytes);

    SoundSlot &s = g_sounds[slot];
    s.data = data;
    s.bytes = h.dataBytes;
    s.looping = loop;
    s.active = true;
    g_resident += h.dataBytes;
    // 60 run-loop frames a second, and sampleCount frames of audio.
    s.framesLeft = h.sampleRate > 0
                       ? static_cast<unsigned>(
                             (static_cast<std::uint64_t>(h.sampleCount) * 60) / h.sampleRate + 2)
                       : 0;

    // The one place the two codecs differ on the device: the hardware decodes both,
    // and which one it is doing is a single argument.
    const bool adpcm = codec != SoundCodec::Pcm16;
    const SoundFormat format = adpcm ? SoundFormat_ADPCM : SoundFormat_16Bit;
    // ADPCM keeps its decoder state in the first word of the payload, which is not
    // sample data. Looping back to word 0 would feed that word to the decoder as if
    // it were nibbles -- a click at the top of every repeat, on exactly the long
    // ambient loops that repeat most.
    const int loopPoint = adpcm ? 1 : 0;

    s.volume = volume;
    s.balance = balance;

    int volL = 0;
    int volR = 0;
    slotVolumes(volume, balance, volL, volR);

    // Twice, hard left and hard right, off one buffer: see panScaleFor. Both
    // channels read the same bytes, so this costs a channel and no RAM. The few
    // cycles between the two writes are far under a sample, and the two go to
    // different speakers anyway.
    soundPlaySampleChannel(s.channelL, data, format, h.dataBytes, h.sampleRate,
                           static_cast<u8>(volL), 0, loop, loopPoint);
    soundPlaySampleChannel(s.channelR, data, format, h.dataBytes, h.sampleRate,
                           static_cast<u8>(volR), 127, loop, loopPoint);
    return slot;
}

bool RivenAudio::soundPlaying(int slot)
{
    if (slot < 0 || slot >= kSoundSlots || !g_sounds[slot].active)
        return false;
    return g_sounds[slot].looping || g_sounds[slot].framesLeft != 0;
}

void RivenAudio::stopSound(int slot)
{
    if (slot < 0 || slot >= kSoundSlots)
        return;
    SoundSlot &s = g_sounds[slot];
    if (!s.active)
        return;
    soundKill(s.channelL);
    soundKill(s.channelR);
    // Freed only after the channels are dead: the ARM7 is reading these bytes.
    std::free(s.data);
    s.data = nullptr;
    g_resident -= s.bytes < g_resident ? s.bytes : g_resident;
    s.bytes = 0;
    s.active = false;
    s.looping = false;
    s.framesLeft = 0;
}

void RivenAudio::stopAllSounds()
{
    for (int i = 0; i < kSoundSlots; ++i)
        stopSound(i);
}

void RivenAudio::setSoundVolume(int slot, int volume, int balance)
{
    if (slot < 0 || slot >= kSoundSlots || !g_sounds[slot].active)
        return;
    g_sounds[slot].volume = volume;
    g_sounds[slot].balance = balance;
    int volL = 0;
    int volR = 0;
    slotVolumes(volume, balance, volL, volR);
    // The panning is the slot's, not the sound's, and never moves: balance lives in
    // the two volumes instead. See panScaleFor.
    soundSetVolume(g_sounds[slot].channelL, static_cast<u8>(volL));
    soundSetVolume(g_sounds[slot].channelR, static_cast<u8>(volR));
}

std::size_t RivenAudio::soundBytesResident() { return g_resident; }

void RivenAudio::pump()
{
    for (int i = 0; i < kSoundSlots; ++i)
    {
        SoundSlot &s = g_sounds[i];
        if (!s.active || s.looping)
            continue;
        if (s.framesLeft != 0)
        {
            --s.framesLeft;
            continue;
        }
        // Finished on its own: reclaim the slot and the RAM so the next card's
        // sounds have somewhere to go.
        stopSound(i);
    }
}
