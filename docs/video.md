# RVID

The video format the port uses. The container and the coding tools are
[`shared/RivenVideo.hpp`](../shared/RivenVideo.hpp), compiled by both the
converter and the ARM9; this file is why they look the way they do.

Nothing here is guessed. Every number comes from
`riven-convert --movie-report <riven-source>`, which walks the QuickTime headers
of every tMOV in an install, or from converting one. Run them against your own
copy before trusting the figures; these are from a French 5-CD install.

**Status:** the converter half is built — demuxer, both source codecs, the
encoder, and a reference decoder the tests check it with. The ARM9 player is
not.

## What Riven actually ships

```
1055 movies, 1.93 GB, 10832 seconds of video, 15 fps throughout

video codecs:
  cvid    1038 movies     Cinepak
  rle       17 movies     QuickTime RLE, all 24-bit
audio tracks:
  ima4 22050 Hz 2ch      793 movies
  ima4 22050 Hz 1ch        2 movies
  (260 movies have no audio track)

profiles:
  FULL   181 movies    65452 frames     4371s
  LITE   874 movies    96987 frames     6461s   widest 608, tallest 392
  398 distinct frame sizes
```

Three things in that are decisions rather than choices:

* **Two video decoders, not one.** Cinepak carries the game; 17 movies are
  QuickTime RLE and cannot be skipped, because a missing movie is a puzzle that
  cannot be solved. Both decode at conversion time on a desktop, so only
  correctness matters.
* **Movie audio is IMA4 at the same 22050 Hz as the game's sounds.** No QDM2, no
  second codec family. IMA4 is the block-framed cousin of the continuous ADPCM
  the sound stage already validated, so RVID's audio track is that encoder's
  output with a different wrapper.
* **The FULL/LITE split is real, not a heuristic.** 181 movies are exactly
  608x392 and 874 are smaller, with nothing ambiguous between.

## The container

[`shared/RivenVideo.hpp`](../shared/RivenVideo.hpp) has the structs: a 32-byte
header, a keyframe index, then frames back to back, each an 8-byte header, its
audio block, and its video payload.

**Why a keyframe index.** Riven restarts movies constantly — a script plays a
lever animation from frame 0 every time the lever is touched. Eight bytes per
I-frame is nothing against decoding from the start of the file every time, and
it is also what makes a dropped frame recoverable.

**Why audio comes first inside a frame.** The ARM7 needs the samples before the
ARM9 needs the picture, so one sequential read serves both in the order they are
consumed.

**Why `largestFrameBytes` is in the header.** The player allocates one frame
buffer at open time and never resizes; growing one mid-movie on a DS means a
heap fragment that never comes back.

**Movies whose audio outlasts their video** get extra frames that repeat the
last picture, so the whole soundtrack fits in the container. Twelve of the 795
need it — ospit's tMOV 0 carries 260 seconds of dialogue over 82 seconds of
video — and the extra frames cost a few bytes each because nothing changes in
them.

## Coding

Per plane (G, then R, then B — R and B predict from the reconstructed G, which
is what makes a plane-per-block codec competitive with a luma/chroma one):

* An 8x8 block is four 4x4 quadrants; each quadrant is four **2x2 groups that
  sample every other pixel**. Polyphase, not four adjacent squares.
* Each group is predicted — from its neighbours inside the same block for an
  intra block, from the motion-compensated picture for an inter one — and the
  residual goes through a **2x2 Hadamard**: four adds forward, and
  `clamp(pred + ((coef + 2) >> 2))` back.
* Quantisation is four fixed steps, `{4, 8, 8, 20}`. The DC step of 4 is exactly
  one output level, so a flat block is carried without error; that is what keeps
  Riven's letterbox black and its walls flat, and `--video-quality` deliberately
  does **not** scale it. The three AC steps are coarse on purpose: halving them
  triples the bitrate to move the mean error from 0.15 to 0.10 levels out of 31.
* The 16 groups x 4 coefficients are flattened **grouped by coefficient** — all
  sixteen DCs, then all sixteen vertical terms, and so on — which clusters the
  energy at the front and leaves one long zero run at the back.

Every intra prediction stays inside its own 8x8 block. That is not incidental:
it is what lets the encoder try a block as intra and as inter using 192 bytes of
scratch instead of two copies of the frame.

### Entropy coding: not MPEG-4

The README used to say "MPEG-4 style run/level entropy coding", following
FastVideoDS. It does not.

