#!/usr/bin/env python3
"""Offline guards for the release packager; no builds or external services."""

import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'tools/package_linux.py'


class PackageGuards(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.build = self.root / 'build'
        self.qt = self.root / 'qt'
        self.notices = self.root / 'notices'
        self.sources = self.root / 'sources'
        for directory in (self.source, self.build, self.qt / 'plugins/platforms', self.notices, self.sources):
            directory.mkdir(parents=True)
        (self.source / 'THIRD_PARTY.md').touch()
        (self.build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n')
        (self.qt / 'plugins/platforms/libqxcb.so').touch()
        (self.notices / 'README.md').touch()
        for name in ('qtbase-everywhere-src-6.5.3.tar.xz', 'qtsvg-everywhere-src-6.5.3.tar.xz', 'gcc-16.2.0.tar.xz'):
            (self.sources / name).touch()
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        subprocess.run(['git', '-C', str(self.source), 'add', 'THIRD_PARTY.md'], check=True)
        subprocess.run(['git', '-C', str(self.source), '-c', 'user.name=Package test',
                        '-c', 'user.email=package@example.invalid', 'commit', '-qm', 'fixture'], check=True)

    def invoke(self):
        return subprocess.run([sys.executable, '-B', str(SCRIPT), '--source', str(self.source),
                               '--build', str(self.build), '--qt-prefix', str(self.qt),
                               '--notices', str(self.notices), '--sources', str(self.sources),
                               '--patchelf', 'unused', '--output', str(self.root / 'output')],
                              capture_output=True, text=True, timeout=10)

    def test_existing_output_is_not_overwritten(self):
        output = self.root / 'output'
        output.mkdir()
        marker = output / 'keep'
        marker.write_text('original')
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn('output already exists', result.stderr)
        self.assertEqual(marker.read_text(), 'original')

    def test_missing_source_is_rejected(self):
        (self.sources / 'qtsvg-everywhere-src-6.5.3.tar.xz').unlink()
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn('missing required input', result.stderr)
        self.assertFalse((self.root / 'output').exists())

    def test_dirty_source_is_rejected_before_build(self):
        (self.source / 'uncommitted').touch()
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn('source worktree is dirty', result.stderr)
        self.assertFalse((self.root / 'output').exists())

    def test_wrong_build_source_is_rejected(self):
        (self.build / 'CMakeCache.txt').write_text('CMAKE_HOME_DIRECTORY:INTERNAL=/wrong-source\n')
        result = self.invoke()
        self.assertEqual(result.returncode, 2)
        self.assertIn('build cache belongs to a different source', result.stderr)
        self.assertFalse((self.root / 'output').exists())

    def test_dependency_checks_ignore_loader_overrides(self):
        spec = importlib.util.spec_from_file_location('package_linux', SCRIPT)
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        overrides = ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT', 'QT_PLUGIN_PATH',
                     'QT_QPA_PLATFORM_PLUGIN_PATH')
        original = dict(os.environ)
        try:
            os.environ.update(dict.fromkeys(overrides, '/nonexistent/package-test'))
            result = package.run(sys.executable, '-c',
                                 'import os; print([k for k in ' + repr(overrides) + ' if k in os.environ])')
            self.assertEqual(result.strip(), '[]')
        finally:
            os.environ.clear()
            os.environ.update(original)


if __name__ == '__main__':
    unittest.main()
