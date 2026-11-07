"""Check native package naming without compiling or installing a Linux app."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class LinuxPackagingTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cmake'), 'CMake is not installed')
    def test_download_name_changes_without_changing_native_package_identity(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            for architecture in ('amd64', 'arm64'):
                script = work / 'names.cmake'
                result_file = work / 'names.txt'
                script.write_text(
                    'set(CMAKE_CURRENT_SOURCE_DIR "' + ROOT.as_posix() + '")\n'
                    'set(CMAKE_CURRENT_BINARY_DIR "' + work.as_posix() + '")\n'
                    'set(PROJECT_VERSION "4.1.2")\n'
                    'set(CLIENT_GOARCH "' + architecture + '")\n'
                    'set(CLIENT_QT_MAJOR "5")\n'
                    'include("' + (ROOT / 'packaging/linux.cmake').as_posix() + '")\n'
                    'file(WRITE "' + result_file.as_posix() + '" '
                    '"${CPACK_DEBIAN_PACKAGE_NAME}\\n${CPACK_RPM_PACKAGE_NAME}\\n'
                    '${CPACK_DEBIAN_FILE_NAME}\\n${CPACK_RPM_FILE_NAME}\\n")\n')
                completed = subprocess.run([shutil.which('cmake'), '-P', str(script)],
                                           capture_output=True, text=True)
                self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
                self.assertEqual(result_file.read_text().splitlines(), [
                    'client-qt', 'client-qt',
                    'OSMClient-4.1.2-linux-' + architecture + '.deb',
                    'OSMClient-4.1.2-linux-' + architecture + '.rpm'])


if __name__ == '__main__':
    unittest.main()
