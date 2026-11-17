#!/usr/bin/env python3
"""xboxiso - an all-in-one Xbox disc image tool.

List, extract, create, verify, report and repair Xbox disc images (ISO, XISO,
XGD), and write a correct trim of a full Redump dump.

An Xbox disc lists each directory as a binary tree of entries: two u16 child
offsets (in dwords), the file's sector and size, its attributes, its name. A
binary search tree must be sorted, and every reader relies on that. Some
repackers and trimmers write the tree as a chain instead of a balanced tree:
each entry's only child is the next one. The entries, their order and their
data are all correct, but the entries at the tail sit dozens of levels deep.

A reader that bounds its walk by depth then reports those files as missing.
hakuX's native core stops at depth 64 (native/core/xdvdfs.c), so a 76-entry
chain hides its last 11 entries: Burnout 3: Takedown opens Tracks\\tlist.bin,
gets "not on the disc", and shows its dirty-disc screen.

The repair rewrites only the two child offsets of each entry, so the same
entries, at the same offsets, in the same order, describe the same files as a
balanced tree. Nothing else changes: no file data, no entry placement, no
name, sector, size or attribute, and no offset moves.

That matters for compatibility. A reader that descends the tree comparing
names (the Xbox kernel, xemu) needs the table's own sort order, so the repair
rebuilds over the table's existing in-order sequence instead of sorting it
again, and refuses to write if that sequence would change. A reader that walks
the whole tree (hakuX) needs the walk to stay shallow, which it becomes.

    ./tools/xboxiso.py ROM.iso                     # report, changes nothing
    ./tools/xboxiso.py ROM.iso -o fixed.iso        # write a repaired copy
    ./tools/xboxiso.py ROM.iso --in-place          # repair, keeping ROM.iso.bak
    ./tools/xboxiso.py roms/xbox/*.iso --in-place  # a library
    ./tools/xboxiso.py ROM.iso --trim ROM.trim.iso # compact copy, no file lost
    ./tools/xboxiso.py ROM.iso --rewrite           # repair and drop the tail
    ./tools/xboxiso.py ROM.iso --extract out/      # extract its files
    ./tools/xboxiso.py --create tree/ -o new.iso   # build one from a directory
    ./tools/xboxiso.py ROM.iso --verify            # check entries and extents
    ./tools/xboxiso.py --self-test

Exit status: 0 nothing to do or repaired, 1 usage or I/O error, 2 an image or
a source tree has a problem that stops the operation (a directory the repair
cannot make safe, a tree that cannot be a disc, content past the end of the
image) and was left untouched, 3 a check found a problem. A repair backs the
image up first (<image>.bak, rotated to <image>.bak.1 and so on, never
overwriting an older one) and verifies every table before writing any; --trim
writes beside the destination and renames only a verified image into place.
"""

import argparse
import os
import shutil
import struct
import sys
import tempfile

VERSION = "1.0.0"
SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# The game partition of an extracted XISO, then full XGD1/2/3 dumps: the same
# offsets hakuX and xemu probe.
PART_OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)
PADDING = (0xFFFF, 0xFFFF, 0xFF)
# Shallow enough that a reader stopping at 64, or at 32, finds every entry.
TARGET_DEPTH = 12


def _safe_text(text):
    """Text taken from an image, made safe to print. A name or an XBE title is
    untrusted input: left raw, a control character moves the cursor, recolours
    the terminal or hides the line before it. Escaped, it stays readable."""
    out = []
    for ch in text:
        code = ord(ch)
        if code < 0x20 or 0x7F <= code <= 0x9F:
            out.append("\\x%02X" % code)
        else:
            out.append(ch)
    return "".join(out)


class Entry:
    __slots__ = ("off", "left", "right", "sector", "size", "attr", "name")

    def __init__(self, off, left, right, sector, size, attr, name):
        self.off = off
        self.left = left
        self.right = right
        self.sector = sector
        self.size = size
        self.attr = attr
        self.name = name

    @property
    def is_dir(self):
        return bool(self.attr & 0x10)


class Disc:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        self.part_off = None
        self.root_sector = 0
        self.root_size = 0

    def read(self, off, n):
        self.f.seek(off)
        data = self.f.read(n)
        if len(data) != n:
            raise IOError("short read at 0x%X (%d of %d bytes)" % (off, len(data), n))
        return data

    def locate(self, forced=None):
        for off in (forced,) if forced is not None else PART_OFFSETS:
            if off + 33 * SECTOR > self.size:
                continue
            vd = self.read(off + 32 * SECTOR, SECTOR)
            if vd[:20] == MAGIC and vd[0x7EC:0x7EC + 20] == MAGIC:
                self.part_off = off
                self.root_sector, self.root_size = struct.unpack_from("<II", vd, 20)
                return True
        return False

    def table(self, sector, size):
        """A directory's table, or None when its extent runs past the end of
        the image (a trimmed image can end before a directory it lists)."""
        off = self.part_off + sector * SECTOR
        if off + size > self.size:
            return None
        return bytearray(self.read(off, size))


def parse_table(table):
    """Every entry reachable from dword 0, following both children, with no
    depth limit. Returns the entries, their visit order and the deepest level."""
    entries = {}
    order = []
    deepest = 0
    stack = [(0, 0)]
    seen = set()
    while stack:
        off, depth = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        e = table[off:off + 14]
        left, right = struct.unpack_from("<HH", e, 0)
        sector, size = struct.unpack_from("<II", e, 4)
        attr, nlen = e[12], e[13]
        if (left, right, attr) == PADDING:
            continue
        if off + 14 + nlen > len(table):
            continue
        name = bytes(table[off + 14:off + 14 + nlen]).decode("latin-1")
        entries[off] = Entry(off, left, right, sector, size, attr, name)
        order.append(entries[off])
        deepest = max(deepest, depth)
        for child in (left, right):
            if child and child != 0xFFFF:
                stack.append((child * 4, depth + 1))
    return entries, order, deepest


def inorder(entries):
    """The entries in tree order (left, node, right), without a depth limit.
    A sorted tree's in-order sequence is its sort order, so this is the order
    every reader considers sorted, whatever collation the disc was written
    with. Rebuilding over it leaves that order untouched."""
    out = []
    stack = [(0, False)]
    seen = set()
    while stack:
        off, visited = stack.pop()
        e = entries.get(off)
        if e is None:
            continue
        if visited:
            out.append(e)
            continue
        if off in seen:
            continue
        seen.add(off)
        right = e.right * 4 if e.right and e.right != 0xFFFF else None
        left = e.left * 4 if e.left and e.left != 0xFFFF else None
        if right is not None:
            stack.append((right, False))
        stack.append((off, True))
        if left is not None:
            stack.append((left, False))
    return out


