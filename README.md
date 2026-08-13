# Riven: The Sequel to Myst — Nintendo DS port

A homebrew port of **Riven** to the Nintendo DS and DSi, built with
[BlocksDS](https://blocksds.skylyrac.net/) and
[Nitro Engine Advanced](https://github.com/Warioware64/nitro-engine).

**It contains no game data.** The ROM is the program only; a converter turns your
own copy of Riven into files the DS can read.

> **Status: early, but it runs.** The converter is complete: it reads a Riven
> install and produces the card graph, the artwork, the sound and the movies the
> DS reads, with a Qt GUI and a CLI. The DS engine now boots into Riven's main
> menu, draws cards, runs the scripts, plays the fullscreen movies with sound and
> follows hotspots between cards and stacks. Most of the game's machinery — the
> inventory, the zoom viewer, saves, the water effects and the per-stack puzzle
> commands — is not there yet. See [Milestones](#milestones).

## How it relates to the Myst port

This is built on the architecture of
[myst-nds-port](https://github.com/Warioware64/myst-nds-port), with two
deliberate departures.

**The converter is C++ and writes the runtime format directly.** The Myst port
converts Mohawk data to JSON in Python; the DS then parses that JSON with
nlohmann on first boot, bakes it to a yas binary, and offers to delete the JSON
afterwards. Here, the converter links the *same schema header the ARM9 compiles*
([`shared/RivenData.hpp`](shared/RivenData.hpp)) and yas-serializes straight to
the file the DS loads. No JSON on the card, no nlohmann in the ROM, no
first-boot bake screen.

**Sound is never re-encoded.** Riven's own audio is Intel DVI ADPCM, the same
algorithm the DS decodes in hardware, so the mono effects are copied through
bit-exactly — 42 of them, untouched. The stereo tracks have to be decoded and
downmixed whatever happens, since SLST gives every sound its own balance and the
DS pans in hardware; what used to happen next was a re-encode back to ADPCM, and
that was the only lossy step left anywhere in the port. Now the downmix is kept as
**PCM16**, which the DS also plays in hardware.

The exception is length, not quality: a hardware channel holds its whole sample
while it plays, and Riven's longest ambient is 157 seconds — 6.9 MB as PCM16
against 1.7 MB as ADPCM. Anything over half a megabyte of PCM stays ADPCM so it
stays audible. See [`shared/RivenSound.hpp`](shared/RivenSound.hpp).

**Video is stored raw, and that is a reversal.** There was a codec — I/P frames,
a 2×2 Hadamard over RGB555 planes, Exp-Golomb coefficients, an ARM9 software
decoder checked byte-for-byte against a reference decoder on the host. It fit
Riven's 1.93 GB of QuickTime into 465 MB, and it was removed.

Two reasons, and neither is that it did not work. It **quantised**, at a measured
mean error of 0.15 levels out of 31 — a trade that only pays if the card is the
scarce thing, and here it is not. And whether a 67 MHz ARM9 could decode a
fullscreen frame at 15 fps was the one question a desktop could not answer, with a
large GPU-based fallback waiting behind it if the answer was no.

Raw frames retire both. A frame in the file is exactly the ARGB1555 texels the DS
samples, so there is no quantiser to lose anything to and no decode to be too slow
— a fullscreen movie is a 1.21 MB/s read and nothing else, and a player that falls
behind seeks past what it missed instead of decoding it. It costs 7.62 GB of card
instead of 465 MB. The two profiles survive as two ways of reaching the screen:
**FULL** into its own texture, **LITE** composited into the card, because nothing
can put a 39×76 movie on top of a card the 3D engine is drawing.

The reasoning, the numbers and what was lost are [documented](docs/video.md).

## Building

```bash
git submodule update --init     # libvaht
./setup-env.sh                  # ./env: architectds + ninja
./make.sh                       # -> riven-nds-port.nds
```

The converter builds separately, and needs no Python:

```bash
cmake -S tools/riven-convert -B build/convert
cmake --build build/convert -j

./build/convert/gui/riven-convert-gui          # Qt GUI
./build/convert/riven-convert /path/to/riven   # or headless
```

It needs **ffmpeg** at run time, though not at build time: the movie stage decodes
through it (`apt install ffmpeg`, `brew install ffmpeg`, or
[ffmpeg.org/download](https://ffmpeg.org/download.html)). Both front ends check for
it before a run starts and say so; `--ffmpeg <path>` points at a copy that is not on
your PATH, and `--no-video` skips the movies entirely and needs nothing.

The GUI is optional: without Qt 6.5+ the build produces the CLI and the tests
and says so. `riven-convert <source>` with no destination probes your copy of
the game and reports what it found without writing anything, and
`riven-convert --movie-report <source>` lists what is inside the movies —
codecs, sizes, frame rates and audio tracks — which is where the numbers in
[docs/video.md](docs/video.md) come from.

Movies are the long pole of a conversion and the reason it wants the whole
machine: `-j` sets how many are converted at once, one per core by default. Every
stage that can be parallel is — each worker opens its own view of the archives, so
the reads run alongside the work rather than between batches of it. Converting
Temple Island on eight cores went from 3:32 at 251% CPU to 48 seconds at 639%, and
that is while writing 1.9 GB instead of 216 MB.

Expect a full conversion to be **write-bound**, not CPU-bound: raw video is 7.6 GB.

Conversion is **resumable**. Every output is written to a temporary name and
renamed into place, so a file under its final name is always complete; anything
already up to date is skipped, and stopping a run mid-way costs only the asset
in flight. `--force` (or "Rebuild everything") redoes the lot.

You need BlocksDS installed through the [Wonderful
toolchain](https://wonderful.asie.pl/), plus Nitro Engine Advanced in
`$BLOCKSDSEXT`.

### Running it

The ROM reads everything from `<card>/_nds/riven_nds/data/`, so point the
converter at the card (or at a directory you then copy across) and put the `.nds`
wherever your loader expects it.

If you converted before a format changed, the converter now notices: each stage
stamps the version it wrote beside its output, and a stamp that does not match
redoes the stage whatever the file dates say. Before that, a card full of movies
written by an older build was reported "up to date" forever while the ROM
rejected every one of them — and nothing said so.

Two stacks are enough to see the game start, and they are much quicker to convert
than all eight:

```bash
./build/convert/riven-convert --stack aspit --stack tspit --no-hires \
    /path/to/riven /path/to/sd
```

`aspit` is the main menu; `tspit` is Temple Island, where the game actually
begins. Booting lands on aspit card 1, and the **Riven** button at the bottom
right goes to Temple Island and plays the opening cutscene — five and a half
minutes of fullscreen video across four movies. **B** or **Start** skips a
cutscene.

The pointer is persistent: touching moves it, lifting leaves it where it was,
and the D-pad nudges it with **A** as the click. That is not a stylus
concession — it is what gives the port *hover*, and hover is the only thing
Riven's cursor shapes are for. The cursors are the game's own, read out of
`riven.exe`; each hotspot names the shape it wants and the pointer takes it.

The books you carry sit in the band under the card view and are touchable.
That band is the only route to Atrus's journal, in this port as in the original.

**The top screen is the log.** Every diagnostic the engine produces goes there:
a movie that is not on the card, a card a script asked for that does not exist,
an external command nobody has written yet. On hardware those used to go
nowhere, which made every failure look like the game simply not doing something.

`--no-hires` skips the zoom art, which is the largest single thing on the card and
which nothing reads yet.

### Tests

```bash
cd build/convert && ctest
```

Several tests read a real install when you point them at one, and skip cleanly
when you don't:

```bash
RIVEN_TEST_DATA=/path/to/riven ctest --test-dir build/convert
```

Two of them are worth knowing about.

`test_rvid_arm9` is not a converter test at all: it compiles the **DS container
reader** ([`source/rvid/RvidFile.cpp`](source/rvid/RvidFile.cpp)) for the host,
which is why that file includes no `<nds.h>`. It converts a real movie, reads it
back the way the DS reads it, and asserts the bytes in the file are **exactly**
what the downscaler produced — not close, equal. On hardware nothing can check that
a movie was read back the way it was written.

`test_image` keeps the old 5-bit quantiser as a reference implementation and checks
the fast one against it on every 8-bit value at every dither position. That
function decides the value of every pixel in the game, and getting it wrong would
look like a slightly different picture rather than like a bug.

## Source data

Both the 5-CD and DVD/GOG layouts are detected automatically. Patch archives
(`b_Data1`/`b2_data`, `j_Data3`) are given priority over the base archives, as
they must be — otherwise you convert the unpatched game.

**The cursors and the inventory art are not in the Mohawk archives at all.**
Riven's cursors are Win32 resources in `riven.exe` and the inventory books are
tBMPs in `extras.mhk`, and the 5-CD release ships neither as a file — both are
inside `program/arcriven.z`, an InstallShield v3 installer archive. The converter
reads it: an InstallShield container reader, a PKWARE DCL decompressor and a PE
resource reader, all self-contained and all with ScummVM cited as the
specification rather than copied. An installed or DVD/GOG copy has the two files
loose and those are preferred; an install with neither converts everything else
and says what will be missing.

Archives that exist but are not readable Mohawk containers are reported and
skipped rather than treated as fatal. This is not hypothetical: an interrupted
download leaves full-size, entirely zero-filled `.MHK` files behind, and can
also zero a *region* inside an otherwise valid archive. Where the damage is
partial the converter salvages what it can and says how much it lost.

## Layout

| Path | |
|---|---|
| [`shared/`](shared/) | Data schema and on-card formats compiled by **both** the converter and the ARM9 |
| [`source/`](source/) | ARM9 runtime — [`engine/`](source/engine/) the card graph and scripts, [`rvid/`](source/rvid/) the video player, [`audio/`](source/audio/) sound, [`render/`](source/render/) the card on screen, [`data/`](source/data/) the on-card file readers |
| [`tools/riven-convert/core/`](tools/riven-convert/core/) | Conversion logic — pure C++20, no Qt |
| [`tools/riven-convert/cli/`](tools/riven-convert/cli/) | `riven-convert`, headless |
| [`tools/riven-convert/gui/`](tools/riven-convert/gui/) | `riven-convert-gui`, Qt 6 Widgets |
| [`thirdparty/`](thirdparty/) | libvaht (submodule), yas |
| [`docs/`](docs/) | [Licensing](docs/licensing.md), [the RVID format](docs/video.md) |
| `helpSrc/` | Reference material: ScummVM, FastVideoDS, the Myst port |

## Milestones

1. ✅ Skeleton, build, libvaht
2. ✅ Converter: archives → structs
3. ✅ Shared schema → yas emit — `stacks/<stack>.bin`, no JSON, no on-device bake
4. ✅ Converter: card art (`.rpic`) and zoom art (`.rpiz`), water effects, Qt GUI
5. ✅ DS engine: stills and navigation — boots into aspit card 1, draws the card,
   hit-tests hotspots in original coordinates, changes card and stack
6. 🔨 Scripts — the whole simple-opcode table is implemented; the per-stack
   external commands are only aspit's menu and books plus the two tspit's opening
   cutscene calls. The rest report their name and do nothing
7. ✅ Audio — `sound/<stack>/<id>.rsnd`, lossless PCM16 where it fits and
   bit-exact ADPCM passthrough where it does not, played on the DS's hardware
   channels with SLST volume and balance
8. ✅ Video — the converter (QuickTime demuxer, Cinepak, QuickTime RLE, IMA4,
   [raw frames](docs/video.md)) **and** the DS player: no codec, the soundtrack as
   the clock, and a skipped frame that costs a seek
9. 🔨 Inventory, zoom viewer, saves, menus, water effects — the inventory strip
   and both journals' route in are built; the zoom viewer, saves and the water
   effects are not

## Credits and licensing

Riven is © Cyan Worlds. This project ships none of its data.

[ScummVM](https://www.scummvm.org/) is the reference for how Riven works and is
cited throughout by file and line. See [docs/licensing.md](docs/licensing.md)
for how ScummVM (GPLv2+), libvaht (LGPL-3.0), Qt 6 (LGPL-3.0) and FastVideoDS
(unlicensed) each relate to this Apache-2.0 project — the rules differ per
dependency and matter. That file also explains why the DS-side compression is
the BIOS LZ77 routine rather than LZO.
