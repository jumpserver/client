"""Windows package logic tests; synthetic payloads are never release artifacts."""
import argparse
import base64
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('package_windows', ROOT / 'scripts' / 'package-windows.py')
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)


def synthetic_pe(path, machine):
    """Only a PE header for architecture validation, not an executable program."""
    path.parent.mkdir(parents=True, exist_ok=True)
    header = bytearray(128)
    header[:2] = b'MZ'
    struct.pack_into('<I', header, 60, 64)
    header[64:68] = b'PE\0\0'
    struct.pack_into('<H', header, 68, machine)
    path.write_bytes(header)


class WindowsPackagingTests(unittest.TestCase):
    def test_version_from_cmake_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            build = Path(temporary)
            (build / 'CMakeCache.txt').write_text('# ignored\nCMAKE_PROJECT_VERSION:STATIC=4.1.2\nCLIENT_VERSION:STRING=4.2.7\n', encoding='utf-8')
            self.assertEqual(PACKAGE.package_version(PACKAGE.cache_values(build)), '4.2.7')
        self.assertEqual(PACKAGE.package_version({'CMAKE_PROJECT_VERSION': '4.1.2'}), '4.1.2')
        for version in ('v4.1.2', '4.1', '4.1.2-beta', '4.1.2.3', '256.0.0', '1.256.0', '1.0.65536'):
            with self.subTest(version=version), self.assertRaises(RuntimeError):
                PACKAGE.package_version({'CLIENT_VERSION': version})

    def test_windeployqt_options_skip_angle_when_qt5_lacks_angle_libraries(self):
        with tempfile.TemporaryDirectory() as temporary:
            qt_bin = Path(temporary)
            deployqt = qt_bin / 'windeployqt.exe'
            (qt_bin / 'libGLESv2.dll').write_bytes(b'synthetic ANGLE library')
            self.assertEqual(PACKAGE.windeployqt_options(deployqt, 5),
                             ['--release', '--compiler-runtime', '--no-angle', '--no-opengl-sw'])
            (qt_bin / 'libEGL.dll').write_bytes(b'synthetic ANGLE library')
            self.assertEqual(PACKAGE.windeployqt_options(deployqt, 5), ['--release', '--compiler-runtime'])
            (qt_bin / 'libGLESv2.dll').unlink()
            (qt_bin / 'libEGL.dll').unlink()
            (qt_bin / 'QtANGLE.dll').write_bytes(b'synthetic combined ANGLE library')
            self.assertEqual(PACKAGE.windeployqt_options(deployqt, 5), ['--release', '--compiler-runtime'])
            (qt_bin / 'QtANGLE.dll').unlink()
            self.assertEqual(PACKAGE.windeployqt_options(deployqt, 5),
                             ['--release', '--compiler-runtime', '--no-angle', '--no-opengl-sw'])
            self.assertEqual(PACKAGE.windeployqt_options(deployqt, 6), ['--release', '--compiler-runtime'])
            self.assertEqual(PACKAGE.deployment_major({'CLIENT_QT_MAJOR': '6'}), 6)
            self.assertEqual(PACKAGE.deployment_major({}), 5)
            with self.assertRaisesRegex(RuntimeError, 'Unsupported'):
                PACKAGE.deployment_major({'CLIENT_QT_MAJOR': '7'})

    def test_pe_architecture_checks_reject_missing_and_non_windows(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / 'client.exe'
            with self.assertRaises(RuntimeError):
                PACKAGE.require_machine(binary, PACKAGE.MACHINES['amd64'])
            binary.write_bytes(b'not an executable')
            with self.assertRaises(RuntimeError):
                PACKAGE.pe_machine(binary)
            synthetic_pe(binary, PACKAGE.MACHINES['386'])
            with self.assertRaises(RuntimeError):
                PACKAGE.require_machine(binary, PACKAGE.MACHINES['amd64'])
            PACKAGE.require_machine(binary, PACKAGE.MACHINES['386'])

    def test_payload_requires_qt_plugins_helper_and_tls(self):
        for arch in ('amd64', '386'):
            with self.subTest(arch=arch), tempfile.TemporaryDirectory() as temporary:
                stage = Path(temporary)
                machine = PACKAGE.MACHINES[arch]
                helper = 'JumpServerClient.exe' if arch == 'amd64' else 'JumpServerClient32.exe'
                for file in ('client.exe', 'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll',
                             'Qt5Network.dll', 'Qt5Svg.dll', 'platforms/qwindows.dll',
                             'libssl-1_1.dll', 'libcrypto-1_1.dll', 'bin/windows/' + helper):
                    synthetic_pe(stage / file, machine)
                for file, bitness in (('putty.exe', '386'), ('AutoItX3.dll', '386'), ('AutoItX3_x64.dll', 'amd64')):
                    synthetic_pe(stage / 'bin' / 'windows' / file, PACKAGE.MACHINES[bitness])
                (stage / 'bin' / 'config.json').write_text('{}')
                self.assertEqual(PACKAGE.validate_payload(stage, arch), helper)
                (stage / 'libssl-1_1.dll').unlink()
                with self.assertRaisesRegex(RuntimeError, 'Missing OpenSSL'):
                    PACKAGE.validate_payload(stage, arch)
                PACKAGE.validate_payload(stage, arch, native_tls=True)
                (stage / 'platforms' / 'qwindows.dll').unlink()
                with self.assertRaisesRegex(RuntimeError, 'Missing binary'):
                    PACKAGE.validate_payload(stage, arch, native_tls=True)

    def test_generated_manifests_escape_paths_and_delete_only_owned_files(self):
        with tempfile.TemporaryDirectory(prefix='package test $ ') as temporary:
            root = Path(temporary)
            stage = root / 'payload'
            stage.mkdir()
            (stage / 'client.exe').write_text('synthetic fixture')
            nested = stage / 'nested & assets'
            nested.mkdir()
            (nested / 'name$.txt').write_text('synthetic fixture')
            manifest = root / 'payload.nsh'
            PACKAGE.write_nsis_manifest(stage, manifest)
            text = manifest.read_text()
            self.assertIn('name$$.txt', text)
            self.assertNotIn('RMDir /r', text)
            self.assertNotIn('Delete "$INSTDIR\\*"', text)
            self.assertIn('Delete "$INSTDIR\\nested & assets\\name$$.txt"', text)
            payload = root / 'payload.wxs'
            PACKAGE.write_wix_payload(stage, payload, 'amd64')
            first = payload.read_bytes()
            PACKAGE.write_wix_payload(stage, payload, 'amd64')
            self.assertEqual(first, payload.read_bytes())
            xml = ET.parse(payload)
            ns = {'w': PACKAGE.WIX_NS}
            files = xml.findall('.//w:File', ns)
            self.assertEqual(len(files), 2)
            self.assertEqual(len(xml.findall('.//w:File[@Id="MainExecutable"]', ns)), 1)
            self.assertEqual({Path(item.attrib['Source']).name for item in files}, {'client.exe', 'name$.txt'})
            self.assertTrue(all(c.attrib['Win64'] == 'yes' for c in xml.findall('.//w:Component', ns)))

    def test_signing_is_explicit_and_missing_credentials_skip(self):
        args = argparse.Namespace(sign=False, sign_file=None, signtool='signtool', timestamp_url='https://example.invalid/timestamp')
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(os.environ, {}, clear=True):
            directory = Path(temporary)
            self.assertFalse(PACKAGE.Signer(args, directory).enabled)
            args.sign = True
            for env in ({}, {'WINDOWS_CERTIFICATE': 'synthetic'},
                        {'WINDOWS_CERTIFICATE_PASSWORD': 'synthetic'},
                        {'WINDOWS_CERTIFICATE': '  \n', 'WINDOWS_CERTIFICATE_PASSWORD': 'synthetic'}):
                with mock.patch.dict(os.environ, env, clear=True), contextlib.redirect_stdout(io.StringIO()) as output:
                    self.assertFalse(PACKAGE.Signer(args, directory).enabled)
                    self.assertIn('UNSIGNED', output.getvalue())
            self.assertFalse((directory / 'signing.pfx').exists())
            with mock.patch.dict(os.environ, {'WINDOWS_CERTIFICATE': 'not base64!', 'WINDOWS_CERTIFICATE_PASSWORD': 'synthetic'}):
                with self.assertRaisesRegex(RuntimeError, 'base64'):
                    PACKAGE.Signer(args, directory)
            args.sign_file = directory / 'uninstaller.exe'
            with self.assertRaisesRegex(RuntimeError, 'became unavailable'):
                PACKAGE.Signer(args, directory)

    def test_signing_failure_does_not_log_password_or_subprocess_output(self):
        args = argparse.Namespace(sign=True, sign_file=None, signtool='signtool', timestamp_url='https://example.invalid/timestamp')
        secret = 'SYNTHETIC-TEST-PASSWORD-never-a-real-secret'
        env = {'WINDOWS_CERTIFICATE': base64.b64encode(b'synthetic-invalid-PFX').decode(), 'WINDOWS_CERTIFICATE_PASSWORD': secret}
        with tempfile.TemporaryDirectory() as temporary, mock.patch.dict(os.environ, env, clear=True), \
                mock.patch.object(PACKAGE, 'tool', return_value='fake-signtool'), \
                mock.patch.object(PACKAGE.subprocess, 'run', return_value=subprocess.CompletedProcess([], 7, secret)) as process:
            output = io.StringIO()
            signer = PACKAGE.Signer(args, Path(temporary))
            with contextlib.redirect_stdout(output), self.assertRaises(RuntimeError) as caught:
                signer.sign(Path(temporary) / 'client.exe')
            self.assertNotIn(secret, output.getvalue() + str(caught.exception))
            self.assertIn(secret, process.call_args.args[0])
            self.assertIs(process.call_args.kwargs['stdout'], subprocess.PIPE)

    def test_missing_certificate_completes_unsigned_package_and_manifest(self):
        # All binaries/installers here are synthetic and live only in this temporary
        # directory. Exercise orchestration without pretending to run Windows tools.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            build.mkdir()
            (build / 'CMakeCache.txt').write_text('CLIENT_VERSION:STRING=4.1.2\nCMAKE_CXX_COMPILER:FILEPATH=fake-g++\n')
            synthetic_pe(build / 'client.exe', PACKAGE.MACHINES['amd64'])
            for name, arch in (('JumpServerClient.exe', 'amd64'), ('putty.exe', '386'),
                               ('AutoItX3.dll', '386'), ('AutoItX3_x64.dll', 'amd64')):
                synthetic_pe(build / 'bin' / 'windows' / name, PACKAGE.MACHINES[arch])
            (build / 'bin' / 'config.json').write_text('{}')
            output = root / 'dist'
            qt_bin = root / 'qt-bin'
            qt_bin.mkdir()
            fake_deployqt = qt_bin / 'windeployqt.exe'
            commands = []

            def fake_run(command, label, env=None, quiet=False):
                commands.append(label)
                if label == 'Deploying matching Qt runtime and plugins':
                    self.assertIn('--no-angle', command)
                    self.assertIn('--no-opengl-sw', command)
                    stage = Path(command[command.index('--dir') + 1])
                    for name in ('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Network.dll',
                                 'Qt5Svg.dll', 'platforms/qwindows.dll'):
                        synthetic_pe(stage / name, PACKAGE.MACHINES['amd64'])
                elif label == 'Building NSIS EXE installer':
                    target = next(str(value)[9:] for value in command if str(value).startswith('/DOUTPUT='))
                    Path(target).write_text('SYNTHETIC INSTALLER - NOT FOR DISTRIBUTION')
                elif label == 'Launching isolated deployed GUI smoke test':
                    self.assertEqual(env['QT_QPA_PLATFORM'], 'windows')
                    self.assertNotIn('QT_PLUGIN_PATH', env)

            argv = ['package-windows.py', '--build', str(build), '--output', str(output),
                    '--format', 'exe', '--skip-build', '--native-tls', '--sign',
                    '--windeployqt', str(fake_deployqt)]
            with mock.patch.object(PACKAGE.sys, 'platform', 'win32'), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.dict(os.environ, {'SystemRoot': str(root / 'windows')}, clear=True), \
                    mock.patch.object(PACKAGE, 'tool', side_effect=str), mock.patch.object(PACKAGE, 'run', side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()) as log:
                PACKAGE.main()
            manifest = json.loads(next(output.glob('*.manifest.json')).read_text())
            self.assertFalse(manifest['signed'])
            self.assertEqual(manifest['version'], '4.1.2')
            self.assertEqual(manifest['format'], 'exe')
            self.assertEqual(manifest['artifact'], 'OSMClient-4.1.2-windows-amd64.exe')
            artifact = output / manifest['artifact']
            self.assertEqual(manifest['sha256'], PACKAGE.hashlib.sha256(artifact.read_bytes()).hexdigest())
            self.assertIn('Skipping Windows signing', log.getvalue())
            self.assertFalse(any(label.startswith('Signing') for label in commands))

    def test_qt6_deployment_omits_qt5_angle_options(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / 'build'
            build.mkdir()
            (build / 'CMakeCache.txt').write_text('CLIENT_VERSION:STRING=4.1.2\nCLIENT_QT_MAJOR:STRING=6\nCMAKE_CXX_COMPILER:FILEPATH=fake-g++\n')
            synthetic_pe(build / 'client.exe', PACKAGE.MACHINES['amd64'])
            for name, arch in (('JumpServerClient.exe', 'amd64'), ('putty.exe', '386'),
                               ('AutoItX3.dll', '386'), ('AutoItX3_x64.dll', 'amd64')):
                synthetic_pe(build / 'bin' / 'windows' / name, PACKAGE.MACHINES[arch])
            (build / 'bin' / 'config.json').write_text('{}')
            output = root / 'dist'
            qt_bin = root / 'qt-bin'
            qt_bin.mkdir()
            fake_deployqt = qt_bin / 'windeployqt.exe'
            deploy_command = []

            def fake_run(command, label, env=None, quiet=False):
                if label == 'Deploying matching Qt runtime and plugins':
                    deploy_command.extend(command)
                    stage = Path(command[command.index('--dir') + 1])
                    for name in ('Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Widgets.dll', 'Qt6Network.dll',
                                 'Qt6Svg.dll', 'platforms/qwindows.dll'):
                        synthetic_pe(stage / name, PACKAGE.MACHINES['amd64'])
                elif label == 'Building NSIS EXE installer':
                    target = next(str(value)[9:] for value in command if str(value).startswith('/DOUTPUT='))
                    Path(target).write_text('SYNTHETIC INSTALLER - NOT FOR DISTRIBUTION')

            argv = ['package-windows.py', '--build', str(build), '--output', str(output),
                    '--format', 'exe', '--skip-build', '--native-tls',
                    '--windeployqt', str(fake_deployqt)]
            with mock.patch.object(PACKAGE.sys, 'platform', 'win32'), mock.patch.object(sys, 'argv', argv), \
                    mock.patch.dict(os.environ, {'SystemRoot': str(root / 'windows')}, clear=True), \
                    mock.patch.object(PACKAGE, 'tool', side_effect=str), \
                    mock.patch.object(PACKAGE, 'run', side_effect=fake_run), \
                    contextlib.redirect_stdout(io.StringIO()):
                PACKAGE.main()
            self.assertNotIn('--no-angle', deploy_command)
            self.assertNotIn('--no-opengl-sw', deploy_command)

    def test_non_windows_packaging_is_rejected(self):
        with mock.patch.object(PACKAGE.sys, 'platform', 'darwin'), mock.patch.object(sys, 'argv', ['package-windows.py', '--build', 'not-a-real-build']), contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                PACKAGE.main()
            self.assertEqual(caught.exception.code, 2)

    def test_msi_shared_protocol_rows_are_temporary_only(self):
        ns = {'w': PACKAGE.WIX_NS}
        xml = ET.parse(ROOT / 'packaging' / 'windows.wxs')
        registrations = xml.findall('.//w:RegistryValue', ns)
        self.assertFalse(any('Classes\\jms' in value.attrib.get('Key', '') for value in registrations))
        action = xml.find('.//w:InstallExecuteSequence/w:Custom', ns)
        self.assertEqual(action.attrib['Before'], 'RemoveRegistryValues')
        self.assertEqual(action.attrib['Action'], 'PrepareJmsRegistration')
        source = (ROOT / 'packaging' / 'windows-jms-ca.cpp').read_text()
        self.assertIn('VALUES (?, ?, ?, ?, ?, ?) TEMPORARY', source)
        self.assertNotIn('DELETE FROM', source)
        self.assertNotIn('MsiDatabaseCommit', source)

    def test_msi_component_scopes_satisfy_ice_validation(self):
        ns = {'w': PACKAGE.WIX_NS}
        product = ET.parse(ROOT / 'packaging' / 'windows.wxs').find('w:Product', ns)
        directories = {directory.attrib['Id']: directory for directory in product.findall('.//w:Directory', ns)}
        # ICE38/ICE43/ICE57: shortcut components are authored in user-profile folders, so MSI
        # requires per-user key paths even though the package installs machine-wide links.
        for directory_id in ('ProgramMenuFolder', 'DesktopFolder'):
            key_paths = [value for value in directories[directory_id].findall('.//w:RegistryValue', ns)
                         if value.attrib.get('KeyPath') == 'yes']
            self.assertEqual(len(key_paths), 1)
            self.assertEqual(key_paths[0].attrib['Root'], 'HKCU')
        # ICE80: the protocol guard keeps the 32-bit registry view, so it must not be anchored in
        # the 64-bit installation directory. It installs no files, so its anchor stays inert.
        guard = product.find('.//w:Component[@Id="JmsRegistration"]', ns)
        self.assertEqual(guard.attrib['Win64'], 'no')
        self.assertEqual(guard.findall('w:File', ns), [])
        self.assertIn(guard, list(directories['TARGETDIR']))
        self.assertEqual(directories['INSTALLFOLDER'].findall('w:Component', ns), [])

    def test_product_rename_preserves_upgrade_identity_and_removes_old_product_first(self):
        ns = {'w': PACKAGE.WIX_NS}
        product = ET.parse(ROOT / 'packaging' / 'windows.wxs').find('w:Product', ns)
        self.assertEqual(product.attrib['Name'], 'OSMClient')
        self.assertEqual(product.attrib['UpgradeCode'], 'DAF0A2B2-B00D-48A9-A57C-668B31E368A0')
        self.assertEqual(product.find('w:MajorUpgrade', ns).attrib['Schedule'], 'afterInstallInitialize')
        self.assertEqual(product.find('.//w:Directory[@Id="INSTALLFOLDER"]', ns).attrib['Name'], 'OSMClient')
        shortcut = product.find('.//w:Shortcut', ns)
        self.assertEqual(shortcut.attrib['Name'], 'OSMClient')
        self.assertEqual(shortcut.attrib['Target'], '[#MainExecutable]')
        desktop = product.find('.//w:Directory[@Id="DesktopFolder"]/w:Component/w:Shortcut', ns)
        self.assertEqual(desktop.attrib['Target'], '[#MainExecutable]')
        nsis = (ROOT / 'packaging' / 'windows.nsi').read_text()
        self.assertLess(nsis.index('ExecWait '), nsis.index('CreateShortcut '))
        self.assertIn('InstallDir "$PROGRAMFILES64\\OSMClient"', nsis)
        self.assertIn('CreateShortcut "$SMPROGRAMS\\OSMClient.lnk" "$INSTDIR\\client.exe"', nsis)
        self.assertIn('CreateShortcut "$DESKTOP\\OSMClient.lnk" "$INSTDIR\\client.exe"', nsis)
        self.assertIn('Delete "$DESKTOP\\OSMClient.lnk"', nsis)
        self.assertIn('AutoCloseWindow true', nsis)
        self.assertIn('Delete "$SMPROGRAMS\\OSMClient.lnk"', nsis)

    @unittest.skipUnless(shutil.which('makensis'), 'NSIS is not installed')
    def test_nsis_templates_compile_with_explicit_synthetic_fixture(self):
        with tempfile.TemporaryDirectory(prefix='nsis syntax test ') as temporary:
            root = Path(temporary)
            stage = root / 'SYNTHETIC-NOT-FOR-DISTRIBUTION'
            stage.mkdir()
            (stage / 'sentinel.txt').write_text('Not a real application payload.')
            manifest = root / 'fixture.nsh'
            PACKAGE.write_nsis_manifest(stage, manifest)
            switch = '/D' if sys.platform == 'win32' else '-D'
            for arch in ('amd64', '386'):
                artifact = root / ('syntax-only-' + arch + '.exe')
                command = [shutil.which('makensis'), '/V2' if sys.platform == 'win32' else '-V2',
                           switch + 'VERSION=4.1.2', switch + 'ARCH=' + arch,
                           switch + 'MANIFEST=' + str(manifest), switch + 'OUTPUT=' + str(artifact),
                           str(ROOT / 'packaging' / 'windows.nsi')]
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                self.assertEqual(result.returncode, 0, result.stdout)
                self.assertTrue(artifact.is_file())
            # Exercise the compiler's uninstaller signing hook without a real key.
            signer = root / 'synthetic signer.py'
            marker = root / 'hook-ran.txt'
            signer.write_text('import pathlib, sys\nassert pathlib.Path(sys.argv[1]).is_file()\n'
                              + 'pathlib.Path(' + repr(str(marker)) + ').write_text("synthetic hook executed")\n')
            command.insert(-1, switch + 'SIGN_UNINSTALLER=' + subprocess.list2cmdline([sys.executable, str(signer)]))
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertTrue(marker.is_file())
            # TemporaryDirectory removes these synthetic installers; never publish them.


if __name__ == '__main__':
    unittest.main()
