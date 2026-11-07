#!/usr/bin/env python3
"""Build native DEB/RPM packages inside the matching Linux distribution."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys


ARCHITECTURES = {
    'x86_64': ('amd64', 62), 'amd64': ('amd64', 62),
    'aarch64': ('arm64', 183), 'arm64': ('arm64', 183),
    'i386': ('386', 3), 'i686': ('386', 3),
    'armv7l': ('arm', 40), 'armv6l': ('arm', 40),
    'riscv64': ('riscv64', 243), 'loongarch64': ('loong64', 258),
    'mips64el': ('mips64le', 8), 'mips64le': ('mips64le', 8),
}


def run(command, **kwargs):
    print('+ ' + ' '.join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def require_elf(path, machine):
    with path.open('rb') as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b'\x7fELF' or header[5] != 1:
        raise RuntimeError('Expected a little-endian Linux ELF executable: {}'.format(path))
    if struct.unpack('<H', header[18:20])[0] != machine:
        raise RuntimeError('Executable architecture does not match the package build host: {}'.format(path))
    if not os.access(str(path), os.X_OK):
        raise RuntimeError('Executable permission is missing: {}'.format(path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--format', choices=('deb', 'rpm'), required=True)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--helpers', type=Path, help='Prebuilt bin tree: config.json and one linux-<goarch> directory')
    parser.add_argument('--go', default='go', help='Go toolchain when --helpers is omitted')
    parser.add_argument('--qt-major', choices=('5', '6'), default='5')
    parser.add_argument('--cmake-arg', action='append', default=[], metavar='KEY=VALUE')
    parser.add_argument('--parallel', type=int, default=4)
    parser.add_argument('--skip-tests', action='store_true')
    args = parser.parse_args()
    if sys.version_info < (3, 8):
        parser.error('Python 3.8 or newer is required (on Rocky 8, install python39 and run python3.9)')
    if platform.system() != 'Linux':
        parser.error('Run inside the intended Linux distribution (for example a disposable Docker container)')
    cpu = platform.machine().lower()
    if cpu == 'mips64' and sys.byteorder == 'little':
        cpu = 'mips64le'
    architecture = ARCHITECTURES.get(cpu)
    if not architecture:
        parser.error('Unsupported Linux CPU: ' + platform.machine())
    goarch, machine = architecture
    for program in ('cmake', 'cpack', 'ctest', 'pkg-config', 'file', 'desktop-file-validate'):
        if not shutil.which(program):
            parser.error('Missing build tool: ' + program)
    if args.format == 'deb':
        if not shutil.which('dpkg-shlibdeps'):
            parser.error('DEB builds need dpkg-dev inside a Debian-based build distribution')
        run(['dpkg-query', '-W', 'libc6'])
    else:
        if not shutil.which('rpmbuild'):
            parser.error('RPM builds need rpm-build inside an RPM-based build distribution')
        # Installing rpm-build on Debian is not a compatible RPM sysroot.
        run(['rpm', '-q', 'glibc'])
    run(['pkg-config', '--exists', 'libsecret-1'])
    source = Path(__file__).resolve().parents[1]
    build, output = args.build.resolve(), args.output.resolve()
    if output.exists() and list(output.glob('*.' + args.format)):
        parser.error('Choose an output directory without existing packages of this format')
    build.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    if args.helpers:
        helper_source = args.helpers.resolve()
        for name in ('JumpServerClient', 'client'):
            require_elf(helper_source / ('linux-' + goarch) / name, machine)
        if not (helper_source / 'config.json').is_file():
            parser.error('--helpers must include config.json')
        destination = build / 'bin'
        destination.mkdir(exist_ok=True)
        shutil.copytree(helper_source / ('linux-' + goarch), destination / ('linux-' + goarch), dirs_exist_ok=True)
        shutil.copy2(helper_source / 'config.json', destination / 'config.json')
    options = ['-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=/usr',
               '-DCLIENT_QT_MAJOR=' + args.qt_major,
               '-DCLIENT_GOARCH=' + goarch,
               '-DBUILD_TESTING=' + ('OFF' if args.skip_tests else 'ON'),
               '-DCLIENT_BUILD_LAUNCHERS=' + ('OFF' if args.helpers else 'ON')]
    if not args.helpers:
        go = shutil.which(args.go)
        if not go:
            parser.error('Go is required when --helpers is omitted')
        options.append('-DGO_EXECUTABLE=' + go)
    for option in args.cmake_arg:
        if '=' not in option or option.startswith('-'):
            parser.error('--cmake-arg expects KEY=VALUE')
        options.append('-D' + option)
    run(['cmake', '-S', source, '-B', build, *options])
    run(['cmake', '--build', build, '--parallel', str(args.parallel)])
    require_elf(build / 'client-qt', machine)
    for name in ('JumpServerClient', 'client'):
        require_elf(build / 'bin' / ('linux-' + goarch) / name, machine)
    run(['desktop-file-validate', source / 'packaging/client-qt.desktop'])
    if not args.skip_tests:
        env = dict(os.environ, QT_QPA_PLATFORM='offscreen')
        run(['cmake', '-E', 'chdir', build, 'ctest', '--output-on-failure'], env=env)
    run(['cpack', '--config', build / 'CPackConfig.cmake', '-G', args.format.upper(), '-B', output])
    packages = sorted(output.glob('*.' + args.format))
    if len(packages) != 1:
        raise RuntimeError('Expected one native package, found {}'.format(len(packages)))
    package = packages[0]
    if args.format == 'deb':
        metadata = subprocess.check_output(['dpkg-deb', '--info', str(package)], universal_newlines=True)
        contents = subprocess.check_output(['dpkg-deb', '--contents', str(package)], universal_newlines=True)
    else:
        metadata = subprocess.check_output(['rpm', '-qip', str(package)], universal_newlines=True)
        metadata += subprocess.check_output(['rpm', '-qpR', str(package)], universal_newlines=True)
        contents = subprocess.check_output(['rpm', '-qplv', str(package)], universal_newlines=True)
    for expected in ('/usr/bin/client-qt', '/usr/lib/client-qt/bin/linux-' + goarch + '/JumpServerClient',
                     '/usr/lib/client-qt/bin/linux-' + goarch + '/client', '/usr/share/applications/client-qt.desktop',
                     '/usr/share/icons/hicolor/512x512/apps/client-qt.png', '/usr/share/licenses/client-qt/LICENSE'):
        if expected not in contents:
            raise RuntimeError('Package payload is incomplete: ' + expected)
    for line in contents.splitlines():
        permissions = line.split()[0] if line.split() else ''
        if len(permissions) == 10 and permissions[0] == 'd' and permissions[8] == 'w':
            raise RuntimeError('Package contains a world-writable directory: ' + line)
    if 'libsecret' not in metadata or 'Qt5' not in metadata and 'qt5' not in metadata and 'Qt6' not in metadata and 'qt6' not in metadata:
        raise RuntimeError('Qt/libsecret runtime dependency metadata is missing')
    (output / (package.name + '.metadata.txt')).write_text(metadata + '\n' + contents)
    manifest = {'package': package.name, 'format': args.format, 'architecture': goarch,
                'sha256': hashlib.sha256(package.read_bytes()).hexdigest(),
                'distribution': Path('/etc/os-release').read_text(), 'qt_major': args.qt_major,
                'qt_version': subprocess.check_output(['pkg-config', '--modversion', 'Qt' + args.qt_major + 'Core'], universal_newlines=True).strip(),
                'tests_run': not args.skip_tests, 'signed': False}
    (output / (package.name + '.manifest.json')).write_text(json.dumps(manifest, indent=2) + '\n')
    print('Native package ready: ' + str(package), flush=True)


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print('Linux packaging failed: {}'.format(error), file=sys.stderr)
        sys.exit(1)
