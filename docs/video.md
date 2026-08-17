# RVID

The video format the port uses. The container is
[`shared/RivenVideo.hpp`](../shared/RivenVideo.hpp), compiled by both the converter
and the ARM9; this file is why it looks the way it does.

Nothing here is guessed. Every number comes from
`riven-convert --movie-report <riven-source>`, which walks the QuickTime headers
of every tMOV in an install, or from converting one. Run them against your own
copy before trusting the figures; these are from a French 5-CD install.

**Frames are stored raw** -- each one is exactly the ARGB1555 texels the DS
samples, with an IMA ADPCM audio block interleaved ahead of it. There is no codec.

There was one, and it worked: a 2x2 Hadamard over RGB555 planes, Exp-Golomb
coefficients, I/P frames with half-pel motion compensation, an ARM9 software
decoder checked byte-for-byte against a reference decoder on the host. It fit the
game's 1.93 GB of QuickTime into 465 MB. It was removed, and
[why](#why-the-codec-went) is the most useful thing in this document.

**Status:** built, both halves.

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

[`shared/RivenVideo.hpp`](../shared/RivenVideo.hpp) has the structs: a 36-byte
header, a frame index, then frames back to back, each an 8-byte header, its audio
block, and its picture.

**Why a frame index.** Riven restarts movies constantly -- a script plays a lever
animation from frame 0 every time the lever is touched. It was a keyframe index
when there were keyframes; now every frame is independent, so there is one entry
per frame and seeking to any of them is a lookup rather than "the last I-frame at
or before, then decode forward". Eight bytes a frame is 37 KB on the longest movie
in the game.

It cannot just be a multiply. The picture is a fixed size per movie, but the audio
block ahead of it is not: the sample clock does not divide evenly into the frame
rate, so blocks alternate between two lengths.

**Why audio comes first inside a frame.** The ARM7 needs the samples before the
ARM9 needs the picture, so one sequential read serves both in the order they are
consumed.

**Why `largestFrameBytes` is in the header.** The player allocates one frame
buffer at open time and never resizes; growing one mid-movie on a DS means a heap
fragment that never comes back.

**Movies whose audio outlasts their video** get extra frames flagged
`kFrameRepeat`, which carry no picture at all. Twelve of the 795 need it -- ospit's
tMOV 0 has 260 seconds of dialogue over 82 seconds of video -- and with raw frames
the saving is no longer a few bytes but the whole 84 KB.

**No padding anywhere.** A frame is `width * height` texels. For a FULL movie the
width is already 256, which is also the texture width, so a frame read off the card
lands in the top of a 256x256 texture as one contiguous copy -- the file's rows and
the texture's rows are the same rows.

## The two profiles

Not two decoders any more. Two ways of reaching the screen.

**FULL** -- 256x165, the card view scaled by `kScaleNum/kScaleDen`. Its own pair of
256x256 materials, drawn as a quad.

**LITE** -- the 874 overlays, same scale, so the widest is 256 and most are well
under 128. Composited into the card's own picture, because nothing can put a 39x76
movie on top of a card the 3D engine is drawing.

The rule is `width >= kCardW && height >= kCardH`, applied by the converter and
recorded in the header so the player never infers it.

## The size of an overlay

Two scales stand between a tMOV's coded size and the size on the card, in this
order.

First the **track matrix**. QuickTime's track header carries a 3x3 display
matrix, and its `a` and `d` terms scale the track: 79 of the 1054 movies are
authored bigger than they are shown, 65 at 0.5 and 12 at 0.25, two of them
non-uniform. ScummVM applies it as `scaleFactorX/Y = Rational(0x10000, xMod)`.
Read the VIDEO track, not the first one -- jspit's tMOV 190 and 335 put a sound
track first, and a sound track's matrix is the identity.

Then the **span**. This is not the card-space size scaled; it is the DS columns
that size covers *at the position the movie is drawn*. A movie of card-width `W`
at MLST left `L` covers

```
[ toDsX(L), toDsX(L + W) )
```

which is `floor(W * 256/608)` wide **or one more**, depending on where `L` falls
between two DS columns. It is the same two-endpoint mapping a PLST rect gets in
`CardSurface::drawPicture` and a water run gets in `WaterEffect`, and for the
same reason: scaling a length loses the pixel that mapping both ends keeps.

The converter used to scale the length, because `L` looked like a property of
the MLST record rather than of the movie. It is not: across the eight stacks,
every one of the 1055 movies appears at exactly **one** `(left, top)`, so
`collectMoviePlacements` can hand the video stage the position before it sizes
anything. Under the old rule 583 of the 1055 were a pixel short in at least one
axis -- 259 in x, 156 in y, 168 in both -- and each of them was also resampled at
a slightly wrong ratio, so its content drifted against the still underneath,
worst at the right and bottom edge. tspit's lever (tMOV 19, 64x40 at (48,307))
came out 26x16 where its span is 27x17; card 18's porthole (tMOV 21, 432x256 at
(4,72)) 181x107 where its span is 182x108.

