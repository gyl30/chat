#!/usr/bin/env python3
"""Offline guards for the release packager; no builds or external services."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / 'tools/package_linux.py'
ARCHIVE_DIGESTS = {
    'qtbase-everywhere-src-6.5.3.tar.xz': 'df2f4a230be4ea04f9798f2c19ab1413a3b8ec6a80bef359f50284235307b546',
    'qtsvg-everywhere-src-6.5.3.tar.xz': 'fc41c47b69ca1f80473cd4b11996394155781105345392961d064d04f95d5bef',
    'gcc-16.2.0.tar.xz': 'e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e',
}


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
        (self.source / 'sql').mkdir()
        for number in range(1, 28):
            (self.source / 'sql' / f'{number:03}_fixture.sql').write_text(f'-- migration {number}\n')
        (self.build / 'CMakeFiles/chat_server.dir').mkdir(parents=True)
        (self.build / 'CMakeFiles/chat_server.dir/flags.make').touch()
        (self.qt / 'bin').mkdir()
        (self.qt / 'bin/qmake').touch()
        for group, names in {
            'platforms': ('libqoffscreen.so',),
            'iconengines': ('libqsvgicon.so',),
            'imageformats': ('libqgif.so', 'libqico.so', 'libqjpeg.so', 'libqsvg.so'),
            'xcbglintegrations': ('libqxcb-egl-integration.so', 'libqxcb-glx-integration.so'),
        }.items():
            (self.qt / 'plugins' / group).mkdir(exist_ok=True)
            for name in names:
                (self.qt / 'plugins' / group / name).touch()
        (self.build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={self.source}\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/c++\n')
        (self.qt / 'plugins/platforms/libqxcb.so').touch()
        (self.notices / 'README.md').touch()
        for name in ('qtbase-everywhere-src-6.5.3.tar.xz', 'qtsvg-everywhere-src-6.5.3.tar.xz', 'gcc-16.2.0.tar.xz'):
            (self.sources / name).touch()
        subprocess.run(['git', 'init', '-q', str(self.source)], check=True)
        subprocess.run(['git', '-C', str(self.source), 'add', 'THIRD_PARTY.md', 'sql'], check=True)
        subprocess.run(['git', '-C', str(self.source), '-c', 'user.name=Package test',
                        '-c', 'user.email=package@example.invalid', 'commit', '-qm', 'fixture'], check=True)

    def invoke(self):
        return subprocess.run([sys.executable, '-B', str(SCRIPT), '--source', str(self.source),
                               '--build', str(self.build), '--qt-prefix', str(self.qt),
                               '--notices', str(self.notices), '--sources', str(self.sources),
                               '--patchelf', 'unused', '--output', str(self.root / 'output')],
                              capture_output=True, text=True, timeout=10)

    def invoke_mocked(self, *, build_ldd=None, runtime_ldd=None, qt_version='6.5.3',
                      source_digests=None, installed_sql=None, output_name='output'):
        """Exercise main with real tiny files; mock only external tools and archive identity."""
        spec = importlib.util.spec_from_file_location('package_linux', SCRIPT)
        package = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(package)
        real_run, real_sha = package.run, package.sha
        invocation = getattr(self, 'mock_invocations', 0)
        self.mock_invocations = invocation + 1
        output = self.root / (output_name if invocation == 0 else f'{output_name}-{invocation}')
        self.last_output = output
        package_root = output / 'chat'
        commands = []
        def fake_run(*command):
            commands.append(command)
            if command[0] == 'git':
                return real_run(*command)
            if command[:2] == ('cmake', '--build'):
                return ''
            if command[:2] == ('cmake', '--install'):
                (package_root / 'bin').mkdir(parents=True)
                for name in ('chat_server', 'chat_tui', 'chat_qt'):
                    (package_root / 'bin' / name).touch()
                shutil.copytree(self.source / 'sql', package_root / 'share/chat/sql')
                if installed_sql:
                    installed_sql(package_root / 'share/chat/sql')
                return ''
            if command[0] == str(self.qt / 'bin/qmake'):
                return qt_version + '\n'
            if command[0] == 'ldd':
                if command[1] == '-r':
                    if runtime_ldd is not None:
                        return runtime_ldd
                    private = package_root / 'lib/libQt6Core.so.6'
                    return f'libQt6Core.so.6 => {private} (0x123)\n' if private.exists() else ''
                return build_ldd if build_ldd is not None else (
                    'linux-vdso.so.1 (0x123)\n\t/lib64/ld-linux-x86-64.so.2 (0x456)\n'
                    '\tlibc.so.6 => /usr/lib/libc.so.6 (0x789)\n')
            if command[0] == 'readelf' or command[0] == 'unused':
                return ''
            if command[1:] == ('--version',):
                return 'fixture tool 1.0\n'
            if command[0] == 'getconf':
                return 'glibc 2.35\n'
            if command[0] == 'uname':
                return 'x86_64\n'
            raise AssertionError(f'Unexpected external command: {command}')
        def input_sha(path):
            if path.parent == self.sources and path.name in ARCHIVE_DIGESTS:
                return (source_digests or ARCHIVE_DIGESTS)[path.name]
            return real_sha(path)
        argv = [str(SCRIPT), '--source', str(self.source), '--build', str(self.build),
                '--qt-prefix', str(self.qt), '--notices', str(self.notices), '--sources', str(self.sources),
                '--patchelf', 'unused', '--output', str(output)]
        with mock.patch.object(package, 'run', side_effect=fake_run), \
             mock.patch.object(package, 'sha', side_effect=input_sha), \
             mock.patch.object(sys, 'argv', argv), contextlib.redirect_stdout(io.StringIO()), \
             contextlib.redirect_stderr(io.StringIO()):
            package.main()
        return package_root, commands

    def commit_source(self):
        subprocess.run(['git', '-C', str(self.source), 'add', 'THIRD_PARTY.md', 'sql'], check=True)
        subprocess.run(['git', '-C', str(self.source), '-c', 'user.name=Package test',
                        '-c', 'user.email=package@example.invalid', 'commit', '-qm', 'changed fixture'], check=True)

    def test_private_dependency_path_with_spaces_is_collected(self):
        origin = self.qt / 'lib with spaces/libQt6Core.so.6'
        origin.parent.mkdir()
        origin.write_bytes(b'private dependency')
        root, _ = self.invoke_mocked(build_ldd=f'\tlibQt6Core.so.6 => {origin} (0x123)\n')
        manifest = json.loads((root / 'manifest.json').read_text())
        self.assertEqual([item['name'] for item in manifest['provided_libraries']], ['libQt6Core.so.6'])
        self.assertEqual((root / 'lib/libQt6Core.so.6').read_bytes(), origin.read_bytes())

    def test_normal_absolute_dependency_and_in_bundle_path_with_spaces_are_accepted(self):
        origin = self.qt / 'lib/libQt6Core.so.6'
        origin.parent.mkdir()
        origin.write_bytes(b'private dependency')
        root, _ = self.invoke_mocked(build_ldd=f'libQt6Core.so.6 => {origin} (0x123)\n',
                                    output_name='output with spaces')
        manifest = json.loads((root / 'manifest.json').read_text())
        self.assertEqual([item['name'] for item in manifest['provided_libraries']], ['libQt6Core.so.6'])
        self.assertEqual((root / 'lib/libQt6Core.so.6').read_bytes(), origin.read_bytes())

    def test_unparseable_build_dependency_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unparseable.*dependency'):
            self.invoke_mocked(build_ldd='libQt6Core.so.6 => relative/location (0x123)\n')

    def test_unparseable_packaged_dependency_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unparseable.*dependency'):
            self.invoke_mocked(runtime_ldd='libQt6Core.so.6 => no absolute origin\n')

    def test_build_unresolved_dependency_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unresolved'):
            self.invoke_mocked(build_ldd='libQt6Core.so.6 => not found\n')

    def test_build_undefined_symbol_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unresolved'):
            self.invoke_mocked(build_ldd='undefined symbol: missing (fixture)\n')

    def test_packaged_unresolved_dependency_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unresolved'):
            self.invoke_mocked(runtime_ldd='libQt6Core.so.6 => not found\n')

    def test_packaged_undefined_symbol_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'unresolved'):
            self.invoke_mocked(runtime_ldd='undefined symbol: missing (fixture)\n')

    def test_packaged_private_dependency_with_spaces_outside_bundle_is_rejected(self):
        origin = self.qt / 'lib/libQt6Core.so.6'
        outside = self.qt / 'external with spaces/libQt6Core.so.6'
        origin.parent.mkdir()
        outside.parent.mkdir()
        origin.touch()
        outside.touch()
        with self.assertRaisesRegex(RuntimeError, 'private dependency resolved outside package'):
            self.invoke_mocked(build_ldd=f'libQt6Core.so.6 => {origin} (0x123)\n',
                               runtime_ldd=f'libQt6Core.so.6 => {outside} (0x456)\n')

    def test_vdso_loader_and_system_dependencies_are_accepted(self):
        root, _ = self.invoke_mocked()
        self.assertTrue((root / 'manifest.json').is_file())

    def test_sql_install_same_count_with_missing_and_extra_file_is_rejected(self):
        def change(sql):
            (sql / '027_fixture.sql').rename(sql / '028_extra.sql')
        with self.assertRaisesRegex(RuntimeError, 'SQL.*filenames'):
            self.invoke_mocked(installed_sql=change)

    def test_sql_install_missing_migration_is_rejected(self):
        def change(sql):
            (sql / '027_fixture.sql').unlink()
        with self.assertRaises(RuntimeError):
            self.invoke_mocked(installed_sql=change)

    def test_sql_install_changed_bytes_are_rejected(self):
        def change(sql):
            (sql / '026_fixture.sql').write_text('wrong bytes\n')
        with self.assertRaisesRegex(RuntimeError, 'SQL.*SHA'):
            self.invoke_mocked(installed_sql=change)

    def test_sql_source_duplicate_number_is_rejected(self):
        (self.source / 'sql/027_fixture.sql').rename(self.source / 'sql/026_duplicate.sql')
        self.commit_source()
        with self.assertRaisesRegex(RuntimeError, 'SQL.*001'):
            self.invoke_mocked()

    def test_sql_source_gap_is_rejected(self):
        (self.source / 'sql/027_fixture.sql').rename(self.source / 'sql/028_extra.sql')
        self.commit_source()
        with self.assertRaisesRegex(RuntimeError, 'SQL.*001'):
            self.invoke_mocked()

    def test_sql_install_duplicate_number_is_rejected(self):
        def change(sql):
            (sql / '027_fixture.sql').rename(sql / '026_duplicate.sql')
        with self.assertRaisesRegex(RuntimeError, 'SQL.*filenames'):
            self.invoke_mocked(installed_sql=change)

    def test_sql_install_extra_file_is_rejected(self):
        def change(sql):
            (sql / '028_extra.sql').touch()
        with self.assertRaises(RuntimeError):
            self.invoke_mocked(installed_sql=change)

    def test_qt_version_mismatch_is_rejected_before_build(self):
        with self.assertRaisesRegex(RuntimeError, 'Qt.*6.5.3'):
            self.invoke_mocked(qt_version='6.6.0')
        self.assertFalse((self.root / 'output').exists())

    def test_source_archive_hash_mismatch_is_rejected_before_build(self):
        for name in ARCHIVE_DIGESTS:
            with self.subTest(archive=name):
                bad = dict(ARCHIVE_DIGESTS, **{name: '0' * 64})
                with self.assertRaisesRegex(RuntimeError, 'source archive SHA'):
                    self.invoke_mocked(source_digests=bad)
                self.assertFalse(self.last_output.exists())

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
