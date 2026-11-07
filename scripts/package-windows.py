#!/usr/bin/env python3
"""Stage a real Windows build, deploy Qt/CRT/TLS, and create NSIS EXE / WiX 3 MSI."""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
MACHINES = {'amd64': 0x8664, '386': 0x14c}
WIX_NS = 'http://schemas.microsoft.com/wix/2006/wi'
GUID_NAMESPACE = uuid.UUID('b04c93d4-f237-499e-b98b-80e56d403d37')


def run(command, label, env=None, quiet=False):
    print(label, flush=True)
    # In particular, never log signtool's command line or captured output: its
    # arguments contain the PFX password. No subprocess is run through a shell.
    result = subprocess.run([str(x) for x in command], env=env,
                            stdout=subprocess.PIPE if quiet else None,
                            stderr=subprocess.STDOUT if quiet else None)
    if result.returncode:
        raise RuntimeError(f'{label} failed (exit {result.returncode})')


def tool(value):
    found = shutil.which(str(value))
    if not found and Path(value).is_file():
        found = str(Path(value).resolve())
    if not found:
        raise RuntimeError(f'Required tool not found: {value}')
    return found


def cache_values(build):
    values = {}
    for line in (build / 'CMakeCache.txt').read_text(encoding='utf-8').splitlines():
        if line and not line.startswith(('#', '//')) and '=' in line and ':' in line:
            key, value = line.split('=', 1)
            values[key.split(':', 1)[0]] = value
    return values


def deployment_major(cache):
    value = cache.get('CLIENT_QT_MAJOR', '5')
    if value not in ('5', '6'):
        raise RuntimeError(f'Unsupported CLIENT_QT_MAJOR in CMakeCache: {value}')
    return int(value)


def windeployqt_options(deployqt, qt_major):
    options = ['--release', '--compiler-runtime']
    qt_bin = Path(deployqt).parent
    angle_library = ((qt_bin / 'libGLESv2.dll').is_file() and (qt_bin / 'libEGL.dll').is_file()) or (qt_bin / 'QtANGLE.dll').is_file()
    if qt_major == 5 and not angle_library:
        options += ['--no-angle', '--no-opengl-sw']
    return options


def package_version(cache):
    version = cache.get('CLIENT_VERSION') or cache.get('CMAKE_PROJECT_VERSION')
    if not version or not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise RuntimeError('CMakeCache must contain CLIENT_VERSION or CMAKE_PROJECT_VERSION as major.minor.patch')
    major, minor, patch = map(int, version.split('.'))
    if major > 255 or minor > 255 or patch > 65535:
        raise RuntimeError('Version exceeds Windows Installer major/minor/build limits')
    return version


def pe_machine(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:2] != b'MZ':
            raise RuntimeError(f'Not a Windows PE binary: {path}')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        pe = stream.read(6)
    if pe[:4] != b'PE\0\0' or len(pe) != 6:
        raise RuntimeError(f'Invalid PE header: {path}')
    return struct.unpack_from('<H', pe, 4)[0]


def require_machine(path, machine):
    if not path.is_file() or pe_machine(path) != machine:
        raise RuntimeError(f'Missing binary or wrong target architecture: {path}')


def copy_dlls(directory, destination, patterns=('*.dll',)):
    directory = Path(directory)
    if not directory.is_dir():
        raise RuntimeError(f'Runtime directory not found: {directory}')
    for pattern in patterns:
        for source in directory.glob(pattern):
            if source.is_file():
                shutil.copy2(source, destination / source.name)


