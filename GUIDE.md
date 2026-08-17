# Riven DS — what to do with these files

A homebrew port of **Riven: The Sequel to Myst** to the Nintendo DS and DSi.

**This archive contains no game data.** It is the program only. The converter turns
your own copy of Riven into files the DS can read; nothing from the game is included
here, and none of it is downloaded.

| File | |
|---|---|
| `riven-nds-port.nds` | The ROM. This is what your DS runs |
| `riven-convert-gui` | The converter, with a wizard. Start here |
| `riven-convert` | The same converter, headless, for scripting a conversion |

## Before you start

- **Your own copy of Riven for Windows** — the 5-CD, DVD and GOG releases are all
  detected automatically, patch archives included. The CDs do not need to be
  installed; a folder with the disc contents is enough.
- **ffmpeg** and **ffprobe** on your PATH, used to read the QuickTime movies:
  `apt install ffmpeg`, `brew install ffmpeg`, or
  <https://ffmpeg.org/download.html>. The converter checks for them before a run
  starts and tells you if they are missing.
- **About 8 GB free on the SD card.** The movies are stored as the raw pixels the
  DS draws, so it can play them without decoding anything.

## Converting

Run `riven-convert-gui`. The wizard asks where Riven is, where your SD card is, and
what to convert, then runs.

On Linux it is an AppImage carrying its own copy of Qt, so no Qt needs to be
installed: `chmod +x riven-convert-gui` and run it. On a distribution without
libfuse2 it will refuse to start, and `./riven-convert-gui --appimage-extract-and-run`
gets past that.

**Expect it to take hours** — it is reading every frame of every movie, and it is
write-bound rather than CPU-bound. It is **resumable**: every file is written under
a temporary name and renamed into place, so a file under its final name is always
complete. Stopping a run costs only the asset in flight, and starting again skips
everything already done.

Tick **"Also copy the game to the card"** on the destination page and point it at
the `riven-nds-port.nds` beside this file — that is why the ROM is in the same
archive. Otherwise copy it onto the card yourself, anywhere you like, and launch it
the way you normally launch homebrew (TWiLight Menu++, your flashcard's menu, or by
dragging it into melonDS).

Everything else goes into `_nds/riven_nds/data/` at the root of the card, and the
game writes its saves, notes and settings back there.

### From the command line

```sh
riven-convert /path/to/riven /path/to/sd --rom riven-nds-port.nds
```

Two stacks are enough to see the game start, and much quicker than all eight:

```sh
riven-convert --stack aspit --stack tspit /path/to/riven /path/to/sd
```

`riven-convert /path/to/riven` with no destination probes your copy and reports what
it found without writing anything. `riven-convert --help` lists the rest — `-j` for
how many movies convert at once, `--no-video` to skip the movies entirely,
`--image` to pack the finished card into one FAT image an emulator can mount, and
`--force` to redo everything.

## Playing

You land on Riven's own main menu; the **Riven** button at the bottom right starts
the game. It is played with the stylus on the bottom screen — **X** zooms the card
to Riven's original resolution, **Y** opens your notebook, **L** puts a picture of
what you are looking at into it, and **START** opens the port's menu.

If you converted with an older build of the converter, just convert again: each
stage stamps the version it wrote and redoes itself when that no longer matches.

## More

Controls in full, troubleshooting and the source:
<https://github.com/Warioware64/Riven-The-Sequel-Of-Myst-NDS-Port>

Apache-2.0; see `LICENSE`. Riven is © Cyan Worlds and is not distributed here in
any form. This is a fan project, not affiliated with or endorsed by Cyan.
