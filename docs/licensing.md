# Licensing

This project is **Apache-2.0** (see [LICENSE](../LICENSE)). That choice constrains
what third-party code can be used and how, so the rules are written down here
rather than rediscovered later.

## The port itself

`source/`, `shared/`, `tools/riven-convert/core/`, `build.py` — Apache-2.0,
original work.

## ScummVM — reference only, never copied

ScummVM's Mohawk engine (`helpSrc/scummvm-master/engines/mohawk/`) is the
authoritative description of how Riven works: resource layouts, the script
opcode table, water-effect semantics, sound fading, transitions.

ScummVM is **GPLv2-or-later**, which is not compatible with distributing a
derived work under Apache-2.0. It is therefore used the same way the Myst port
used it: as documentation. Behaviour is reimplemented from it, and the source
of each fact is cited in a comment as `file.cpp:line` so any claim can be
checked. No ScummVM code is copied.

If you are adding a feature, read the ScummVM implementation, understand it,
cite it, and write the code yourself.

Three of the converter's readers are worth naming here, because they are the
newest and because "read the spec, write your own" is easiest to get wrong when
the spec is itself an implementation:

| Ours | Described by |
|---|---|
| [`core/src/installer/Dcl.cpp`](../tools/riven-convert/core/src/installer/Dcl.cpp) | `common/compression/dcl.cpp` — PKWARE DCL "implode", a published format |
| [`core/src/installer/InstallerArchive.cpp`](../tools/riven-convert/core/src/installer/InstallerArchive.cpp) | `common/compression/installshieldv3_archive.cpp`, cited by line for each header field |
| [`core/src/pe/PeCursors.cpp`](../tools/riven-convert/core/src/pe/PeCursors.cpp) | `engines/mohawk/cursors.cpp` and the documented Win32 `RT_GROUP_CURSOR` / `RT_CURSOR` structures |

None of these is a transcription. The DCL Huffman tables are the format's own
constants and are the only literal values shared with any other implementation
of it — a table of code lengths is not expression, and there is no other correct
set. The rest is written from the structure descriptions, which is why the
comments in those files read as specifications rather than as annotations.

## libvaht — LGPL-3.0, host-side only

[`thirdparty/libvaht`](../thirdparty/libvaht) (agrif/libvaht) reads Mohawk
archives and most Riven resources. It is **LGPL-3.0**.

Two things keep this clean:

1. **It is only ever linked into the converter**, which is an ordinary desktop
   program. It never goes into the DS ROM.
2. **The converter is published as source** with libvaht as a submodule, so the
   LGPL's requirement that a user be able to relink against a modified libvaht
   is satisfied even though the link is static (LGPL-3.0 §4(d)(1)).

Do not link libvaht into `source/` (the ARM9 binary). Nothing there needs it —
all Mohawk parsing happens at conversion time.

### The libvaht patches

Both patches fix the same class of bug — an on-disc struct declared without
packing, which upstream never sees because its autotools build passes
`-fpack-struct=1` (`src/Makefile.am:8`) and this project's CMake build does not.
CMake applies them at configure time and the step is idempotent, so the
submodule keeps pointing at upstream and a fresh clone works with no manual
step. **Both should go upstream.**

`0002-pack-twav-data-header.patch` packs the tWAV Data-chunk header, which is 20
bytes on disc and 24 in memory. Without it every field after the sample rate is
read from the wrong offset, the encoding check fails, and `vaht_wav_open`
returns NULL for every sound in the game. The converter does not depend on the
fix — it parses tWAV itself — but `test_sound` uses libvaht's decoder as an
independent oracle for ours, and that needs it to work.

`0001-pack-mohawk-file-table.patch` is the one without which nothing works at
all: libvaht 0.3 as published **cannot open any Riven archive**. Its
`struct vaht_mohawk_file_table` is 10 bytes on disc but pads to 12 bytes in
memory on every normal C ABI, and `vaht_archive_open` both validates the entry
count against `sizeof` that struct and `fread`s entries straight into it.

Both use `#pragma pack` rather than `__attribute__((packed))`, because the
converter is built with MSVC on Windows and MSVC does not have the attribute.
Sending them to agrif/libvaht would let both patches and this whole mechanism be
deleted.

## Qt 6 — LGPL-3.0, and it must stay dynamically linked

The converter's GUI (`tools/riven-convert/gui/`) uses **Qt 6 Widgets**, which is
LGPL-3.0. An Apache-2.0 application may use it, but only on the LGPL's terms,
and the one that constrains packaging is that the user must be able to swap in
their own build of Qt.

**Dynamic linking is what satisfies that**, and both shipping routes do it:
`windeployqt` copies the Qt DLLs next to the `.exe` on Windows, and the AppImage
bundles the Qt shared objects on Linux. Do **not** switch to a static Qt build
without reading LGPL-3.0 §4 and arranging to publish the object files.

Note also that `Qt6::Svg` is deliberately unused: it is not part of a default
`qtbase` install, so depending on it would break the build on machines where
everything else is present.

