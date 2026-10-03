"""Real-process TUI shutdown checks sharing the scale driver's existing fixture.

No protocol imitation, SQL mutations, or timing-based claim of an in-flight RPC:
a stopped server plus its socket Receive-Q proves the triggered request cannot
complete until the client has exited. The attachment case covers an active SDK
upload awaiting its begin response, with the full 10 MiB input buffer retained.
"""
from __future__ import annotations

import contextlib
import hashlib
import os
from pathlib import Path
import re
import shlex
import signal
import time


SANITIZER_FAILURE = re.compile(
    r'AddressSanitizer:|ERROR: LeakSanitizer|heap-use-after-free|stack-use-after-return|'
    r'ThreadSanitizer:|runtime error:|Resource deadlock avoided|'
    r'terminate called|Assertion .*failed|DEADLYSIGNAL'
)


def _eventually(predicate, message, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.05)
    raise AssertionError(message)


def _pane_state(d, actor):
    value = d.tmux('display-message', '-p', '-t', d.panes[actor],
                   '#{pane_dead},#{pane_dead_status},#{pane_dead_signal}').stdout.strip()
    dead, status, exit_signal = (value + ',,').split(',')[:3]
    return {'dead': dead == '1', 'status': status, 'signal': exit_signal}


def _server_receive_queue(pid, port):
    """Find the server-side TCP peer of this exact TUI process's socket."""
    inodes = set()
    for descriptor in Path(f'/proc/{pid}/fd').iterdir():
        with contextlib.suppress(FileNotFoundError):
            target = os.readlink(descriptor)
            if target.startswith('socket:['):
                inodes.add(target[8:-1])
    rows = [line.split() for line in Path(f'/proc/{pid}/net/tcp').read_text().splitlines()[1:]]
    client = next((row for row in rows if row[9] in inodes and row[3] == '01' and
                   int(row[2].split(':')[1], 16) == port), None)
    assert client is not None, 'The live TUI has no established server socket'
    server = next((row for row in rows if row[1] == client[2] and row[2] == client[1] and row[3] == '01'), None)
    assert server is not None, 'The established server-side peer is missing'
    return int(server[4].split(':')[1], 16)


