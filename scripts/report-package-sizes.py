#!/usr/bin/env python3
"""Record generated installer sizes in a GitHub Actions run summary."""
import argparse
import os
from pathlib import Path
import sys


def format_summary(files, title):
    lines = [
        f'### {title}',
        '',
        '| Artifact | Bytes | MiB |',
        '| --- | ---: | ---: |',
    ]
    for path in files:
        size = path.stat().st_size
        lines.append(f'| `{path.name}` | {size:,} | {size / (1024 * 1024):.2f} |')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--platform', required=True)
    parser.add_argument('--arch', required=True)
    parser.add_argument('--extension', action='append', required=True,
                        help='Installer extension without a leading dot; repeatable')
    args = parser.parse_args()
    if not args.directory.is_dir():
        parser.error(f'Artifact directory does not exist: {args.directory}')
    suffixes = {'.' + value.lower().lstrip('.') for value in args.extension}
    files = sorted(
        (path for path in args.directory.iterdir()
         if path.is_file() and path.suffix.lower() in suffixes),
        key=lambda path: path.name,
    )
    if not files:
        parser.error('No installer artifacts matched {}'.format(', '.join(sorted(suffixes))))
    summary = format_summary(files, f'{args.platform} {args.arch} installers')
    destination = os.environ.get('GITHUB_STEP_SUMMARY')
    if destination:
        with open(destination, 'a', encoding='utf-8') as stream:
            stream.write(summary)
    else:
        sys.stdout.write(summary)


if __name__ == '__main__':
    main()
