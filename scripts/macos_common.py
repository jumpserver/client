"""Shared macOS packaging operations; never include credentials in errors."""
import os
import subprocess
from pathlib import Path


SECRET_NAMES = (
    'APPLE_CERTIFICATE', 'APPLE_CERTIFICATE_PASSWORD', 'KEYCHAIN_PASSWORD',
    'APPLE_PASSWORD', 'APPLE_ID', 'APPLE_TEAM_ID', 'APPLE_SIGNING_IDENTITY',
)


def redact(value):
    for name in SECRET_NAMES:
        secret = os.environ.get(name)
        if secret:
            value = value.replace(secret, '[redacted]')
    return value


def run(command, timeout=120):
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError('{} timed out'.format(Path(command[0]).name)) from None
    if result.returncode:
        detail = redact((result.stderr or result.stdout).strip())[-3000:]
        raise RuntimeError('{} failed ({}): {}'.format(Path(command[0]).name, result.returncode, detail))
    return result.stdout


def signing_identity(release):
    identity = os.environ.get('APPLE_SIGNING_IDENTITY', '') if release else '-'
    if not identity or (release and identity == '-'):
        raise RuntimeError('--sign requires APPLE_SIGNING_IDENTITY from the certificate setup step')
    return identity


def resolve_signing(requested):
    if not requested:
        return False
    identity = os.environ.get('APPLE_SIGNING_IDENTITY', '')
    keychain = os.environ.get('APPLE_SIGNING_KEYCHAIN')
    if not identity or identity == '-' or (keychain and not Path(keychain).is_file()):
        print('Signing skipped: no available Apple signing identity/keychain; producing an unsigned development package.')
        return False
    return True


def sign(path, release=False, entitlements=None, runtime=True):
    command = ['codesign', '--force', '--sign', signing_identity(release)]
    if release:
        command.append('--timestamp')
        if runtime:
            command.extend(['--options', 'runtime'])
        keychain = os.environ.get('APPLE_SIGNING_KEYCHAIN')
        if keychain:
            command.extend(['--keychain', keychain])
        if entitlements:
            command.extend(['--entitlements', str(entitlements)])
    else:
        command.append('--timestamp=none')
    run(command + [str(path)])


def smoke(bundle):
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('DYLD_', 'QT_', 'QML_', 'QML2_'))}
    env['QT_QPA_PLATFORM'] = 'cocoa'
    try:
        result = subprocess.run([str(bundle / 'Contents/MacOS/client'), '--smoke-test'],
                                env=env, cwd=bundle.parent, timeout=20,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    except subprocess.TimeoutExpired:
        raise RuntimeError('Packaged app smoke test timed out') from None
    if result.returncode:
        raise RuntimeError('Packaged app smoke test failed: ' + redact(result.stderr)[-2000:])