No FULL movie is affected: all 181 are 608x392 at (0,0), which is 256x165 either
way. No movie's span runs past the card edge, so nothing new is clipped.

Then the **grid**, which is the same argument a third time. The span fixed how many
DS pixels an overlay covers; it did not fix *where the samples are taken*.
`scale=w:h` resamples on the movie's own pixel grid -- period `srcW/dstW`, anchored
at the movie's pixel 0 -- while the still underneath went through the card's:
608 to 256 across, 392 to 165 down, anchored at card 0. The two coincide only when
the movie's position lands on a whole DS pixel *and* its own ratio happens to match
the card's, and across the install 716 of the 874 overlays miss by half a DS pixel
or more. 1053 of the 1055 have a resample ratio that is not the card's in at least
one axis.

jspit's gallows carriage is what made it visible: tMOV 116, 152x336 at (224,56),
comes out 0.32 px too far left, 0.58 px too high and stretched 0.37% vertically --
a seam along the top of the animation against the still it is drawn on.

So an overlay is now taken from ffmpeg at its **native** size and resampled here,
by the same box filter in the same linear light with the same Bayer threshold the
stills go through, sampled where the card samples:

```
sx0 = ((toDsX(L) + dx)     * 608 - 256*L) * srcW / (256 * W)
sx1 = ((toDsX(L) + dx + 1) * 608 - 256*L) * srcW / (256 * W)
```

clamped to `[0, srcW]` -- and the clamp is the edge replication the span's outward
rounding needs. **The two axes are not the same ratio**: across, 608 to 256 is
exactly 19/8; down, 392 to 165 is 2.37576, because 165 is where `392 * 256/608` was
rounded. A full-card still is resampled 608x392 to 256x165, so 392/165 is the
card's vertical grid whatever `toDsY` says about positions.

FULL movies keep ffmpeg's `scale=`: at 608x392 and (0,0) the two grids are the same
grid, the phase is zero, and piping 65 452 native frames to reproduce what ffmpeg's
area filter already gets right would cost hours and change no pixel. There is a
test that asserts the two agree exactly in that case, because if it ever stops
being true every fullscreen movie in the game has moved.

## Size

Raw is 7.62 GB of video, measured over every movie in the install:

```
FULL   181 movies   65452 frames   5.15 GB   largest single file 377 MB
LITE   874 movies   96987 frames   2.47 GB   largest single file 108 MB
                                   7.62 GB   + ~0.6 GB sound, ~0.4 GB card art
```

A fullscreen frame is 256 x 165 x 2 = 84480 bytes, so 15 fps is **1.21 MB/s** off
the card. That number is the whole of the port's video risk now, and there is
nothing else in it: no decode, no allocation, no per-frame work but a read and a
copy. The Myst port sustains ~800 KB/s of the same kind of read today, which makes
1.21 MB/s plausible and unproven. If a card cannot hold it, the fix is to convert
at a lower frame rate -- 10 fps is 806 KB/s, exactly what Myst already does -- and
that needs no decoder either.

The 377 MB single file is why the converter streams its output
([`AtomicFileWriter`](../tools/riven-convert/core/include/riven/AtomicWrite.hpp)):
assembling one in memory and handing it to `writeFileAtomic` would need it twice
over.

For scale, what the intro alone asks of the player -- tspit's four fullscreen
movies, back to back:

```
tMOV 61   155 frames  10 fps   256x165    12.5 MB
tMOV 60   520 frames  15 fps   256x165    41.9 MB
tMOV 59  2036 frames  15 fps   256x165   164.1 MB
tMOV 58  2070 frames  15 fps   256x165   166.8 MB
```