def validate_payload(stage, arch, native_tls=False):
    machine = MACHINES[arch]
    require_machine(stage / 'client.exe', machine)
    qt_versions = [major for major in (5, 6) if (stage / f'Qt{major}Core.dll').is_file()]
    if len(qt_versions) != 1:
        raise RuntimeError('Payload must contain exactly one Qt major version')
    for module in ('Core', 'Gui', 'Widgets', 'Network', 'Svg'):
        require_machine(stage / f'Qt{qt_versions[0]}{module}.dll', machine)
    require_machine(stage / 'platforms' / 'qwindows.dll', machine)
    helper = 'JumpServerClient.exe' if arch == 'amd64' else 'JumpServerClient32.exe'
    require_machine(stage / 'bin' / 'windows' / helper, machine)
    require_machine(stage / 'bin' / 'windows' / 'putty.exe', MACHINES['386'])
    require_machine(stage / 'bin' / 'windows' / 'AutoItX3.dll', MACHINES['386'])
    require_machine(stage / 'bin' / 'windows' / 'AutoItX3_x64.dll', MACHINES['amd64'])
    if not (stage / 'bin' / 'config.json').is_file():
        raise RuntimeError('Missing launcher default configuration')
    # Go dependencies intentionally include both AutoIt architectures and x86 PuTTY.
    for path in stage.rglob('*.dll'):
        if 'bin' not in path.relative_to(stage).parts:
            require_machine(path, machine)
    if not native_tls:
        if not list(stage.glob('libssl*.dll')) or not list(stage.glob('libcrypto*.dll')):
            raise RuntimeError('Missing OpenSSL DLLs; pass --openssl-dir matching this Qt build, '
                               'or --native-tls only for a verified native Schannel Qt build')
    return helper


class Signer:
    def __init__(self, args, directory):
        self.enabled = args.sign or args.sign_file is not None
        if not self.enabled:
            return
        encoded = ''.join(os.environ.get('WINDOWS_CERTIFICATE', '').split())
        password = os.environ.get('WINDOWS_CERTIFICATE_PASSWORD')
        if not encoded or password is None:
            # The hook only runs after the parent selected signed packaging. If
            # credentials disappear there, fail instead of falsely reporting signed.
            if args.sign_file is not None:
                raise RuntimeError('Uninstaller signing credentials became unavailable')
            self.enabled = False
            print('Skipping Windows signing: certificate or password environment variable is missing; output is UNSIGNED.', flush=True)
            return
        try:
            certificate = base64.b64decode(encoded, validate=True)
        except (ValueError, TypeError):
            raise RuntimeError('WINDOWS_CERTIFICATE must be a base64-encoded PFX') from None
        if not certificate:
            raise RuntimeError('WINDOWS_CERTIFICATE contains an empty PFX')
        self.pfx = directory / 'signing.pfx'
        self.pfx.write_bytes(certificate)
        self.pfx.chmod(0o600)
        self.password = password
        self.executable = tool(args.signtool)
        self.timestamp = args.timestamp_url

    def sign(self, path):
        if not self.enabled:
            return
        run([self.executable, 'sign', '/fd', 'SHA256', '/td', 'SHA256', '/tr', self.timestamp,
             '/f', self.pfx, '/p', self.password, path], f'Signing {path.name}', quiet=True)
        run([self.executable, 'verify', '/pa', path], f'Verifying signature of {path.name}', quiet=True)


def nsis_string(value):
    return str(value).replace('$', '$$').replace('"', '$\\"').replace('\n', '$\\n').replace('\r', '')


def write_nsis_manifest(stage, destination):
    files = sorted(p for p in stage.rglob('*') if p.is_file())
    lines = ['; Generated from the staged payload. Delete only files owned by this package.',
             '!macro InstallPayload']
    for path in files:
        relative = path.relative_to(stage)
        folder = relative.parent.as_posix().replace('/', '\\')
        folder = '' if folder == '.' else '\\' + nsis_string(folder)
        lines += [f' SetOutPath "$INSTDIR{folder}"', f' File "{nsis_string(path)}"']
    lines += ['!macroend', '!macro RemovePayload']
    for path in reversed(files):
        relative = path.relative_to(stage).as_posix().replace('/', '\\')
        lines.append(f' Delete "$INSTDIR\\{nsis_string(relative)}"')
    directories = sorted((p for p in stage.rglob('*') if p.is_dir()), key=lambda p: len(p.parts), reverse=True)
    for path in directories:
        relative = path.relative_to(stage).as_posix().replace('/', '\\')
        lines.append(f' RMDir "$INSTDIR\\{nsis_string(relative)}"')
    lines += ['!macroend', '']
    destination.write_text('\n'.join(lines), encoding='utf-8')


