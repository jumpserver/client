import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


ci = load("ci_version", "ci-version.py")
qt = load("build_qt", "build-qt.py")


class VersionPolicyTests(unittest.TestCase):
    cmake = 'set(CLIENT_VERSION "4.1.2" CACHE STRING "version")'

    def test_branch_default_is_unsigned(self):
        result = ci.resolve({}, self.cmake)
        self.assertEqual(result["version"], "4.1.2")
        self.assertEqual(result["sign_macos"], "false")
        self.assertEqual(result["sign_windows"], "false")
        self.assertNotIn('"386"', result["windows_matrix"])

    def test_tag_requests_mac_signing(self):
        result = ci.resolve({"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v4.2.7", "SIGN_MACOS": "false"}, self.cmake)
        self.assertEqual(result["version"], "4.2.7")
        self.assertEqual(result["sign_macos"], "true")

    def test_dispatch_version_architecture_and_signing(self):
        result = ci.resolve({"REQUESTED_VERSION": "4.2.8", "INCLUDE_WINDOWS_X86": "true", "SIGN_WINDOWS": "true"}, self.cmake)
        self.assertEqual(result["version"], "4.2.8")
        self.assertEqual(result["sign_windows"], "true")
        self.assertIn('"386"', result["windows_matrix"])

    def test_rejects_injection_prerelease_and_msi_overflow(self):
        for value in ["4.2.3\nSIGN_MACOS=false", "$(echo bad)", "4.2.3-beta", "256.1.1", "1.256.1", "1.1.65536", "01.2.3"]:
            with self.subTest(value=value), self.assertRaises(ValueError):
                ci.version(value)

    def test_tag_and_input_must_agree(self):
        with self.assertRaises(ValueError):
            ci.resolve({"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v4.2.7", "REQUESTED_VERSION": "4.2.8"}, self.cmake)


class QtSourceTests(unittest.TestCase):
    def test_hash_mismatch_never_extracts(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / "source.tar.xz"
            archive.write_bytes(b"untrusted replacement")
            with self.assertRaises(RuntimeError):
                qt.download("unused", archive, "0" * 64)

    @unittest.skipUnless(hasattr(tarfile, "data_filter"), "Qt CI builder requires Python 3.12")
    def test_source_extraction_rejects_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = root / "source.tar.xz"
            with tarfile.open(archive, "w:xz") as output:
                entry = tarfile.TarInfo("../escape")
                entry.size = 1
                output.addfile(entry, io.BytesIO(b"x"))
            with self.assertRaises(tarfile.FilterError):
                qt.extract(archive, root / "out")
            self.assertFalse((root / "escape").exists())

    def test_platform_tls_and_macos_deployment_are_explicit(self):
        mac = qt.configure_args(Path("/qt"), "Darwin", "arm64", "11.0")
        win = qt.configure_args(Path("C:/qt"), "Windows", "x86", "")
        self.assertIn("-securetransport", mac)
        self.assertIn("QMAKE_MACOSX_DEPLOYMENT_TARGET=11.0", mac)
        self.assertIn("-schannel", win)
        self.assertIn("-no-openssl", win)


if __name__ == "__main__":
    unittest.main()