Five and a half minutes of fullscreen video, 385 MB of it, none of it decoded.

## The player

On the DS. Three files, and the shortest of them is the one that used to be the
whole story:

| | |
|---|---|
| [`source/rvid/RvidFile.cpp`](../source/rvid/RvidFile.cpp) | the container |
| [`source/rvid/RvidPlayer.cpp`](../source/rvid/RvidPlayer.cpp) | playback and presentation |
| [`source/audio/RivenAudio.cpp`](../source/audio/RivenAudio.cpp) | the soundtrack |

**FULL** gets two 256x256 A1RGB5 materials, one per texture VRAM bank, drawn
alternately. A frame is `fread` **straight into the back material's RAM buffer** --
the file's rows and the texture's rows are the same rows, so nothing is copied in
between -- and then the bank holding the material that is *not* on screen is briefly
remapped to LCD and written with the CPU, which is safe during active display
precisely because nothing samples it that frame. That is the Myst port's proven
pattern ([`VideoPlayer.cpp`](../helpSrc/myst-nds-port/source/VideoPlayer.cpp)).

Two of them is 256 KB -- the entire texture pool -- so the card surface gives its
textures up for the duration and gets them back afterwards from the RAM picture it
kept. Kept, not rebuilt: re-entering the card to redraw it would re-run its scripts
and start the movie again.

**LITE** is read into one scratch buffer and composited into the card's own picture
at the MLST position, then left to
[`CardSurface`](../source/render/CardSurface.cpp) to upload the rows it touched.

**The soundtrack is the clock.** Each frame's IMA block is software-decoded to PCM16
and pushed into a maxmod stream, and the count of samples the hardware really took
decides which frame must be on screen. Only *real* samples are counted, so a player
that cannot keep up stalls the clock and the picture waits, rather than the picture
running away from the sound and never coming back.

**Falling behind is free.** A late player seeks past the frames it missed and reads
only the one it will show. That is the part raw storage buys that nothing else
would: with a codec, a skipped frame still has to be decoded, because the next one
predicts from it.

Movie audio cannot use the hardware ADPCM channels that
[`.rsnd`](../shared/RivenSound.hpp) sounds do, and the reason is worth writing
down: a hardware ADPCM channel reloads its state from the start of its sample on
loop, so it cannot be fed incrementally. Sounds are whole files and go to hardware;
a movie's soundtrack arrives one frame at a time and goes to the stream. maxmod
allows one stream, and the movie is what wants it.

## Why the codec went

It was correct, it was tested, and it was solving the wrong problem.

RVID quantised. The steps were `{4, 8, 8, 20}` and the measured mean error was 0.15
levels out of 31 -- small, deliberate, and chosen to fit the game into 465 MB.
That trade only makes sense if the card is the scarce thing. On this port it is not:
the game lives on an SD card with room to spare, and the picture is what matters.

And the milestone's one unmeasurable risk was the decoder. A fullscreen frame is
32x21 blocks across three planes; whether a 67 MHz ARM9 could reconstruct that at
15 fps could not be answered on a desktop, and the documented fallback -- motion
compensation on the 3D engine with the result taken by the display-capture unit --
was a large piece of work that would also have needed hardware to verify.

Raw frames retire both at once. There is no quantiser to lose anything to, and
there is no decode to be too slow. What is left is a read, and a read has a number:
1.21 MB/s.

What was actually lost:

* **7.2 GB more on the card**, from 465 MB to 7.62 GB.
* **~2000 lines** of encoder, reference decoder, ARM9 decoder, bitstream reader and
  the transform -- and the strongest test in the suite with them, the one that
  asserted the two decoders agreed byte for byte on every frame of a real clip.

What replaced that test is duller and better: convert a movie, then assert the bytes
in the file are **exactly** what the downscaler produced. Not close -- equal
([`tests/test_rvid_arm9.cpp`](../tools/riven-convert/tests/test_rvid_arm9.cpp)). A
codec needed a bit-exactness argument because it predicted from its own
reconstruction and a one-sample disagreement compounded across a GOP. Raw storage
needs an equality, and gets one.

The one thing that genuinely got worse is the fallback. There is no compressed mode
to drop to if a card cannot sustain the read; the fallback is frame decimation at
conversion time, which lands on Myst's proven 806 KB/s at 10 fps.

### What is not built

The 3D-engine path, and now nothing depends on it. `blendHalf` and the half-pel
prediction it existed for are gone with the codec.