def write_wix_payload(stage, destination, arch):
    def tag(name):
        return f'{{{WIX_NS}}}{name}'
    def identifier(prefix, relative):
        return prefix + hashlib.sha256(relative.encode('utf-8')).hexdigest()[:24]
    root = ET.Element(tag('Wix'))
    fragment = ET.SubElement(root, tag('Fragment'))
    install = ET.SubElement(fragment, tag('DirectoryRef'), Id='INSTALLFOLDER')
    group = ET.SubElement(fragment, tag('ComponentGroup'), Id='PayloadComponents')
    directories = {Path('.'): install}
    paths = sorted((p for p in stage.rglob('*') if p.is_dir()), key=lambda p: (len(p.parts), str(p)))
    for path in paths:
        relative = path.relative_to(stage)
        directories[relative] = ET.SubElement(directories[relative.parent], tag('Directory'),
                                              Id=identifier('d', relative.as_posix()), Name=path.name)
    for path in sorted(p for p in stage.rglob('*') if p.is_file()):
        relative = path.relative_to(stage)
        key = relative.as_posix()
        component_id = identifier('c', key)
        component = ET.SubElement(directories[relative.parent], tag('Component'), Id=component_id,
                                  Guid=str(uuid.uuid5(GUID_NAMESPACE, arch + '/' + key)),
                                  Win64='yes' if arch == 'amd64' else 'no')
        file_id = 'MainExecutable' if key == 'client.exe' else identifier('f', key)
        ET.SubElement(component, tag('File'), Id=file_id, Source=str(path), Name=path.name)
        # Machine-level key paths keep installation and repair system-wide.
        ET.SubElement(component, tag('RegistryValue'), Root='HKLM',
                      Key='Software\\FIT2CLOUD\\OSMClient\\InstallerComponents',
                      Name=component_id, Type='integer', Value='1', KeyPath='yes')
        ET.SubElement(group, tag('ComponentRef'), Id=component_id)
    for relative, directory in directories.items():
        key = relative.as_posix()
        component_id = identifier('cleanup', key)
        component = ET.SubElement(directory, tag('Component'), Id=component_id,
                                  Guid=str(uuid.uuid5(GUID_NAMESPACE, arch + '/directory/' + key)),
                                  Win64='yes' if arch == 'amd64' else 'no')
        ET.SubElement(component, tag('RemoveFolder'), Id=identifier('remove', key), On='uninstall')
        ET.SubElement(component, tag('RegistryValue'), Root='HKLM',
                      Key='Software\\FIT2CLOUD\\OSMClient\\InstallerComponents',
                      Name=component_id, Type='integer', Value='1', KeyPath='yes')
        ET.SubElement(group, tag('ComponentRef'), Id=component_id)
    ET.register_namespace('', WIX_NS)
    ET.ElementTree(root).write(destination, encoding='utf-8', xml_declaration=True)