def walk_lookup(table, name, cap):
    """A reader that walks the whole table and stops at a depth (hakuX)."""
    # Upper-case the latin-1 bytes, never the decoded str: str.upper() maps
    # "ü" to "Ü" (0xDC) while bytes.upper() only folds ASCII, so a name with a
    # latin-1 character above ASCII could never match itself. That made --verify
    # call such an entry unreachable and stopped a repair that was sound.
    target = name.encode("latin-1").upper()
    stack = [(0, 0)]
    seen = set()
    while stack:
        off, depth = stack.pop()
        if off in seen or off + 14 > len(table) or depth > cap:
            continue
        seen.add(off)
        e = table[off:off + 14]
        left, right = struct.unpack_from("<HH", e, 0)
        attr, nlen = e[12], e[13]
        if (left, right, attr) == PADDING:
            continue
        if off + 14 + nlen <= len(table):
            stored = bytes(table[off + 14:off + 14 + nlen])
            if stored.upper() == target:
                return off
        for child in (right, left):
            if child and child != 0xFFFF:
                stack.append((child * 4, depth + 1))
    return None


def unreachable_within(table, cap):
    """The names a reader that walks the whole table but stops at a depth
    cannot reach, in one traversal instead of a full walk per entry (the repair
    has to prove this for every entry, and a directory can hold thousands).
    Same answer as walk_lookup(name, cap) is None for each of them."""
    names = []
    stack = [(0, 0)]
    seen = set()
    while stack:
        off, depth = stack.pop()
        if off in seen or off + 14 > len(table):
            continue
        seen.add(off)
        e = table[off:off + 14]
        left, right = struct.unpack_from("<HH", e, 0)
        attr, nlen = e[12], e[13]
        if (left, right, attr) == PADDING:
            continue
        if off + 14 + nlen > len(table):
            continue
        if depth > cap and off + 14 + nlen <= len(table):
            names.append(bytes(table[off + 14:off + 14 + nlen]).decode("latin-1"))
        for child in (right, left):
            if child and child != 0xFFFF:
                stack.append((child * 4, depth + 1))
    return names


def descend_lookup(table, name, case_insensitive=True):
    """A reader that searches by descending the tree and comparing names the
    way the Xbox kernel and xemu do, which needs the tree sorted."""
    target = name.encode("latin-1").upper() if case_insensitive else name.encode("latin-1")
    off = 0
    for _ in range(len(table)):
        if off + 14 > len(table):
            return None
        e = table[off:off + 14]
        left, right = struct.unpack_from("<HH", e, 0)
        attr, nlen = e[12], e[13]
        if (left, right, attr) == PADDING or off + 14 + nlen > len(table):
            return None
        stored = bytes(table[off + 14:off + 14 + nlen])
        if (stored.upper() if case_insensitive else stored) == target:
            return off
        child = left if target < (stored.upper() if case_insensitive else stored) else right
        if not child or child == 0xFFFF:
            return None
        off = child * 4
    return None


def balanced_links(entries):
    """Child offsets that make the entries a balanced tree rooted where the
    tree is rooted now (dword 0), keeping every entry where it is and the
    table's in-order sequence unchanged. Splitting by position in that
    sequence, never by comparing names, keeps the repair independent of which
    collation a reader uses."""
    ordered = inorder(entries)
    root_at = next((i for i, e in enumerate(ordered) if e.off == 0), None)
    if root_at is None:
        return None, None
    links = {}

    def build(nodes):
        if not nodes:
            return 0
        mid = len(nodes) // 2
        node = nodes[mid]
        links[node.off] = (build(nodes[:mid]), build(nodes[mid + 1:]))
        return node.off // 4

    links[0] = (build(ordered[:root_at]), build(ordered[root_at + 1:]))
    return links, ordered


def apply_links(table, links, entries):
    """Writes only the two child offsets of every linked entry: 4 bytes each."""
    touched = 0
    for off, (left, right) in links.items():
        e = entries[off]
        if (e.left, e.right) != (left, right):
            struct.pack_into("<HH", table, off, left, right)
            touched += 1
    return touched


def depth_of(table, links):
    depth = 0
    stack = [(0, 0)]
    seen = set()
    while stack:
        off, d = stack.pop()
        if off in seen or off not in links:
            continue
        seen.add(off)
        depth = max(depth, d)
        for child in links[off]:
            if child and child != 0xFFFF:
                stack.append((child * 4, d + 1))
    return depth


def repair_table(table, cap):
    """The repaired bytes for one table, or (None, reason)."""
    entries, order, deepest = parse_table(table)
    links, original = balanced_links(entries)
    if links is None:
        return None, "no entry at dword 0", deepest, len(order)
    patch = bytearray(table)
    touched = apply_links(patch, links, entries)
    if not touched:
        return bytes(table), None, deepest, len(order)
    patched, patched_order, _patched_deep = parse_table(patch)
    if [e.off for e in inorder(patched)] != [e.off for e in original]:
        return None, "the repair would change the table's order", deepest, len(order)
    too_deep = unreachable_within(patch, cap)
    if too_deep:
        return None, "%s would still be unreachable" % too_deep[0], deepest, len(order)
    for e in patched_order:
        if descend_lookup(patch, e.name) is None:
            return None, "%s would not be found by a descending search" % e.name, deepest, len(order)
    return patch, None, deepest, len(order)


def survey(disc, cap):
    """Every directory table: (sector, size, table, entries, order, deepest, path)."""
    dirs = []
    past_end = []
    pending = [(disc.root_sector, disc.root_size, "\\")]
    seen = set()
    while pending:
        sector, size, path = pending.pop()
        if (sector, size) in seen or not size:
            continue
        seen.add((sector, size))
        table = disc.table(sector, size)
        if table is None:
            past_end.append(path)
            continue
        entries, order, deepest = parse_table(table)
        dirs.append((sector, size, table, entries, order, deepest, path))
        for e in order:
            if e.is_dir and e.size:
                pending.append((e.sector, e.size, path + e.name + "\\"))
    return dirs, past_end


