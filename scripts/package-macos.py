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
from replay_payload import deploy_replay_plugins, without_ffmpeg


MACHO_MAGIC = {bytes.fromhex(value) for value in (
    'feedface', 'feedfacf', 'cefaedfe', 'cffaedfe',
    'cafebabe', 'bebafeca', 'cafebabf', 'bfbafeca')}
LOAD_COMMANDS = {'LC_LOAD_DYLIB', 'LC_LOAD_WEAK_DYLIB', 'LC_REEXPORT_DYLIB',
                 'LC_LOAD_UPWARD_DYLIB', 'LC_LAZY_LOAD_DYLIB'}
QT_RUNTIME_PLUGINS = {
    5: (
        'platforms/libqcocoa.dylib',
        'styles/libqmacstyle.dylib',
        'imageformats/libqjpeg.dylib',
        'imageformats/libqwebp.dylib',
        'imageformats/libqsvg.dylib',
        'iconengines/libqsvgicon.dylib',
        'mediaservice/libqavfmediaplayer.dylib',
        'mediaservice/libqtmedia_audioengine.dylib',
        'audio/libqtaudio_coreaudio.dylib',
    ),
    6: (
        'platforms/libqcocoa.dylib',
        'styles/libqmacstyle.dylib',
        'imageformats/libqjpeg.dylib',
        'imageformats/libqwebp.dylib',
        'imageformats/libqsvg.dylib',
        'iconengines/libqsvgicon.dylib',
        'multimedia/libdarwinmediaplugin.dylib',
        'tls/libqsecuretransportbackend.dylib',
    ),
}


def cmake_cache_value(cache, key):
    match = re.search(r'^{}:[^=]*=(.+)$'.format(re.escape(key)),
                      cache.read_text(), re.MULTILINE)
    return match.group(1).strip() if match else None


def qt_runtime_roots(build, major, macdeployqt):
    cache = build / 'CMakeCache.txt'
    prefixes = []
    value = cmake_cache_value(cache, 'CMAKE_PREFIX_PATH')
    if value:
        prefixes.extend(Path(item) for item in value.split(';'))
    for key in ('Qt{}_DIR'.format(major), 'Qt{}Core_DIR'.format(major)):
        value = cmake_cache_value(cache, key)
        if not value:
            continue
        for item in value.split(';'):
            path = Path(item)
            prefixes.append(path.parents[2] if len(path.parents) >= 3 else path)
    for path in (Path(macdeployqt).absolute(), Path(macdeployqt).resolve()):
        prefixes.append(path.parent.parent)
    seen = set()
    for prefix in prefixes:
        prefix = prefix.resolve()
        if prefix in seen:
            continue
        seen.add(prefix)
        for plugins in (prefix / 'plugins', prefix / 'share/qt/plugins'):
            if (plugins / 'platforms/libqcocoa.dylib').is_file():
                return plugins, prefix / 'lib'
    raise RuntimeError('Matching Qt Cocoa plugin root is missing')


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
    executable = bundle / 'Contents/MacOS/client'
    frameworks = bundle / 'Contents/Frameworks'
    frameworks.mkdir(parents=True, exist_ok=True)
    search_paths = list(dict.fromkeys(path.resolve() for path in search_paths))
    for path in macho_files(bundle):
        for dependency in inspect_macho(path)['dependencies']:
            if not system_library(dependency) and Path(dependency).is_absolute():
                source = Path(dependency)
                if source.is_file():
                    search_paths.append(source.parent.resolve())
    search_paths = list(dict.fromkeys(search_paths))
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
                    external_root = target.parent.resolve()
                    if external_root not in search_paths:
                        search_paths.append(external_root)
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


def deploy_qt_plugins(source, destination, major):
    plugins = QT_RUNTIME_PLUGINS.get(major)
    if plugins is None:
        raise RuntimeError('Unsupported Qt major version for macOS packaging: {}'.format(major))
    if not (source / 'platforms/libqcocoa.dylib').is_file():
        raise RuntimeError('Target Qt Cocoa plugin is missing')
    copied = []
    for relative in plugins:
        plugin = source / relative
        if not plugin.is_file():
            continue
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(plugin, target)
        copied.append(relative)
    return copied


def remove_development_artifacts(bundle):
    frameworks = bundle / 'Contents/Frameworks'
    if not frameworks.is_dir():
        return
    headers = sorted((path for path in frameworks.rglob('Headers')
                      if path.is_symlink() or path.is_dir()),
                     key=lambda path: len(path.parts), reverse=True)
    for path in headers:
        if path.is_symlink():
            path.unlink()
        else:
            shutil.rmtree(path)
    for path in frameworks.rglob('*'):
        if path.is_file() and path.suffix in ('.a', '.prl'):
            path.unlink()


def audit_bundle(bundle):
    executable = bundle / 'Contents/MacOS/client'
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
    main_executable = bundle / 'Contents/MacOS/client'
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
    parser.add_argument('--macdeployqt', type=Path, required=True,
                        help='Matching Qt macdeployqt; used to locate the Qt runtime roots')
    parser.add_argument('--libpath', type=Path, action='append', default=[],
                        help='Additional dependency library search directory (repeatable)')
    parser.add_argument('--plugins-dir', type=Path,
                        help='Target Qt plugin root for a cross-build; defaults to the macdeployqt Qt prefix')
    parser.add_argument('--output', type=Path, default=Path('dist/OSMClient.app'))
    parser.add_argument('--sign', action='store_true', help='Request Developer ID signing; skip when no signing identity is available')
    parser.add_argument('--deployment-target', required=True, help='Reject every binary requiring a newer macOS')
    parser.add_argument('--skip-smoke', action='store_true', help='Cross-build only: record that runtime smoke was not performed')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('Run this on the macOS build host')
    args.sign = resolve_signing(args.sign)
    source = args.build.resolve() / 'client.app'
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
    qt_major = 6 if 'Qt6Multimedia_DIR:' in (args.build / 'CMakeCache.txt').read_text() else 5
    qt_plugins, qt_lib = ((args.plugins_dir.resolve(), None)
                          if args.plugins_dir
                          else qt_runtime_roots(args.build.resolve(), qt_major, args.macdeployqt))
    copied_plugins = deploy_qt_plugins(qt_plugins, destination / 'Contents/PlugIns', qt_major)
    (destination / 'Contents/Resources/qt.conf').write_text('[Paths]\nPlugins = PlugIns\n')
    destination.with_suffix('.deployment.log').write_text(
        'Deployed Qt runtime plugin allowlist and recursive Mach-O dependencies:\n{}\n'.format(
            '\n'.join(copied_plugins)))
    deploy_replay_plugins(qt_plugins, destination / 'Contents/PlugIns', qt_major, windows=False)
    without_ffmpeg(destination)
    library_paths = [path.resolve() for path in args.libpath]
    if qt_lib:
        library_paths.append(qt_lib)
    repair_dependencies(destination, library_paths)
    remove_development_artifacts(destination)
    without_ffmpeg(destination)
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
