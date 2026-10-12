#!/usr/bin/env python3
"""Owned native server observer. GDB stops are boundary checks, not performance samples.

No RPC, SDK, inferior function calls, or database access. Requires Linux /proc,
GDB with Python, libstdc++ debug types, and a chat_server built with debug info.
"""
import argparse
import ast
from collections import deque
import json
import os
from pathlib import Path
import queue
import re
import shutil
import signal
import subprocess
import threading
import time
import urllib.request


class ObserverError(RuntimeError):
    pass


def _process(pid):
    root = Path('/proc') / str(pid)
    stat = (root / 'stat').read_text()
    fields = stat[stat.rfind(')') + 2:].split()
    executable = os.readlink(root / 'exe').removesuffix(' (deleted)')
    file_stat = (root / 'exe').stat()
    return {'pid': pid, 'start_ticks': int(fields[19]), 'state': fields[0],
            'executable': str(Path(executable).resolve()),
            'file_id': (file_stat.st_dev, file_stat.st_ino)}


def _field(record, name):
    match = re.search(r'(?:^|,)' + re.escape(name) + r'=("(?:[^"\\]|\\.)*")', record)
    return ast.literal_eval(match.group(1)) if match else None


class Server:
    """One GDB-owned real server; the caller supplies its isolated database env."""
    timeout = 30.0

    def __init__(self, binary, port, workers, connections, work, env=None):
        self.binary = Path(binary).resolve()
        self.port, self.workers, self.connections = map(int, (port, workers, connections))
        if not 1 <= self.port <= 65535 or min(self.workers, self.connections) <= 0:
            raise ValueError('Invalid port, workers, or connections')
        self.work = Path(work).resolve()
        self.env = os.environ.copy()
        if env is not None:
            self.env.update(env)
        self._secrets = [str(v) for k, v in self.env.items() if len(str(v)) >= 3 and
                         (k.startswith('PG') or any(s in k.upper() for s in ('PASSWORD', 'SECRET', 'TOKEN')))]
        self._gdb = None
        self._log = None
        self._reader = None
        self._lines = queue.Queue()
        self._events = deque()
        self._lock = threading.RLock()
        self._token = 0
        self._identity = None
        self._address = None
        self._paused = False
        self._exit_code = None
        self.cleanup = None

    def _redact(self, value):
        for secret in self._secrets:
            value = value.replace(secret, '<redacted>')
        return value

    def _bind_location(self):
        source = Path(__file__).resolve().parent.parent / 'src/main.cpp'
        for number, line in enumerate(source.read_text().splitlines(), 1):
            if 'server.bind(' in line:
                return str(source) + ':' + str(number)
        raise ObserverError('Cannot locate post-construction server.bind source line')

    def _read(self):
        try:
            for line in self._gdb.stdout:
                line = line.rstrip('\n')
                self._log.write(self._redact(line) + '\n')
                self._log.flush()
                self._lines.put(line)
        finally:
            self._lines.put(None)

    def _observe(self, line):
        if line.startswith('=thread-group-started,'):
            pid = _field(line, 'pid')
            if pid:
                identity = _process(int(pid))
                # GDB reports the process before its exec has necessarily completed.
                # Retain the creation identity, but require the expected executable
                # and inode before observation or sending any process signal.
                binary_stat = self.binary.stat()
                identity['executable'] = str(self.binary)
                identity['file_id'] = (binary_stat.st_dev, binary_stat.st_ino)
                self._identity = identity
        if line.startswith('*stopped,'):
            self._paused = True
            if (_field(line, 'reason') or '').startswith('exited'):
                self._paused = False
        elif line.startswith('*running,'):
            self._paused = False
        elif line.startswith('=thread-group-exited,'):
            self._exit_code = _field(line, 'exit-code')
            self._paused = False

    def _line(self, deadline):
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ObserverError('GDB/MI event timeout; see server-gdb.log')
        try:
            line = self._lines.get(timeout=remaining)
        except queue.Empty as error:
            raise ObserverError('GDB/MI event timeout; see server-gdb.log') from error
        if line is None:
            raise ObserverError('GDB exited before expected MI event; see server-gdb.log')
        self._observe(line)
        return line

    def _command(self, command, timeout=None):
        self._token += 1
        token = str(self._token)
        self._gdb.stdin.write(token + command + '\n')
        self._gdb.stdin.flush()
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        streams = []
        while True:
            line = self._line(deadline)
            if line.startswith(token + '^'):
                result = line[len(token) + 1:]
                if result.startswith('error'):
                    raise ObserverError(self._redact(_field(result, 'msg') or 'GDB command failed'))
                return result, streams
            if line.startswith(('~', '@', '&')):
                streams.append(ast.literal_eval(line[1:]))
            elif line.startswith(('*', '=')):
                self._events.append(line)

    def _stopped(self, timeout=None):
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        while True:
            line = self._events.popleft() if self._events else self._line(deadline)
            if line.startswith('*stopped,'):
                reason = _field(line, 'reason') or ''
                if reason.startswith('exited'):
                    raise ObserverError('Inferior exited before observation')
                self._paused = True
                return line

    def _owned(self):
        if self._identity is None:
            return False
        try:
            current = _process(self._identity['pid'])
        except (OSError, ValueError, IndexError):
            return False
        return current['state'] not in ('Z', 'X', 'x') and all(
            current[key] == self._identity[key] for key in ('pid', 'start_ticks', 'executable', 'file_id'))

    def _resume(self, force=False):
        if (self._paused or force) and self._owned():
            try:
                self._command('-exec-continue')
            except ObserverError as error:
                if not (force and 'running' in str(error).lower()):
                    raise
            self._paused = False

    def start(self):
        with self._lock:
            if self._gdb is not None:
                raise ObserverError('Server observer instances cannot be restarted')
            if not self.binary.is_file() or not shutil.which('gdb'):
                raise ObserverError('Server binary or GDB is unavailable')
            location = self._bind_location()
            self.work.mkdir(parents=True, exist_ok=True)
            fd = os.open(self.work / 'server-gdb.log', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            self._log = os.fdopen(fd, 'w', buffering=1)
            try:
                self._gdb = subprocess.Popen(
                    ['gdb', '--nx', '--quiet', '--interpreter=mi2', '--args', str(self.binary),
                     str(self.port), str(self.workers), str(self.connections)],
                    cwd=self.work, env=self.env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, bufsize=1)
                self._reader = threading.Thread(target=self._read, daemon=True)
                self._reader.start()
                for command in ('-gdb-set pagination off', '-gdb-set confirm off',
                                '-gdb-set mi-async on', '-gdb-set non-stop off',
                                '-gdb-set startup-with-shell off',
                                '-gdb-set may-call-functions off', '-gdb-set debuginfod enabled off'):
                    self._command(command)
                for setting in ('handle SIGINT stop print nopass',
                                'handle SIGTERM nostop noprint pass',
                                'handle SIGPIPE nostop noprint pass'):
                    self._command('-interpreter-exec console ' + json.dumps(setting))
                result, _ = self._command('-break-insert ' + json.dumps(location))
                number = re.search(r'number="([0-9]+)"', result)
                if not number:
                    raise ObserverError('Cannot establish initialization breakpoint')
                self._command('-exec-run')
                stopped = self._stopped()
                if _field(stopped, 'reason') != 'breakpoint-hit':
                    raise ObserverError('Initialization did not reach server construction breakpoint')
                if not self._owned():
                    raise ObserverError('Initialization inferior identity does not match owned binary')
                result, _ = self._command('-data-evaluate-expression ' + json.dumps('(unsigned long long)&server'))
                self._address = int(_field(result, 'value'), 0)
                self._command('-break-delete ' + number.group(1))
                self._resume()
                deadline = time.monotonic() + self.timeout
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                while time.monotonic() < deadline:
                    if not self._owned():
                        raise ObserverError('Owned server exited before health became available')
                    try:
                        with opener.open('http://127.0.0.1:%d/health' % self.port, timeout=0.5) as response:
                            if response.status == 200 and json.load(response) == {'status': 'ok'}:
                                return {key: self._identity[key] for key in ('pid', 'start_ticks', 'executable')}
                    except (OSError, ValueError):
                        pass
                    time.sleep(0.05)
                raise ObserverError('Owned server health deadline exceeded')
            except BaseException:
                self.stop()
                raise

    def snapshot(self, include_ids=False):
        with self._lock:
            if not self._owned() or self._address is None:
                raise ObserverError('Owned initialized server is unavailable')
            self._events.clear()
            started = time.monotonic()
            try:
                self._command('-exec-interrupt --all')
                self._stopped()
                expression = '((chat_server*)%d)->users_.sessions_' % self._address
                result, _ = self._command('-data-evaluate-expression ' + json.dumps(
                    '(unsigned long)' + expression + '._M_h._M_element_count'))
                online = int(_field(result, 'value'), 0)
                value = {'online': online}
                if include_ids:
                    # GDB Python only reads Values and types. may-call-functions is disabled.
                    code = '''import gdb, json
mapping = gdb.parse_and_eval(EXPRESSION)
expected = int(mapping['_M_h']['_M_element_count'])
if expected > 1000000:
    raise RuntimeError('Implausible online container size')
ids = None
printer = gdb.default_visualizer(mapping)
if printer is not None and getattr(printer, 'display_hint', lambda: None)() == 'map':
    try:
        values = []
        for index, (_, item) in enumerate(printer.children()):
            if index >= expected * 2:
                raise RuntimeError('Pretty-printer count mismatch')
            if index % 2 == 0:
                values.append(int(item))
        if len(values) == expected:
            ids = values
    except Exception:
        ids = None
if ids is None:
    table = mapping['_M_h']
    table_type = table.type.strip_typedefs()
    pair_type = table_type.template_argument(1)
    # Obtain the exact node type from the allocator base, avoiding reconstructed
    # C++ names (GDB may prefix a template argument with 'struct').
    allocator_base = next(field.type.strip_typedefs() for field in table_type.fields()
                          if field.is_base_class and '_Hashtable_alloc<' in str(field.type))
    node_type = allocator_base.template_argument(0).template_argument(0)
    node = table['_M_before_begin']['_M_nxt']
    ids, seen = [], set()
    while int(node):
        address = int(node)
        if address in seen or len(ids) >= expected:
            raise RuntimeError('Invalid unordered-map node chain')
        seen.add(address)
        typed = node.cast(node_type.pointer()).dereference()
        pair = typed['_M_storage'].address.cast(pair_type.pointer()).dereference()
        ids.append(int(pair['first']))
        node = typed['_M_nxt']
if len(ids) != expected or len(set(ids)) != expected:
    raise RuntimeError('Online IDs/count mismatch')
print('NATIVE_SERVER_OBSERVER=' + json.dumps(sorted(ids)))
'''.replace('EXPRESSION', repr(expression))
                    result, streams = self._command('-interpreter-exec console ' + json.dumps('python exec(' + repr(code) + ')'))
                    output = ''.join(streams)
                    match = re.search(r'NATIVE_SERVER_OBSERVER=(\[[^\n]*\])', output)
                    if not match:
                        raise ObserverError('GDB did not return online user IDs')
                    value['users'] = json.loads(match.group(1))
                    if len(value['users']) != online:
                        raise ObserverError('Snapshot online IDs/count mismatch')
            finally:
                # Even a timed-out interrupt may already have stopped the process.
                self._resume(force=True)
            value['pause_ms'] = round((time.monotonic() - started) * 1000, 3)
            return value

    def stop(self):
        with self._lock:
            if self.cleanup is not None:
                return self.cleanup
            forced = False
            errors = []
            try:
                self._resume()
                if self._owned():
                    os.kill(self._identity['pid'], signal.SIGTERM)
                    deadline = time.monotonic() + self.timeout
                    while self._owned() and time.monotonic() < deadline:
                        time.sleep(0.05)
                    if self._owned():
                        forced = True
                        os.kill(self._identity['pid'], signal.SIGKILL)
                        errors.append('Owned inferior ignored normal SIGTERM deadline')
            except BaseException as error:
                errors.append(self._redact(str(error)))
                if self._owned():
                    forced = True
                    os.kill(self._identity['pid'], signal.SIGKILL)
            finally:
                if self._gdb is not None and self._gdb.poll() is None:
                    try:
                        self._command('-gdb-exit', timeout=5)
                        self._gdb.wait(timeout=5)
                    except (ObserverError, OSError, subprocess.TimeoutExpired) as error:
                        errors.append(self._redact(str(error)))
                        self._gdb.terminate()
                        try:
                            self._gdb.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            self._gdb.kill()
                            self._gdb.wait(timeout=5)
                if self._reader is not None:
                    self._reader.join(timeout=5)
                if self._log is not None:
                    self._log.close()
            try:
                normal_exit = self._exit_code is not None and int(self._exit_code, 8) == 0
            except (TypeError, ValueError):
                normal_exit = False
            if not normal_exit:
                errors.append('Owned inferior did not report normal exit code 0')
            self.cleanup = {'status': 'FAIL' if forced or errors else 'PASS',
                            'forced_cleanup': forced, 'errors': errors,
                            'owned_inferior_exited': not self._owned(), 'inferior_exit_code': self._exit_code}
            return self.cleanup

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.stop()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--workers', type=int, default=32)
    parser.add_argument('--connections', type=int, default=8)
    parser.add_argument('--work', required=True)
    parser.add_argument('--include-ids', action='store_true')
    parser.add_argument('--check', action='store_true', help='Check interface/source location without starting GDB or server')
    args = parser.parse_args()
    server = Server(args.binary, args.port, args.workers, args.connections, args.work)
    if args.check:
        print(json.dumps({'binary_exists': server.binary.is_file(), 'gdb_available': bool(shutil.which('gdb')),
                          'initialization_breakpoint': server._bind_location()}))
        return 0
    try:
        print(json.dumps({'started': server.start()}))
        print(json.dumps({'snapshot': server.snapshot(args.include_ids)}))
    finally:
        print(json.dumps({'cleanup': server.stop()}))
    return 0 if server.cleanup['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
