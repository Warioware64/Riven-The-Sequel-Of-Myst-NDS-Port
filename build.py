#!/usr/bin/env python3

# Equivalent of the project Makefile, expressed with architectds_nea_mod.
#
# Usage (after running ./setup-env.sh and `source env/bin/activate`):
#   python build.py          # build
#   python build.py --help   # see all options
#
# NitroFS carries ONLY the UI font and menu backgrounds. Every byte of game
# content lives on the SD card under _nds/riven_nds/data/ and is streamed at
# runtime -- Riven's converted assets are far too large to bake into a ROM.

from architectds import *

nitrofs = NitroFS()

# resources/bg/ is intentionally absent from the build until Riven menu art
# exists -- architectds errors on an empty grit input directory. Re-add
#   nitrofs.add_grit(['resources/bg/'], out_dir='bg/')
# once resources/bg holds a .png + .grit pair.
nitrofs.add_ptexconv(['resources/font/'],   out_dir='font/')
nitrofs.add_bmfont_fnt(['resources/font/'], out_dir='font/')
nitrofs.generate_image()

arm9 = Arm9Binary(
    sourcedirs=['source'],
    # 'thirdparty' puts yas on the include path as <yas/...>, the same spelling
    # the host converter uses -- both sides must agree on the serializer.
    includedirs=['source', 'shared', 'thirdparty'],
    # Add 'RIVEN_PROFILE=1' here to build the frame-time readout in: where the
    # frame went, split between the movie read, the composite, the VRAM upload
    # and the water, reported once a second to the top screen and to debug.log.
    # Off, every one of its entry points is an empty inline (DebugLog.hpp), so
    # the shipped build carries none of it. Then `perf` in the debug console
    # turns it on, and `perf read <path to a .rvid>` times the card itself.
    defines=['NEA_MAXMOD'],
    libs=['NEA', 'mm9', 'nds9', 'dswifi9d_noip'],
    libdirs=[
        '${BLOCKSDS}/libs/libnds',
        '${BLOCKSDS}/libs/maxmod',
        '${BLOCKSDS}/libs/dswifi',
        '${BLOCKSDSEXT}/nitro-engine-advanced',
    ],
    # -D_LITTLE_ENDIAN + force-including yas's endian config first locks yas to
    # little-endian in every TU *before* any <nds.h> can define _BIG_ENDIAN
    # (which yas would otherwise misread as "big-endian"). Order-independent, so
    # it survives files that include <nds.h> before the project headers.
    #
    # This matters more here than it did for Myst: the converter writes the yas
    # archives on the host and the DS reads them back, so a byte-order
    # disagreement between the two would corrupt every card silently.
    cxxflags='-Wall -O2 -std=gnu++26 -D_LITTLE_ENDIAN -include yas/detail/config/endian.hpp',
)

arm9.generate_elf()

nds = NdsRom(
    binaries=[arm9, nitrofs],
    nds_path='riven-nds-port.nds',
    game_icon="Logo.bmp",
    game_title='RIVEN DS',
    game_subtitle='Ported by Warioware64',
    game_author='github.com/Warioware64',
)
nds.generate_nds()

nds.run_command_line_arguments()
