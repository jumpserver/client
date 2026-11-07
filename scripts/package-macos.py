#!/usr/bin/env python3
"""Create a self-contained local macOS app; does not install or publish it."""
import argparse
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import stat
import subprocess
import sys
from macos_common import sign, resolve_signing, smoke


MACHO_MAGIC = {bytes.fromhex(value) for value in (
    'feedface', 'feedfacf', 'cefaedfe', 'cffaedfe',
    'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}
LOAD_COMMANDS = {'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
                 'LC_LOAD_UPWARD_DYLIB', 'LC_LAZY_LOAD_DYLIB'}


def inside(path, root):
    return os.path.commonpath((str(path.resolve()), str(root.resolve()))) == str(root.resolve())


def macho_files(root):
    files = []
    for path in root.rglob('*'):
        if path.is_symlink() or not path.is_file():
            continue
        with path.open('rb') as stream:
            if stream.read(4) in MACHO_MAGIC:
                files.append(path)
    return sorted(files)


def inspect_macho(path):
    output = subprocess.check_output(['otool', '-l', str(path)], text=True)
    info = {'dependencies': [], 'rpaths': [], 'id': None, 'minimum_os': []}
    for block in re.split(r'\nLoad command \d+\n', output)[1:]:
        command = re.search(r'^\s*cmd (\S+)', block, re.MULTILINE)
        if not command:
            continue
        command = command.group(1)
        if command in LOAD_COMMANDS or command == 'LC_ID_DYLIB':
            name = re.search(r'^\s*name (.+) \(offset \d+\)', block, re.MULTILINE).group(1)
            if command == 'LC_ID_DYLIB':
                info['id'] = name
            else:
                info['dependencies'].append(name)
        elif command == 'LC_RPATH':
            info['rpaths'].append(re.search(r'^\s*path (.+) \(offset \d+\)', block, re.MULTILINE).group(1))
        elif command in ('LC_BUILD_VERSION', 'LC_VERSION_MIN_MACOSX'):
            minimum = re.search(r'^\s*(?:minos|version) ([\d.]+)', block, re.MULTILINE)
            if minimum:
                info['minimum_os'].append(minimum.group(1))
    return info


def system_library(name):
    return name.startswith(('/System/Library/', '/usr/lib/'))


def expand_path(name, owner, executable):
    for prefix, directory in (('@loader_path', owner.parent),
                              ('@executable_path', executable.parent)):
        if name == prefix or name.startswith(prefix + '/'):
            return directory / name[len(prefix):].lstrip('/')
    if name.startswith('/'):
        return Path(name)
    return None


def resolve_dependency(name, owner, info, executable, frameworks, search_paths):
    candidates = []
    direct = expand_path(name, owner, executable)
    if direct:
        candidates.append(direct)
    if name.startswith('@rpath/'):
        suffix = name[len('@rpath/'):]
        candidates.append(frameworks / suffix)
        for rpath in info['rpaths'] + inspect_macho(executable)['rpaths']:
            directory = expand_path(rpath, owner, executable)
            if directory:
                candidates.append(directory / suffix)
    else:
        suffix = name
    # macdeployqt can copy a dylib while leaving its transitive @rpath entries
    # unresolved. Search explicit library roots as well as the deployed directory.
    candidates.append(frameworks / Path(name).name)
    for directory in search_paths:
        if not suffix.startswith(('@', '/')):
            candidates.append(directory / suffix)
        candidates.append(directory / Path(name).name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate.resolve()
    raise RuntimeError('Unresolved dependency {} in {}'.format(name, owner))


def copy_dependency(source, frameworks):
    framework = next((parent for parent in source.parents if parent.suffix == '.framework'), None)
    if framework:
        destination = frameworks / framework.name
        if not destination.exists():
            shutil.copytree(framework, destination, symlinks=True)
        return destination / source.relative_to(framework)
    destination = frameworks / source.name
    if not destination.exists():
        shutil.copy2(source, destination)
    return destination


def repair_dependencies(bundle, search_paths):
    executable = bundle / 'Contents/MacOS/client-qt'
    frameworks = bundle / 'Contents/Frameworks'
    frameworks.mkdir(parents=True, exist_ok=True)
    processed = set()
    while True:
        pending = [path for path in macho_files(bundle) if path not in processed]
        if not pending:
            break
        for path in pending:
            info = inspect_macho(path)
            changes = []
            for dependency in dict.fromkeys(info['dependencies']):
                if system_library(dependency):
                    continue
                target = resolve_dependency(dependency, path, info, executable, frameworks, search_paths)
                if not inside(target, bundle):
                    target = copy_dependency(target, frameworks)
                replacement = '@loader_path/' + os.path.relpath(target, path.parent)
                if dependency != replacement:
                    changes.extend(['-change', dependency, replacement])
            if info['id']:
                replacement = '@loader_path/' + path.name
                if info['id'] != replacement:
                    changes.extend(['-id', replacement])
            for rpath in dict.fromkeys(info['rpaths']):
                expanded = expand_path(rpath, path, executable)
                if expanded is None or not inside(expanded, bundle):
                    changes.extend(['-delete_rpath', rpath])
            if changes:
                path.chmod(path.stat().st_mode | stat.S_IWUSR)
                subprocess.run(['install_name_tool', *changes, str(path)], check=True,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            processed.add(path)


def audit_bundle(bundle):
    executable = bundle / 'Contents/MacOS/client-qt'
    architecture = subprocess.check_output(['lipo', '-archs', str(executable)], text=True).strip()
    if architecture not in ('arm64', 'x86_64'):
        raise RuntimeError('Expected a single macOS application architecture')
    report = {'bundle': str(bundle), 'minimum_macos': None, 'binaries': {}}
    report['architecture'] = architecture
    minimums = []
    for path in macho_files(bundle):
        info = inspect_macho(path)
        info['architecture'] = subprocess.check_output(['lipo', '-archs', str(path)], text=True).strip()
        if info['architecture'] != architecture:
            raise RuntimeError('Mismatched binary architecture: ' + str(path))
        for dependency in info['dependencies']:
            if system_library(dependency):
                continue
            target = expand_path(dependency, path, executable)
            if target is None or not target.is_file() or not inside(target, bundle):
                raise RuntimeError('Nonportable or missing dependency {} in {}'.format(dependency, path))
        for rpath in info['rpaths']:
            target = expand_path(rpath, path, executable)
            if target is None or not inside(target, bundle):
                raise RuntimeError('External runtime search path {} in {}'.format(rpath, path))
        if info['id'] and info['id'].startswith('/'):
            raise RuntimeError('External install ID {} in {}'.format(info['id'], path))
        report['binaries'][str(path.relative_to(bundle))] = info
        minimums.extend(info['minimum_os'])
    if not report['binaries']:
        raise RuntimeError('No Mach-O executables found in deployed app')
    if minimums:
        report['minimum_macos'] = max(minimums, key=lambda version: tuple(map(int, version.split('.'))))
    return report


def sign_bundle(bundle, release=False):
    # Sign nested code first, then framework resource seals, then the outer app.
    entitlements = Path(__file__).resolve().parents[1] / 'packaging/entitlements.macos.plist'
    main_executable = bundle / 'Contents/MacOS/client-qt'
    for path in macho_files(bundle):
        if path == main_executable:
            continue # Signing this path also seals its enclosing app bundle.
        automation = path.name == 'JumpServerClient'
        sign(path, release, entitlements if automation else None)
    nested = [path for path in bundle.rglob('*')
              if path.is_dir() and not path.is_symlink() and path.suffix in ('.framework', '.app')]
    for path in sorted(nested, key=lambda item: len(item.parts), reverse=True):
        sign(path, release, None)
    sign(main_executable, release, entitlements)
    sign(bundle, release, entitlements)
    subprocess.run(['codesign', '--verify', '--deep', '--strict', str(bundle)], check=True)


def enforce_deployment(report, target):
    if not re.fullmatch(r'\d+\.\d+(?:\.\d+)?', target):
        raise RuntimeError('Invalid macOS deployment target: ' + target)
    version = lambda value: tuple(map(int, value.split('.'))) + (0,) * (3 - len(value.split('.')))
    for name, info in report['binaries'].items():
        minimums = info.get('minimum_os', [])
        if not minimums or any(version(v) > version(target) for v in minimums):
            raise RuntimeError('Deployment target {} exceeded or missing: {} ({})'.format(target, name, minimums))
    report['deployment_target'] = target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--macdeployqt', type=Path, required=True)
    parser.add_argument('--libpath', type=Path, action='append', default=[],
                        help='Additional dependency library search directory (repeatable)')
    parser.add_argument('--plugins-dir', type=Path, help='Target Qt plugins for a cross-build using native macdeployqt')
    parser.add_argument('--output', type=Path, default=Path('dist/OSMClient.app'))
    parser.add_argument('--sign', action='store_true', help='Request Developer ID signing; skip when no signing identity is available')
    parser.add_argument('--deployment-target', required=True, help='Reject every binary requiring a newer macOS')
    parser.add_argument('--skip-smoke', action='store_true', help='Cross-build only: record that runtime smoke was not performed')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('Run this on the macOS build host')
    args.sign = resolve_signing(args.sign)
    source = args.build.resolve() / 'client-qt.app'
    destination = args.output.resolve()
    if destination.exists():
        parser.error(f'Output exists; choose a fresh output path: {destination}')
    helpers = source / 'Contents/Resources/bin'
    if not source.is_dir() or not helpers.is_dir():
        parser.error('Build the complete app including launchers before packaging')
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, destination, symlinks=True)
    info_path = destination / 'Contents/Info.plist'
    info = plistlib.loads(info_path.read_bytes())
    info['LSMinimumSystemVersion'] = args.deployment_target
    info_path.write_bytes(plistlib.dumps(info))
    if args.plugins_dir:
        # macdeployqt searches its own Qt prefix even with -libpath/-no-plugins.
        # Cross-builds must collect target libraries through the Mach-O resolver.
        if not (args.plugins_dir / 'platforms/libqcocoa.dylib').is_file():
            raise RuntimeError('Target Qt Cocoa plugin is missing')
        shutil.copytree(args.plugins_dir, destination / 'Contents/PlugIns', symlinks=True)
        (destination / 'Contents/Resources/qt.conf').write_text('[Paths]\nPlugins = PlugIns\n')
        destination.with_suffix('.deployment.log').write_text('Target Qt plugins and recursive Mach-O dependency deployment.\n')
    else:
        deployed = subprocess.run([str(args.macdeployqt.resolve()), str(destination),
                                   '-always-overwrite',
                                   *['-libpath=' + str(path.resolve()) for path in args.libpath]],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        destination.with_suffix('.deployment.log').write_text(deployed.stdout)
        if deployed.returncode != 0 or re.search(r'^\s*ERROR\b', deployed.stdout, re.MULTILINE):
            raise RuntimeError('macdeployqt reported a failure; inspect {}'.format(destination.with_suffix('.deployment.log')))
    repair_dependencies(destination, [path.resolve() for path in args.libpath])
    report = audit_bundle(destination)
    enforce_deployment(report, args.deployment_target)
    sign_bundle(destination, args.sign)
    if not args.skip_smoke:
        smoke(destination)
    report['smoke_tested'] = not args.skip_smoke
    report['signing'] = 'developer-id' if args.sign else 'ad-hoc'
    destination.with_suffix('.manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Local development app ready: {destination}')
    print('Recursive Mach-O minimum macOS: {}'.format(report['minimum_macos']))
    print('Dependency manifest: {}'.format(destination.with_suffix('.manifest.json')))
    print('Developer ID signature verified; notarize the DMG before distribution.' if args.sign
          else 'Ad-hoc signature only; use --sign and notarize the DMG for distribution.')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
        print('Packaging failed: {}'.format(exc), file=sys.stderr)
        if isinstance(exc, subprocess.CalledProcessError) and exc.stderr:
            print(exc.stderr, file=sys.stderr)
        sys.exit(1)