def used_end(disc, dirs):
    """The first sector past every byte the filesystem refers to, so an image
    can be cut there without losing anything."""
    end = max(34, disc.root_sector + (disc.root_size + SECTOR - 1) // SECTOR)
    for sector, size, table, entries, order, deep, dpath in dirs:
        end = max(end, sector + (size + SECTOR - 1) // SECTOR)
        for e in order:
            end = max(end, e.sector + (e.size + SECTOR - 1) // SECTOR)
    return end


def table_patches(dirs, args):
    """The repaired bytes of every table that needs one, all verified in memory
    before a single byte is written: a table that cannot be repaired safely
    stops the repair with nothing on disk changed. Returns (patches, error)."""
    patches = []
    for sector, size, table, entries, order, deep, dpath in dirs:
        if deep <= TARGET_DEPTH:
            continue
        patch, reason, _deep, _count = repair_table(table, args.cap)
        if patch is None:
            return None, "%s: %s" % (dpath, reason)
        if patch != table:
            patches.append((sector, patch))
    return patches, None


def write_patches(path, part_off, patches):
    """Writes verified table patches into an image already on disk."""
    with open(path, "r+b") as out:
        for sector, patch in patches:
            out.seek(part_off + sector * SECTOR)
            out.write(patch)
    return len(patches)


def backup_path(path):
    """A backup name that never overwrites an older one (image.iso.bak,
    image.iso.bak.1, ...), so a second run cannot destroy the last good copy."""
    base = path + ".bak"
    if not os.path.exists(base):
        return base
    n = 1
    while os.path.exists("%s.%d" % (base, n)):
        n += 1
    return "%s.%d" % (base, n)


def trim_image(path, out_path, args):
    """Writes a compact image: the game partition alone (so every reader finds
    XDVDFS at +0x0), cut to the last sector the filesystem uses, with its
    tables repaired. Loses no file, unlike cutting files out."""
    disc = Disc(path)
    tmp_path = out_path + ".part"
    promoted = False
    keep_tmp = False
    try:
        if not disc.locate(args.part_offset):
            print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
            return 2
        dirs, past_end = survey(disc, args.cap)
        if past_end:
            print("%s: %d directory table(s) lie past the end of this image; "
                  "refusing to trim an image that is already short"
                  % (path, len(past_end)), file=sys.stderr)
            return 2
        end = used_end(disc, dirs)
        keep = end * SECTOR
        src_off = disc.part_off
        src_entries = sum(len(d[3]) for d in dirs)
        if src_off + keep > disc.size:
            print("%s: last used sector is past the end of the image" % path, file=sys.stderr)
            return 2
        print("image        %s (%.2f GiB), game partition +0x%X"
              % (path, disc.size / 2**30, disc.part_off))
        print("content      %d entries, last used sector %d (%.2f GiB)"
              % (src_entries, end, keep / 2**30))
        # Written beside the destination and renamed only once it verifies, so
        # a failed or interrupted trim can never leave a broken image under the
        # name the caller asked for, and an existing file there is replaced
        # only after the new one is known good.
        if os.path.exists(tmp_path):
            if os.path.isdir(tmp_path):
                print("%s: is a directory, so a stray file from an interrupted trim cannot "
                      "be cleared out of the way" % tmp_path, file=sys.stderr)
                return 2
            os.unlink(tmp_path)
        with open(tmp_path, "wb") as out:
            left = keep
            pos = src_off
            while left:
                chunk = min(8 << 20, left)
                out.write(disc.read(pos, chunk))
                pos += chunk
                left -= chunk
        new = Disc(tmp_path)
        try:
            if not new.locate():
                print("trimmed image has no readable volume descriptor at +0x0", file=sys.stderr)
                return 2
            copied, past_end = survey(new, args.cap)
            if past_end or sum(len(d[3]) for d in copied) != src_entries:
                print("trimmed image lost content, refusing to keep it", file=sys.stderr)
                return 2
            patches, error = table_patches(copied, args)
            if error:
                print("  %s, refusing to keep the trimmed image" % error, file=sys.stderr)
                return 2
            repaired = write_patches(tmp_path, new.part_off, patches)
        finally:
            new.f.close()
        # Gated on what the trimmed image is now, never on what it was meant to
        # become: a readback that cannot even be surveyed is a refusal, not an
        # empty survey whose zero deepest table reads as success.
        problems, summary = check_image(tmp_path, args.cap)
        if summary is None or problems:
            for line in problems or []:
                print(line, file=sys.stderr)
            print("trimmed image does not verify, refusing to keep it", file=sys.stderr)
            return 2
        if summary["entries"] != src_entries:
            print("trimmed image has %d entries, not the %d it copied, refusing to keep it"
                  % (summary["entries"], src_entries), file=sys.stderr)
            return 2
        if summary["deepest"] > TARGET_DEPTH:
            print("trimmed image still has a table %d level(s) deep, refusing to keep it"
                  % summary["deepest"], file=sys.stderr)
            return 2
        try:
            os.replace(tmp_path, out_path)  # the verified image takes its name here
        except OSError as exc:
            print("%s: %s; the verified image stays at %s" % (out_path, exc, tmp_path), file=sys.stderr)
            keep_tmp = True
            return 2
        promoted = True
        print("trimmed      %s (%.2f GiB, was %.2f GiB)"
              % (out_path, os.path.getsize(out_path) / 2**30, disc.size / 2**30))
        print("kept         %d entries, partition at +0x0, %d table(s) rebalanced, deepest %d level(s)"
              % (summary["entries"], repaired, summary["deepest"]))
        return 0
    finally:
        disc.f.close()
        if not promoted and not keep_tmp and os.path.exists(tmp_path):
            os.unlink(tmp_path)


def survey_image(path, cap):
    """survey() of an image this opens and always closes."""
    disc = Disc(path)
    try:
        if not disc.locate():
            return None, None
        return survey(disc, cap)
    finally:
        disc.f.close()


def process(path, args):
    disc = Disc(path)
    try:
        if not disc.locate(args.part_offset):
            print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
            return 2
        print("image        %s (%.2f GiB)" % (path, disc.size / 2**30))
        print("partition    +0x%X, root sector %d (%d bytes)"
              % (disc.part_off, disc.root_sector, disc.root_size))
        dirs, past_end = survey(disc, args.cap)
        total_entries = sum(len(d[3]) for d in dirs)
        deepest = max([d[5] for d in dirs] or [0])
        bad = [d for d in dirs if d[5] > TARGET_DEPTH]
        print("directories  %d tables, %d entries, deepest %d level(s)"
              % (len(dirs), total_entries, deepest))
        if past_end:
            print("past end     %d table(s) lie beyond the image (a trimmed image ends before them)"
                  % len(past_end))

        hidden = 0
        if bad:
            print("\ntables a depth-limited reader cannot search completely:")
            for sector, size, table, entries, order, deep, dpath in sorted(bad, key=lambda d: -d[5]):
                over = [e for e in order if walk_lookup(table, e.name, args.cap) is None]
                hidden += len(over)
                print("  %-34s sector %-8d %3d entries, deepest %3d, %d unreachable past depth %d"
                      % (dpath[:34], sector, len(order), deep, len(over), args.cap))
                if args.verbose:
                    for e in over[:8]:
                        print("      %s (dword %d)" % (e.name, e.off // 4))
        if not bad:
            print("\nnothing to repair: every table is searchable within depth %d" % args.cap)
            if args.output:
                shutil.copyfile(path, args.output)
                print("copied unchanged to %s" % args.output)
            return 0
        if not args.output and not args.in_place:
            print("\nre-run with -o fixed.iso to write a repaired copy, or --in-place to repair it "
                  "in place (keeping <image>.bak)")
            return 0

        patches, error = table_patches(dirs, args)
        if error:
            print("  %s, nothing written" % error, file=sys.stderr)
            return 2
        out_path = path if args.in_place else args.output
        bk = None
        try:
            if args.in_place:
                if not args.no_backup:
                    bk = backup_path(path)
                    shutil.copyfile(path, bk)
                    print("\nbackup       %s" % bk)
            else:
                shutil.copyfile(path, out_path)
                print("\ncopy         %s" % out_path)
        except (IOError, OSError) as exc:
            # A copy that cannot be made (a full disk is the usual reason) has
            # touched nothing yet: report it with the documented exit status
            # instead of a traceback out of shutil.
            print("%s: %s; the image is unchanged" % (path, exc), file=sys.stderr)
            if bk:
                _discard(bk)
            return 2

        try:
            repaired = write_patches(out_path, disc.part_off, patches)
        except (IOError, OSError) as exc:
            # A write that fails half way leaves some tables patched. Say that,
            # and name the copy to restore, instead of announcing the backup and
            # leaving the caller to discover the rest later.
            print("%s: %s" % (out_path, exc), file=sys.stderr)
            if bk:
                print("the image may be partially patched; restore it from %s" % bk, file=sys.stderr)
            else:
                print("the image may be partially patched, and no backup was kept", file=sys.stderr)
            return 2
        if args.verbose:
            for sector, size, table, entries, order, deep, dpath in bad:
                if any(sector == p[0] for p in patches):
                    print("  %-34s %3d entries: depth %3d -> repaired"
                          % (_safe_text(dpath)[:34], len(order), deep))

        # Read the result back and gate the report on it: a repair says what the
        # image is now, not what it was meant to become. An image that cannot be
        # surveyed at all is a failure, never an empty survey that passes.
        problems, summary = check_image(out_path, args.cap)
        if summary is None or problems:
            for line in problems or []:
                print(line, file=sys.stderr)
            print("%s: the repaired image does not verify%s"
                  % (out_path, "; restore it from " + bk if bk else ", and no backup was kept"),
                  file=sys.stderr)
            return 3
        # Compare each directory's in-order sequence by its path, never by the
        # survey's discovery order: rebalancing changes the order the walk finds
        # subdirectories in, while the sequence that matters is unchanged.
        orders = {d[6]: [e.off for e in inorder(d[3])] for d in dirs}
        if {d[6]: [e.off for e in inorder(d[3])] for d in summary["dirs"]} != orders:
            print("%s: the repaired image's table order differs from the disc's" % out_path,
                  file=sys.stderr)
            return 3
        print("\nrepaired     %d table(s) in %d table(s) of the image, deepest is now %d level(s)"
              % (repaired, len(dirs), summary["deepest"]))
        print("verified     every entry findable by a reader that stops at depth %d" % args.cap)
        print("verified     every entry findable by a reader that descends the tree comparing names")
        print("verified     each table's entry order is the disc's own, unchanged")
        return 0
    finally:
        disc.f.close()


# --- self-test -------------------------------------------------------------


def build_table(names, chain=True, pad=False):
    """A directory table holding names in the given order: as a chain (each
    entry's right child is the next) or as a balanced tree rooted at the entry
    in dword 0, which is where every reader starts."""
    table = bytearray(SECTOR)
    slots = []
    off = 0
    for name in names:
        nlen = len(name)
        used = (14 + nlen + 3) & ~3
        slots.append((off, name, used))
        off += used
    assert off <= SECTOR, "self-test fixture does not fit"
    children = {}
    if chain:
        for i, (o, _n, _u) in enumerate(slots):
            children[o] = (0, slots[i + 1][0] // 4 if i + 1 < len(slots) else 0)
    else:
        def build(lo, hi):
            if lo >= hi:
                return 0
            mid = (lo + hi) // 2
            o = slots[mid][0]
            children[o] = (build(lo, mid), build(mid + 1, hi))
            return o // 4

        # dword 0 is the root, so the rest of the table hangs off its right
        # child, split evenly: depth stays logarithmic and the table's own
        # order is the in-order sequence.
        children[0] = (0, build(1, len(slots)))
    for o, name, used in slots:
        left, right = children[o]
        struct.pack_into("<HH", table, o, left, right)
        struct.pack_into("<II", table, o + 4, 100 + o // 4, used)
        table[o + 12] = 0x20
        table[o + 13] = len(name)
        table[o + 14:o + 14 + len(name)] = name.encode("latin-1")
    if pad:
        last = slots[-1][0] + slots[-1][2]
        struct.pack_into("<HH", table, last, 0xFFFF, 0xFFFF)
        table[last + 4] = 0xFF
    return bytes(table)


def balanced_fixture(names):
    """A balanced tree over sorted names, rooted at dword 0: the median goes
    first (the root), then the lower half, then the upper half."""
    names = sorted(names)
    mid = len(names) // 2
    lower, root_name, upper = names[:mid], names[mid], names[mid + 1:]
    table = bytearray(SECTOR)
    slots = []
    off = 0
    for name in [root_name] + lower + upper:
        used = (14 + len(name) + 3) & ~3
        slots.append((off, name, used))
        off += used
    assert off <= SECTOR, "self-test fixture does not fit"
    children = {}

    def build(lo, hi):
        if lo >= hi:
            return 0
        m = (lo + hi) // 2
        o = slots[m][0]
        children[o] = (build(lo, m), build(m + 1, hi))
        return o // 4

    base = 1
    children[0] = (build(base, base + len(lower)), build(base + len(lower), len(slots)))
    for o, name, used in slots:
        left, right = children[o]
        struct.pack_into("<HH", table, o, left, right)
        struct.pack_into("<II", table, o + 4, 100 + o // 4, used)
        table[o + 12] = 0x20
        table[o + 13] = len(name)
        table[o + 14:o + 14 + len(name)] = name.encode("latin-1")
    return bytes(table)


def self_test(args):
    fails = 0

    def check(cond, what):
        nonlocal fails
        if not cond:
            print("FAIL %s" % what)
            fails += 1

    names = ["f%02d.bin" % i for i in range(75)] + ["target.bin"]
    chain = build_table(names, chain=True)
    balanced = balanced_fixture(names)

    # A chain of 76 entries is far deeper than 64: hakuX cannot see its tail.
    check(walk_lookup(chain, "target.bin", 64) is None, "a chain is unsearchable past the depth cap")
    check(walk_lookup(chain, "f00.bin", 64) == 0, "the first entry of a chain is found")

    # The repair makes every entry reachable, at a shallow depth.
    patch, reason, deepest, count = repair_table(chain, args.cap)
    check(patch is not None, "a chain is repairable (%s)" % reason)
    if patch:
        check(deepest > 64, "the fixture really is deep (%d)" % deepest)
        after = balanced_links(parse_table(patch)[0])[0]
        check(depth_of(patch, after) <= TARGET_DEPTH + 2, "the repaired tree is shallow (%d)"
              % depth_of(patch, after))
        for name in names:
            check(walk_lookup(patch, name, 8) is not None, "a walk finds %s" % name)
            check(descend_lookup(patch, name) is not None, "a descending search finds %s" % name)
        check([e.off for e in inorder(parse_table(patch)[0])] == [e.off for e in inorder(parse_table(chain)[0])],
              "the repair keeps the table's order")
        # Only child offsets may differ.
        changed = [i for i in range(len(chain)) if chain[i] != patch[i]]
        allowed = set()
        for e in parse_table(chain)[0].values():
            allowed.update((e.off, e.off + 1, e.off + 2, e.off + 3))
        check(all(i in allowed for i in changed), "only child offsets change (%d bytes)" % len(changed))
        # Idempotent.
        again, reason2, _d, _c = repair_table(patch, args.cap)
        check(again is not None and again == patch, "a repaired table repairs to itself")
        # The repair now proves reachability in one traversal instead of a walk
        # per entry, so the bulk answer has to match the per-entry one.
        walked_missing = [n for n in names if walk_lookup(chain, n, 64) is None]
        check(sorted(unreachable_within(chain, 64)) == sorted(walked_missing),
              "the bulk reachability check agrees with a walk per entry (%d)" % len(walked_missing))
        check(unreachable_within(chain, 4096) == [], "nothing is out of reach without a depth cap")
        check(unreachable_within(patch, args.cap) == [],
              "nothing is out of reach in a repaired table")

    # Text and names taken from an image are untrusted: control characters are
    # escaped before printing, and names that cannot be host paths are refused.
    check(_safe_text("A\x1b[2JB\x00") == "A\\x1B[2JB\\x00", "control characters are escaped")
    check(_safe_text("Burnout 3") == "Burnout 3", "ordinary text is untouched")
    check(_host_name("roms/a.bin") == "roms/a.bin", "a plain disc path is a host path")
    for bad in ("a:b", "a\x1bb", "a\x00b"):
        try:
            _host_name(bad)
            check(False, "%r is refused as a host name" % bad)
        except ValueError:
            check(True, "%r is refused as a host name" % bad)

    # A table that is already balanced needs no repair, and stays valid.
    check(walk_lookup(balanced, "target.bin", 64) is not None, "a balanced table is searchable")
    check(descend_lookup(balanced, "target.bin") is not None, "a balanced table is searchable by descent")
    check([e.name for e in inorder(parse_table(balanced)[0])] ==
          [e.name for e in inorder(parse_table(chain)[0])], "both fixtures sort the same")
    check(repair_table(balanced, args.cap)[0] == balanced, "a balanced table needs no repair")

    # Padding marker, a table too short to hold an entry, and a cycle.
    # A latin-1 name has to match itself. Folding is ASCII-only on both sides,
    # so the "ü" byte is compared as written and only ASCII case varies.
    latin = build_table(["f00.bin", "Gigi R\xfcf.res"], chain=True)
    check(walk_lookup(latin, "Gigi R\xfcf.res", 64) is not None,
          "a latin-1 name is found by a walk")
    check(descend_lookup(latin, "Gigi R\xfcf.res") is not None,
          "a latin-1 name is found by a descending search")
    check(walk_lookup(latin, "GIGI R\xfcf.RES", 64) is not None,
          "an ASCII upper-case query still finds it")

    check(walk_lookup(build_table(names[:3], pad=True), "f01.bin", 64) is not None, "padding is skipped")
    check(parse_table(b"\0" * 8)[1] == [], "a short table yields no entries")
    cyc = bytearray(build_table(names[:5]))
    struct.pack_into("<H", cyc, 2, 0)      # f00's right child points back at the root
    struct.pack_into("<H", cyc, 0, 0)      # and its left child too
    check(parse_table(bytes(cyc))[1] != [], "a cycle does not hang the walker")
    check(repair_table(bytes(cyc), args.cap)[0] is not None, "a cycle does not hang the repair")

    # A source tree with a symlink is refused, so a link loop cannot recurse.
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "d"))
        with open(os.path.join(tmp, "d", "f.bin"), "wb") as f:
            f.write(b"x")
        os.symlink(tmp, os.path.join(tmp, "d", "loop"))
        try:
            _node(tmp, "", True)
            check(False, "a symlink in the source tree is refused")
        except ValueError:
            check(True, "a symlink in the source tree is refused")

    print("self-test: %s" % ("all checks passed" if not fails else "%d check(s) failed" % fails))
    return 0 if not fails else 1


# --- reading a disc ---------------------------------------------------------


def read_file(disc, entry, off=0, length=None):
    """A file's bytes from the image: the entry's own sector and size bound it."""
    n = entry.size - off if length is None else length
    if off < 0 or n < 0 or off + n > entry.size:
        raise IOError("read past the end of %s" % entry.name)
    return disc.read(disc.part_off + entry.sector * SECTOR + off, n)


def catalog(disc, cap):
    """path -> Entry for everything on the disc; a directory's key ends in \\."""
    out = {}
    dirs, past_end = survey(disc, cap)
    for _sector, _size, _table, _entries, order, _deep, dpath in dirs:
        for e in order:
            out[dpath + e.name] = e
    return out, past_end


def _sort_key(path):
    return path.upper()


def safe_relpath(path):
    """A disc path as a host path that cannot leave the destination directory."""
    parts = [p for p in path.replace("\\", "/").split("/") if p not in ("", ".", "..")]
    return os.path.join(*parts) if parts else ""


def _host_name(rel):
    """The host path for a disc path, or ValueError for a name that cannot be
    one: a control character would drive the terminal that prints it, and a
    colon starts a drive letter on Windows, where the name would leave the
    destination directory. Both come from the image, so neither is trusted."""
    for ch in rel:
        code = ord(ch)
        if code < 0x20 or code == 0x7F or ch == ":":
            raise ValueError("%r cannot appear in a name written to disk (%s)"
                             % (ch, _safe_text(rel)))
    return rel


def cmd_list(path, args):
    disc = Disc(path)
    try:
        if not disc.locate(args.part_offset):
            print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
            return 2
        entries, _past = catalog(disc, args.cap)
        files = [p for p in entries if not entries[p].is_dir]
        print("%s" % path)
        print("  %d entries, %d files, %.2f GiB" % (len(entries), len(files),
                                                    sum(entries[p].size for p in files) / 2**30))
        for p in sorted(entries, key=_sort_key):
            e = entries[p]
            if e.is_dir:
                print("  %-58s      dir" % _safe_text(p))
            else:
                print("  %-58s %9d" % (_safe_text(p), e.size))
        return 0
    finally:
        disc.f.close()


def cmd_extract(path, dest, args):
    disc = Disc(path)
    try:
        if not disc.locate(args.part_offset):
            print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
            return 2
        entries, _past = catalog(disc, args.cap)
        try:
            os.makedirs(dest, exist_ok=True)
        except OSError as exc:
            print("%s: %s: cannot create %s" % (path, exc, dest), file=sys.stderr)
            return 2
        count = 0
        written = 0
        limit = (disc.size - disc.part_off) // SECTOR
        for p in sorted(entries, key=_sort_key):
            e = entries[p]
            rel = safe_relpath(p)
            if not rel:
                continue
            try:
                target = os.path.join(dest, _host_name(rel))
            except ValueError as exc:
                print("%s: %s; stopped after %d file(s)" % (path, exc, count), file=sys.stderr)
                return 2
            # Say so before makedirs() creates a tree for a file the image
            # cannot supply, rather than leaning on a short read later.
            last = e.sector + (e.size + SECTOR - 1) // SECTOR
            if last > limit:
                print("%s: %s ends at sector %d, past the end of the image (%d); stopped "
                      "after %d file(s)" % (path, _safe_text(rel), last, limit, count),
                      file=sys.stderr)
                return 2
            try:
                if e.is_dir:
                    os.makedirs(target, exist_ok=True)
                    continue
                parent = os.path.dirname(target)
                if parent:
                    os.makedirs(parent, exist_ok=True)
                with open(target, "wb") as out:
                    off = 0
                    while off < e.size:
                        chunk = read_file(disc, e, off, min(8 << 20, e.size - off))
                        out.write(chunk)
                        off += len(chunk)
                wrote = os.path.getsize(target)
                if wrote != e.size:
                    raise IOError("wrote %d of %d bytes" % (wrote, e.size))
            except (OSError, ValueError) as exc:
                # Leave nothing that looks like a finished file, and say where it
                # stopped: an extract that fails half way is not a quiet success.
                try:
                    if not e.is_dir:
                        os.unlink(target)
                except OSError:
                    pass
                print("%s: %s; stopped at %s after %d file(s), and its partial copy was removed"
                      % (path, exc, _safe_text(rel), count), file=sys.stderr)
                return 2
            count += 1
            written += wrote
            if args.verbose:
                print("  %s" % rel)
        print("extracted %d files, %.2f GiB, into %s" % (count, written / 2**30, dest))
        return 0
    finally:
        disc.f.close()


XBE_MAGIC = b"XBEH"


def xbe_info(disc, entry):
    """Title, title id and layout from an XBE's own header and certificate."""
    head = bytearray(read_file(disc, entry, 0, min(0x4000, entry.size)))
    if len(head) < 0x120 or bytes(head[:4]) != XBE_MAGIC:
        return None

    def u32(o):
        return struct.unpack_from("<I", head, o)[0]

    base, cert, sections = u32(0x104), u32(0x118), u32(0x11C)
    info = {"base": base, "entry": u32(0x128), "sections": sections, "image": u32(0x10C),
            "tls": u32(0x12C)}
    off = cert - base if cert >= base else cert
    # The certificate's fields run to +0xAF (version at +0xAC), so the header
    # must hold that much: a truncated XBE otherwise raises struct.error out of
    # a function whose whole job is to read untrusted headers.
    if 0 <= off and off + 0xB0 <= len(head):
        info["title_id"] = u32(off + 0x08)
        words = head[off + 0x0C:off + 0x34].decode("utf-16-le", "replace")
        info["title"] = words.split("\0")[0]
        info["region"] = u32(off + 0xA0)
        info["version"] = u32(off + 0xAC)
        info["disk"] = u32(off + 0xA8)
    return info


REGIONS = ((1, "US"), (2, "Japan"), (4, "Europe"), (0x80000000, "debug"))


def _regions(bits):
    named = [name for bit, name in REGIONS if bits & bit]
    if bits & ~sum(bit for bit, _n in REGIONS) & 0xFFFFFFFF:
        named.append("0x%X" % bits)
    return ", ".join(named) or "none"


def cmd_info(path, args):
    disc = Disc(path)
    try:
        if not disc.locate(args.part_offset):
            print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
            return 2
        dirs, past_end = survey(disc, args.cap)
        entries, _past = catalog(disc, args.cap)
        files = [p for p in entries if not entries[p].is_dir]
        end = used_end(disc, dirs)
        print("image        %s (%.2f GiB)" % (path, disc.size / 2**30))
        print("partition    +0x%X, root sector %d (%d bytes)"
              % (disc.part_off, disc.root_sector, disc.root_size))
        print("filesystem   %d tables, %d entries, %d files, %.2f GiB of data"
              % (len(dirs), len(entries), len(files), sum(entries[p].size for p in files) / 2**30))
        print("depth        deepest table %d level(s)" % max([d[5] for d in dirs] or [0]))
        trailing = disc.size // SECTOR - (disc.part_off // SECTOR + end)
        print("used         ends at sector %d; %d sector(s) after it (%.2f GiB unused in place)"
              % (disc.part_off // SECTOR + end, trailing, trailing * SECTOR / 2**30))
        if past_end:
            print("past end     %d table(s) lie beyond the image" % len(past_end))
        def _leaf(path):
            return path.replace("\\", "/").rsplit("/", 1)[-1].upper()

        xbe = next((entries[p] for p in files if _leaf(p) == "DEFAULT.XBE"), None)
        if xbe is None:
            print("xbe          no default.xbe on the disc")
        else:
            info = xbe_info(disc, xbe)
            if info is None:
                print("xbe          default.xbe is not an XBE")
            else:
                print("title        %s" % _safe_text(info.get("title", "?")))
                print("title id     %08X" % info.get("title_id", 0))
                print("regions      %s, disk %d, version %d.%d" % (
                    _regions(info.get("region", 0)), info.get("disk", 0),
                    info.get("version", 0) >> 24, info.get("version", 0) & 0xFFFFFF))
                entry = info["entry"]
                reachable = info["base"] <= entry < info["base"] + info["image"]
                print("base         %08X, %d section(s), image %.2f MiB, tls %08X"
                      % (info["base"], info["sections"], info["image"] / 2**20, info["tls"]))
                print("entry        %08X%s" % (entry, "" if reachable else " (outside the image)"))
        return 0
    finally:
        disc.f.close()


def check_image(path, cap):
    """(problems, summary) for an image read back from disk, or (None, None)
    when it has no readable volume. Every extent inside the image, no overlaps,
    every entry reachable within cap and found by a descending search. An empty
    problems list means it verifies; the summary says what was found."""
    disc = Disc(path)
    try:
        if not disc.locate():
            return None, None
        dirs, past_end = survey(disc, cap)
        entries = {}
        for _sector, _size, _table, _e, order, _deep, dpath in dirs:
            for e in order:
                entries[dpath + e.name] = e
        problems = []
        limit = (disc.size - disc.part_off) // SECTOR
        spans = []
        for p, e in entries.items():
            if e.is_dir:
                continue
            if e.sector + (e.size + SECTOR - 1) // SECTOR > limit:
                problems.append("past end     %s: ends past the image" % _safe_text(p))
            spans.append((e.sector, e.sector + (e.size + SECTOR - 1) // SECTOR, p))
        spans.sort()
        for (s1, e1, n1), (s2, e2, n2) in zip(spans, spans[1:]):
            if s2 < e1:
                problems.append("overlap      %s and %s share sectors %d..%d"
                                % (_safe_text(n1), _safe_text(n2), s2, min(e1, e2) - 1))
        for _sector, _size, table, _e, order, deep, dpath in dirs:
            if deep > cap:
                problems.append("deep         %s: deepest entry %d > %d, hidden by a "
                                "depth-limited reader" % (_safe_text(dpath), deep, cap))
            for e in order:
                if walk_lookup(table, e.name, cap) is None:
                    problems.append("unreachable  %s%s" % (_safe_text(dpath), _safe_text(e.name)))
                elif descend_lookup(table, e.name) is None:
                    problems.append("unsorted     %s%s: not found by a descending search"
                                    % (_safe_text(dpath), _safe_text(e.name)))
        if past_end:
            problems.append("past end     %d table(s) lie beyond the image" % len(past_end))
        summary = {"entries": len(entries), "tables": len(dirs), "dirs": dirs,
                   "deepest": max([d[5] for d in dirs] or [0])}
        return problems, summary
    finally:
        disc.f.close()


def cmd_verify(path, args):
    problems, summary = check_image(path, args.cap)
    if summary is None:
        print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
        return 2
    for line in problems:
        print(line, file=sys.stderr)
    print("%s: %d entries, %d table(s), %s"
          % (path, summary["entries"], summary["tables"],
             "no problems found" if not problems else "%d problem(s)" % len(problems)))
    return 0 if not problems else 3


# --- writing a disc ---------------------------------------------------------


# A source tree that nests deeper than this, or that contains a symlink, cannot
# be a disc: refusing both keeps a link loop from recursing for ever and keeps
# the image to what the source tree really holds.
MAX_TREE_DEPTH = 64


def _node(src, name, upper, depth=0):
    full = os.path.join(src, name) if name else src
    if name and os.path.islink(full):
        raise ValueError("%s is a symlink; a disc image holds only files and directories" % full)
    if depth > MAX_TREE_DEPTH:
        raise ValueError("%s: directories nested more than %d deep" % (full, MAX_TREE_DEPTH))
    is_dir = os.path.isdir(full)
    node = {"name": (name.upper() if upper else name), "src": full, "is_dir": is_dir,
            "children": [], "sector": 0, "size": 0, "table": 0, "offsets": {}}
    if is_dir:
        for child in sorted(os.listdir(full), key=str.lower):
            node["children"].append(_node(full, child, upper, depth + 1))
    else:
        node["size"] = os.path.getsize(full)
    return node


def _entry_bytes(node):
    return (14 + len(node["name"]) + 3) & ~3


def _allocate(root):
    """Directory tables first from sector 33, then file data, each on its own sectors."""
    dirs, files = [], []
    stack = [root]
    while stack:
        node = stack.pop()
        dirs.append(node)
        for child in node["children"]:
            (stack if child["is_dir"] else files).append(child)
    sector = 33
    for node in dirs:
        used = sum(_entry_bytes(c) for c in node["children"])
        node["table"] = max(SECTOR, ((used + SECTOR - 1) // SECTOR) * SECTOR)
        node["sector"] = sector
        node["size"] = node["table"]  # a directory's size is its table's
        sector += node["table"] // SECTOR
    for node in files:
        node["sector"] = sector
        sector += (node["size"] + SECTOR - 1) // SECTOR
    return sector, dirs, files


def _dir_table(node):
    """A directory's table: entries ordered by name, tree balanced, rooted at dword 0."""
    kids = sorted(node["children"], key=lambda c: c["name"].upper())
    n = len(kids)
    table = bytearray(node["table"])
    if not n:
        return bytes(table)
    mid = n // 2
    order = [kids[mid]] + kids[:mid] + kids[mid + 1:]
    offsets = []
    off = 0
    for kid in order:
        offsets.append(off)
        off += _entry_bytes(kid)
    links = {}

    def build(lo, hi):
        if lo >= hi:
            return 0
        m = (lo + hi) // 2
        links[m] = (build(lo, m), build(m + 1, hi))
        return offsets[m] // 4

    # Every reader takes a child offset as a dword number, so every entry has
    # to start on a 4-byte boundary. _entry_bytes rounds up to hold that, and
    # this says so where it matters instead of trusting the rounding.
    for off in offsets:
        if off % 4:
            raise ValueError("entry offset %d is not dword-aligned" % off)
    links[0] = (build(1, mid + 1), build(mid + 1, n))
    for i, kid in enumerate(order):
        o = offsets[i]
        left, right = links[i]
        struct.pack_into("<HH", table, o, left, right)
        struct.pack_into("<II", table, o + 4, kid["sector"], kid["size"])
        table[o + 12] = 0x10 if kid["is_dir"] else 0x20
        table[o + 13] = len(kid["name"])
        table[o + 14:o + 14 + len(kid["name"])] = kid["name"].encode("latin-1")
    return bytes(table)


def _inside(path, root):
    """True when path lands inside root: an image written into the tree it is
    built from would try to hold itself."""
    path = os.path.realpath(path)
    root = os.path.realpath(root)
    return path == root or os.path.commonpath([path, root]) == root


def cmd_create(src, out_path, args):
    if not os.path.isdir(src):
        print("%s: not a directory" % src, file=sys.stderr)
        return 2
    if _inside(out_path, src):
        print("%s: the image would be written inside its own source tree (%s)"
              % (out_path, src), file=sys.stderr)
        return 2
    try:
        root = _node(src, "", args.upper)
    except ValueError as exc:
        print("%s: %s" % (src, exc), file=sys.stderr)
        return 2
    if not any(c["name"].upper() == "DEFAULT.XBE" for c in root["children"]):
        # An Xbox boots a disc by its default.xbe. Without one the image cannot
        # boot, so this is refused before anything is written, instead of
        # written, warned about, and then reported as a success.
        print("%s: no default.xbe at the top, so the disc would not boot; put one there "
              "and try again (the source tree is unchanged)" % src, file=sys.stderr)
        return 2
    total, dirs, files = _allocate(root)
    try:
        with open(out_path, "wb") as out:
            out.truncate(total * SECTOR)
            vd = bytearray(SECTOR)
            vd[:20] = MAGIC
            struct.pack_into("<II", vd, 20, root["sector"], root["table"])
            vd[0x7EC:0x7EC + 20] = MAGIC
            out.seek(32 * SECTOR)
            out.write(vd)
            for node in dirs:
                out.seek(node["sector"] * SECTOR)
                out.write(_dir_table(node))
            for node in files:
                out.seek(node["sector"] * SECTOR)
                copied = 0
                with open(node["src"], "rb") as f:
                    while True:
                        chunk = f.read(8 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                        copied += len(chunk)
                if copied != node["size"]:
                    raise IOError("%s: read %d of %d bytes" % (node["src"], copied, node["size"]))
    except (IOError, OSError) as exc:
        # A half-written image is not a disc: remove it rather than leave one
        # under the name that was asked for.
        print("%s: %s; nothing was kept" % (out_path, exc), file=sys.stderr)
        _discard(out_path)
        return 2
    status = cmd_verify(out_path, args)
    if status:
        print("%s: the image it wrote does not verify; removing it" % out_path, file=sys.stderr)
        _discard(out_path)
        return status
    print("created      %s (%.2f GiB)" % (out_path, os.path.getsize(out_path) / 2**30))
    print("layout       %d directories, %d files, %d sectors" % (len(dirs), len(files), total))
    return 0


def _discard(path):
    """Removes a file this run wrote, ignoring a failure to do so."""
    try:
        os.unlink(path)
    except OSError:
        pass


def cmd_rewrite(path, args):
    """Repair the tables, drop the sectors after the last one used when the
    partition starts at +0x0, then read the result back and report it. This
    rewrites the image and truncates it, so it keeps a backup unless
    --no-backup is given."""
    disc = Disc(path)
    try:
        if not disc.locate(args.part_offset):
            print("%s: no XDVDFS volume at any known partition offset" % path, file=sys.stderr)
            return 2
        dirs, past_end = survey(disc, args.cap)
        if past_end:
            print("%s: %d table(s) lie beyond the image, cannot rewrite it"
                  % (path, len(past_end)), file=sys.stderr)
            return 2
        end = used_end(disc, dirs)
        patches, error = table_patches(dirs, args)
        if error:
            print("  %s, refusing to write" % error, file=sys.stderr)
            return 2
        bk = None
        try:
            if not args.no_backup:
                bk = backup_path(path)
                shutil.copyfile(path, bk)
                print("backup       %s" % bk)
        except (IOError, OSError) as exc:
            print("%s: %s; the image is unchanged" % (path, exc), file=sys.stderr)
            if bk:
                _discard(bk)
            return 2
        shrunk = 0
        keep = disc.part_off + end * SECTOR
        try:
            repaired = write_patches(path, disc.part_off, patches)
            if disc.part_off == 0 and keep < disc.size:
                shrunk = disc.size - keep
                os.truncate(path, keep)
        except (IOError, OSError) as exc:
            print("%s: %s" % (path, exc), file=sys.stderr)
            if bk:
                print("the image may be partially rewritten; restore it from %s" % bk, file=sys.stderr)
            else:
                print("the image may be partially rewritten, and --no-backup kept no copy",
                      file=sys.stderr)
            return 2
        print("rewrote      %s: %d table(s) rebalanced, %.2f GiB%s"
              % (path, repaired, os.path.getsize(path) / 2**30,
                 " (dropped %.2f GiB after the last used sector)" % (shrunk / 2**30) if shrunk else ""))
        # Read the result back: a rewrite reports what the image is now, not what
        # it was meant to become.
        after, past = survey_image(path, args.cap)
        if after is None:
            print("%s: the rewritten image has no readable volume at +0x0" % path, file=sys.stderr)
            return 2
        deepest = max([d[5] for d in after] or [0])
        if past or deepest > TARGET_DEPTH:
            print("%s: the rewritten image still has %d table(s) past the end and a deepest "
                  "table of %d level(s)" % (path, len(past), deepest), file=sys.stderr)
            return 2
        print("verified     %d table(s), deepest %d level(s), every entry reachable"
              % (len(after), deepest))
        if disc.part_off != 0:
            print("note         the partition starts at +0x%X, so the image keeps its front "
                  "padding; --trim OUT.iso writes one with the partition at +0x0" % disc.part_off)
        return 0
    finally:
        disc.f.close()

def main(argv):
    ap = argparse.ArgumentParser(
        prog="xboxiso",
        description="An all-in-one Xbox disc image tool: list, extract, create, verify, info, "
                    "repair directory tables that depth-limited readers cannot search, and write "
                    "a correct trim of a full disc image.")
    ap.add_argument("--version", action="version", version="xboxiso " + VERSION)
    ap.add_argument("images", nargs="*", help="disc images (ISO, XISO, XGD)")
    ap.add_argument("--list", action="store_true", help="list the files on an image")
    ap.add_argument("--extract", metavar="DIR", help="extract an image's files into DIR")
    ap.add_argument("--create", metavar="SRC", help="build an image from a directory")
    ap.add_argument("--info", action="store_true", help="volume and XBE title details")
    ap.add_argument("--verify", action="store_true",
                    help="check every entry is reachable and every extent is inside the image")
    ap.add_argument("--rewrite", action="store_true",
                    help="repair in place and drop the sectors after the last used one")
    ap.add_argument("-o", "--output", help="write the repaired (or created) image here")
    ap.add_argument("--in-place", action="store_true", help="repair each image in place")
    backup = ap.add_mutually_exclusive_group()
    backup.add_argument("--backup", dest="no_backup", action="store_false",
                        help="keep <image>.bak (the default)")
    backup.add_argument("--no-backup", action="store_true",
                        help="with --in-place or --rewrite, do not keep <image>.bak")
    # store_false defaults to True, which would silently disable the backup on
    # every run: pin the default the two flags share.
    ap.set_defaults(no_backup=False)
    ap.add_argument("--trim", metavar="OUT.iso",
                    help="write a compact copy: the game partition alone, cut to the last used "
                         "sector, tables repaired (loses no file)")
    ap.add_argument("--part-offset", type=lambda s: int(s, 0),
                    help="game partition offset (default: probe the known ones)")
    ap.add_argument("--cap", type=int, default=64,
                    help="a reader's depth limit to test against (default 64, hakuX's)")
    ap.add_argument("--no-upper", dest="upper", action="store_false",
                    help="--create keeps file name case (default: upper, the Xbox convention)")
    ap.add_argument("--self-test", action="store_true", help="check the tool itself, then exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    # One mode at a time: two of them used to be silently decided by the order
    # of the dispatch chain, so `--info --verify` ran --info and reported
    # success for a verify nobody performed.
    modes = [flag for flag, on in (("--list", args.list), ("--extract", args.extract),
                                   ("--info", args.info), ("--verify", args.verify),
                                   ("--rewrite", args.rewrite), ("--in-place", args.in_place),
                                   ("--trim", args.trim), ("--create", args.create)) if on]
    if len(modes) > 1:
        ap.error("%s ask for different things; give one at a time" % " and ".join(modes))

    if args.self_test:
        return self_test(args)
    if args.create:
        if args.images or not args.output:
            ap.error("--create takes a directory and -o OUT.iso")
        return cmd_create(args.create, args.output, args)
    if args.trim:
        if len(args.images) != 1 or args.output or args.in_place:
            ap.error("--trim takes one image and its own output")
        return trim_image(args.images[0], args.trim, args)
    if not args.images:
        ap.error("give at least one image, or --self-test")
    if len(args.images) > 1 and (args.extract or args.output or args.info or args.list):
        ap.error("--list, --extract, --info and -o take one image")

    worst = 0
    for path in args.images:
        # A path that is not a file used to raise out of open() as a traceback;
        # the tool documents exit status 2 for a name it cannot use.
        if not os.path.isfile(path):
            print("%s: not a file" % path, file=sys.stderr)
            status = 2
        else:
            try:
                if args.list:
                    status = cmd_list(path, args)
                elif args.extract:
                    status = cmd_extract(path, args.extract, args)
                elif args.info:
                    status = cmd_info(path, args)
                elif args.verify:
                    status = cmd_verify(path, args)
                elif args.rewrite:
                    status = cmd_rewrite(path, args)
                else:
                    status = process(path, args)
            except (IOError, OSError) as exc:
                print("%s: %s" % (path, exc), file=sys.stderr)
                status = 2
        worst = max(worst, status)
        if status and status != 3:
            break
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
