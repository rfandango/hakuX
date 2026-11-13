"""jmp2mod.py MAGIC_PATCHES_DIR MODS_DIR [--report FILE]: mods from Jay's Magic Patches.

Reads the .JMP patch files of "Xbox Magic Patches by Jay"
(https://github.com/JayYardley/Magic-Patches-by-Jay: Jay's Magic Patcher's
format, www.jayxbox.com) and writes one mod per file as
MODS_DIR/<TITLE ID>/<mod>/mod.txt, plus MODS_DIR/CREDITS.md. Run it again for
a newer copy of the library; a mod's folder is named by what the file says, so
the same file gives the same folder.

What a .JMP file holds, and what it becomes (mods/README.md has the mod format):

  #Jay's Magic Patcher ...        the header: game-title=, region=, version=
  region=NTSC, JAPAN              (title ID, XBE CRC32), author=, notes=
  version=4D530064 (MS-100) {CRC}
  #a comment                      a comment above the record it describes; "[-]" in it
  0x713B:D0020000                 marks a record that is off by default
  AABBCC                          position record: bytes written at a position of the XBE file
  DDEEFF                          -> "offset 0x713B: D0 02 00 00"
                                  search record: two lines, the bytes to find and their
                                  replacement -> "search AABBCC -> DDEEFF"

The mod lists the regions its file names (`region: NTSC, NTSC-J, PAL`; a file for all
regions lists the three), and the app applies it only to a disc made for one of them. A
file says its regions twice, in its header (region=) and in its name ("(NTSC+JPN)"); where
they differ, the mod takes the regions both name. A prototype's file that names none and
only searches for bytes has no "region:" line: it is for every disc of its title.

Not imported, and listed in the report:
  - files whose title ID can't be told (prototypes with 00000000, dashboards, Chihiro),
    that name no region and have position records, whose header and file name name
    different regions, that patch a title update's
    XBE ("- TU"), or that append code to the XBE ("APPEND");
  - the records that edit the header's media and region flags: they let a real console
    run another region's discs and mean nothing to an emulator.
Records off by default ("[-]") are kept as comments.
"""
import argparse
import collections
import os
import re
import sys

SOURCE = 'Xbox Magic Patches by Jay'
SOURCE_URL = 'https://github.com/JayYardley/Magic-Patches-by-Jay'
VIA = "via Jay's Magic Patches"
MAX_BYTES = 4096          # the app's limit for the bytes of one line (XbePatch)
REGION_ORDER = ('NTSC', 'NTSC-J', 'PAL')

HEADER_KEY = re.compile(r'^(system|game-title|region|version|author|notes)\s*=(.*)$', re.I)
HEX = re.compile(r'^[0-9A-Fa-f]+$')
POSITION = re.compile(r'^(?:0x)?([0-9A-Fa-f]+):([0-9A-Fa-f ]*)$')
FILE_NAME = re.compile(
    r'^(?P<title>.*?)\s*\{(?P<nature>[^}]*)\}\s*\((?P<region>[^)]*)\)\s*(?P<extra>.*?)\s*\[(?P<author>[^\]]*)\]\.jmp$', re.I)
FILE_NAME_NO_NATURE = re.compile(
    r'^(?P<title>.*?)\s*\((?P<region>[^)]*)\)\s*(?P<extra>.*?)\s*\[(?P<author>[^\]]*)\]\.jmp$', re.I)
HARDWARE_EDIT = re.compile(r'header edit.*(media|region|flags)', re.I)
UNKNOWN_AUTHOR = {'', 'author unknown', 'unknown author', 'unknown'}


def read_text(path):
    raw = open(path, 'rb').read()
    for encoding in ('utf-8-sig', 'cp1252'):
        try:
            return raw.decode(encoding)
        except UnicodeDecodeError:
            pass
    return raw.decode('latin-1')