FastVideoDS's coder is driven by the MPEG-4 TCOEF VLC tables, and
[`licensing.md`](licensing.md) forbids copying them — "not the ARM assembly
decoder, not the C# encoder, **not the VLC tables**". Lifting them from FFmpeg
instead would be copying either way, from an LGPL source.

The escape hatch is that **we own both ends**: RVID is read by exactly one
decoder, which we also write, so MPEG-4 compatibility buys nothing. Coefficients
are coded as `(last, run, level)` triples with **Exp-Golomb** — one bit for
last, an unsigned code for the run, a signed one for the level. No table to
copy, none to ship, and a decoder that is a few shifts per symbol. An all-zero
block is the single symbol `(last=1, run=0, level=0)`.

## The two profiles

**FULL** — 256x165, the card view scaled by `kScaleNum/kScaleDen`. One motion
vector per 8x8 block, half-pel, coded as a delta from the median of the left,
above and above-right neighbours. Half-pel uses **the DS's blend, not an
average**: the GPU expands a 5-bit channel as `c ? 2c+1 : 0` and mixes at 16/64,
so `blendHalf` reproduces that exactly — otherwise the encoder would predict
from a picture the hardware will not draw. Motion vectors are constrained so
every sample the prediction touches is inside the frame, rather than relying on
edge clamping, which would tie the encoder to a particular texture layout.

**LITE** — the 874 overlays, same scale, so the widest is 256 and most are well
under 128. Zero-motion-vector P-frames, no vector sub-stream at all: Riven's
overlays are locked-off shots, so the static background of a block codes as an
all-zero residual anyway, and display-capture motion compensation cannot
composite a small movie onto a card the 3D engine is already drawing.

The rule is `width >= kCardW && height >= kCardH`, applied by the converter and
recorded in the header so the player never infers it.

## Rate

Measured over a fullscreen cutscene (ospit tMOV 0, 256x165, 150 frames):

| `--video-quality` | bytes/frame | at 15 fps | mean error |
|---|---|---|---|
| 150 | 2168 | 32 KB/s | 0.12 / 31 |
| **100** (default) | **1768** | **26 KB/s** | **0.15 / 31** |
| 60 | 1570 | 23 KB/s | 0.18 / 31 |

Audio adds a flat 735 bytes a frame — 22050 Hz mono ADPCM is 11 KB/s whatever
the picture does, and across 10832 seconds of movie that is a floor no codec
setting can move.

Converting the whole game at the default quality, measured:

```
1055 files, 189928 frames, 4977 keyframes (one per 38 frames)
video 354.8 MB, audio 108.3 MB, headers and indexes 1.5 MB   total 464.6 MB
FULL   181 movies   85211 frames  272.7 MB  (3356 B/frame)
LITE   874 movies  104717 frames  191.9 MB  (1921 B/frame)
largest single frame 41569 bytes
```

That is 1.93 GB of QuickTime in and 465 MB out, in ten and a half minutes on
eight cores. The frame count is higher than the 162439 the source contains
because of the audio-tail frames above.

The largest frame matters for the player: it is what `largestFrameBytes` lets it
size its one buffer from, and at 41 KB it fits comfortably in the 64 KB the
reference player allocates.

## Milestone 8's other half

The ARM9/ARM7 player. The container was defined now rather than later precisely
so that work has something fixed to build against. What it has to do:

1. Read a frame, hand the audio block to the ARM7 and the video payload to the
   decoder.
2. LITE: decode into RAM and composite onto the card.
3. FULL: motion compensation on the 3D engine — one triangle per 8x8 block, the
   texture coordinate *is* the motion vector, half-pel comes free from the 50%
   alpha blend — with the result taken by the display-capture unit.
   [`source/Global.cpp:52-58`](../source/Global.cpp) already reserves VRAM banks
   D and E for the residual scratch, and takes them only while a fullscreen
   movie is playing.

FastVideoDS's decoder is 1960 lines of ARM assembly and is unlicensed, so this
is a reimplementation from the design, cited the same way ScummVM is.

## Licensing, again

Repeated here because this is the file someone will read while writing the
player:

* **No FastVideoDS code may be copied** — not the ARM assembly, not the C#
  encoder, not the VLC tables. Neither repository ships a licence. The format
  and the technique are not copyrightable; the expression is.
  [Ask Gericom for a grant](licensing.md) — it would save a great deal of this.
* **No ScummVM code may be copied.** Its Cinepak, QuickTime and IMA4
  implementations are the specification to read and cite by file and line, and
  then to write yourself. The converter's decoders were written that way and
  carry those citations.
* **No new dependency.** The converter's CI has no codec libraries at all and
  builds on GCC 11 and MSVC. Everything above is self-contained C++20.
