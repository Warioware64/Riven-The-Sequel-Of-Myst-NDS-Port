#pragma once

// Sound on the DS: the movie soundtrack, and the game's own sounds.
//
// The two go different ways round, and the reason is in shared/RivenSound.hpp.
// The DS decodes IMA ADPCM IN HARDWARE, and .rsnd is IMA ADPCM whose payload
// already begins with the 4-byte state word the hardware wants -- so a converted
// Riven sound can be read into RAM and handed to a channel with no conversion at
// all. That is the whole point of the format, and it is what lets ambient tracks
// and effects sound while a movie's soundtrack is also playing.
//
// Movie audio cannot go that way. An RVID audio block arrives one video frame at
// a time and a hardware channel cannot be fed incrementally -- the DS reloads an
// ADPCM channel's state from its start on loop, which is exactly why streaming
// ADPCM on hardware is a dead end. So movie audio is software-decoded to PCM16
// and pushed into a maxmod stream, the pattern the Myst port uses for its music
// (helpSrc/myst-nds-port/source/AudioStream.cpp), with the file read replaced by
// a push from the video pump. maxmod allows one stream, and the movie is what
// wants it.
//
// The stream's sample counter is the master clock for video presentation, so it
// counts only samples that were really delivered: an underrun stalls the clock,
// the video pump stops presenting and keeps decoding, the ring refills and the
// clock resumes. Counting silence instead would let the picture run away from
// the soundtrack and never come back.

#include <cstddef>
#include <cstdint>
#include <string>

namespace RivenAudio
{

/// Bring up maxmod (streaming only, no soundbank) and reserve the hardware
/// channels that play .rsnd. Idempotent; call after NEA_Init3D().
void initSystem();

// ---------------------------------------------------------------------------
// The movie stream
// ---------------------------------------------------------------------------

/// Start an empty PCM16 mono stream at `sampleRate`. Whatever was streaming is
/// stopped. The stream stays silent until pushIma() has given it something.
bool streamOpen(int sampleRate);

/// Decode one RVID frame's audio block and queue it.
///
/// `block` is kAdpcmStateBytes of initial state followed by 4-bit nibbles, low
/// nibble first -- so each block decodes without the one before it, which is why
/// a movie can be restarted at any keyframe.
///
/// False if the ring is full, meaning the caller has decoded further ahead than
/// the buffer holds; the caller should present a frame and try again rather than
/// drop the audio.
bool streamPushIma(const std::uint8_t *block, std::size_t bytes);

/// Samples really delivered to the hardware since streamOpen(). The video
/// pump's clock.
std::uint32_t streamSamplesPlayed();

/// Samples queued and not yet delivered. Zero means the next callback will be
/// silence.
std::uint32_t streamQueued();

/// How many samples the ring can still take.
std::uint32_t streamFree();

/// The movie soundtrack's own gain, 0..256 (256 = unity), applied as the
/// samples are handed over. The player's master volume is applied on top and is
/// not this: a caller sets what the movie asks for and forgets about it.
void streamSetVolume(int volume);

/// Gain arithmetic. 256 is unity; the master gain runs to kGainMax, which is
/// twice that, because the port needs to be able to make a quiet cutscene
/// LOUDER and not only quieter.
inline constexpr int kGainUnity = 256;
inline constexpr int kGainMax = 2 * kGainUnity;

/// The player's master gain as a 0..255 setting byte, spread across the whole
/// 0..kGainMax range -- so 127 is roughly unity and 255 is twice it. Applied to
/// EVERYTHING: the ambient layers, the one-shot effects and the movie
/// soundtrack.
///
/// ABOVE UNITY IT REACHES THE MOVIE SOUNDTRACK ONLY, and that asymmetry is the
/// hardware's, not a choice. Ambient and effects are played by DS sound
/// channels straight out of their own buffers, and a channel's volume register
/// stops at 127 with a divider that can only divide -- so the sample data's own
/// amplitude is a hard ceiling for them. The soundtrack is mixed in software by
/// streamCallback, which is the one place there is room to multiply.
///
/// Applied here rather than at the call sites so that no future caller can
/// forget it, and applied to the sounds already playing rather than only to the
/// next one, so that changing it from the in-game options screen is heard
/// immediately instead of at the next card.
void setMasterVolume(int volume);

void streamClose();
bool streamIsOpen();

// ---------------------------------------------------------------------------
// .rsnd on hardware channels
// ---------------------------------------------------------------------------

/// Number of sounds that can be audible at once. Riven's SLST lists a handful of
/// ambient layers per card and opcode 4 plays one effect over them.
///
/// Five and not six because a slot costs TWO hardware channels: the DS pan law is
/// linear, so a centred channel puts half its amplitude in each speaker, and the
/// only way to get the other half back is to play the sample twice, hard left and
/// hard right (see playSound). Ten channels is what is left after maxmod's stream,
/// which is nailed to channels 4 and 5.
inline constexpr int kSoundSlots = 5;

/// Play `path` (a .rsnd) on a hardware channel.
///
/// `volume` is 0..255 and `balance` is the SLST field: negative left, 0 centre,
/// positive right. `loop` repeats the whole sound.
///
/// Returns a slot index, or -1 when the file is missing, is not a readable
/// .rsnd, is larger than the RAM budget, or no slot is free. A sound that does
/// not fit is reported rather than truncated -- a half-played ambient is worse
/// than a silent one, and the cap is what keeps a long track from taking the
/// heap the card art needs.
int playSound(const std::string &path, int volume = 255, int balance = 0,
              bool loop = false);

/// True while the slot's sound is still sounding. A looping sound always
/// reports true: nothing else would ever stop it.
bool soundPlaying(int slot);

void stopSound(int slot);
void stopAllSounds();

/// Adjust a playing sound without restarting it -- what a re-activated SLST
/// wants when it lists a track that is already sounding.
void setSoundVolume(int slot, int volume, int balance);

/// Age the one-shot sounds so soundPlaying() can go false. The DS gives no
/// per-channel "finished" flag, so a sound's remaining length is counted down
/// here. Call once per frame from the run loop.
///
/// The movie stream needs nothing from this: streamPushIma() writes straight into
/// the ring the interrupt drains.
void pump();

/// Largest single .rsnd payload that will be loaded.
///
/// A hardware channel plays a whole resident buffer, so a sound costs its full
/// converted size for as long as it sounds. This is sized for the longest thing
/// the converter produces: Riven's longest ambient is 157 seconds, which is 1.7 MB
/// of mono ADPCM -- and the converter keeps anything that long AS ADPCM for exactly
/// this reason (shared/RivenSound.hpp, kPcm16Budget). Everything shorter is
/// lossless PCM16 and well under 512 KB.
inline constexpr std::size_t kMaxSoundBytes = 2u * 1024 * 1024;

/// Total across every sound resident at once.
///
/// The per-sound cap alone is not a budget: an SLST can name six ambient layers,
/// and six times the largest would be twelve megabytes of a four-megabyte machine.
/// This is the number that actually protects the heap the card art and the movie
/// buffers come out of; a sound that would cross it is refused and says so, which
/// is a missing layer rather than a failed allocation somewhere else later.
inline constexpr std::size_t kSoundBudgetBytes = 3u * 1024 * 1024;

/// How much of that budget is currently held, for the status line and for tests.
std::size_t soundBytesResident();

} // namespace RivenAudio