def parse_jmp(path):
    """(header dict, records): a record is a dict with kind offset | search | append | unknown."""
    lines = read_text(path).replace('\r\n', '\n').replace('\r', '\n').split('\n')
    header = {}
    i = 0
    while i < len(lines):
        line = lines[i].strip('﻿')
        m = HEADER_KEY.match(line)
        if m:
            header[m.group(1).lower()] = m.group(2).strip()
        elif not (line.strip() == '' or line.startswith('#Jay') or (line.startswith('#') and not header)):
            break
        i += 1
    items = []
    comments = []
    for line in lines[i:]:
        line = line.rstrip()
        if not line.strip():
            continue
        if line.startswith('#'):
            comments.append(line[1:].strip())
        else:
            items.append((comments, line.strip()))
            comments = []
    records = []
    j = 0
    while j < len(items):
        comments, line = items[j]
        m = POSITION.match(line)
        if line.upper().startswith('APPEND'):
            records.append(dict(kind='append', comments=comments, raw=line))
        elif m:
            records.append(dict(kind='offset', comments=comments, raw=line,
                                offset=m.group(1), data=m.group(2).replace(' ', '')))
        elif HEX.match(line) and j + 1 < len(items) and HEX.match(items[j + 1][1]):
            records.append(dict(kind='search', comments=comments + items[j + 1][0], raw=line,
                                find=line, repl=items[j + 1][1]))
            j += 1
        else:
            records.append(dict(kind='unknown', comments=comments, raw=line))
        j += 1
    return header, records


def regions_of(text):
    """The regions a region= value or a file name's "(NTSC+JPN)" names, in REGION_ORDER; empty if none."""
    t = ' ' + text.lower() + ' '
    if re.search(r'all regions|global|multi[- ]?region|worldwide|\bmulti\b', t):
        return list(REGION_ORDER)
    found = set()
    t = re.sub(r'ntsc-j(?![a-z])', ' jpn ', t)
    if re.search(r'\bjap(?:an)?\b|\bjpn\b|ntsc-jap', t):
        found.add('NTSC-J')
    if re.search(r'\bntsc\b|us[_/ ]?canada|\busa?\b|north america', t):
        found.add('NTSC')
    if re.search(r'\bpal\b|elsewhere|everywhere else|europe|rest of (?:the )?world', t):
        found.add('PAL')
    return [r for r in REGION_ORDER if r in found]


def title_id_of(header):
    """The title ID (8 hex digits) the version= line gives, else its readable form (EA-145), else None
    (00000000, a prototype's, is none: the app finds a disc without an ID by its path, not by a folder)."""
    version = header.get('version', '')
    m = re.search(r'(?<![0-9A-Fa-f])([0-9A-Fa-f]{8})(?![0-9A-Fa-f])', version)
    if m and int(m.group(1), 16):
        return m.group(1).upper()
    m = re.search(r'\(([A-Za-z0-9]{2})-(\d+)\)', version)
    if m:
        return '%08X' % ((ord(m.group(1)[0]) << 24) | (ord(m.group(1)[1]) << 16) | int(m.group(2)))
    return None


def slug(text):
    return re.sub(r'[^a-z0-9]+', '-', text.lower()).strip('-')


def hex_groups(data, size=4):
    """'D0020000680005' -> 'D0020000 680005': four bytes to a group."""
    data = data.upper()
    return ' '.join(data[i:i + size * 2] for i in range(0, len(data), size * 2))


class Skip(Exception):
    pass


