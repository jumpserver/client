"""Offline signing/packaging regression checks using synthetic credentials only."""
import base64
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import subprocess
import shutil
import sys
import tempfile
import unittest
from unittest.mock import Mock, patch

SCRIPTS = Path(__file__).resolve().parents[1] / 'scripts'
sys.path.insert(0, str(SCRIPTS))
import macos_common


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), SCRIPTS / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


app_package = load('package-macos')
disk_package = load('package-dmg')
certificate = load('setup-apple-cert')


class PackagingTests(unittest.TestCase):
    def test_bundle_rejects_wrong_architecture_plugin(self):
        bundle = Path('/synthetic/OSMClient.app')
        files = [bundle / 'Contents/MacOS/client-qt', bundle / 'Contents/PlugIns/platforms/libqcocoa.dylib']
        info = {'minimum_os': ['10.15'], 'dependencies': [], 'rpaths': [], 'id': None}
        with patch.object(app_package, 'macho_files', return_value=files), \
                patch.object(app_package, 'inspect_macho', side_effect=lambda _: dict(info)), \
                patch.object(app_package.subprocess, 'check_output', side_effect=['x86_64', 'x86_64', 'arm64']):
            with self.assertRaisesRegex(RuntimeError, 'Mismatched binary architecture'):
                app_package.audit_bundle(bundle)
    def test_deployment_limit_checks_every_binary(self):
        report = {'binaries': {'GUI': {'minimum_os': ['10.15.0']},
                              'QtCore': {'minimum_os': ['10.13']}}}
        app_package.enforce_deployment(report, '10.15')
        for minimum in (['27.0'], []):
            report['binaries']['transitive-library'] = {'minimum_os': minimum}
            with self.assertRaisesRegex(RuntimeError, 'transitive-library'):
                app_package.enforce_deployment(report, '10.15')
    def test_dmg_uses_product_name_for_volume_bundle_and_readme(self):
        # Fake files and command responses only: no image mounting or keychain access.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'client-qt.app'
            helpers = source / 'Contents/Resources/bin/darwin-arm64'
            (helpers / 'Scripts').mkdir(parents=True)
            for name in ('JumpServerClient', 'client', 'Scripts/iterm2_loader.scpt'):
                path = helpers / name
                path.write_text('synthetic fixture')
                path.chmod(0o755)
            info = {'CFBundleShortVersionString': '4.1.2',
                    'CFBundleURLTypes': [{'CFBundleURLSchemes': ['jms']}]}
            (source / 'Contents/Info.plist').write_bytes(plistlib.dumps(info))
            output = root / 'OSMClient-4.1.2-macos-arm64.dmg'
            audit = Mock()
            audit.audit_bundle.return_value = {'minimum_macos': '11.0'}

            def command(args, **kwargs):
                if args[0] == 'lipo':
                    return 'arm64'
                if args[0] == 'ditto':
                    self.assertEqual(Path(args[2]).name, 'OSMClient.app')
                    shutil.copytree(args[1], args[2])
                if args[:2] == ['hdiutil', 'create']:
                    self.assertEqual(args[args.index('-volname') + 1], 'OSMClient')
                    staging = Path(args[args.index('-srcfolder') + 1])
                    self.assertEqual((staging / 'Applications').readlink(), Path('/Applications'))
                    self.assertIn('Drag OSMClient.app to Applications.', (staging / 'Read me.txt').read_text())
                    Path(args[-1]).write_bytes(b'synthetic image, not a disk image')
                if args[:2] == ['hdiutil', 'attach']:
                    mount = Path(args[args.index('-mountpoint') + 1])
                    shutil.copytree(mount.parent / 'payload', mount, dirs_exist_ok=True, symlinks=True)
                return ''

            argv = ['package-dmg.py', '--app', str(source), '--output', str(output), '--deployment-target', '11.0']
            with patch.object(sys, 'argv', argv), patch.object(disk_package.platform, 'system', return_value='Darwin'), \
                    patch.object(disk_package, 'run', side_effect=command), \
                    patch.object(disk_package, 'deployment_module', return_value=audit), \
                    patch.object(disk_package, 'smoke') as smoke:
                disk_package.main()
            self.assertEqual(smoke.call_args.args[0].name, 'OSMClient.app')
            manifest = json.loads(output.with_suffix('.manifest.json').read_text())
            self.assertEqual(manifest['file'], 'OSMClient-4.1.2-macos-arm64.dmg')
            self.assertFalse(manifest['signed'])

    def test_keychain_search_failure_still_attempts_private_key_removal(self):
        with tempfile.TemporaryDirectory(prefix='client-qt-signing-') as temporary:
            directory = Path(temporary)
            keychain = directory / 'signing.keychain-db'
            state = directory / 'state.json'
            keychain.touch()
            state.write_text(json.dumps({'keychain': str(keychain)}))
            def security(command, **kwargs):
                if command[1] == 'list-keychains':
                    raise RuntimeError('synthetic search failure')
                if command[1] == 'delete-keychain':
                    keychain.unlink()
                return ''
            with patch.object(certificate, 'run', side_effect=security) as command:
                with self.assertRaisesRegex(RuntimeError, 'cleanup incomplete'):
                    certificate.cleanup(state)
            command.assert_any_call(['security', 'delete-keychain', str(keychain)])
            self.assertFalse(keychain.exists())
            self.assertTrue(state.exists())

    def test_dmg_detach_retries_before_force(self):
        with patch.object(disk_package, 'run', side_effect=[RuntimeError('busy'), RuntimeError('busy'), '']) as command, \
                patch.object(os.path, 'ismount', return_value=True):
            disk_package.detach_mount(Path('/synthetic/mounted'))
        self.assertEqual(command.call_count, 3)
        self.assertEqual(command.call_args.args[0], ['hdiutil', 'detach', '/synthetic/mounted', '-force'])

    def test_active_mount_is_not_recursively_cleaned(self):
        temporary = None
        try:
            with patch.object(os.path, 'ismount', return_value=True):
                with disk_package.workspace() as temporary:
                    (temporary / 'mounted').mkdir()
            self.assertTrue(temporary.exists())
        finally:
            if temporary and temporary.exists():
                shutil.rmtree(temporary)

    def test_missing_certificate_skips_setup_without_touching_keychains(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(certificate, 'run') as command:
            certificate.setup()
            self.assertEqual(os.environ['APPLE_SIGNING_ENABLED'], 'false')
            self.assertFalse(macos_common.resolve_signing(True))
            self.assertFalse(disk_package.resolve_notarization(True, False))
            command.assert_not_called()

    def test_missing_notary_credentials_keeps_signed_package_without_upload(self):
        with patch.dict(os.environ, {'APPLE_SIGNING_IDENTITY': 'synthetic-identity'}, clear=True):
            self.assertTrue(macos_common.resolve_signing(True))
            self.assertFalse(disk_package.resolve_notarization(True, True))

    def test_release_signing_fails_closed_without_identity(self):
        with patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(RuntimeError, 'APPLE_SIGNING_IDENTITY'):
                macos_common.signing_identity(True)
            self.assertEqual(macos_common.signing_identity(False), '-')

    def test_tool_errors_do_not_echo_credentials_or_command(self):
        fake = 'synthetic-only-password'
        result = subprocess.CompletedProcess(['security'], 1, '', 'tool reported ' + fake)
        with patch.dict(os.environ, {'APPLE_PASSWORD': fake}, clear=True), \
                patch.object(subprocess, 'run', return_value=result):
            with self.assertRaises(RuntimeError) as raised:
                macos_common.run(['security', 'test', fake])
        self.assertNotIn(fake, str(raised.exception))
        self.assertIn('[redacted]', str(raised.exception))

    def test_hardened_runtime_and_timestamp_only_for_release(self):
        with patch.dict(os.environ, {'APPLE_SIGNING_IDENTITY': 'synthetic-identity',
                                     'APPLE_SIGNING_KEYCHAIN': '/synthetic/keychain'}, clear=True), \
                patch.object(macos_common, 'run') as command:
            macos_common.sign(Path('/synthetic/app'), True, Path('/synthetic/entitlements'))
            release = command.call_args.args[0]
            self.assertIn('--timestamp', release)
            self.assertIn('runtime', release)
            self.assertIn('--entitlements', release)
            self.assertIn('--keychain', release)
            macos_common.sign(Path('/synthetic/app'), False)
            development = command.call_args.args[0]
            self.assertIn('--timestamp=none', development)
            self.assertNotIn('synthetic-identity', development)

    def test_nested_binaries_signed_before_outer_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle = Path(temporary) / 'Client.app'
            framework = bundle / 'Contents/Frameworks/QtCore.framework'
            framework.mkdir(parents=True)
            files = [bundle / 'Contents/MacOS/client-qt',
                     bundle / 'Contents/Resources/bin/darwin-arm64/JumpServerClient',
                     bundle / 'Contents/Resources/bin/darwin-arm64/client', framework / 'QtCore']
            with patch.object(app_package, 'macho_files', return_value=files), \
                    patch.object(app_package, 'sign') as signer, \
                    patch.object(app_package.subprocess, 'run'):
                app_package.sign_bundle(bundle, release=True)
            calls = signer.call_args_list
            self.assertEqual(calls[-1].args[0], bundle)
            self.assertEqual([call.args[0] for call in calls[:3]], files[1:])
            self.assertEqual(calls[-2].args[0], files[0])
            self.assertIsNotNone(calls[0].args[2])
            self.assertIsNotNone(calls[-2].args[2])
            self.assertIsNone(calls[1].args[2])
            self.assertIsNone(calls[2].args[2])
            self.assertIsNone(calls[3].args[2])

    def test_certificate_failure_removes_temporary_keychain(self):
        with tempfile.TemporaryDirectory() as temporary:
            env = {'RUNNER_TEMP': temporary, 'GITHUB_ENV': str(Path(temporary) / 'github.env'),
                   'APPLE_CERTIFICATE': base64.b64encode(b'synthetic-p12').decode(),
                   'APPLE_CERTIFICATE_PASSWORD': 'synthetic-cert-password',
                   'KEYCHAIN_PASSWORD': 'synthetic-keychain-password'}
            calls = []

            def security(command, **kwargs):
                calls.append(command)
                if command[1] == 'list-keychains':
                    return '"/synthetic/existing.keychain-db"\n'
                if command[1] == 'create-keychain':
                    Path(command[-1]).touch()
                if command[1] == 'import':
                    raise RuntimeError('Synthetic import failure')
                if command[1] == 'delete-keychain':
                    Path(command[-1]).unlink()
                return ''

            with patch.dict(os.environ, env, clear=True), patch.object(certificate, 'run', side_effect=security):
                with self.assertRaisesRegex(RuntimeError, 'Synthetic import failure'):
                    certificate.setup()
            self.assertFalse(list(Path(temporary).glob('client-qt-signing-*')))
            self.assertIn(['security', 'list-keychains', '-d', 'user', '-s', '/synthetic/existing.keychain-db'], calls)
            self.assertTrue(any(command[1] == 'delete-keychain' for command in calls))
            self.assertFalse(any(command[1] == 'default-keychain' for command in calls))

    def test_certificate_cleanup_preserves_new_unrelated_keychains(self):
        with tempfile.TemporaryDirectory(prefix='client-qt-signing-') as temporary:
            directory = Path(temporary)
            keychain = directory / 'signing.keychain-db'
            state = directory / 'state.json'
            keychain.touch()
            state.write_text(json.dumps({'keychain': str(keychain)}))
            with patch.object(certificate, 'run', side_effect=lambda command, **kw:
                              '"/existing" "{}" "/another-step"'.format(keychain)
                              if command == ['security', 'list-keychains', '-d', 'user'] else '') as command:
                certificate.cleanup(state)
            command.assert_any_call(['security', 'list-keychains', '-d', 'user', '-s', '/existing', '/another-step'])
            self.assertFalse(directory.exists())

    def test_notary_rejection_never_staples(self):
        env = {'APPLE_ID': 'synthetic@example.invalid', 'APPLE_PASSWORD': 'synthetic-password', 'APPLE_TEAM_ID': 'SYNTHETIC'}
        with patch.dict(os.environ, env, clear=True), \
                patch.object(disk_package, 'run', return_value=json.dumps({'id': 'synthetic-id', 'status': 'Invalid'})) as command:
            with self.assertRaisesRegex(RuntimeError, 'not accepted'):
                disk_package.notarize(Path('/synthetic/test.dmg'))
            self.assertEqual(command.call_count, 1)

    def test_notary_acceptance_staples_then_validates(self):
        env = {'APPLE_ID': 'synthetic@example.invalid', 'APPLE_PASSWORD': 'synthetic-password', 'APPLE_TEAM_ID': 'SYNTHETIC'}
        with patch.dict(os.environ, env, clear=True), \
                patch.object(disk_package, 'run', side_effect=[json.dumps({'id': 'synthetic-id', 'status': 'Accepted'}), '', '']) as command:
            self.assertEqual(disk_package.notarize(Path('/synthetic/test.dmg')), 'synthetic-id')
            self.assertEqual(command.call_args_list[-2].args[0][1:3], ['stapler', 'staple'])
            self.assertEqual(command.call_args_list[-1].args[0][1:3], ['stapler', 'validate'])


if __name__ == '__main__':
    unittest.main()
