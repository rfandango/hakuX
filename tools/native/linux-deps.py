#!/usr/bin/env python3
"""Fetch/verify the pinned day-one sources without modifying external/fex."""
import argparse
import json
from pathlib import Path
import subprocess


def git(path, *args):
    return subprocess.check_output(['git', '-C', str(path), *args], text=True).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dest', type=Path, default=Path('build/deps'))
    parser.add_argument('--verify', action='store_true', help='Verify existing sources without network access')
    args = parser.parse_args()
    lock = json.loads(Path(__file__).with_suffix('.json').read_text())
    for relative, source in lock['sources'].items():
        path = args.dest / relative
        if not (path / '.git').exists():
            if args.verify:
                raise SystemExit(f'Missing source: {path}')
            path.mkdir(parents=True, exist_ok=True)
            git(path, 'init', '-q')
            git(path, 'remote', 'add', 'origin', source['url'])
            git(path, 'fetch', '--depth', '1', 'origin', source['commit'])
            git(path, 'checkout', '-q', '--detach', 'FETCH_HEAD')
        if git(path, 'rev-parse', 'HEAD') != source['commit']:
            raise SystemExit(f'Wrong revision: {path}; expected {source["commit"]}')
        if git(path, 'status', '--porcelain', '--untracked-files=no'):
            raise SystemExit(f'Modified source: {path}; use a clean dependency directory')
        print(f'{relative}: {source["commit"]}', flush=True)
    fex = args.dest / 'fex'
    if not args.verify:
        git(fex, 'submodule', 'update', '--init', '--depth', '1', *lock['fex_submodules'])
    for relative, commit in lock['fex_submodules'].items():
        if git(fex / relative, 'rev-parse', 'HEAD') != commit:
            raise SystemExit(f'Wrong FEX submodule revision: {relative}')
        if git(fex / relative, 'status', '--porcelain', '--untracked-files=no'):
            raise SystemExit(f'Modified FEX submodule: {relative}')
        print(f'fex/{relative}: {commit}', flush=True)


if __name__ == '__main__':
    main()