def convert(path, root):
    """(title ID, mod id base, mod.txt text, info dict) for a .JMP file; raises Skip with the reason."""
    file_name = os.path.basename(path)
    header, records = parse_jmp(path)
    parts = FILE_NAME.match(file_name)
    if parts:
        nature = parts.group('nature').strip()
    else:
        parts = FILE_NAME_NO_NATURE.match(file_name)
        nature = ''
    name_parts = parts.groupdict() if parts else {}

    tid = title_id_of(header)
    if not tid:
        raise Skip('title ID not told by the file (%r)' % header.get('version', ''))
    label = name_parts.get('region', '')
    if re.search(r'\bTU\b', label):
        raise Skip("patches a title update's XBE (%s)" % label)
    from_header = regions_of(header.get('region', ''))
    from_name = regions_of(label)
    # the header and the file name each say the regions: where both do, only those they agree on
    regions = [r for r in REGION_ORDER if r in from_header and r in from_name] if from_header and from_name \
        else from_header or from_name
    if not regions:
        if from_header and from_name:
            raise Skip('the header (%s) and the file name (%s) name no region in common' % (
                '/'.join(from_header), '/'.join(from_name)))
        # neither names a region (a prototype or debug build): the mod is for every disc of the title,
        # which is safe only if it searches for its bytes (a position would be written on any build)
        if any(r['kind'] == 'offset' for r in records):
            raise Skip('region not told (region=%r, name says %r) and the file has position records' % (
                header.get('region', ''), label))
    if any(r['kind'] == 'append' for r in records):
        raise Skip('appends code to the XBE (APPEND)')
    if any(r['kind'] == 'unknown' for r in records):
        raise Skip('a line that is no record: %r' % next(r['raw'] for r in records if r['kind'] == 'unknown')[:60])

    author = header.get('author', '').strip() or name_parts.get('author', '').strip()
    if author.lower() in UNKNOWN_AUTHOR:
        author = 'Unknown author'
    extra = name_parts.get('extra', '').strip()
    # the name as the library gives it: Game {Nature} (Region) [Author], the game's name left out
    shown = ' '.join(x for x in ('{%s}' % nature if nature else '', '(%s)' % label if label else '', extra,
                                 '[%s]' % name_parts['author'] if name_parts.get('author') else '') if x)
    if not shown:
        shown = os.path.splitext(file_name)[0]
    crc = re.search(r'\{([0-9A-Fa-f]{8})\}', header.get('version', ''))

    body = []
    kept = dropped = disabled = 0
    for r in records:
        for c in r['comments']:
            if c:
                body.append('# ' + c)
        off = '[-]' in ' '.join(r['comments'])
        if r['kind'] == 'offset':
            data = r['data']
            comment = ' '.join(r['comments'])
            if HARDWARE_EDIT.search(comment):
                body.append('# (not imported: media and region flags of a real console) offset 0x%s: %s'
                            % (r['offset'].upper(), hex_groups(data)))
                dropped += 1
                continue
            if len(r['offset']) > 8 or not data or len(data) % 2 or len(data) // 2 > MAX_BYTES:
                raise Skip('an offset record the app can not take: %r' % r['raw'][:60])
            line = 'offset 0x%s: %s' % (r['offset'].upper().lstrip('0') or '0', hex_groups(data))
        else:
            find, repl = r['find'], r['repl']
            if len(find) % 2 or len(find) != len(repl) or len(find) // 2 > MAX_BYTES:
                raise Skip('a search record the app can not take: %r' % r['raw'][:60])
            line = 'search %s -> %s' % (hex_groups(find), hex_groups(repl))
        if off:
            body.append('# [off by default in the source] ' + line)
            disabled += 1
        else:
            body.append(line)
            kept += 1
    if not kept:
        raise Skip('nothing left to apply (%d records not imported, %d off by default)' % (dropped, disabled))

    notes = ' '.join(header.get('notes', '').split())
    text = ['name: ' + shown]
    if notes:
        text.append('description: ' + notes)
    text.append('author: %s, %s' % (author, VIA))
    if regions:
        text.append('region: ' + ', '.join(regions))
    text += [
        '',
        '# Converted from "%s" of %s (%s),' % (file_name, SOURCE, SOURCE_URL),
        "# a Jay's Magic Patcher (.JMP) patch made by %s." % author,
        '# Region in the file: %s%s.' % (header.get('region', '') or '?', ('; XBE CRC32 {%s}' % crc.group(1).upper()) if crc else ''),
        '# "offset" is a position in the XBE file and "search" the first place its bytes are (mods/README.md).',
    ] + ([] if regions else [
        '# No "region:" line: the file names no region (a prototype build), so the mod fits every disc of the',
        '# title, and its "search" lines only change a disc where their bytes are.',
    ]) + [
        '',
        '[default.xbe]',
    ] + body
    stem = os.path.splitext(file_name)[0]
    info = dict(file=os.path.relpath(path, root), title=tid, author=author, regions=regions, shown=shown, stem=stem,
                from_header=from_header, from_name=from_name, kept=kept, dropped=dropped, disabled=disabled,
                game=header.get('game-title', name_parts.get('title', '')).strip())
    base = slug(' '.join(x for x in (nature, label, extra, author) if x)) or 'patch'
    return tid, base, '\n'.join(text) + '\n', info


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('library', help="the folder of Xbox Magic Patches by Jay (its author folders hold the .JMP files)")
    ap.add_argument('mods', help='the mods/ folder to write the mods to')
    ap.add_argument('--report', help='write what was imported and what not, as Markdown, to this file')
    args = ap.parse_args()

    files = sorted(os.path.join(d, f) for d, _, fs in os.walk(args.library) for f in fs if f.lower().endswith('.jmp'))
    converted, skipped = [], []
    for path in files:
        try:
            converted.append(convert(path, args.library))
        except Skip as why:
            skipped.append((os.path.relpath(path, args.library), str(why)))
    # two files of one title that the library names alike (Halo 2 and its DLC) keep their whole name
    names = collections.Counter((tid, info['shown']) for tid, _, _, info in converted)
    used = collections.defaultdict(set)
    done = []
    for tid, base, text, info in converted:
        if names[(tid, info['shown'])] > 1:
            text = text.replace('name: ' + info['shown'] + '\n', 'name: ' + info['stem'] + '\n', 1)
            base = slug(info['stem']) or base
        mod = base
        n = 2
        while mod in used[tid]:
            mod = '%s-%d' % (base, n)
            n += 1
        used[tid].add(mod)
        folder = os.path.join(args.mods, tid, mod)
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, 'mod.txt'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
        info['mod'] = '%s/%s' % (tid, mod)
        done.append(info)

    credits(args.mods, done)
    print('%d mods for %d titles from %d files; %d files left out' % (
        len(done), len({d['title'] for d in done}), len(files), len(skipped)))
    if args.report:
        report(args.report, files, done, skipped)


