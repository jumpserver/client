#!/usr/bin/env python3
"""Import the original client's Apple certificate Secrets into an isolated keychain."""
import argparse
import base64
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import sys
import tempfile
from macos_common import run


def export_environment(values):
    target = os.environ.get('GITHUB_ENV')
    for key, value in values.items():
        if '\n' in value or '\r' in value:
            raise RuntimeError('Invalid multiline signing environment value')
        os.environ[key] = value
    if target:
        with open(target, 'a') as stream:
            for key, value in values.items():
                stream.write('{}={}\n'.format(key, value))


def mask(value):
    if os.environ.get('GITHUB_ACTIONS') == 'true':
        print('::add-mask::' + value.replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A'))


def cleanup(state_path):
    if not state_path or not state_path.is_file():
        print('No temporary signing keychain to clean up.')
        return
    state = json.loads(state_path.read_text())
    keychain = Path(state['keychain'])
    if keychain.name != 'signing.keychain-db' or not keychain.parent.name.startswith('client-qt-signing-'):
        raise RuntimeError('Refusing to clean up an unrecognized keychain location')
    errors = []
    try:
        current = shlex.split(run(['security', 'list-keychains', '-d', 'user']))
        remaining = [path for path in current if Path(path).resolve() != keychain.resolve()]
        run(['security', 'list-keychains', '-d', 'user', '-s', *remaining])
    except (OSError, RuntimeError) as error:
        errors.append(str(error))
    # Search-list failures must not prevent removal of the imported private key.
    if keychain.exists():
        try:
            run(['security', 'delete-keychain', str(keychain)])
        except (OSError, RuntimeError) as error:
            errors.append(str(error))
            try:
                run(['security', 'lock-keychain', str(keychain)])
            except (OSError, RuntimeError) as lock_error:
                errors.append(str(lock_error))
    if errors:
        raise RuntimeError('Temporary signing cleanup incomplete; state retained for retry: ' +
                           str(state_path) + '; ' + '; '.join(errors))
    shutil.rmtree(keychain.parent)
    if state_path.exists():
        state_path.unlink()
    print('Temporary signing keychain removed; existing keychains preserved.')


def setup():
    required = ('APPLE_CERTIFICATE', 'APPLE_CERTIFICATE_PASSWORD', 'KEYCHAIN_PASSWORD')
    missing = [name for name in required if not os.environ.get(name)]
    if missing:
        export_environment({'APPLE_SIGNING_ENABLED': 'false', 'APPLE_SIGNING_IDENTITY': '', 'APPLE_SIGNING_KEYCHAIN': ''})
        print('Signing skipped: missing ' + ', '.join(missing) + '; unsigned packages will still be built.')
        return
    try:
        certificate = base64.b64decode(''.join(os.environ['APPLE_CERTIFICATE'].split()), validate=True)
    except ValueError:
        raise RuntimeError('APPLE_CERTIFICATE is not valid base64') from None
    directory = Path(tempfile.mkdtemp(prefix='client-qt-signing-', dir=os.environ.get('RUNNER_TEMP')))
    directory.chmod(0o700)
    keychain = directory / 'signing.keychain-db'
    p12 = directory / 'certificate.p12'
    state_path = directory / 'state.json'
    original = shlex.split(run(['security', 'list-keychains', '-d', 'user']))
    state_path.write_text(json.dumps({'keychain': str(keychain), 'original_search_list': original}))
    state_path.chmod(0o600)
    export_environment({'APPLE_KEYCHAIN_STATE': str(state_path)})
    try:
        p12.write_bytes(certificate)
        p12.chmod(0o600)
        password = os.environ['KEYCHAIN_PASSWORD']
        run(['security', 'create-keychain', '-p', password, str(keychain)])
        run(['security', 'unlock-keychain', '-p', password, str(keychain)])
        run(['security', 'set-keychain-settings', '-t', '3600', '-u', str(keychain)])
        run(['security', 'import', str(p12), '-k', str(keychain), '-P',
             os.environ['APPLE_CERTIFICATE_PASSWORD'], '-T', '/usr/bin/codesign'])
        run(['security', 'set-key-partition-list', '-S', 'apple-tool:,apple:,codesign:',
             '-s', '-k', password, str(keychain)])
        run(['security', 'list-keychains', '-d', 'user', '-s', *original, str(keychain)])
        identities = run(['security', 'find-identity', '-v', '-p', 'codesigning', str(keychain)])
        matches = re.findall(r'\b([0-9A-Fa-f]{40})\s+"(Developer ID Application:[^"\r\n]+)"', identities)
        if len(matches) != 1:
            raise RuntimeError('Expected exactly one valid Developer ID Application identity in the imported certificate')
        identity, name = matches[0]
        mask(identity)
        mask(name)
        values = {'APPLE_SIGNING_IDENTITY': identity, 'APPLE_SIGNING_KEYCHAIN': str(keychain),
                  'APPLE_SIGNING_ENABLED': 'true'}
        team = re.search(r'\(([A-Z0-9]+)\)$', name)
        if team:
            mask(team.group(1))
            configured = os.environ.get('APPLE_TEAM_ID')
            if configured and configured != team.group(1):
                raise RuntimeError('APPLE_TEAM_ID does not match the imported certificate')
            values['APPLE_TEAM_ID'] = team.group(1)
        export_environment(values)
        print('Developer ID certificate imported into a temporary keychain.')
        if not os.environ.get('GITHUB_ENV'):
            print('Signing environment is recorded in: ' + str(state_path))
            state = json.loads(state_path.read_text())
            state['environment'] = values
            state_path.write_text(json.dumps(state))
    except Exception as original:
        try:
            cleanup(state_path)
        except (OSError, RuntimeError) as cleanup_error:
            raise RuntimeError(str(original) + '; ' + str(cleanup_error)) from None
        raise
    finally:
        if p12.exists():
            p12.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('setup', 'cleanup'))
    parser.add_argument('--state', type=Path, help='Cleanup state path; defaults to APPLE_KEYCHAIN_STATE')
    args = parser.parse_args()
    if platform.system() != 'Darwin':
        parser.error('Run certificate setup on macOS')
    if args.action == 'setup':
        setup()
    else:
        value = args.state or os.environ.get('APPLE_KEYCHAIN_STATE')
        cleanup(Path(value) if value else None)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        print('Certificate setup failed: ' + str(error), file=sys.stderr)
        sys.exit(1)