def run(d):
    actor = 'B'
    repository = Path(__file__).resolve().parents[1]
    sanitized = repository / 'build' / 'asan' / 'chat_tui'
    assert sanitized.is_file(), 'Build the current ASan chat_tui before lifecycle smoke'
    regular = d.build / 'chat_tui'
    sdk_actors = [item['alias'] for item in d.manifest['sdk'] if item['alias'] != 'S100']
    d.tmux('set-window-option', '-t', d.panes[actor], 'remain-on-exit', 'on')

    def wait_offline():
        identity = d.manifest['actors'][actor]['id']
        _eventually(lambda: identity not in d.control('status', verify=True)['online_member_ids'],
                    'Server did not release the exited TUI identity')

    def stop_tui(label):
        d.keys(actor, 'C-c')
        info = _eventually(lambda: (value if (value := _pane_state(d, actor))['dead'] else None),
                          f'TUI did not exit from {label}')
        terminal = d.tmux('capture-pane', '-p', '-S', '-2000', '-t', d.panes[actor]).stdout
        assert d.password not in terminal, 'Password appeared in terminal output'
        path = d.work / f'{d.case_id}-{actor}-{label}-exit.txt'
        path.write_text(terminal)
        d.observer.capture_reference(d.case_id, path, label + ' exit')
        assert info['status'] == '0' and not info['signal'], (label, info, terminal)
        assert not SANITIZER_FAILURE.search(terminal), (label, terminal)
        d.observer.remove_process('tui_' + actor)
        d.evidence(label + '-exit', {**info, 'terminal': str(path)})
        return info

    def start_tui(binary):
        assert _pane_state(d, actor)['dead'], 'Never replace a still-running TUI'
        command = 'exec ' + shlex.quote(str(binary)) + ' ' + shlex.quote(d.url)
        d.tmux('respawn-pane', '-t', d.panes[actor], command)
        pid = int(d.tmux('display-message', '-p', '-t', d.panes[actor], '#{pane_pid}').stdout.strip())
        d.observer.set_process('tui_' + actor, pid)
        d.wait(actor, 'Login / Register')
        d.login(actor, first=True)
        d.open_main(actor)
        # An opened header alone does not prove the initial history RPC has
        # completed; PgUp is intentionally ignored while that request is busy.
        latest = d.query('S005', 'get_messages', conversation=d.group)['messages'][-1]
        marker = ('消息已删除' if latest['deleted'] else
                  latest['text'].split()[0] if latest['text'] else latest['attachment']['filename'])
        d.wait(actor, marker)
        d.barrier(actor)
        return pid

    with d.case('asan-terminal-exit',
                'Actual ASan TUI exits with status 0 in online, reconnecting, history, attachment, requests and typing states'):
        d.evidence('binary', {'path': str(sanitized),
                             'sha256': hashlib.sha256(sanitized.read_bytes()).hexdigest(),
                             'states': ['online', 'reconnecting', 'history', 'attachment', 'requests', 'typing']})
        d.open_main(actor)
        stop_tui('normal-to-asan-handoff')
        wait_offline()

        # Keep this identity exclusively in the real TUI; the SDK observer only
        # checks authoritative presence before each new process authenticates.
        pid = start_tui(sanitized)
        stop_tui('online')
        wait_offline()

        pid = start_tui(sanitized)
        d.control('close')
        d.stop_server()
        try:
            d.wait(actor, lambda text: '正在重连' in text or 'reconnecting' in text)
            stop_tui('reconnecting')
        finally:
            d.start_server()
            d.control('connect', actors=sdk_actors, timeout=120)
        wait_offline()
        for other in 'ACDE':
            d.wait(other, lambda text: 'connected' in text and '正在重连' not in text, timeout=30)

        payload = d.work / 'lifecycle-upload-10MiB.bin'
        payload.write_bytes(b'L' * (10 * 1024 * 1024))
        for state in ('history', 'attachment', 'requests'):
            pid = start_tui(sanitized)
            if state == 'requests':
                d.command(actor, 'group')
                d.wait(actor, 'Your role: admin')
                d.keys(actor, 'Escape')
            server = d.server
            try:
                server.send_signal(signal.SIGSTOP)
                _eventually(lambda: re.search(r'^State:\s+[Tt]',
                            Path(f'/proc/{server.pid}/status').read_text(), re.MULTILINE),
                            'Server did not enter the stopped state')
                initial_bytes = _server_receive_queue(pid, d.args.port)
                if state == 'history':
                    d.keys(actor, 'PPage')
                elif state == 'attachment':
                    d.command(actor, 'file ' + str(payload))
                    d.wait(actor, '正在上传附件')
                else:
                    d.command(actor, 'requests')
                    d.wait(actor, 'Join requests')
                queued = _eventually(
                    lambda: (size if (size := _server_receive_queue(pid, d.args.port)) > initial_bytes else None),
                    f'{state} did not queue an actual request on the stopped server socket')
                d.screenshot(actor, state + '-pending')
                d.evidence(state + '-pending',
                           {'tui_pid': pid, 'server_pid': server.pid, 'server_signal': 'SIGSTOP',
                            'receive_queue_before': initial_bytes, 'receive_queue_after': queued,
                            'attachment_phase': 'SDK upload awaiting begin response' if state == 'attachment' else None})
                stop_tui(state)
            finally:
                if server.poll() is None:
                    server.send_signal(signal.SIGCONT)
            wait_offline()

        pid = start_tui(sanitized)
        d.open_main('A')
        d.keys(actor, 'i')
        d.paste(actor, 'lifecycle_typing_' + d.args.run_id)
        d.wait('A', '正在输入')
        d.screenshot('A', 'typing-before-exit')
        stop_tui('typing')
        wait_offline()
        start_tui(regular)
        for other in 'ACDE':
            d.open_main(other)
        snapshot = d.control('status', verify=True)
        assert snapshot['member_count'] == snapshot['online_member_count'] == 100, snapshot
        d.evidence('restored-five-tui', snapshot)