Also absent: the water effects (`sfxe/` is converted, nothing animates it) and
anything to do with the inventory or the zoom viewer.

## Licensing

The codec was where the licensing care went, and it is worth recording what
happened to it.

FastVideoDS's decoder is 1960 lines of unlicensed ARM assembly, and its entropy
coder is driven by the MPEG-4 TCOEF VLC tables that
[`licensing.md`](licensing.md) forbids copying. RVID answered that by using
Exp-Golomb instead -- structured codes, no table to copy or to ship -- on the
grounds that we owned both ends and had nothing to be compatible with.

None of that applies to a raw frame. There is no entropy coder, no transform and no
prediction, so there is nothing in the video path that could resemble anyone else's
expression. The format is a size, a stride, and the pixels.

What still stands:

* **No ScummVM code may be copied.** Its Mohawk engine is the specification to read
  and cite by file and line, and then to write yourself.
* **Nothing GPL is linked.** ffmpeg is invoked as a separate binary, never linked,
  so nothing about its licence reaches this program -- see
  [`licensing.md`](licensing.md).

## Why the decoders went too

The converter used to carry its own QuickTime demuxer, its own Cinepak decoder and
its own QuickTime RLE decoder: `core/src/mov/QuickTime.cpp` (543 lines),
`Cinepak.cpp` (310) and `QtRle.cpp` (248), plus about 230 lines of headers. They
existed to keep the build free of codec libraries.

They are gone, and the movie stage now shells out to **ffmpeg**. What that bought:

* **~1100 lines retired**, and with them the standing obligation to keep two
  hand-written decoders correct against 1055 movies nobody re-checks.
* **Cancellation that works.** The old decoder polled nothing; a cancel during
  ospit's tMOV 21 (377 MB) took effect when the movie finished. The read loop now
  checks once per frame and kills the child.
* **The audio path collapsed into one call.** `decodeIma4` + `downmixToMono` + the
  sample-table trimming that compensated for tables describing more packets than
  the track's duration used, all replaced by `-ac 1 -ar 22050 -f s16le`.

What it cost: ffmpeg has to be installed. That is checked once, before a run starts
([`Preflight.cpp`](../tools/riven-convert/core/src/Preflight.cpp): `checkFFmpeg`),
rather than discovered at movie 1 of 1055.

### What ffmpeg is NOT allowed to do

**Quantise.** `-pix_fmt rgb555le` would have ffmpeg apply its own dither, and 874 of
Riven's movies are overlays composited into a card still
([`RvidPlayer::compositeInto`](../source/rvid/RvidPlayer.cpp)). If the overlay and
the still underneath were quantised differently, the seam would be visible. So
ffmpeg hands over `rgb24` and `downscaleToTexels` at 1:1 does the ARGB1555 pack --
the same 4x4 Bayer threshold in linear light the stills go through.

ffmpeg *does* scale, with `flags=area` (a box filter, the same family as ours). That
leaves one real difference: swscale averages in gamma space and `downscaleToTexels`
averages in linear light. Measured over sampled frames of a 5-CD install
([`tests/test_video.cpp`](../tools/riven-convert/tests/test_video.cpp)), the two
differ by a **mean of 0.3 and a worst case of 9, out of 31 levels** -- and the test
asserts those bounds so a change to either scaler is noticed rather than shipped.

### Frame counts: not from the metadata

A movie pulled out of a Mohawk archive has its header rewritten on the way out
(`vaht_mov`), and both of the obvious frame-count fields come back wrong:

* `nb_frames` reads **23** for a 350-frame movie and **195** for a 13-frame one.
* `duration` in seconds comes back numerically equal to the FRAME count, so
  `duration x rate` overshoots by exactly the frame rate.

Neither is off by a rounding error. The count therefore comes from
`ffprobe -count_packets`, which demuxes without decoding: Riven's codecs are
intra-only, so one packet is one frame. It is exact for every movie in the game but
one (916 packets, 625 frames), and that error is in the safe direction -- the number
only SIZES the frame index, and the real count comes from how many frames ffmpeg
hands over. A movie that ends up shorter than its index was sized for leaves unused
entries between the index and frame 0, which is why
[`RvidFile::open`](../source/rvid/RvidFile.cpp) seeks to `index[0].offset` instead of
assuming frame 0 follows the index.
