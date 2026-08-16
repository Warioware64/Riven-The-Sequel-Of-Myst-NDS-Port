# Riven: The Sequel to Myst — Nintendo DS port

A homebrew port of **Riven** to the Nintendo DS and DSi, built with
[BlocksDS](https://blocksds.skylyrac.net/) and
[Nitro Engine Advanced](https://github.com/Warioware64/nitro-engine).

**It contains no game data.** The ROM is the program only; a converter turns your
own copy of Riven into files the DS can read.

> **Status: early, but it runs.** The converter is complete: it reads a Riven
> install and produces the card graph, the artwork, the sound and the movies the
> DS reads, with a Qt GUI and a CLI. The DS engine has a menu and a settings
> screen, boots into Riven's own main menu, draws cards, runs the scripts, plays
> the fullscreen movies with sound, follows hotspots between cards and stacks,
> zooms into any card at its original resolution, saves to five slots, and keeps
> a notebook you can draw in. **Every one of Riven's 131 external commands is
> implemented** — all eight stacks, which is the telescope, every ending and the
> credits roll behind them,
> the marble grid, the boiler and the Ytram trap, the rebel tunnel, the pin map,
> the imagers, the trap book, and all five domes. The water effects are what is
> left. See [Milestones](#milestones).

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
./build/convert/riven-convert --stack aspit --stack tspit \
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

**The top screen is the log, over Riven's own splash.** Every diagnostic the
engine produces goes there: a movie that is not on the card, a card a script
asked for that does not exist, an external command this release carries and the
port has never heard of. On
hardware those used to go nowhere, which made every failure look like the game
simply not doing something. Behind them is `Autorun/AUTORUN.BMP`, the picture the
CD's autorun shell put behind its buttons — extracted from your copy like
everything else, because the ROM ships no art. An install without one (it belongs
to the disc, not to the game) gets the black screen it had before.

**X zooms.** Riven's stills are 608x392 and the card view shows all of them at
0.42x, which turns a good deal of what the game asks you to read into a smudge.
**X** opens the full-resolution twin at 1:1 and the D-pad or the stylus pans it;
**B** leaves. This is what `pics_hi/` has been for, so **do not pass
`--no-hires`** if you want it — without that stage the button says so and does
nothing.

**Settings** live behind the menu the ROM starts on, and behind Riven's own
Options button. Zip mode (off by default, as in the original), transitions, water,
a master volume and the debug log, kept in `_nds/riven_nds/data/settings.dat`.

**START opens the port's menu** — save, load, notebook, settings, resume — over
whatever card you are on. Riven's own menu card reaches the same screens: its
Save and Restore buttons open the slot list, and Resume goes back to where you
were. Five slots in `_nds/riven_nds/data/saves/`, listed by island and time. A
slot that will not read is reported as **damaged** rather than shown as empty,
because an empty-looking slot gets saved over and the file might be the only copy
of a game you still want.

The save format is at version 2 and **slots written by an earlier build list as
damaged**. There is no migration and deliberately so: the archive is not
self-describing, so the version number is the only thing standing between a new
build and an old file, and reading one into the wrong fields is worse than
refusing it. Version 2 adds the frame clock, which Riven's three timed things —
the sunners, the wharks and the Ytram trap — store their deadline as a reading
of. Without it, a trap baited before a save was never going to go off again.

**L takes a note, Y opens the notebook.** Riven is a game of written-down things
— D'ni numerals, which animal goes with which dome sound, a grid of fire marbles
— and the DS has a stylus. **L** puts a picture of what is on screen into the
notebook; **Y** pages through them and lets you scribble on the page in six
colours, with an eraser that removes whole strokes rather than painting over
them. **L works inside the zoom viewer too**, which is where you usually want it
— you opened the full-resolution view to read something — and there it captures
all 192 rows rather than the card view's 165, so the page is exactly what was on
screen.

**L works at any moment**, in fact — during a cutscene, mid transition, in the
middle of a slider drag. It used to be read only by the card loop, so the whole
of Gehn's speech and every second of a marble being dragged onto a square were
stretches of the game no note could be taken of, which are exactly the stretches
the game is asking you to remember something. Taking one costs a fifth of a
second of SD write, so a cutscene's soundtrack will skip; that is the price of
being able to take one there at all.

Notes live in `_nds/riven_nds/data/notes/` and are **global, not part of a
save slot**: what you wrote down is knowledge you keep whichever game you load.
Twenty-four pages, and the notebook says when it is full rather than quietly
dropping the oldest one.

**The top screen is quiet.** It is the splash and nothing else: no card
summaries, no missing-asset warnings, no trace. That is a deliberate reversal —
those lines used to print whether you asked for them or not, and because the
console draws opaque white straight onto the picture, a minute of ordinary play
buried it. Everything they said is still said, to `nocashMessage` (free under an
emulator) and to `debug.log` when the trace is on; it just stops being painted
over Riven.

Two things still reach it. **Startup notices** — no `video/`, no `cursors/`, no
menu font — print once, stay readable while you are on the menu, and are wiped
when the game starts, because those are things to go and fix. And a **status
line** at the bottom answers a button you just pressed (`note taken`, `notebook
full`) and clears itself after a couple of seconds.

**Debug log** turns the top screen into a trace instead of a picture, and it is
the setting that unlocks everything above: the stack and card being entered with
its PLST/HSPT/SLST/MLST counts, a line for every picture, sound, movie and zoom
twin as it is loaded with the ids and rectangles the scripts asked for, and every
warning that is otherwise held back. It goes to the console, to `nocashMessage`
(so an emulator gets it for free) and to `_nds/riven_nds/data/debug.log` — synced
to the card line by line, so the log survives the power switch, which is how a DS
run actually ends.

With it on, **L** writes the bottom screen to `shotNNN.bmp` and **R** dumps every
mapped VRAM bank to `vramNNN/`, at the same any-moment reach the note has, and
for the same reason: those are the moments a rendering bug lives in, and the old
`SELECT+L`/`SELECT+R` chords could not reach them because only the card loop read
them. So debug mode does not change *when* **L** works — it changes *what* **L**
does, swapping the note for the screenshot. **Y** still opens the notebook either
way, so nothing already written is out of reach, and with debug mode off the
chords still mean what they always did. Debug mode is read once at startup, so it
takes effect on the next boot.

**SELECT is a command prompt** — tap it and let go, in debug mode. Holding it and
pressing something else is still a chord (a direction replays the card with that
transition, **A** dissolves it), so the two do not collide: the prompt opens on
the *release* of a SELECT that nothing joined. **SELECT+START** opens it too, and
is the spelling that works with debug mode off.

The prompt has the DS keyboard on the touch screen
and the card still live above it, so what a command did is visible while the next
one is typed. `card 300` and `stack jspit 155` go somewhere, `var blabopen 1`
sets a variable **by name** and reads it back, `hotspots` lists every hotspot on
the card with its rectangle and whether it is enabled, `timer` names the card
timer that is armed and counts down the frames until it fires, and `vars`,
`zips`, `sound`, `movie`, `save`, `load`, `notes`, `shot` and `vram` do what they
say.
TAB completes, the D-pad's up and down walk the history, **B** closes. Debug mode
only — the keyboard writes over palette entries the top-screen picture uses.

**If your card was converted by an older build**, just convert again — the video
stage notices by itself and logs `video was converted by an older build: redoing
it`. There is nothing to delete first, and `tools/stale-movies.py` is no longer
worth running.

That redo is not optional. Movies are versioned, the ARM9 rejects a file it does
not recognise, and two fixes changed what is on the card: the QuickTime **track
matrix**, which put 79 of the 1054 movies on at twice or four times their size,
and the overlay **span**, which left 583 of them a pixel short of the still they
sit on — tspit's lever was both. It is the long stage; expect hours.

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
6. ✅ Scripts — the whole simple-opcode table is implemented, zip mode (45)
   included, and **all 131 external commands across all eight stacks**: every
   name in every stack's NAME 3 resource, plus the few ScummVM registers that
   this release's data never calls. That is aspit's menu, both journals and the
   trap book; Jungle's sunners, rebel tunnel, whark elevator, number game and
   gallows carriage; Temple's telescope, marble grid and the ending it opens
   onto; Boiler's lab journal, boiler, Ytram trap and water valve; Garden's pin
   map, two viewers, whark and scribe; Gehn's office, the cage and the watch;
   Prison's elevator combination and Catherine; Tay's window and its ending —
   and all five domes, which are one shared implementation and five lines each.
   Every ending now rolls the credits behind it (extras.MHK's tBMP 302-320,
   scrolled by the background hardware rather than by copying the screen), and
   the debug console's `endings` command fires each ending's branch in turn and
   reports which one it chose.
   Between them they put a card timer, drag loops, resource-name lookup, movable
   hotspots, a saved frame clock and a script-abort into the engine
7. ✅ Audio — `sound/<stack>/<id>.rsnd`, lossless PCM16 where it fits and
   bit-exact ADPCM passthrough where it does not, played on the DS's hardware
   channels with SLST volume and balance
8. ✅ Video — the converter (QuickTime demuxer, Cinepak, QuickTime RLE, IMA4,
   [raw frames](docs/video.md)) **and** the DS player: no codec, the soundtrack as
   the clock, and a skipped frame that costs a seek
9. 🔨 Inventory, zoom viewer, saves, menus, water effects — the inventory strip,
   both journals' route in, the port's menu and settings screen, the top-screen
   picture, the zoom viewer, five save slots, the notebook and the debug console
   are built. **The water effects are not**, and they are what is left of this
   milestone

## Credits and licensing

Riven is © Cyan Worlds. This project ships none of its data.

[ScummVM](https://www.scummvm.org/) is the reference for how Riven works and is
cited throughout by file and line. See [docs/licensing.md](docs/licensing.md)
for how ScummVM (GPLv2+), libvaht (LGPL-3.0), Qt 6 (LGPL-3.0) and FastVideoDS
(unlicensed) each relate to this Apache-2.0 project — the rules differ per
dependency and matter. That file also explains why the DS-side compression is
the BIOS LZ77 routine rather than LZO.
