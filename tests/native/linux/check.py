#!/usr/bin/env python3
"""Run positive/negative platform checks and generate copyright-free XISO fixtures."""
import argparse
import os
from pathlib import Path
import struct
import subprocess
import tempfile


def disc(path, code, imports=()):
    """A disc whose default.xbe runs code at 0x11000 (its section takes whole
    pages), with the kernel's ordinals in imports through the thunks at 0x10f00."""
    size = max(0x1000, -(-len(code) // 0x1000) * 0x1000)
    xbe = bytearray(max(0x1010, 0x1000 + len(code)))
    def word(offset, value):
        struct.pack_into('<I', xbe, offset, value)
    for offset, value in {
        0: 0x48454258, 0x104: 0x10000, 0x108: 0x1000, 0x10c: 0x1000 + size,
        0x118: 0x10200, 0x11c: 1, 0x120: 0x10400,
        0x128: 0x11000 ^ 0xa8fc57ab, 0x130: 0x1000,
        0x158: 0x10f00 ^ 0x5b6d40b6, 0x164: 0x10a00, 0x208: 0xffff0001,
        0x400: 6, 0x404: 0x11000, 0x408: size,
        0x40c: 0x1000, 0x410: len(code), 0x414: 0x10800,
    }.items():
        word(offset, value)
    for i, ordinal in enumerate(imports):
        word(0xf00 + 4 * i, 0x80000000 | ordinal)
    xbe[0x800:0x806] = b'.text\0'
    xbe[0x1000:0x1000 + len(code)] = code
    image = bytearray(34 * 2048) + xbe
    magic = b'MICROSOFT*XBOX*MEDIA'
    image[32 * 2048:32 * 2048 + 20] = magic
    image[32 * 2048 + 0x7ec:33 * 2048] = magic
    struct.pack_into('<II', image, 32 * 2048 + 20, 33, 32)
    name = b'default.xbe'
    struct.pack_into('<HHIIBB', image, 33 * 2048, 0, 0, 34, len(xbe), 0, len(name))
    image[33 * 2048 + 14:33 * 2048 + 14 + len(name)] = name
    path.write_bytes(image)


def run(command, expected, marker, env=None):
    result = subprocess.run([str(x) for x in command], env=env, capture_output=True, text=True, timeout=15)
    print(result.stdout, end='')
    print(result.stderr, end='')
    markers = (marker,) if isinstance(marker, str) else marker
    if result.returncode != expected or any(m not in result.stdout + result.stderr for m in markers):
        raise SystemExit(f'FAIL: {command}: expected exit {expected} with {markers!r}, got {result.returncode}')
    print(f'PASS: {Path(command[0]).name} {list(map(str, command[1:]))}: expected exit {expected}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('build', type=Path)
    parser.add_argument('--fixtures', type=Path, help='Only generate fixtures for transfer to a device without Python')
    args = parser.parse_args()
    if args.fixtures:
        args.fixtures.mkdir(parents=True, exist_ok=True)
        disc(args.fixtures / 'halt.iso', b'\xf4')
        disc(args.fixtures / 'spin.iso', b'\xeb\xfe')
        return
    build = args.build.resolve()
    run([build / 'linux-smoke'], 0, 'translated MMIO')
    run([build / 'linux-smoke', '--wrong-expectation'], 1, 'FAIL: translated arithmetic')
    run([build / 'linux-smoke', '--host-fault'], 3, 'FAIL: unexpected host fault')
    run([build / 'audio-smoke'], 0, 'audio resumes')
    with tempfile.TemporaryDirectory(prefix='hakux-day1-') as folder:
        root = Path(folder)
        env = dict(os.environ, NC_TIMEOUT='1')
        run([build / 'xboxrun', '--headless', root / 'missing.iso', root / 'data'], 2, 'No such file', env)
        (root / 'invalid.iso').write_bytes(b'not an Xbox disc')
        run([build / 'xboxrun', '--headless', root / 'invalid.iso', root / 'data'], 1, 'boot failed', env)
        disc(root / 'halt.iso', b'\xf4')
        disc(root / 'spin.iso', b'\xeb\xfe')
        run([build / 'xboxrun', '--headless', root / 'halt.iso', root / 'data'], 0, 'guest executed HLT', env)
        run([build / 'xboxrun', '--headless', root / 'halt.iso', root / 'data', 'lang=2', 'net=off'], 0,
            ('console language 2', 'guest executed HLT'), env)
        run([build / 'xboxrun', '--headless', root / 'halt.iso', root / 'data', 'unknown=1'], 2,
            'unknown option unknown=1', env)
        # The renderer's boot path (drawing itself needs a title and a GPU).
        run([build / 'xboxrun', '--render', root / 'halt.iso', root / 'render'], 0,
            ('renderer ON without a display', 'guest executed HLT'), env)
        run([build / 'xboxrun', '--headless', root / 'spin.iso', root / 'data'], 5, 'timeout after 1 s', env)


if __name__ == '__main__':
    main()
