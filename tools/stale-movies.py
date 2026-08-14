#!/usr/bin/env python3
"""List the .rvid files on a card that predate the track-matrix fix.

A tMOV's CODED size is not the size Riven draws it at: QuickTime's track header
carries a matrix whose `a` and `d` terms scale the track, and 79 of a 5-CD
install's 1054 movies use one.  The converter ignored it until now, so those 79
went onto the card at twice or four times their size -- tspit's lever at 2x, the
telescope button at 4x.

Only those 79 files are wrong, and `isUpToDate` redoes any output that is
missing, so deleting exactly them and re-running the converter is minutes rather
than the hours a full video pass costs:

    python3 tools/stale-movies.py /path/to/riven /path/to/card/_nds/riven_nds/data \\
        | xargs -r rm -v

Then convert again.  The run logs "N movie(s) were scaled down by their track
matrix"; N should equal the count this printed.  `--force` is always correct too,
and always slow.

SUPERSEDED.  kVideoVersion is 5 now, and the per-stack `.format` stamp makes the
converter redo every movie on a card written by an older build without being
asked -- which covers the 79 this script was for, and the 583 the overlay-span
fix changed as well.  Running it first saves nothing.

This is a one-off migration aid.  Nothing depends on it, and once every card in
circulation has been redone it can go.
"""
import glob
import os
import struct
import sys

# The archive each stack's movies live in, by the first letter of its name.
STACK = {'a': 'aspit', 'b': 'bspit', 'g': 'gspit', 'j': 'jspit',
         'o': 'ospit', 'p': 'pspit', 'r': 'rspit', 't': 'tspit'}


class Mhk:
    """Just enough Mohawk to walk the resource table."""

    def __init__(self, path):
        f = open(path, 'rb')
        self.f = f
        assert f.read(4) == b'MHWK'
        f.read(4)
        assert f.read(4) == b'RSRC'
        _ver, _comp, _size, abs_off, ft_off, _ft_size = struct.unpack('>HHIIHH', f.read(16))

        f.seek(abs_off)
        _names, type_count = struct.unpack('>HH', f.read(4))
        types = [struct.unpack('>4sHH', f.read(8)) for _ in range(type_count)]

        f.seek(abs_off + ft_off)
        (count,) = struct.unpack('>I', f.read(4))
        self.files = [struct.unpack('>IHBBH', f.read(10))[:3] for _ in range(count)]

        self.res = {}
        for tag, rsrc_off, _name_off in types:
            f.seek(abs_off + rsrc_off)
            (n,) = struct.unpack('>H', f.read(2))
            self.res[tag.decode()] = {rid: idx for rid, idx in
                                      (struct.unpack('>HH', f.read(4)) for _ in range(n))}

    def read(self, tag, rid):
        off, lo, hi = self.files[self.res[tag][rid] - 1]
        self.f.seek(off)
        return self.f.read(lo | (hi << 16))


def atoms(d, off, end):
    """QuickTime atoms in [off, end): uint32 size, char type[4], payload."""
    while off + 8 <= end and off + 8 <= len(d):
        size, typ = struct.unpack('>I4s', d[off:off + 8])
        if size < 8:
            return
        yield typ.decode('latin1', 'replace'), off, size
        off += size


def find(d, path, off=0, end=None):
    end = len(d) if end is None else end
    cur = [(off, end)]
    for want in path:
        cur = [(a + 8, min(a + s, e)) for o, e in cur for t, a, s in atoms(d, o, e) if t == want]
    return cur


def scaled(d):
    """True when the VIDEO track carries a matrix that is not 1:1.

    The video track, not the first one: jspit's tMOV 190 and 335 put their sound
    track first, and a sound track's matrix is always the identity -- reading
    whichever track came first would call those two unscaled and leave them at
    four times their size.
    """
    for o, e in find(d, ['moov', 'trak']):
        hdlr = find(d, ['mdia', 'hdlr'], o, e)
        if not hdlr:
            continue
        h = d[hdlr[0][0]:hdlr[0][1]]
        # 4 bytes of version/flags, the component type, then its subtype.
        if len(h) < 12 or h[8:12] != b'vide':
            continue
        tkhd = find(d, ['tkhd'], o, e)
        if not tkhd:
            continue
        b = d[tkhd[0][0]:tkhd[0][1]]
        if len(b) < 44:
            continue
        # The payload ends with the 36-byte matrix then width and height, in
        # both tkhd versions; `a` and `d` are its first and fifth terms.
        a, _, _, _, dd = struct.unpack('>5i', b[-44:-24])
        return a != 0x10000 or dd != 0x10000
    return False


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    data, card = sys.argv[1], sys.argv[2]

    found = 0
    for path in sorted(glob.glob(os.path.join(data, 'Data', '*.MHK'))):
        stack = STACK.get(os.path.basename(path).lower()[0])
        if stack is None:
            continue
        try:
            mhk = Mhk(path)
        except Exception as e:
            print(f'# skipped {path}: {e}', file=sys.stderr)
            continue
        for rid in sorted(mhk.res.get('tMOV', {})):
            try:
                if not scaled(mhk.read('tMOV', rid)):
                    continue
            except Exception:
                continue
            out = os.path.join(card, 'video', stack, f'{rid}.rvid')
            if os.path.exists(out):
                print(out)
                found += 1

    print(f'# {found} movie(s) to redo', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