def build_custom_action(compiler, destination, arch):
    source = ROOT / 'packaging' / 'windows-jms-ca.cpp'
    if Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'):
        command = [compiler, '/nologo', '/LD', '/O2', '/GS-', '/utf-8', str(source),
                   '/Fo' + str(destination.with_suffix('.obj')), '/link', '/OUT:' + str(destination),
                   '/NODEFAULTLIB', '/ENTRY:' + ('DllMain' if arch == 'amd64' else '_DllMain@12'),
                   '/IMPLIB:' + str(destination.with_suffix('.lib')),
                   'msi.lib', 'advapi32.lib', 'kernel32.lib']
        if arch == '386':
            command.append('/EXPORT:PrepareJmsRegistration=_PrepareJmsRegistration@4')
    else:
        entry = 'DllMain' if arch == 'amd64' else 'DllMain@12'
        command = [compiler, '-shared', '-O2', '-nostdlib', '-fno-stack-protector',
                   '-Wl,--kill-at,--entry,' + entry, str(source), '-lmsi', '-ladvapi32', '-lkernel32', '-o', str(destination)]
    run(command, 'Building MSI protocol ownership guard')
    require_machine(destination, MACHINES[arch])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--output', type=Path, default=ROOT / 'dist')
    parser.add_argument('--config', choices=('Release', 'RelWithDebInfo', 'MinSizeRel'), default='Release')
    parser.add_argument('--arch', choices=MACHINES, default='amd64')
    parser.add_argument('--format', choices=('exe', 'msi', 'both'), default='both')
    parser.add_argument('--windeployqt', default='windeployqt')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--makensis', default='makensis')
    parser.add_argument('--candle', default='candle')
    parser.add_argument('--light', default='light')
    parser.add_argument('--compiler', help='Matching MSVC/MinGW C++ compiler; defaults to CMakeCache')
    parser.add_argument('--runtime-dir', type=Path, help='App-local MSVC CRT DLL directory; overrides VCToolsRedistDir')
    parser.add_argument('--ucrt-dir', type=Path, help='Optional matching Windows SDK app-local UCRT DLL directory for legacy targets')
    parser.add_argument('--openssl-dir', type=Path, help='OpenSSL libssl/libcrypto DLLs matching the target Qt ABI')
    parser.add_argument('--native-tls', action='store_true', help='Assert this Qt build uses native Schannel instead of OpenSSL')
    parser.add_argument('--skip-build', action='store_true')
    parser.add_argument('--sign', action='store_true', help='Sign using PFX environment credentials; skip explicitly if credentials are missing')
    parser.add_argument('--signtool', default='signtool')
    parser.add_argument('--timestamp-url', default='http://timestamp.digicert.com')
    parser.add_argument('--sign-file', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if sys.platform != 'win32':
        parser.error('Packaging requires Windows with a real Qt build; cross-host template checks do not produce a deliverable')
    if not args.sign_file and not args.build:
        parser.error('--build is required')
    with tempfile.TemporaryDirectory(prefix='osm-windows-package-') as temporary:
        work = Path(temporary)
        signer = Signer(args, work)
        if args.sign_file:
            signer.sign(args.sign_file.resolve())
            return
        build = args.build.resolve()
        cache = cache_values(build)
        version = package_version(cache)
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=True)
        artifacts = work / 'artifacts'
        artifacts.mkdir()
        label = f'OSMClient-{version}-windows-{args.arch}'
        formats = ('exe', 'msi') if args.format == 'both' else (args.format,)
        for extension in formats:
            if any((output / f'{label}.{extension}{suffix}').exists()
                   for suffix in ('', '.sha256', '.manifest.json')):
                raise RuntimeError(f'Destination exists: {output / (label + "." + extension)}')
        if not args.skip_build:
            run([tool(args.cmake), '--build', build, '--config', args.config, '--target', 'client-qt'], 'Building Windows GUI and launchers')
        executable = build / args.config / 'client.exe'
        if not executable.is_file():
            executable = build / 'client.exe'
        require_machine(executable, MACHINES[args.arch])
        stage = work / 'payload'
        stage.mkdir()
        shutil.copy2(executable, stage / 'client.exe')
        shutil.copytree(build / 'bin' / 'windows', stage / 'bin' / 'windows')
        shutil.copy2(build / 'bin' / 'config.json', stage / 'bin' / 'config.json')
        alternate = stage / 'bin' / 'windows' / ('JumpServerClient32.exe' if args.arch == 'amd64' else 'JumpServerClient.exe')
        if alternate.exists():
            alternate.unlink()
        shutil.copy2(ROOT / 'LICENSE', stage / 'LICENSE.txt')
        deployqt = tool(args.windeployqt)
        deploy_options = windeployqt_options(deployqt, deployment_major(cache))
        run([deployqt, *deploy_options, '--dir', stage, stage / 'client.exe'], 'Deploying matching Qt runtime and plugins')
        compiler = tool(args.compiler or cache.get('CMAKE_CXX_COMPILER', 'cl'))
        is_msvc = Path(compiler).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe')
        if is_msvc:
            runtime = args.runtime_dir
            if runtime is None:
                redist = os.environ.get('VCToolsRedistDir')
                arch = 'x64' if args.arch == 'amd64' else 'x86'
                candidates = sorted((Path(redist) / arch).glob('Microsoft.VC*.CRT')) if redist else []
                runtime = candidates[-1] if candidates else None
            if runtime is None:
                raise RuntimeError('App-local MSVC CRT required: initialize the matching Developer Prompt or pass --runtime-dir')
            copy_dlls(runtime, stage)
            # windeployqt may copy vc_redist.exe; this installer uses app-local CRT
            # instead of silently attempting an elevated nested runtime installer.
            for path in stage.glob('vc_redist*.exe'):
                path.unlink()
            for name in ('vcruntime140.dll', 'msvcp140.dll'):
                require_machine(stage / name, MACHINES[args.arch])
        if args.ucrt_dir:
            copy_dlls(args.ucrt_dir, stage)
        if args.openssl_dir:
            copy_dlls(args.openssl_dir, stage, ('libssl*.dll', 'libcrypto*.dll'))
        elif not args.native_tls:
            copy_dlls(Path(deployqt).parent, stage, ('libssl*.dll', 'libcrypto*.dll'))
        helper = validate_payload(stage, args.arch, args.native_tls)
        (stage / 'qt.conf').write_text('[Paths]\nPrefix=.\nPlugins=.\n', encoding='utf-8')
        signer.sign(stage / 'client.exe')
        signer.sign(stage / 'bin' / 'windows' / helper)
        # windeployqt deploys qwindows, but need not deploy qoffscreen. Validate
        # the same native plugin that users run (CI must have a desktop session).
        env = dict(os.environ, QT_QPA_PLATFORM='windows')
        for name in ('QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QML2_IMPORT_PATH'):
            env.pop(name, None)
        env['PATH'] = str(stage) + os.pathsep + str(Path(os.environ['SystemRoot']) / 'System32') + os.pathsep + os.environ['SystemRoot']
        run([stage / 'client.exe', '--smoke-test'], 'Launching isolated deployed GUI smoke test', env=env)
        if 'exe' in formats:
            manifest = work / 'payload.nsh'
            write_nsis_manifest(stage, manifest)
            command = [tool(args.makensis), '/V2', '/DVERSION=' + version,
                       '/DARCH=' + args.arch, '/DMANIFEST=' + str(manifest),
                       '/DOUTPUT=' + str(artifacts / f'{label}.exe')]
            if signer.enabled:
                # Only paths/tool options enter the generated command, never credentials.
                sign_command = subprocess.list2cmdline([sys.executable, str(Path(__file__).resolve()),
                    '--signtool', signer.executable, '--timestamp-url', signer.timestamp, '--sign-file'])
                command.append('/DSIGN_UNINSTALLER=' + sign_command)
            command.append(ROOT / 'packaging' / 'windows.nsi')
            run(command, 'Building NSIS EXE installer')
        if 'msi' in formats:
            custom_action = work / 'JmsRegistration.dll'
            build_custom_action(compiler, custom_action, args.arch)
            signer.sign(custom_action)
            payload = work / 'payload.wxs'
            write_wix_payload(stage, payload, args.arch)
            wix_arch = 'x64' if args.arch == 'amd64' else 'x86'
            run([tool(args.candle), '-nologo', '-arch', wix_arch, '-dVersion=' + version,
                 '-dProtocolGuard=' + str(custom_action), '-dIcon=' + str(ROOT / 'resources' / 'icon.ico'),
                 '-dProgramFilesDirectory=' + ('ProgramFiles64Folder' if args.arch == 'amd64' else 'ProgramFilesFolder'),
                 '-out', str(work) + os.sep, ROOT / 'packaging' / 'windows.wxs', payload], 'Compiling WiX 3 MSI sources')
            run([tool(args.light), '-nologo', '-out', artifacts / f'{label}.msi',
                 work / 'windows.wixobj', work / 'payload.wixobj'], 'Linking and validating MSI installer')
        for extension in formats:
            artifact = artifacts / f'{label}.{extension}'
            signer.sign(artifact)
        # Publish locally only after every requested package and signature passed.
        for extension in formats:
            artifact = artifacts / f'{label}.{extension}'
            digest = hashlib.sha256(artifact.read_bytes()).hexdigest()
            target = output / artifact.name
            with artifact.open('rb') as source, target.open('xb') as destination:
                shutil.copyfileobj(source, destination)
            with target.with_suffix(target.suffix + '.sha256').open('x', encoding='ascii') as checksum:
                checksum.write(f'{digest}  {target.name}\n')
            metadata = {'artifact': target.name, 'version': version, 'arch': args.arch,
                        'format': extension, 'signed': signer.enabled, 'sha256': digest}
            with target.with_suffix(target.suffix + '.manifest.json').open('x', encoding='utf-8') as manifest:
                json.dump(metadata, manifest, indent=2)
                manifest.write('\n')
            print(f'Created {target} ({"signed" if signer.enabled else "UNSIGNED"})')


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError) as exc:
        print(f'Windows packaging failed: {exc}', file=sys.stderr)
        sys.exit(1)
