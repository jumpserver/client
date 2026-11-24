#!/usr/bin/env python3
"""Create an offline FFmpeg plugin TAR from two previously downloaded binaries."""
import argparse
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile

ASSETS = {
    'darwin-arm64': ('a90e3db6a3fd35f6074b013f948b1aa45b31c6375489d39e572bea3f18336584', 'bb2db6f5d8cef919da12fbf592119a987202a8c060a886f3cab091f9cab90b64'),
    'darwin-x64': ('ebdddc936f61e14049a2d4b549a412b8a40deeff6540e58a9f2a2da9e6b18894', 'fa3add0ce901f7241abe0dfc0155d958fc834aca3f8ce61f87cc712ae669c1e0'),
    'win32-x64': ('04e1307997530f9cf2fe35cba2ca7e8875ca91da02f89d6c7243df819c94ad00', '3a7e2dc003dc2cd1472827e4c7c4f056ae1ae0ae7c5bbc580c99b49827351ba4'),
    'linux-x64': ('e7e7fb30477f717e6f55f9180a70386c62677ef8a4d4d1a5d948f4098aa3eb99', '4f231a1960d83e403d08f7971e271707bec278a9ae18e21b8b5b03186668450d'),
    'linux-arm64': ('6bb182d0d75d23028db82e9e4f723ca69b853d055698486e6984ddb2c06fb8ce', 'd17ae9b4c297d48e2521ba14e417bb0537c6ff77c584cdbcd6bb0d8d0307a2e8'),
}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', required=True, choices=ASSETS)
    parser.add_argument('--ffmpeg', required=True, type=Path)
    parser.add_argument('--ffprobe', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument(
        '--self-verified',
        action='store_true',
        help='package FFmpeg 6.0 or newer using digests computed from the supplied files',
    )
    args = parser.parse_args()
    paths = (args.ffmpeg, args.ffprobe)
    if args.self_verified:
        hashes = tuple(digest(path) for path in paths)
    else:
        hashes = ASSETS[args.platform]
        for path, expected in zip(paths, hashes):
            if digest(path) != expected:
                parser.error('SHA-256 mismatch for ' + str(path))
    suffix = '.exe' if args.platform.startswith('win32-') else ''
    names = ('ffmpeg' + suffix, 'ffprobe' + suffix)
    sums = ''.join(f'{sha}  {name}\n' for sha, name in zip(hashes, names)).encode('ascii')
    with tempfile.TemporaryFile(dir=args.output.parent) as staged:
        with tarfile.open(fileobj=staged, mode='w', format=tarfile.USTAR_FORMAT) as archive:
            for path, name in zip(paths, names):
                info = tarfile.TarInfo(name)
                info.size = path.stat().st_size
                info.mode = 0o700
                with path.open('rb') as source:
                    archive.addfile(info, source)
            info = tarfile.TarInfo('SHA256SUMS')
            info.size = len(sums)
            info.mode = 0o600
            archive.addfile(info, io.BytesIO(sums))
        staged.seek(0)
        with args.output.open('xb') as output:
            for chunk in iter(lambda: staged.read(1024 * 1024), b''):
                output.write(chunk)
    print('Created', args.output)


if __name__ == '__main__':
    main()
