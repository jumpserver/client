#!/usr/bin/env python3
"""Create and verify a drag-to-Applications DMG from a deployed Qt app."""
import argparse
from contextlib import contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import plistlib
import shutil
import sys
import tempfile
import time
from macos_common import run, sign, resolve_signing, smoke


STAPLER_NETWORK_ERRORS = (
    'error 68', 'network connection was lost', 'cloudkit',
    'nsurlerrordomain', 'kcfstreamerror',
)


def run_stapler(action, image, attempts=5, delay=5):
    command = ['xcrun', 'stapler', action, str(image)]
    for attempt in range(attempts):
        try:
            return run(command, timeout=300)
        except RuntimeError as error:
            message = str(error).lower()
            transient = any(fragment in message for fragment in STAPLER_NETWORK_ERRORS)
            if not transient or attempt == attempts - 1:
                raise
            print('Apple ticket lookup failed transiently; retrying stapler {}.'.format(action))
            time.sleep(delay)


def deployment_module():
    spec = importlib.util.spec_from_file_location('package_macos', Path(__file__).with_name('package-macos.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def notarize(image):
    required = ('APPLE_ID', 'APPLE_PASSWORD', 'APPLE_TEAM_ID')
    missing = [name for name in required if not os.environ.get(name)]
    if missing:
        raise RuntimeError('Missing notarization Secrets: ' + ', '.join(missing))
    # run() captures output and redacts credentials; do not echo the command.
    output = run(['xcrun', 'notarytool', 'submit', str(image), '--apple-id', os.environ['APPLE_ID'],
                  '--password', os.environ['APPLE_PASSWORD'], '--team-id', os.environ['APPLE_TEAM_ID'],
                  '--wait', '--timeout', '30m', '--output-format', 'json'], timeout=2100)
    result = json.loads(output)
    if result.get('status') != 'Accepted':
        raise RuntimeError('Apple notarization was not accepted (submission {}); inspect its notarytool log'.format(result.get('id', 'unknown')))
    run_stapler('staple', image)
    run_stapler('validate', image)
    return result.get('id')


def resolve_notarization(requested, signed):
    if not requested:
        return False
    missing = [key for key in ('APPLE_ID', 'APPLE_PASSWORD', 'APPLE_TEAM_ID') if not os.environ.get(key)]
    if not signed or missing:
        reason = 'package is unsigned' if not signed else 'missing ' + ', '.join(missing)
        print('Notarization skipped: ' + reason)
        return False
    return True


def create_image(staging, volume_name, output):
    command = ['hdiutil', 'create', '-ov', '-volname', volume_name, '-srcfolder', str(staging),
               '-fs', 'HFS+', '-format', 'UDZO', '-imagekey', 'zlib-level=9', str(output)]
    for attempt in range(10):
        try:
            return run(command, timeout=600)
        except RuntimeError as error:
            if 'Resource busy' not in str(error):
                raise
            output.unlink(missing_ok=True)
            if attempt == 9:
                raise
            print('Temporary disk image resource conflict; retrying hdiutil create.')
            time.sleep(5)


def detach_mount(mount):
    for force in (False, False, True):
        try:
            run(['hdiutil', 'detach', str(mount)] + (['-force'] if force else []), timeout=30)
            return
        except (OSError, RuntimeError):
            if not os.path.ismount(mount):
                return
    raise RuntimeError('Could not detach verification image; workspace retained at ' + str(mount.parent))


@contextmanager
def workspace():
    root = Path(tempfile.mkdtemp(prefix='client-dmg-'))
    try:
        yield root
    finally:
        # Never recursively remove a still-mounted volume, even after a failed
        # force detach. Keep the path for an explicit cleanup retry instead.
        if os.path.ismount(root / 'mounted'):
            print('Verification mount is still active; retained temporary workspace: ' + str(root))
        else:
            shutil.rmtree(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', type=Path, required=True, help='Self-contained app from package-macos.py')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--volume-name', default='OSMClient')
    parser.add_argument('--deployment-target', required=True)
    parser.add_argument('--skip-smoke', action='store_true', help='Cross-build only: omit runtime verification and record it')
    parser.add_argument('--sign', action='store_true', help='Request DMG signing; skip when no signing identity is available')
    parser.add_argument('--notarize', action='store_true', help='Notarize when signing and Apple credentials are available')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('Build DMG images on macOS')
    if args.notarize and not args.sign:
        parser.error('--notarize requires --sign')
    args.sign = resolve_signing(args.sign)
    args.notarize = resolve_notarization(args.notarize, args.sign)
    source = args.app.resolve()
    output = args.output.resolve()
    manifest_path = output.with_suffix('.manifest.json')
    if output.suffix.lower() != '.dmg' or output.exists() or manifest_path.exists():
        parser.error('Choose a fresh .dmg output path')
    if not source.is_dir() or source.suffix != '.app':
        parser.error('--app must be an existing deployed .app bundle')
    info = plistlib.loads((source / 'Contents/Info.plist').read_bytes())
    schemes = [scheme for item in info.get('CFBundleURLTypes', []) for scheme in item.get('CFBundleURLSchemes', [])]
    if 'jms' not in schemes:
        parser.error('The app does not declare its jms URL handler')
    architecture = run(['lipo', '-archs', str(source / 'Contents/MacOS/client')]).strip()
    folders = list((source / 'Contents/Resources/bin').glob('darwin-*'))
    expected = {'arm64': 'arm64', 'x86_64': 'amd64'}.get(architecture)
    if not expected or len(folders) != 1 or folders[0].name != 'darwin-' + expected:
        parser.error('Package one matching GUI/Go architecture per disk image')
    for name in ('JumpServerClient', 'client', 'Scripts/iterm2_loader.scpt'):
        if not (folders[0] / name).is_file():
            parser.error('Missing Go launcher resource: ' + name)
    for name in ('JumpServerClient', 'client'):
        helper = folders[0] / name
        if not os.access(helper, os.X_OK) or run(['lipo', '-archs', str(helper)]).strip() != architecture:
            parser.error('Go launcher architecture or executable mode does not match the GUI')
    report = deployment_module().audit_bundle(source)
    deployment_module().enforce_deployment(report, args.deployment_target)
    run(['codesign', '--verify', '--deep', '--strict', str(source)])
    if args.sign:
        # Require a Developer ID app, not an ad-hoc bundle inside a signed image.
        # -R must carry the requirement inline (-R=<string>); a separate value is
        # treated as a requirement *file path* and fails with "invalid req".
        run(['codesign', '--verify', '--strict',
             '-R=anchor apple generic and certificate leaf[field.1.2.840.113635.100.6.1.13] exists',
             str(source)])
    output.parent.mkdir(parents=True, exist_ok=True)
    with workspace() as root:
        staging = root / 'payload'
        staging.mkdir()
        app_name = 'OSMClient.app'
        run(['ditto', str(source), str(staging / app_name)])
        (staging / 'Applications').symlink_to('/Applications', target_is_directory=True)
        (staging / 'Read me.txt').write_text(
            'OSMClient {}\n\nDrag OSMClient.app to Applications.\n'
            'Launch it from Applications to sign in. Existing user settings are preserved.\n\n'
            'Architecture: {}\nMinimum macOS for this build: {}\n'.format(
                info.get('CFBundleShortVersionString', ''), architecture, report['minimum_macos']))
        create_image(staging, args.volume_name, output)
        if args.sign:
            sign(output, release=True, runtime=False)
            run(['codesign', '--verify', '--strict', str(output)])
        submission = notarize(output) if args.notarize else None
        run(['hdiutil', 'verify', str(output)], timeout=300)
        mount = root / 'mounted'
        mount.mkdir()
        verification_error = None
        try:
            run(['hdiutil', 'attach', '-readonly', '-noautoopen', '-mountpoint', str(mount), str(output)], timeout=120)
            installed_app = mount / app_name
            run(['codesign', '--verify', '--deep', '--strict', str(installed_app)])
            mounted_report = deployment_module().audit_bundle(installed_app)
            deployment_module().enforce_deployment(mounted_report, args.deployment_target)
            if not (mount / 'Applications').is_symlink() or os.readlink(mount / 'Applications') != '/Applications':
                raise RuntimeError('Missing Applications shortcut in mounted DMG')
            if not args.skip_smoke:
                smoke(installed_app)
        except Exception as error:
            verification_error = error
            raise
        finally:
            if os.path.ismount(mount):
                try:
                    detach_mount(mount)
                except RuntimeError as error:
                    if verification_error:
                        raise RuntimeError(str(verification_error) + '; ' + str(error)) from None
                    raise
    checksum = hashlib.sha256()
    with output.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            checksum.update(chunk)
    manifest = {'file': output.name, 'version': info.get('CFBundleShortVersionString'),
                'architecture': architecture, 'minimum_macos': report['minimum_macos'],
                'sha256': checksum.hexdigest(), 'signed': args.sign, 'notarized': args.notarize,
                'notarization_submission': submission, 'verified': ['image-checksum', 'mounted-app-signature', 'mounted-app-dependencies', 'mounted-app-smoke']}
    manifest['deployment_target'] = args.deployment_target
    manifest['smoke_tested'] = not args.skip_smoke
    if args.skip_smoke:
        manifest['verified'].remove('mounted-app-smoke')
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print('DMG ready: ' + str(output))
    print('SHA256: ' + manifest['sha256'])
    print('Minimum macOS: ' + str(report['minimum_macos']))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print('DMG packaging failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
