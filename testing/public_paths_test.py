"""Privacy guard coverage for encoded paths and safe portable examples."""
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('public_paths', Path(__file__).resolve().parents[1]/'scripts/check_public_paths.py')
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


class PublicPathTest(unittest.TestCase):
    def test_personal_variants(self):
        windows = 'C:' + '/Users/' + 'PrivateAccount/project'
        for text in (windows, windows.replace('/', '\\'), windows.replace('/', '\\\\'),
                     '/home/'+'PrivateAccount/project', '/Users/'+'PrivateAccount/project',
                     windows.replace('/', '%2F'), 'file://'+'/tmp/private-report'):
            with self.subTest(text=text):
                self.assertEqual(guard.violations(text.encode()), [1])
        self.assertEqual(guard.violations(windows.encode('utf-16le')), [1])

    def test_portable_and_hosted_examples(self):
        for text in ('docs/report.md', '/kaggle/working/report', 'C:'+'/msys64/bin',
                     'C:'+'/Users/runneradmin/work', '/home/'+'runner/work',
                     'C:'+'/Users/username/project', 'https://github.com/example/project'):
            self.assertEqual(guard.violations(text.encode()), [])


if __name__ == '__main__':
    unittest.main()
