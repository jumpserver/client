"""Exercise the Shell entry's public CLI without native platform toolchains."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BuildEntryTests(unittest.TestCase):
    def invoke(self, *args, system='Darwin', arch='arm64'):
        with tempfile.TemporaryDirectory() as temp:
            uname = Path(temp) / 'uname'
            uname.write_text('#!/bin/sh\ncase "$1" in -s) echo "$TEST_SYSTEM";; -m) echo "$TEST_ARCH";; esac\n')
            uname.chmod(0o755)
            env = dict(os.environ, PATH=temp + os.pathsep + os.environ['PATH'],
                       TEST_SYSTEM=system, TEST_ARCH=arch)
            return subprocess.run(['bash', str(ROOT / 'build.sh'), '--dry-run', *args],
                                  env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_native_macos_and_quoted_paths(self):
        result = self.invoke('--qt-prefix', '/tmp/Qt with spaces', '--output', '/tmp/osm-shell-new-output',
                             '--cmake-arg', 'SAMPLE=literal $(do-not-run)')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('ctest', result.stdout)
        self.assertIn('11.0', result.stdout)
        self.assertIn('Qt\\ with\\ spaces', result.stdout)
        self.assertIn('package-dmg.py', result.stdout)

    def test_intel_cross_build_skips_execution(self):
        result = self.invoke('--arch', 'x86_64', '--qt-prefix', '/qt-intel', '--qt-host-bins', '/qt-arm/bin')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn('ctest', result.stdout)
        self.assertIn('--plugins-dir', result.stdout)
        self.assertIn('--skip-smoke', result.stdout)
        self.assertIn('10.15', result.stdout)

    def test_windows_formats(self):
        for fmt in ('exe', 'msi', 'both'):
            result = self.invoke('--format', fmt, '--qt-prefix', 'C:/Qt 5', system='MINGW64_NT', arch='x86_64')
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('--format ' + fmt, result.stdout)
            self.assertIn('--native-tls', result.stdout)
        result = self.invoke('--arch', '386', '--qt-prefix', 'C:/Qt-x86', system='MINGW64_NT', arch='x86_64')
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('--arch 386', result.stdout)

    def test_linux_formats_and_architectures(self):
        for cpu in ('x86_64', 'aarch64', 'loongarch64', 'mips64el', 'riscv64'):
            for fmt in ('deb', 'rpm'):
                result = self.invoke('--format', fmt, system='Linux', arch=cpu)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn('package-linux.py', result.stdout)

    def test_rejects_invalid_combinations(self):
        for args in (['--jobs', '0'], ['--qt-major', '7'], ['--version', 'bad'],
                     ['--platform', 'linux'], ['--arch', 'amd64', '--qt-prefix', '/qt'],
                     ['--format', 'rpm', '--qt-prefix', '/qt'], ['--unknown']):
            self.assertNotEqual(self.invoke(*args).returncode, 0, args)

    def test_never_overwrites_delivery(self):
        with tempfile.TemporaryDirectory() as output:
            result = self.invoke('--qt-prefix', '/qt', '--output', output)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('already exists', result.stderr)


if __name__ == '__main__':
    unittest.main()