def credits(mods, done):
    by_author = collections.defaultdict(list)
    for d in done:
        by_author[d['author']].append(d)
    lines = [
        '# Credits',
        '',
        'The mods in the title folders (`mods/<TITLE ID>/`, the ones whose `mod.txt` has a `region:` line) are',
        'the patches of **%s** (%s), a collection of patches for Jay\'s Magic Patcher (`.JMP`,' % (SOURCE, SOURCE_URL),
        'www.jayxbox.com), converted to this app\'s mod format by `tools/mods/jmp2mod.py`. The patches are the work of',
        'the authors below, as the collection credits them: each `mod.txt` names its author (the in-app list shows',
        '"by <author>, via Jay\'s Magic Patches") and the `.JMP` file it comes from. Thanks to Jay for collecting',
        'and keeping them, and to every author for making them.',
        '',
        'Patch authors, with how many mods of theirs are here:',
        '',
        '| Author | Mods | Games |',
        '| --- | ---: | ---: |',
    ]
    for author, ds in sorted(by_author.items(), key=lambda kv: (-len(kv[1]), kv[0].lower())):
        lines.append('| %s | %d | %d |' % (author.replace('|', '/'), len(ds), len({d['title'] for d in ds})))
    lines += ['', '%d mods in all.' % len(done), '']
    with open(os.path.join(mods, 'CREDITS.md'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))


def report(path, files, done, skipped):
    lines = ['# jmp2mod report', '',
             '%d .JMP files read: %d mods written for %d titles, %d files left out.' % (
                 len(files), len(done), len({d['title'] for d in done}), len(skipped)), '']
    lines += ['## Left out', '']
    reasons = collections.Counter(re.sub(r'\(.*|:.*|\'.*', '', why).strip() for _, why in skipped)
    for r, n in reasons.most_common():
        lines.append('- %d: %s' % (n, r))
    lines.append('')
    for f, why in skipped:
        lines.append('- `%s`: %s' % (f, why))
    lines += ['', '## Regions: the file name and the header differ (the mod takes the ones both name)', '']
    for d in done:
        if d['from_header'] and d['from_name'] and d['from_header'] != d['from_name']:
            lines.append('- `%s`: header %s, name %s, used %s' % (
                d['file'], '/'.join(d['from_header']), '/'.join(d['from_name']), '/'.join(d['regions'])))
    lines += ['', '## No region named: the mod fits every disc of its title (it only searches)', '']
    for d in done:
        if not d['regions']:
            lines.append('- `%s` (%s)' % (d['file'], d['title']))
    lines += ['', '## Records', '',
              'imported: %d, off by default (kept as comments): %d, media/region flag edits not imported: %d' % (
                  sum(d['kept'] for d in done), sum(d['disabled'] for d in done), sum(d['dropped'] for d in done)), '']
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))


if __name__ == '__main__':
    sys.exit(main())
