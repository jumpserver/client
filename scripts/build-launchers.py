#!/usr/bin/env python3
"""Build the vendored osm Go launchers; never modifies the source client project."""
import argparse
import os
import platform
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--go', default='go', help='Go executable (select a toolchain compatible with the target OS)')
    host_os = {'Darwin': 'darwin', 'Windows': 'windows', 'Linux': 'linux'}.get(platform.system())
    host_arch = {'x86_64': 'amd64', 'AMD64': 'amd64', 'aarch64': 'arm64', 'arm64': 'arm64', 'i686': '386', 'loongarch64': 'loong64', 'mips64el': 'mips64le', 'mips64le': 'mips64le', 'riscv64': 'riscv64', 'armv7l': 'arm', 'armv6l': 'arm', 'armv5tel': 'arm'}.get(platform.machine())
    if platform.machine() == 'mips64' and sys.byteorder == 'little':
        host_arch = 'mips64le'
    parser.add_argument('--goos', choices=('darwin', 'linux', 'windows'), default=host_os)
    parser.add_argument('--goarch', choices=('amd64', 'arm64', '386', 'loong64', 'mips64le', 'riscv64', 'arm'), default=host_arch)
    parser.add_argument('--goarm', choices=('5', '6', '7'), default='7', help='32-bit ARM ISA baseline (default: 7; overrides inherited GOARM)')
    args = parser.parse_args()
    if not args.goos or not args.goarch:
        parser.error('Specify --goos and --goarch for this host')
    source = Path(__file__).resolve().parents[1] / 'go-client'
    if args.goos == 'windows' and args.goarch not in ('amd64', '386'):
        parser.error('Vendored Windows AutoIt/PuTTY resources support amd64/386 only; native ARM64 requires a separate port.')
    folder = 'windows' if args.goos == 'windows' else f'{args.goos}-{args.goarch}'
    output = args.output.resolve() / folder
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, GOOS=args.goos, GOARCH=args.goarch, CGO_ENABLED='0')
    if args.goarch == 'arm':
        env['GOARM'] = args.goarm
    # The source snapshot predates LoongArch syscall definitions. Keep its module
    # files untouched and use the narrowly scoped dependency overlay for this port.
    module_flags = []
    if args.goos == 'linux' and args.goarch == 'loong64':
        module_flags = ['-modfile=' + str(Path(__file__).resolve().with_name('go-loong64.mod'))]
    name = 'JumpServerClient32.exe' if args.goarch == '386' and args.goos == 'windows' else ('JumpServerClient.exe' if args.goos == 'windows' else 'JumpServerClient')
    flags = '-s -w' + (' -H windowsgui' if args.goos == 'windows' else '')
    subprocess.run([args.go, 'build', *module_flags, '-trimpath', '-ldflags', flags, '-o', str(output / name), './cmd/awaken'], cwd=source, env=env, check=True)
    if args.goos != 'windows':
        subprocess.run([args.go, 'build', *module_flags, '-trimpath', '-ldflags', '-s -w', '-o', str(output / 'client'), './cmd/sshc'], cwd=source, env=env, check=True)
    if args.goos == 'darwin':
        shutil.copytree(source / 'Scripts', output / 'Scripts', dirs_exist_ok=True)
    if args.goos == 'windows':
        shutil.copy2(source / 'putty.exe', output)
        for dll in (source / 'pkg' / 'autoit').glob('*.dll'):
            shutil.copy2(dll, output)
    shutil.copy2(source / 'config.json', output.parent / 'config.json')
    print(f'Built launcher resources: {output}')


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f'Launcher build failed: {exc}', file=sys.stderr)
        sys.exit(1)