The core and the CLI do not link Qt at all. That is a licensing convenience as
well as a design one — `riven_convert_core` and `riven-convert` are pure
Apache-2.0 with no LGPL component.

## ffmpeg — a separate program, never linked

The converter's movie stage decodes through **ffmpeg** and probes with **ffprobe**
([`core/include/riven/FFmpeg.hpp`](../tools/riven-convert/core/include/riven/FFmpeg.hpp)).
ffmpeg is LGPL-2.1-or-later, or GPL-2.0-or-later when built with `--enable-gpl`,
and a distributor cannot know which build a user has installed.

That is exactly why it is a **child process and not a library**. Running a program
is not linking against it, and no ffmpeg code, header or symbol enters this binary:

* Nothing is `#include`d from ffmpeg. `FFmpeg.hpp` declares nothing but paths,
  argument vectors and a pipe.
* Nothing is linked. `CMakeLists.txt` has no `find_package`, no `pkg_check_modules`
  and no ffmpeg import library; the converter builds on a machine with no ffmpeg
  at all and only fails when a movie is actually converted.
* No ffmpeg binary is shipped with this project. The user supplies it, on their
  PATH or through `--ffmpeg` / the GUI's ffmpeg field.

So the converter stays Apache-2.0 and the GPL question never arises. **If anyone
later wants to link libavcodec instead, this section stops being true** and the
converter would have to be relicensed or shipped without that path -- which is the
main reason the subprocess design is written down here rather than treated as an
implementation detail.

The same reasoning is why the ARM9 side is untouched: the ROM contains no decoder
of any kind, ffmpeg-derived or otherwise. A `.rvid` frame is the texels the DS
samples ([`docs/video.md`](video.md)).

## minimp3 — CC0-1.0, converter only

[`thirdparty/minimp3`](../thirdparty/minimp3) (lieff/minimp3) decodes the
MPEG-2 Layer II tWAV sounds that the DVD and GOG releases ship in place of some
of the 5-CD release's ADPCM ones. libvaht cannot read them
(`vaht_wav.h:38` calls MP2 "not yet supported"), and the converter's CI has no
codec libraries at all, so the decoder has to come from somewhere in-tree.

It is **CC0-1.0**, a public-domain dedication, which places no conditions on
use, modification or redistribution and is compatible with Apache-2.0. The
`LICENSE` file is vendored alongside the header for the record.

Two boundaries keep it tidy:

1. **One translation unit instantiates it**
   ([`core/src/audio/minimp3_impl.c`](../tools/riven-convert/core/src/audio/minimp3_impl.c)),
   compiled as C with warnings off, exactly like libvaht. `Mp2.cpp` includes the
   same header without `MINIMP3_IMPLEMENTATION` and sees only declarations.
2. **It never goes into the ARM9 binary.** MP2 is decoded once, at conversion
   time, and what reaches the card is ADPCM like everything else.

Vendored as a single header rather than a submodule because it is one file with
no build system, and a submodule for 76 KB would cost every contributor a
`git submodule update` for nothing.

## Compression: why not LZO

The DS-side zoom art is decompressed with the **NDS BIOS LZ77** routine
(`swiDecompressLZSSWram` / `decompress(..., LZ77)`), and the matching compressor
is written from scratch in `core/src/image/Lz77.cpp`.

That is a licensing decision as much as a technical one. The obvious candidate,
`lzo` in `BLOCKSDSEXT`, is **GPL-2.0**: statically linking it into the ARM9
binary would relicense the entire ROM under the GPL, which is incompatible with
Apache-2.0. zlib would be fine licence-wise but BlocksDS does not ship it for
the ARM9. The BIOS routine links nothing, costs nothing, and carries no licence
at all — and measured on real Riven art it reaches 55% of raw 8bpp, which is
better than the game's own packing.

## FastVideoDS — no license at all, and no longer relevant

Neither [FastVideoDSEncoder](../helpSrc/FastVideoDSEncoder-master) nor
[FastVideoDSPlayer](../helpSrc/FastVideoDSPlayer-master) ships a license file.
Absent a grant, **no code from either may be copied into this project** — not
the ARM assembly decoder, not the C# encoder, not the VLC tables.

This governed the RVID codec, which was a reimplementation from the design (a file
format is not copyrightable, and neither is the technique of motion compensation on
the 3D engine) and which deliberately used Exp-Golomb rather than the MPEG-4 TCOEF
VLC tables the reference is driven by.

**That codec no longer exists.** Video is stored as raw ARGB1555 frames
([docs/video.md](video.md) says why), so there is no transform, no entropy coder and
no prediction anywhere in the video path — nothing that could resemble anyone's
expression of anything. The restriction above still stands as a rule about the
`helpSrc/` reference material; it no longer constrains any code that ships.

Asking Gericom for a grant would now buy only the option of going back to a codec,
which nothing needs.

## Game data

None is included, and none may be. The converter turns a user's own copy of
Riven into the files the port reads; the ROM ships empty.
