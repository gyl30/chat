#!/usr/bin/env python3
"""Package an existing Chat install and its Linux runtime dependencies."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys


def run(*command):
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT,
                                   env={k: v for k, v in os.environ.items()
                                        if k not in ('LD_LIBRARY_PATH', 'LD_PRELOAD', 'LD_AUDIT',
                                                     'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH')})


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


QT_VERSION = '6.5.3'
# Reviewed Phase 3 source companion checksums; filenames alone do not establish identity.
SOURCE_ARCHIVES = {
    'qtbase-everywhere-src-6.5.3.tar.xz': 'df2f4a230be4ea04f9798f2c19ab1413a3b8ec6a80bef359f50284235307b546',
    'qtsvg-everywhere-src-6.5.3.tar.xz': 'fc41c47b69ca1f80473cd4b11996394155781105345392961d064d04f95d5bef',
    'gcc-16.2.0.tar.xz': 'e6738e29597f733270731aa90600f37ffdc045079dfc27ec7e8192cc81085c3e',
}


def parse_ldd(output, binary):
    """Read dependencies in both ldd stages without silently omitting a path."""
    if re.search(r'not found|undefined symbol', output, re.IGNORECASE):
        raise RuntimeError(f'unresolved dependency for {binary}:\n{output}')
    dependencies = []
    for line in output.splitlines():
        if not line.strip():
            continue
        # ldd also emits this for data ELFs with no DT_NEEDED entries (e.g. ICU data).
        if line.strip() == 'statically linked':
            continue
        match = re.fullmatch(r'\s*(\S+)\s+=>\s+(/.+?)\s+\(0x[0-9a-fA-F]+\)\s*', line)
        if match:
            dependencies.append(match.groups())
        elif re.fullmatch(r'\s*(?:linux-vdso\.so\.\d+|linux-gate\.so\.\d+)\s+\(0x[0-9a-fA-F]+\)\s*', line):
            continue
        elif re.fullmatch(r'\s*/.*/(?:ld-linux[^/]*\.so(?:\.\d+)*|ld(?:64)?\.so(?:\.\d+)*)\s+\(0x[0-9a-fA-F]+\)\s*', line):
            continue
        else:
            raise RuntimeError(f'unparseable dependency for {binary}: {line}')
    return dependencies


def source_sql_checksums(directory):
    files = sorted(directory.glob('*.sql'))
    numbers = []
    for path in files:
        match = re.fullmatch(r'([0-9]{3})_.+\.sql', path.name)
        if not match or not path.is_file():
            raise RuntimeError(f'source SQL must contain regular migrations 001–027: {path}')
        numbers.append(int(match[1]))
    if numbers != list(range(1, 28)):
        raise RuntimeError('source SQL must contain continuous migrations 001–027 without duplicate numbers')
    return {path.name: sha(path) for path in files}


def check_installed_sql(directory, expected):
    files = list(directory.iterdir())
    if {path.name for path in files} != set(expected) or any(not path.is_file() for path in files):
        raise RuntimeError('installed SQL filenames must exactly match source migrations 001–027')
    for path in files:
        if sha(path) != expected[path.name]:
            raise RuntimeError(f'installed SQL SHA-256 differs from source: {path.name}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=pathlib.Path, required=True)
    parser.add_argument('--build', type=pathlib.Path, required=True)
    parser.add_argument('--qt-prefix', type=pathlib.Path, required=True)
    parser.add_argument('--notices', type=pathlib.Path, required=True,
                        help='Prepared complete notices and corresponding source archives')
    parser.add_argument('--sources', type=pathlib.Path, required=True,
                        help='Matching Qtbase, Qtsvg and GCC source archives')
    parser.add_argument('--patchelf', required=True)
    parser.add_argument('--output', type=pathlib.Path, required=True,
                        help='New output directory; never overwrites an existing path')
    args = parser.parse_args()
    source, build, qt = (p.resolve() for p in (args.source, args.build, args.qt_prefix))
    if args.output.exists():
        parser.error('output already exists; choose a new directory')
    for required in (source / 'THIRD_PARTY.md', build / 'CMakeCache.txt',
                     qt / 'plugins/platforms/libqxcb.so', qt / 'bin/qmake', args.notices / 'README.md',
                     *(args.sources / name for name in SOURCE_ARCHIVES)):
        if not required.is_file():
            parser.error(f'missing required input: {required}')
    if run('git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all').strip():
        parser.error('source worktree is dirty; package a committed version')
    cache = (build / 'CMakeCache.txt').read_text()
    home = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache, re.MULTILINE)
    if not home or pathlib.Path(home[1]).resolve() != source:
        parser.error('build cache belongs to a different source directory')
    compiler = re.search(r'^CMAKE_CXX_COMPILER:FILEPATH=(.*)$', cache, re.MULTILINE)
    if not compiler:
        parser.error('build cache does not identify its C++ compiler')
    qt_version = run(str(qt / 'bin/qmake'), '-query', 'QT_VERSION').strip()
    if qt_version != QT_VERSION:
        raise RuntimeError(f'Qt version must be {QT_VERSION}; received {qt_version}')
    for name, expected in SOURCE_ARCHIVES.items():
        if sha(args.sources / name) != expected:
            raise RuntimeError(f'source archive SHA-256 mismatch: {name}')
    sql_checksums = source_sql_checksums(source / 'sql')
    head = run('git', '-C', str(source), 'rev-parse', 'HEAD').strip()
    epoch = run('git', '-C', str(source), 'show', '-s', '--format=%ct', 'HEAD').strip()
    # Refresh the exact build before assigning its source revision to the release.
    run('cmake', '--build', str(build), '--parallel', '2')
    if run('git', '-C', str(source), 'rev-parse', 'HEAD').strip() != head or run(
            'git', '-C', str(source), 'status', '--porcelain', '--untracked-files=all').strip():
        raise RuntimeError('source changed while building; no release was generated')
    output = args.output.resolve()
    output.mkdir(parents=True)
    root = output / 'chat'
    run('cmake', '--install', str(build), '--prefix', str(root))
    for name in ('chat_server', 'chat_tui', 'chat_qt'):
        if not (root / 'bin' / name).is_file():
            raise RuntimeError(f'install missing {name}; enable both clients and rebuild')
    check_installed_sql(root / 'share/chat/sql', sql_checksums)
    plugins = []
    plugin_files = {
        'platforms': ('libqxcb.so', 'libqoffscreen.so'),
        'iconengines': ('libqsvgicon.so',),
        'imageformats': ('libqgif.so', 'libqico.so', 'libqjpeg.so', 'libqsvg.so'),
        'xcbglintegrations': ('libqxcb-egl-integration.so', 'libqxcb-glx-integration.so'),
    }
    for group, names in plugin_files.items():
        candidates = [qt / 'plugins' / group / name for name in names]
        for plugin in sorted(candidates):
            destination = root / 'plugins' / group / plugin.name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(plugin, destination)
            plugins.append(destination)
    (root / 'bin/qt.conf').write_text('[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\n')
    # Only the declared private runtime is redistributed; the full system closure
    # remains visible in the manifest and must be checked on the target machine.
    private_library = re.compile(r'^(libQt6.*\.so\.6|libicu.*\.so\.56|'
                                 r'libboost_(json|container|url)\.so\.1\.92\.0|'
                                 r'libstdc\+\+\.so\.6|libgcc_s\.so\.1|libgomp\.so\.1|'
                                 r'lib(ssl|crypto)\.so\.3|libpq\.so\.5|'
                                 r'libpng16\.so\.16|libjpeg\.so\.8|libz\.so\.1|'
                                 r'libxcb-cursor\.so\.0)$')
    provided, external = {}, {}
    executables = [root / 'bin' / name for name in ('chat_server', 'chat_tui', 'chat_qt')]
    for binary in executables + [qt / 'plugins' / binary.relative_to(root / 'plugins') for binary in plugins]:
        dependencies = run('ldd', str(binary))
        for name, origin in parse_ldd(dependencies, binary):
            if not private_library.match(name):
                if not origin.startswith(('/lib/', '/usr/lib/')):
                    raise RuntimeError(f'undeclared private dependency: {name}: {origin}')
                external[name] = origin
            else:
                previous = provided.get(name)
                if previous and pathlib.Path(previous).resolve() != pathlib.Path(origin).resolve():
                    raise RuntimeError(f'conflicting dependency {name}: {previous} / {origin}')
                provided[name] = origin
    library_dir = root / 'lib'
    library_dir.mkdir()
    for name, origin in sorted(provided.items()):
        shutil.copy2(pathlib.Path(origin).resolve(), library_dir / name)
    # RPATH also covers dependencies reached through system libraries (e.g.
    # system FreeType -> bundled PNG); all paths remain relative to the ELF.
    for binary in executables:
        run(args.patchelf, '--force-rpath', '--set-rpath', '$ORIGIN/../lib', str(binary))
    for binary in sorted(library_dir.iterdir()):
        run(args.patchelf, '--force-rpath', '--set-rpath', '$ORIGIN', str(binary))
    for binary in plugins:
        run(args.patchelf, '--force-rpath', '--set-rpath', '$ORIGIN/../../lib', str(binary))
    shutil.copytree(args.notices, root / 'licenses')
    (root / 'licenses/sources').mkdir()
    for name in SOURCE_ARCHIVES:
        shutil.copy2(args.sources / name, root / 'licenses/sources' / name)
    shutil.copy2(source / 'THIRD_PARTY.md', root / 'licenses/CHAT-THIRD-PARTY.md')
    (root / 'README.md').write_text(
        '# Chat Linux release\n\n'
        'Ubuntu 22.04 x86_64 / glibc 2.35 is the tested platform, not all Linux.\n'
        'Extract and run bin/chat_server PORT WORKERS DB_CONNECTIONS, '
        'bin/chat_tui ws://HOST:PORT/ws or bin/chat_qt ws://HOST:PORT/ws.\n'
        'The server needs an independent PostgreSQL database and libpq connection '
        'configuration. Apply share/chat/sql/001–027 only to a new empty database.\n'
        'For WSS use a valid server certificate and client trust store; no TLS bypass.\n'
        'Qt uses bundled libraries/plugins via relative RPATH and bin/qt.conf. '
        'System X11/XCB/OpenGL drivers, display access, fonts and optional Fcitx5 '
        'daemon are external. No SDK or LD_LIBRARY_PATH is required.\n'
        'See manifest.json for exact provenance and system dependencies; see '
        'licenses/ for complete notices and licenses/sources for the corresponding '
        'Qt/GCC source releases. Qt is distributed under LGPLv3; compatible '
        'modified shared libraries may replace lib/ and plugins/ files, and debugging '
        'such modifications is permitted. The optional native Fcitx plugin is not bundled.\n'
        'This software is based in part on the work of the Independent JPEG Group.\n')
    checks = {}
    for binary in executables + list(library_dir.iterdir()) + plugins:
        result = run('ldd', '-r', str(binary))
        resolved_dependencies = parse_ldd(result, binary)
        dynamic = run('readelf', '-d', str(binary))
        for line in dynamic.splitlines():
            if 'NEEDED' in line and '/' in line.split('[', 1)[1].split(']', 1)[0]:
                raise RuntimeError(f'absolute DT_NEEDED remains: {binary}: {line}')
            if 'RPATH' in line or 'RUNPATH' in line:
                if '/home/' in line or '/usr/local/' in line or str(build) in line:
                    raise RuntimeError(f'development RUNPATH remains: {binary}: {line}')
        for name, origin in resolved_dependencies:
            if name in provided and pathlib.Path(origin).resolve() != (library_dir / name).resolve():
                raise RuntimeError(f'private dependency resolved outside package: {binary}: {name}: {origin}')
        checks[str(binary.relative_to(root))] = re.sub(r' \(0x[0-9a-f]+\)', '', result).replace(str(root), '$PACKAGE')
    manifest = {
        'head': head, 'dirty': False,
        'packager_sha256': sha(pathlib.Path(__file__)),
        'patchelf_version': run(args.patchelf, '--version').strip(),
        'compiler_version': run(compiler[1], '--version').splitlines()[0],
        'build_glibc': run('getconf', 'GNU_LIBC_VERSION').strip(),
        'architecture': run('uname', '-m').strip(),
        'submodules': run('git', '-C', str(source), 'submodule', 'status').splitlines(),
        'build_cache_sha256': sha(build / 'CMakeCache.txt'),
        'build_configuration': [line for line in (build / 'CMakeCache.txt').read_text().splitlines()
                                if re.match(r'(CMAKE_(C|CXX)_COMPILER|CMAKE_BUILD_TYPE|Boost_DIR|Qt6_DIR):', line)],
        'build_flags_sha256': sha(build / 'CMakeFiles/chat_server.dir/flags.make'),
        'qt_version': qt_version,
        'provided_libraries': [{'name': name, 'origin': origin, 'original_sha256': sha(pathlib.Path(origin)),
                                'packaged_sha256': sha(library_dir / name)}
                               for name, origin in sorted(provided.items())],
        'system_library_sonames': sorted(external),
        'fcitx_bundled': False,
        'ldd_r': checks,
        'files': {str(path.relative_to(root)): sha(path) for path in sorted(root.rglob('*')) if path.is_file()},
    }
    (root / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    archive = output / f'chat-{head[:12]}-linux-x86_64.tar.gz'
    tar = subprocess.Popen(['tar', '--sort=name', f'--mtime=@{epoch}', '--owner=0', '--group=0',
                            '--numeric-owner', '-C', str(output), '-cf', '-', 'chat'], stdout=subprocess.PIPE)
    with archive.open('wb') as stream:
        subprocess.run(['gzip', '-n'], stdin=tar.stdout, stdout=stream, check=True)
    tar.stdout.close()
    if tar.wait():
        raise RuntimeError('tar failed')
    (output / 'SHA256SUMS').write_text(f'{sha(archive)}  {archive.name}\n')
    print(json.dumps({'archive': str(archive), 'sha256': sha(archive), 'bytes': archive.stat().st_size,
                      'provided_libraries': len(provided), 'system_sonames': sorted(external)}, indent=2))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'打包失败：{error}', file=sys.stderr)
        sys.exit(1)
