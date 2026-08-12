# Riven: The Sequel to Myst — Nintendo DS port

A homebrew port of **Riven** to the Nintendo DS and DSi, built with
[BlocksDS](https://blocksds.skylyrac.net/) and
[Nitro Engine Advanced](https://github.com/Warioware64/nitro-engine).

**It contains no game data.** The ROM is the program only; a converter turns your
own copy of Riven into files the DS can read.

> **Status: early.** The converter is complete: it reads a Riven install and
> produces the card graph, the artwork, the sound and the movies the DS reads,
> with a Qt GUI and a CLI. The DS engine itself is not written yet. See
> [Milestones](#milestones).

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

**Sound is not re-encoded where it does not have to be.** Riven's own audio is
Intel DVI ADPCM, which is the same algorithm the DS decodes in hardware, so the
mono effects are copied through bit-exactly rather than decoded and re-encoded.
The stereo ambient tracks — which are most of the game's 152 MB of sound — are
downmixed to mono, since SLST gives every sound its own balance and the DS pans
in hardware. The DVD release's MPEG-2 Layer II sounds are decoded and
re-encoded. See [`shared/RivenSound.hpp`](shared/RivenSound.hpp).

**Video is compressed.** Myst's `.mvpc` stores every frame raw at ~80 KB each,
which is why it wants a couple of gigabytes for a much smaller game. Riven ships
1.93 GB of QuickTime video across 1055 movies. The RVID codec is derived from
[FastVideoDS](https://github.com/Gericom/FastVideoDSPlayer) — I/P frames, a 2×2
Hadamard transform over RGB555 planes, and audio interleaved per frame — in two
profiles:

- **FULL** — fullscreen cutscenes. Motion compensation runs on the 3D engine
  (one triangle per 8×8 block, the texture coordinate *is* the motion vector,
  half-pel comes free from the GPU's 50% alpha blend) and the result is grabbed
  by the display-capture unit.
- **LITE** — the movies composited onto a card, which is most of them. Riven's
  overlays are locked-off shots of animated elements, so these use zero-motion-
  vector P-frames decoded on the ARM9 into RAM. Display-capture MC cannot
  composite a 39×76 movie onto a card the 3D engine is already drawing.

Entropy coding is Exp-Golomb rather than FastVideoDS's MPEG-4 VLC tables, which
[may not be copied](docs/licensing.md). RVID has exactly one decoder and we
write that too, so there is nothing to be compatible with and no table to ship.
The format is [documented](docs/video.md).

## Building

```bash
git submodule update --init     # libvaht
./setup-env.sh                  # ./env: architectds + ninja
./make.sh                       # -> riven-nds-port.nds
```

The converter builds separately, and needs no Python, numpy or ffmpeg:

```bash
cmake -S tools/riven-convert -B build/convert
cmake --build build/convert -j

./build/convert/gui/riven-convert-gui          # Qt GUI
./build/convert/riven-convert /path/to/riven   # or headless
```

The GUI is optional: without Qt 6.5+ the build produces the CLI and the tests
and says so. `riven-convert <source>` with no destination probes your copy of
the game and reports what it found without writing anything, and
`riven-convert --movie-report <source>` lists what is inside the movies —
codecs, sizes, frame rates and audio tracks — which is where the numbers in
[docs/video.md](docs/video.md) come from.

Movies are the long pole of a conversion: `--video-quality` trades size against
sharpness, and `-j` sets how many are encoded at once (one per core, less one,
by default).

Conversion is **resumable**. Every output is written to a temporary name and
renamed into place, so a file under its final name is always complete; anything
already up to date is skipped, and stopping a run mid-way costs only the asset
in flight. `--force` (or "Rebuild everything") redoes the lot.

You need BlocksDS installed through the [Wonderful
toolchain](https://wonderful.asie.pl/), plus Nitro Engine Advanced in
`$BLOCKSDSEXT`.

### Tests

```bash
cd build/convert && ctest
```

The archive test reads a real install when you point it at one, and skips
cleanly when you don't:

```bash
RIVEN_TEST_DATA=/path/to/riven ctest --test-dir build/convert
```

## Source data

Both the 5-CD and DVD/GOG layouts are detected automatically. Patch archives
(`b_Data1`/`b2_data`, `j_Data3`) are given priority over the base archives, as
they must be — otherwise you convert the unpatched game.

Archives that exist but are not readable Mohawk containers are reported and
skipped rather than treated as fatal. This is not hypothetical: an interrupted
download leaves full-size, entirely zero-filled `.MHK` files behind, and can
also zero a *region* inside an otherwise valid archive. Where the damage is
partial the converter salvages what it can and says how much it lost.

## Layout

| Path | |
|---|---|
| [`shared/`](shared/) | Data schema compiled by **both** the converter and the ARM9 |
| [`source/`](source/) | ARM9 runtime |
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
5. 🔨 DS engine: stills and navigation — first playable
6. ⬜ Scripts: full opcode table and per-stack external commands
7. ✅ Audio — `sound/<stack>/<id>.rsnd`, mono IMA ADPCM
8. 🔨 RVID codec — converter side done (QuickTime demuxer, Cinepak, QuickTime
   RLE, IMA4, the [encoder](docs/video.md)); the ARM9 player is next
9. ⬜ Inventory, zoom viewer, saves, menus

## Credits and licensing

Riven is © Cyan Worlds. This project ships none of its data.

[ScummVM](https://www.scummvm.org/) is the reference for how Riven works and is
cited throughout by file and line. See [docs/licensing.md](docs/licensing.md)
for how ScummVM (GPLv2+), libvaht (LGPL-3.0), Qt 6 (LGPL-3.0) and FastVideoDS
(unlicensed) each relate to this Apache-2.0 project — the rules differ per
dependency and matter. That file also explains why the DS-side compression is
the BIOS LZ77 routine rather than LZO.
