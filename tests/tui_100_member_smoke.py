#!/usr/bin/env python3
"""Real FTXUI/tmux acceptance driver; the SDK helper owns protocol operations.

The shell entry owns an isolated PostgreSQL database. This driver never writes
SQL, never puts passwords on a command line, and preserves evidence on failure.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import selectors
import signal
import subprocess
import sys
import time
import traceback
import urllib.request

sys.dont_write_bytecode = True
from tui_scale_observer import Observer, read_only_sql

ANSI = re.compile(r'\x1b\[([0-9;]*)m')
CSI = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')


def inverse_text(value: str) -> str:
    """Read FTXUI's highlighted cells, not pixels/OCR or application internals."""
    inverse = False
    output = []
    offset = 0
    for match in ANSI.finditer(value):
        if inverse:
            output.append(CSI.sub('', value[offset:match.start()]))
        codes = [int(item or '0') for item in match.group(1).split(';')]
        for code in codes:
            if code in (0, 27):
                inverse = False
            elif code == 7:
                inverse = True
        offset = match.end()
    if inverse:
        output.append(CSI.sub('', value[offset:]))
    return ''.join(output)


class Driver:
    def __init__(self, args):
        self.args = args
        self.work = Path(args.work_dir).resolve()
        self.build = Path(args.build_dir).resolve()
        self.work.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.url = f'ws://127.0.0.1:{args.port}/ws'
        self.password = secrets.token_urlsafe(24)
        self.socket = 'chat_scale_' + args.run_id
        self.session = 'chat_tui_100_smoke'
        self.server = None
        self.helper = None
        self.request = 0
        self.secret_tokens = set()
        self.panes = {}
        self.manifest = {}
        self.observer = Observer(self.work, {})
        self.case_id = 'setup'
        self.case_count = 0
        self.control_log = (self.work / 'sdk-control.jsonl').open('a')

    def redact(self, value):
        if isinstance(value, dict):
            for key, item in value.items():
                if 'token' in key and isinstance(item, str) and re.fullmatch('[0-9a-f]{64}', item):
                    self.secret_tokens.add(item)
            return {key: self.redact(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.redact(item) for item in value]
        if isinstance(value, str):
            result = re.sub(r'chat://join/[0-9a-f]{64}', 'chat://join/[REDACTED]', value)
            for token in self.secret_tokens:
                result = result.replace(token, '[REDACTED_INVITE_TOKEN]')
            return result.replace(self.password, '[REDACTED_PASSWORD]')
        return value

    def subprocess(self, command, **kwargs):
        return subprocess.run(command, text=True, check=True, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, **kwargs)

    def tmux(self, *args, check=True, input=None):
        return subprocess.run(['tmux', '-L', self.socket, *args], text=True,
                              check=check, stdout=subprocess.PIPE, stderr=subprocess.PIPE, input=input)

    def keys(self, actor, *keys, repeat=1):
        for key in keys:
            self.tmux('send-keys', '-t', self.panes[actor], '-N', str(repeat), key)
            if repeat > 1:
                time.sleep(.25)  # Let a batched physical-key stream reach a rendered frame.
            if key == 'Escape':
                time.sleep(.15)  # ESC+key in one burst is terminal Alt-key encoding.

    def paste(self, actor, text):
        # Includes passwords: bytes only cross stdin and the PTY, never argv.
        buffer = 'chat_scale_' + actor
        self.tmux('load-buffer', '-b', buffer, '-', input=text)
        self.tmux('paste-buffer', '-p', '-b', buffer, '-d', '-t', self.panes[actor])

    def capture(self, actor, styled=False):
        return self.tmux('capture-pane', '-p', *(['-e'] if styled else []),
                         '-t', self.panes[actor]).stdout

    def screenshot(self, actor, label):
        path = self.work / f'{self.case_id}-{actor}-{label}.txt'
        output = self.capture(actor)
        assert self.password not in output, 'Password leaked into terminal output'
        path.write_text(self.redact(output))
        self.observer.capture_reference(self.case_id, path, label)
        return path

    def wait(self, actor, condition, timeout=15, description=''):
        deadline = time.monotonic() + timeout
        last = ''
        while time.monotonic() < deadline:
            last = self.capture(actor)
            assert self.password not in last, 'Password leaked into terminal output'
            if condition(last) if callable(condition) else condition in last:
                return last
            time.sleep(.08)
        self.screenshot(actor, 'timeout')
        raise AssertionError(f'{actor}: {description or condition!r} not reached\n{self.redact(last)}')

    def wait_selected(self, actor, value, timeout=10):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if value in inverse_text(self.capture(actor, styled=True)):
                return
            time.sleep(.06)
        self.screenshot(actor, 'selection-timeout')
        raise AssertionError(f'{actor}: selected item does not contain {value!r}')

    def command(self, actor, command):
        self.keys(actor, ':')
        self.paste(actor, command)
        self.wait(actor, 'Enter: run · Esc: cancel')
        self.keys(actor, 'Enter')
        self.wait(actor, lambda screen: 'Enter: run · Esc: cancel' not in screen)

    def barrier(self, actor):
        # A visible command-input nonce acknowledges preceding PTY key events.
        # Cancel it without executing: no product test hooks or network refresh.
        nonce = 'ui_barrier_' + secrets.token_hex(4)
        self.keys(actor, ':')
        self.paste(actor, nonce)
        self.wait(actor, nonce)
        self.keys(actor, 'Escape')
        self.wait(actor, lambda screen: nonce not in screen)

    def confirm(self, actor, expected):
        self.wait(actor, lambda text: expected in text and 'Enter: confirm y' in text)
        self.paste(actor, 'y')
        self.keys(actor, 'Enter')

    def clear_input(self, actor):
        self.keys(actor, 'Home')
        self.keys(actor, 'DC', repeat=512)

    @contextlib.contextmanager
    def case(self, name, expected):
        self.case_id = name
        start = time.monotonic()
        print(f'CASE {name}: {expected}', flush=True)
        try:
            yield
        except Exception as error:
            self.observer.record(name, expected, str(error), False)
            for actor in self.panes:
                with contextlib.suppress(Exception):
                    self.screenshot(actor, 'failure')
            raise
        else:
            self.case_count += 1
            self.observer.record(name, expected, {'elapsed_seconds': time.monotonic() - start}, True)
            print(f'PASS {name}', flush=True)

    def control(self, command, timeout=90, **payload):
        self.request += 1
        request = {'id': self.request, 'command': command, **payload}
        safe = {key: value for key, value in request.items() if key != 'password'}
        self.control_log.write(json.dumps(self.redact(safe), ensure_ascii=False) + '\n')
        self.control_log.flush()
        self.helper.stdin.write(json.dumps(request, ensure_ascii=False) + '\n')
        self.helper.stdin.flush()
        selector = selectors.DefaultSelector()
        try:
            selector.register(self.helper.stdout, selectors.EVENT_READ)
            if not selector.select(timeout):
                raise TimeoutError(f'SDK fixture command {command} timed out')
            line = self.helper.stdout.readline()
        finally:
            selector.close()
        if not line:
            raise RuntimeError(f'SDK fixture exited during {command}')
        response = json.loads(line)
        assert response.get('id') == self.request, 'SDK fixture response context mismatch'
        if not response.get('ok'):
            raise RuntimeError(f'SDK {command}: {response.get("error", response)}')
        result = response.get('result')
        if command == 'sdk' and payload.get('method') in ('get_group_invite', 'create_group_invite') and result.get('ok') and result.get('value'):
            self.secret_tokens.add(result['value'])
        # Helper output contains fixture identity facts, never passwords.
        (self.work / f'sdk-{self.request:04d}-{command}.json').write_text(json.dumps(self.redact(result), ensure_ascii=False, indent=2))
        return result

    def sdk(self, actor, method, **payload):
        return self.control('sdk', actor=actor, method=method, **payload)

    def start_server(self):
        output = (self.work / 'server.log').open('ab')
        self.server = subprocess.Popen([str(self.build / 'chat_server'), str(self.args.port), '256', '16'],
                                       stdout=output, stderr=subprocess.STDOUT)
        output.close()
        self.observer.set_process('server', self.server.pid)
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.server.poll() is not None:
                raise RuntimeError('Real chat_server exited; see server.log')
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{self.args.port}/health', timeout=.5) as response:
                    if response.status == 200:
                        return
            except OSError:
                pass
            time.sleep(.1)
        raise TimeoutError('chat_server health endpoint unavailable')

    def stop_server(self):
        if self.server and self.server.poll() is None:
            self.server.send_signal(signal.SIGTERM)
            self.server.wait(timeout=15)
        self.observer.remove_process('server')
        self.server = None

    def setup(self):
        self.start_server()
        output = (self.work / 'fixture-stderr.log').open('ab')
        self.helper = subprocess.Popen([self.args.fixture_tool], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                       stderr=output, text=True, bufsize=1)
        output.close()
        self.observer.set_process('sdk_fixture', self.helper.pid)
        self.control('configure', url=self.url, prefix=self.args.run_id, password=self.password)
        self.control('seed', timeout=600, messages=150, search_matches=120, pending=59, conversations=65)
        self.manifest = self.control('manifest')
        (self.work / 'manifest.json').write_text(json.dumps(self.redact(self.manifest), ensure_ascii=False, indent=2))
        os.chmod(self.work / 'manifest.json', 0o600)
        for actor in 'ABCDE':
            self.spawn_tui(actor)

    def spawn_tui(self, actor):
        binary = str(self.build / 'chat_tui')
        # These two words are trusted harness paths/URL, not message or filename data.
        import shlex
        command = 'exec ' + shlex.quote(binary) + ' ' + shlex.quote(self.url)
        if not self.panes:
            result = self.tmux('new-session', '-d', '-P', '-F', '#{pane_id}', '-s', self.session,
                               '-n', actor, '-x', '160', '-y', '45', command)
        else:
            result = self.tmux('new-window', '-d', '-P', '-F', '#{pane_id}', '-t', self.session,
                               '-n', actor, command)
        self.panes[actor] = result.stdout.strip()
        pid = int(self.tmux('display-message', '-p', '-t', self.panes[actor], '#{pane_pid}').stdout.strip())
        self.observer.set_process('tui_' + actor, pid)
        self.wait(actor, 'Login / Register')

    def name(self, actor):
        return self.manifest['actors'][actor]['username']

    @property
    def group(self):
        return self.manifest['group']['id']

    @property
    def title(self):
        return self.manifest['group']['title']

    def login(self, actor, first=True, bad_password=False):
        self.wait(actor, 'Login / Register')
        # Password Enter submits and leaves focus in that field across logout.
        if first:
            self.keys(actor, 'Tab')
            self.paste(actor, self.name(actor))
            self.keys(actor, 'Tab')
        if bad_password:
            self.paste(actor, 'incorrect-fixture-password')
            self.keys(actor, 'Enter')
            self.wait(actor, '用户名或密码错误')
            self.screenshot(actor, 'bad-password')
        self.paste(actor, self.password)
        self.keys(actor, 'Enter')
        self.wait(actor, lambda screen: 'connected' in screen and 'Conversations' in screen, timeout=25)

    def registration_edges(self, actor):
        self.keys(actor, 'Tab')
        self.paste(actor, ' Alice')
        self.keys(actor, 'Tab')
        self.paste(actor, self.password)
        self.keys(actor, 'Tab', 'Right', 'Enter')
        self.wait(actor, '用户名须为 1–64 UTF-8 字节')
        for name in ('Alice ', ' Alice', 'Alice ', '　张三', '张三　'):
            self.keys(actor, 'Up', 'BTab')
            self.clear_input(actor)
            self.paste(actor, name)
            self.keys(actor, 'Tab', 'Tab', 'Right', 'Enter')
            self.wait(actor, '用户名须为 1–64 UTF-8 字节')
        self.screenshot(actor, 'registration-edge-rejection')
        self.keys(actor, 'Up', 'BTab')
        self.clear_input(actor)
        registered = '注册 用户_' + self.args.run_id
        self.paste(actor, registered)
        self.keys(actor, 'Tab', 'Tab', 'Right', 'Enter')
        self.wait(actor, '注册成功，请登录', timeout=25)
        rows = read_only_sql("SELECT id,username FROM users WHERE username='" + registered + "'")
        assert len(rows) == 1 and rows[0][1] == registered, rows
        self.evidence('real-tui-registration', {'user': rows[0][0], 'username': registered})
        self.screenshot(actor, 'successful-unicode-registration')
        self.keys(actor, 'Up', 'BTab')
        self.clear_input(actor)
        self.paste(actor, self.name(actor))
        self.keys(actor, 'Tab')
        self.clear_input(actor)

    def logout(self, actor):
        self.keys(actor, 'Escape')
        self.command(actor, 'logout')
        self.confirm(actor, '退出当前账号')
        self.wait(actor, 'Login / Register')

    def open_main(self, actor):
        self.keys(actor, 'Escape')
        self.command(actor, 'conversations')
        self.wait(actor, 'Conversations')
        self.keys(actor, 'k', repeat=200)
        self.barrier(actor)
        marker = self.title[:5]
        for _ in range(140):
            if marker in inverse_text(self.capture(actor, styled=True)):
                time.sleep(.1)
                if marker not in inverse_text(self.capture(actor, styled=True)):
                    continue
                self.keys(actor, 'Enter')
                self.wait(actor, lambda screen: self.title in screen and 'members' in screen)
                # Header metadata can precede the independent member snapshot.
                # Wait for that observable snapshot before management actions.
                self.command(actor, 'group')
                self.wait(actor, lambda screen: re.search(r'Members: [1-9][0-9]*', screen) is not None)
                self.keys(actor, 'Escape')
                self.wait(actor, 'i: compose')
                return
            self.keys(actor, 'j')
            time.sleep(.045)
        raise AssertionError(f'{actor}: main conversation not accessible via list pagination')

    def choose_member(self, actor, target):
        self.command(actor, 'members')
        self.wait(actor, 'Members (')
        self.keys(actor, 'k', repeat=160)
        self.barrier(actor)
        for _ in range(130):
            if self.name(target) in inverse_text(self.capture(actor, styled=True)):
                return
            self.keys(actor, 'j')
            time.sleep(.045)
        raise AssertionError(f'{actor}: member {target} not reachable')

    def find_profile(self, actor, target):
        self.command(actor, 'search-users ' + self.name(target))
        self.wait(actor, lambda screen: 'User search' in screen and self.name(target) in screen and 'No items' not in screen)
        self.keys(actor, 'Enter')
        self.wait(actor, 'Profile ·')

    def request_friend(self, actor, target):
        self.find_profile(actor, target)
        self.wait(actor, 'Add friend')
        self.command(actor, 'add')
        self.wait(actor, 'Waiting for acceptance')
        output = self.capture(actor)
        assert 'Message' not in output and 'online' not in output
        self.screenshot(actor, 'outgoing-pending')

    def accept_friend(self, actor, target):
        self.find_profile(actor, target)
        self.wait(actor, 'Accept friend request')
        self.command(actor, 'accept-friend')
        self.wait(actor, 'Remove friend')

    def add_contact(self, actor, target):
        self.find_profile(actor, target)
        if 'Remove friend' in self.capture(actor):
            return
        self.request_friend(actor, target)
        self.accept_friend(target, actor)
        self.wait(actor, 'Remove friend')

    def remove_contact(self, actor, target):
        self.find_profile(actor, target)
        self.wait(actor, 'Remove friend')
        self.command(actor, 'remove-contact')
        self.confirm(actor, '删除好友')
        self.wait(actor, 'Add friend')

    def open_named(self, actor, title):
        self.keys(actor, 'Escape')
        self.command(actor, 'conversations')
        self.wait(actor, 'Conversations')
        self.keys(actor, 'k', repeat=200)
        self.barrier(actor)
        for _ in range(200):
            if title[:12] in inverse_text(self.capture(actor, styled=True)):
                time.sleep(.1)
                if title[:12] not in inverse_text(self.capture(actor, styled=True)):
                    continue
                self.keys(actor, 'Enter')
                self.wait(actor, title)
                return
            self.keys(actor, 'j')
            time.sleep(.04)
        raise AssertionError(f'{actor}: conversation {title!r} missing')

    def selected_message_lines(self, actor):
        # In a wide layout the conversation list is independently highlighted.
        # Restrict message selection evidence to the right pane, and preserve
        # physical row boundaries so a reply quote cannot masquerade as its body.
        width = int(self.tmux('display-message', '-p', '-t', self.panes[actor], '#{pane_width}').stdout)
        lines = []
        for line in self.capture(actor, styled=True).splitlines():
            parts = line.split('│', 2 if width >= 100 else 1)
            if len(parts) == (3 if width >= 100 else 2):
                value = inverse_text(parts[-1]).strip()
                if value and not value.startswith('↪'):
                    lines.append(value)
        return lines

    def selected_message(self, actor, marker, older=False):
        if older:
            self.keys(actor, 'PPage')
        self.keys(actor, 'G')
        self.barrier(actor)
        for _ in range(180):
            if any(marker in line for line in self.selected_message_lines(actor)):
                return
            self.keys(actor, 'k')
            time.sleep(.025)
        raise AssertionError(f'{actor}: message selection {marker!r} unavailable')

    def resize(self, actor, width, height):
        self.tmux('resize-window', '-t', self.panes[actor], '-x', str(width), '-y', str(height))
        self.wait(actor, (lambda text: 'Terminal too small' in text) if width < 40 or height < 12 else
                  (lambda text: 'Chat ·' in text and 'Terminal too small' not in text))

    def evidence(self, label, value):
        path = self.work / (self.case_id + '-' + label + '.json')
        path.write_text(json.dumps(self.redact(value), ensure_ascii=False, indent=2))
        self.observer.capture_reference(self.case_id, path, label)

    def query(self, actor, method, **payload):
        result = self.sdk(actor, method, **payload)
        assert result['ok'], f'{method}: {result.get("error")}'
        return result['value']

    def rejected(self, actor, method, **payload):
        result = self.sdk(actor, method, **payload)
        assert not result['ok'], f'{method} unexpectedly succeeded'
        return result

    def fixtures(self):
        # Reuse the repository's valid tiny PNG/JPEG, rather than add an image library.
        header = (Path(__file__).parent / 'avatar_fixture.hpp').read_text()
        files = {}
        for media in ('png', 'jpeg'):
            source = re.search(r'avatar_' + media + r'_base64\s*=\s*(.*?);', header, re.S).group(1)
            encoded = ''.join(re.findall(r'"([^"\n]*)"', source))
            path = self.work / ('图像_' + media + '.' + media)
            path.write_bytes(base64.b64decode(encoded))
            files[media] = path
        files['text'] = self.work / '中文 文本.txt'
        files['text'].write_text('ASCII 中文 😀 terminal attachment\n')
        files['binary'] = self.work / 'binary.dat'
        files['binary'].write_bytes(bytes(range(256)) * 2048)
        files['boundary'] = self.work / 'boundary.bin'
        files['boundary'].write_bytes(b'X' * (10 * 1024 * 1024 - 1))
        return files

    def open_direct(self, actor, target):
        self.find_profile(actor, target)
        self.command(actor, 'message')
        self.wait(actor, lambda screen: self.name(target) in screen and 'i: compose' in screen)

    def send(self, actor, marker):
        self.keys(actor, 'i')
        self.clear_input(actor)
        self.paste(actor, marker)
        self.keys(actor, 'Enter', 'Escape')
        # Wait for the draft to clear as well as the delivered body. A stale
        # success status or text still in the composer is not delivery evidence.
        visible_marker = marker.split(' ', 1)[0]
        self.wait(actor, lambda screen: visible_marker in screen and '消息已发送' in screen and
                  any('i: compose' in line and visible_marker[:18] not in line for line in screen.splitlines()))
        return marker

    def link(self, actor, create=False):
        self.command(actor, 'link-create' if create else 'link')
        self.wait(actor, 'Copyable text')
        output = self.wait(actor, lambda screen: re.search(r'chat://join/[0-9a-f]{64}', screen) is not None)
        token = re.search(r'chat://join/([0-9a-f]{64})', output).group(1)
        self.secret_tokens.add(token)
        self.keys(actor, 'Escape')
        return token

    def close(self):
        with contextlib.suppress(Exception):
            self.tmux('kill-server', check=False)
        if self.helper:
            if self.helper.poll() is None:
                with contextlib.suppress(Exception):
                    self.control('close', timeout=15)
                with contextlib.suppress(Exception):
                    self.helper.stdin.close()
                with contextlib.suppress(Exception):
                    self.helper.wait(timeout=10)
                if self.helper.poll() is None:
                    self.helper.terminate()
                    self.helper.wait(timeout=10)
            self.helper = None
        with contextlib.suppress(Exception):
            self.stop_server()
        self.control_log.close()


def choose_row(d, actor, marker, limit=180, pages=False):
    d.keys(actor, 'k', repeat=250)
    d.barrier(actor)
    for index in range(limit):
        if marker[:26] in inverse_text(d.capture(actor, styled=True)):
            return
        d.keys(actor, 'j')
        if pages and index and index % 45 == 0:
            d.keys(actor, 'NPage')
        time.sleep(.035)
    raise AssertionError(f'{actor}: row {marker!r} not reachable')


def message_id(d, marker):
    values = d.query('S005', 'search_messages', conversation=d.group, query=marker)
    messages = values['messages']
    assert len(messages) == 1, f'Unique fixture marker {marker!r} must resolve once'
    return messages[0]['id']


def stage_friendships(d):
    with d.case('friend-requests-incoming', 'Five real UI incoming friend requests can be rejected, without presence/direct permission'):
        d.command('A', 'friend-requests')
        d.wait('A', 'Incoming')
        for fixture in d.manifest['friend_pending']['incoming']:
            choose_row(d, 'A', fixture['username'])
            d.keys('A', 'Enter')
            d.wait('A', 'Incoming friend request')
            assert 'Message' not in d.capture('A')
            d.command('A', 'reject-friend')
            d.wait('A', 'Add friend')
            d.command('A', 'friend-requests')
        d.screenshot('A', 'rejected-five')
    with d.case('friend-requests-outgoing', 'Five real UI outgoing requests remain pending until explicitly cancelled'):
        d.command('A', 'friend-sent')
        d.wait('A', 'Outgoing')
        for fixture in d.manifest['friend_pending']['outgoing']:
            choose_row(d, 'A', fixture['username'])
            d.keys('A', 'Enter')
            d.wait('A', 'Waiting for acceptance')
            assert 'Message' not in d.capture('A')
            d.command('A', 'cancel-friend')
            d.wait('A', 'Add friend')
            d.command('A', 'friend-sent')
        d.screenshot('A', 'cancelled-five')
    with d.case('friend-from-group-profile', 'Nonfriend group profile exposes request; explicit recipient acceptance enables both direct directions'):
        d.open_main('B')
        d.choose_member('B', 'C')
        d.keys('B', 'Enter')
        d.wait('B', 'Add friend')
        assert 'Message' not in d.capture('B') and 'online' not in d.capture('B')
        d.command('B', 'add')
        d.wait('B', 'Waiting for acceptance')
        d.command('B', 'message')
        d.wait('B', '双方接受好友申请后可发送消息')
        d.find_profile('C', 'B')
        d.wait('C', 'Incoming friend request')
        assert 'Message' not in d.capture('C')
        d.command('C', 'accept-friend')
        d.wait('C', 'Remove friend')
        d.wait('B', 'Remove friend')
        d.screenshot('B', 'accepted-profile')
        d.open_direct('B', 'C')
        d.open_direct('C', 'B')
        d.send('B', 'friend_ASCII_' + d.args.run_id)
        d.wait('C', 'friend_ASCII_' + d.args.run_id)
        d.send('C', '好友中文😀_' + d.args.run_id)
        d.wait('B', '好友中文😀_' + d.args.run_id)
        d.screenshot('B', 'bilateral-messages')
    with d.case('ten-friend-pairs', 'Ten explicitly accepted SDK friend pairs exchange text in both directions; owner still has exactly 20 friends'):
        result = d.control('exercise_friend_pairs')
        assert result['pair_count'] >= 10 and result['directions'] >= 20
        assert len(d.manifest['owner_friends']) == 20
        d.evidence('pair-exchanges', result)


def stage_onboard(d):
    with d.case('join-approval-sixty', 'Outsider E submits real UI link request; sixty pending requests paginate without granting history'):
        d.open_main('A')
        token = d.link('A')
        d.command('E', 'join chat://join/' + token)
        d.wait('E', '等待管理员审批')
        assert d.title not in d.capture('E')
        d.command('A', 'requests')
        d.wait('A', 'Join requests')
        choose_row(d, 'A', d.name('E'), pages=True)
        d.screenshot('A', 'e-pending')
        requests = d.query('S005', 'get_group_join_requests', conversation=d.group)
        count = len(requests['requests'])
        while requests.get('next'):
            requests = d.query('S005', 'get_group_join_requests', conversation=d.group, before=requests['next'])
            count += len(requests['requests'])
        assert count == 60, f'Expected 60 pending requests, got {count}'
        d.keys('A', 'NPage')
        d.screenshot('A', 'paginated-requests')
    with d.case('link-revoke-preserves-pending', 'Revoking invitation leaves pending requests intact; old link rejected and replacement link differs'):
        d.open_main('A')
        d.command('A', 'link-revoke')
        d.confirm('A', '撤销群邀请链接')
        d.wait('A', '邀请链接已撤销')
        d.command('E', 'join ' + token)
        d.wait('E', lambda text: '无效' in text or 'invalid' in text.lower() or 'Invite unavailable' in text)
        replacement = d.link('A', create=True)
        assert replacement != token
        d.manifest['invite_token'] = replacement
        requests = d.query('S005', 'get_group_join_requests', conversation=d.group)
        assert requests['requests'], 'Revocation must preserve pending requests'
    with d.case('hundred-current-members', 'Reserved SDK member leaves; real owner accepts E, restoring exactly 100 members and 100 authenticated clients'):
        d.control('replace-reserved')
        d.command('A', 'requests')
        d.wait('A', 'Join requests')
        choose_row(d, 'A', d.name('E'), pages=True)
        d.keys('A', 'y')
        d.wait('A', '入群申请已处理')
        d.open_main('E')
        d.command('E', 'members')
        d.wait('E', 'Members (100)')
        d.screenshot('E', 'accepted-hundred')
        status = d.control('status', verify=True)
        assert status['member_count'] == 100 and status['online_member_count'] == 100, status
        d.evidence('hundred-online', status)
        people = d.query('S005', 'get_members', conversation=d.group)
        assert len(people) == 100
        assert [value['role'] for value in people].count('owner') == 1
        assert [value['role'] for value in people].count('admin') == 3
        d.command('E', 'members')
        d.wait('E', 'Members (100)')
        d.keys('E', 'k', repeat=150)
        d.barrier('E')
        last = 0
        for index in (0,50,99):
            if index > last:
                d.keys('E', 'j', repeat=index-last)
                d.barrier('E')
            d.wait_selected('E', people[index]['username'])
            assert people[index]['role'] in inverse_text(d.capture('E', styled=True))
            d.screenshot('E', 'members-position-' + str(index))
            last = index
        d.evidence('member-role-order', people)

        d.evidence('database-identity', read_only_sql('SELECT current_database(), count(*) FROM users'))
        d.command('A', 'requests')
        choose_row(d, 'A', d.manifest['pending'][0]['username'], pages=True)
        d.keys('A', 'n')
        d.wait('A', '入群申请已处理')


def stage_messages(d):
    with d.case('history-search-pagination', 'Real TUI loads earlier history and more than one page of matching search results'):
        for method, expected in [('get_messages', d.manifest['messages']),
                                 ('search_messages', d.manifest['messages'][:120])]:
            cursor = None
            collected = []
            pages = []
            while True:
                arguments = {'conversation': d.group}
                if cursor is not None:
                    arguments['before'] = cursor
                if method == 'search_messages':
                    arguments['query'] = d.manifest['search_query']
                result = d.query('S005', method, **arguments)
                ids = [value['id'] for value in result['messages']]
                assert ids == sorted(ids) and len(ids) == len(set(ids)), 'SDK page IDs must be unique/ascending'
                if cursor is not None and ids:
                    assert max(ids) < cursor, 'Older cursor cannot repeat or move forwards'
                assert not set(collected).intersection(ids)
                collected.extend(ids)
                pages.append(ids)
                if not result['has_more']:
                    break
                assert ids
                cursor = min(ids)
            assert sorted(collected) == sorted(value['id'] for value in expected)
            d.evidence(method + '-all-page-ids', pages)
        d.open_main('C')
        d.keys('C', 'PPage')
        time.sleep(.15)
        d.keys('C', 'PPage')
        time.sleep(.15)
        d.keys('C', 'k', repeat=220)
        d.screenshot('C', 'oldest-history')
        assert d.manifest['messages'][0]['marker'] + ' 中文群消息' in d.capture('C')
        d.command('C', 'search ' + d.manifest['search_query'])
        d.wait('C', d.manifest['messages'][119]['marker'] + ' 中文群消息')
        for boundary in (69,19):
            d.keys('C', 'j', repeat=160)
            d.barrier('C')
            d.wait('C', 'earlier results')
            d.keys('C', 'NPage')
            d.wait('C', d.manifest['messages'][boundary]['marker'] + ' 中文群消息')
        d.keys('C', 'j', repeat=140)
        d.barrier('C')
        d.keys('C', 'Enter')
        d.wait('C', 'Copyable text')
        d.wait('C', d.manifest['messages'][0]['marker'] + ' 中文群消息')
        d.screenshot('C', 'last-search-result')
        history = d.query('S005', 'get_messages', conversation=d.group)
        assert history['has_more']
    with d.case('five-tui-group-delivery', 'All five real TUIs display each other’s ASCII, Chinese, emoji and Unicode mention messages'):
        for actor in 'ABCDE':
            d.open_main(actor)
        for actor in 'ABCDE':
            marker = f'{actor}_中文😀_{d.args.run_id}'
            d.send(actor, marker + ' @' + d.name('C'))
            for target in 'ABCDE':
                d.keys(target, 'G')
                d.wait(target, marker)
            d.screenshot(actor, 'sent-mention')
    with d.case('reply-edit-reaction-delete', 'Actual TUI reply/edit/delete and add/replace/clear reaction round trips remain visible to peers'):
        marker = 'reply_target_' + d.args.run_id
        d.send('B', marker)
        d.wait('C', marker)
        d.selected_message('C', marker)
        d.keys('C', 'r')
        d.wait('C', 'Reply ')
        d.paste('C', 'reply_body_' + d.args.run_id)
        d.keys('C', 'Enter', 'Escape')
        d.wait('B', 'reply_body_' + d.args.run_id)
        d.selected_message('C', 'reply_body_' + d.args.run_id)
        d.keys('C', 'e')
        d.clear_input('C')
        d.paste('C', 'edited_reply_' + d.args.run_id)
        d.keys('C', 'Enter', 'Escape')
        d.wait('B', 'edited_reply_' + d.args.run_id)
        for choice, emoji in ((1, '👍'), (2, '❤️'), (0, None)):
            d.selected_message('A', marker)
            d.command('A', 'reaction ' + str(choice))
            if emoji:
                d.wait('A', emoji)
                d.wait('B', emoji)
            else:
                d.wait('A', lambda text: '❤️' not in text and '👍' not in text)
                values = d.query('S005', 'search_messages', conversation=d.group, query=marker)
                assert not values['messages'][0]['reactions'], values['messages'][0]
        # A second reply is left unsent while the reference is removed.
        d.selected_message('D', marker)
        d.keys('D', 'r')
        d.paste('D', 'draft_survives_' + d.args.run_id)
        d.selected_message('B', marker)
        d.keys('B', 'd')
        d.confirm('B', '删除')
        d.wait('D', lambda text: 'draft_survives_' + d.args.run_id in text and 'Reply ' not in text)
        d.screenshot('D', 'deleted-reply-preserves-draft')
        d.keys('D', 'Enter', 'Escape')
        d.wait('C', 'draft_survives_' + d.args.run_id)
        sent_draft = d.query('S005', 'search_messages', conversation=d.group, query='draft_survives_' + d.args.run_id)['messages']
        assert len(sent_draft) == 1 and not sent_draft[0].get('reply'), sent_draft
        d.evidence('deleted-reply-draft-sent-as-normal', sent_draft)
        d.selected_message('C', 'edited_reply_' + d.args.run_id)
        d.wait('C', lambda text: '↪' in inverse_text(d.capture('C', styled=True)) and '消息已删除' in inverse_text(d.capture('C', styled=True)) and marker not in inverse_text(d.capture('C', styled=True)))
        reply = d.query('S005', 'search_messages', conversation=d.group, query='edited_reply_' + d.args.run_id)['messages']
        assert len(reply) == 1 and reply[0]['reply']['deleted'] and not reply[0]['deleted'], reply
        d.evidence('historical-quote-deleted-target', reply[0])
        d.screenshot('C', 'historical-quote-tombstone')
    with d.case('typing-and-read', 'Real text editing announces typing; Esc clears it; group read counts advance 0/1/10/50/99'):
        for actor in 'BCDE':
            d.command(actor, 'help')
            d.wait(actor, 'Keyboard help')
        d.open_main('A')
        marker = 'read_probe_' + d.args.run_id
        d.send('A', marker)
        identity = message_id(d, marker)
        d.wait('A', '已读 0 人')
        d.screenshot('A', 'read-0')
        initial = d.control('read_snapshot', message=identity)
        assert initial['count'] == 0, initial
        for count in (1, 10, 50):
            snapshot = d.control('read', message=identity, target_count=count)
            assert snapshot['count'] == count, snapshot
            d.evidence('read-snapshot-' + str(count), snapshot)
            d.wait('A', f'已读 {count} 人')
            d.screenshot('A', f'read-{count}')
        d.control('read', message=identity, target_count=99)
        for actor in 'BCDE':
            d.open_main(actor)
            d.keys(actor, 'G')
        d.wait('A', '已读 99 人')
        snapshot = d.control('read_snapshot', message=identity)
        assert snapshot['count'] == 99, snapshot
        d.evidence('read-final', snapshot)
        d.keys('B', 'i')
        d.paste('B', 'typing_probe')
        d.wait('A', '正在输入')
        d.screenshot('A', 'typing-active')
        d.keys('B', 'Escape')
        d.wait('A', lambda text: '正在输入' not in text)


def stage_terminal_messages(d):
    with d.case('unicode-multiple-mentions', 'One real TUI message mentions multiple Unicode, spaced and punctuation usernames; server excludes a nonmember'):
        d.open_main('A')
        people = [d.manifest['actors'][key] for key in ('B','C','E')]
        people += [next(value for value in d.manifest['sdk'] if value['alias'] == alias) for alias in ('S010','S011')]
        outsider = d.manifest['pending'][-1]
        assert '.' in people[3]['username'] and '-' in people[3]['username'] and '_' in people[3]['username']
        marker = 'multiple_mentions_' + d.args.run_id
        body = marker + ' ' + ' '.join('@' + person['username'] for person in people + [outsider])
        d.send('A', body)
        result = d.query('S005', 'search_messages', conversation=d.group, query=marker)['messages'][0]
        assert {value['user'] for value in result['mentions']} == {person['id'] for person in people}
        assert outsider['id'] not in {value['user'] for value in result['mentions']}
        d.evidence('authoritative-mention-ids', result)
        d.screenshot('A', 'multiple-mentions')
    with d.case('terminal-controls-and-60k-multiline', 'TUI renders a 60-KiB multiline message safely; head/tail and conversation navigation remain accessible'):
        start = 'ANSI_HEAD_' + d.args.run_id + '\x1b[2J\x1b]52;c;dW50cnVzdGVk\a\n'
        tail = '\nBOUNDARY_TAIL_' + d.args.run_id
        budget = 61440 - len(start.encode()) - len(tail.encode())
        body = start + ('line 中文 😀 ' + 'x'*45 + '\n') * (budget//64)
        while len(body.encode()) + len(tail.encode()) > 61440:
            body = body[:-1]
        body += 'x' * (61440-len(body.encode())-len(tail.encode())) + tail
        assert len(body.encode()) == 61440
        assert len(json.dumps(body, ensure_ascii=False).encode()) < 63 * 1024
        sent = d.query('S005', 'send_message', conversation=d.group, text=body)
        stored = d.query('S005', 'search_messages', conversation=d.group, query='ANSI_HEAD_' + d.args.run_id)['messages'][0]
        assert stored['text'] == body and '\x1b' in stored['text'] and '\a' in stored['text']
        d.evidence('stored-control-message', {'message': sent['message_id'], 'utf8_bytes': len(body.encode()),
                                            'sha256': hashlib.sha256(body.encode()).hexdigest(),
                                            'esc_count': body.count('\x1b'), 'bel_count': body.count('\a')})
        d.open_main('C')
        d.keys('C', 'G')
        d.wait('C', 'BOUNDARY_TAIL_' + d.args.run_id)
        d.command('C', 'copy')
        d.wait('C', 'ANSI_HEAD_' + d.args.run_id)
        d.wait('C', '[2J]52;c;dW50cnVzdGVk')
        d.screenshot('C', 'literal-control-text-head')
        for _ in range(60):
            d.keys('C', 'j', repeat=40)
            d.barrier('C')
            if 'BOUNDARY_TAIL_' + d.args.run_id in d.capture('C'):
                break
        d.wait('C', 'BOUNDARY_TAIL_' + d.args.run_id)
        d.screenshot('C', '60k-tail')
        d.keys('C', 'Escape')
        d.resize('C', 70,20)
        d.command('C', 'conversations')
        d.wait('C', d.title)
        d.screenshot('C', 'multiline-conversation-row')
        d.keys('C', 'Enter')
        d.wait('C', 'i: compose')
        d.resize('C', 160,45)
        d.query('S005', 'delete_message', conversation=d.group, message=sent['message_id'])


def stage_large_text(d):
    with d.case('real-tui-61440-byte-single-line', 'Real TUI paste and Enter sends exactly 61440 UTF-8 bytes; peer copy view and SDK prove no truncation or duplicates'):
        for actor in 'AB':
            d.open_main(actor)
        head = 'TUI_HEAD_' + d.args.run_id + ' 中文😀 '
        tail = ' 中文😀 TUI_TAIL_' + d.args.run_id
        body = head + 'x' * (61440 - len(head.encode()) - len(tail.encode())) + tail
        assert len(body.encode()) == 61440 and '\n' not in body
        d.keys('A', 'i')
        d.clear_input('A')
        started = time.monotonic()
        d.paste('A', body)
        # A large bracketed paste takes real input/render time; do not race Enter.
        d.wait('A', 'TUI_TAIL_' + d.args.run_id, timeout=60)
        ready = time.monotonic()
        d.screenshot('A', 'full-input-tail')
        d.keys('A', 'Enter')
        d.wait('A', '消息已发送', timeout=60)
        d.keys('A', 'Escape')
        d.keys('B', 'G')
        d.wait('B', 'TUI_TAIL_' + d.args.run_id, timeout=60)
        peer_visible = time.monotonic()
        d.screenshot('B', 'received-tail')
        d.command('B', 'copy')
        d.wait('B', 'TUI_HEAD_' + d.args.run_id)
        d.screenshot('B', 'copy-head')
        for _ in range(60):
            d.keys('B', 'j', repeat=40)
            d.barrier('B')
            if 'TUI_TAIL_' + d.args.run_id in d.capture('B'):
                break
        d.wait('B', 'TUI_TAIL_' + d.args.run_id)
        d.screenshot('B', 'copy-tail')
        result = d.query('S005', 'search_messages', conversation=d.group, query='TUI_HEAD_' + d.args.run_id)
        assert len(result['messages']) == 1 and not result['has_more']
        message = result['messages'][0]
        assert message['text'] == body
        d.evidence('boundary-summary', {
            'message': message['id'], 'utf8_bytes': len(body.encode()), 'matching_messages': 1,
            'expected_sha256': hashlib.sha256(body.encode()).hexdigest(),
            'actual_sha256': hashlib.sha256(message['text'].encode()).hexdigest(),
            'paste_to_input_tail_ms': (ready-started)*1000,
            'paste_to_peer_visible_ms': (peer_visible-started)*1000,
            'input_ready_to_peer_visible_ms': (peer_visible-ready)*1000,
            'measurement': 'Actual paste/render/polling and Enter until peer tail is visible; not callback or network-only latency. Single-line input, not a multiline editor.'})
        d.keys('B', 'Escape')


def check_rejected_avatar(d, actor, path):
    def metadata():
        return next(user['avatar'] for user in d.query('S005', 'search_users', query=d.name(actor))
                    if user['username'] == d.name(actor))
    before = metadata()
    assert before['present'], 'Rejection must preserve a previously valid avatar'
    d.command(actor, 'avatar set ' + str(path))
    d.wait(actor, lambda text: 'invalid' in text.lower() or '无效' in text or '参数' in text)
    after = metadata()
    assert after == before, (before, after)
    d.evidence(path.stem + '-rejection', {'bytes': path.stat().st_size, 'before': before, 'after': after})
    d.screenshot(actor, path.stem + '-rejected')


def stage_files(d):
    files = d.fixtures()
    with d.case('attachments-five-types', 'Five real TUI uploads/downloads (text, binary, PNG, JPEG, near 10 MiB), exact SHA256, no silent overwrite'):
        d.open_main('A')
        d.open_main('C')
        hashes = []
        for name, source in files.items():
            d.command('A', 'file ' + str(source))
            d.wait('A', '附件已发送', timeout=40)
            d.wait('C', source.name, timeout=40)
            d.selected_message('C', source.name)
            target = d.work / ('saved-' + source.name)
            d.command('C', 'save ' + str(target))
            d.wait('C', '已保存', timeout=40)
            assert target.exists()
            digest = hashlib.sha256(source.read_bytes()).hexdigest()
            assert hashlib.sha256(target.read_bytes()).hexdigest() == digest
            hashes.append({'type': name, 'bytes': source.stat().st_size, 'sha256': digest})
            d.command('C', 'save ' + str(target))
            d.wait('C', lambda text: '已存在' in text or 'File exists' in text or 'exists' in text, timeout=40)
            assert hashlib.sha256(target.read_bytes()).hexdigest() == digest
            d.screenshot('C', 'download-' + name)
        d.evidence('attachment-hashes', hashes)
    with d.case('avatar-profile', 'Actual account UI sets PNG, replaces JPEG, rejects truncated/oversize image, clears, and never displays inline graphics'):
        d.command('D', 'account')
        revision = 0
        for media in ('png', 'jpeg'):
            d.command('D', 'avatar set ' + str(files[media]))
            d.wait('D', '头像已更新')
            d.wait('D', 'Avatar: set')
            deadline = time.monotonic() + 10
            while True:
                users = d.query('S005', 'search_users', query=d.name('D'))
                profile = next(user for user in users if user['username'] == d.name('D'))
                if profile['avatar']['present'] and profile['avatar']['revision'] > revision:
                    revision = profile['avatar']['revision']
                    break
                assert time.monotonic() < deadline, 'New avatar revision not visible through SDK'
                time.sleep(.1)
            d.evidence('avatar-' + media + '-revision', profile)
            d.screenshot('D', 'avatar-' + media)
        for media in ('png', 'jpeg'):
            truncated = d.work / ('truncated-' + media + '.' + media)
            truncated.write_bytes(files[media].read_bytes()[:12])
            check_rejected_avatar(d, 'D', truncated)
        large = d.work / 'oversize.png'
        large.write_bytes(files['png'].read_bytes() + bytes(1024 * 1024))
        d.command('D', 'avatar set ' + str(large))
        d.wait('D', lambda text: '超过' in text or '大' in text or 'limit' in text.lower())
        d.command('D', 'avatar-clear')
        d.wait('D', 'Avatar: default')
        d.screenshot('D', 'cleared-avatar')
    return files

def stage_friend_direct(d, files):
    with d.case('accepted-friend-direct-actions', 'Two real TUI friends exchange ASCII/Chinese/emoji, reply/edit/delete/reaction/typing/read/search in both direct directions'):
        d.open_direct('B', 'C')
        d.open_direct('C', 'B')
        for sender, recipient in [('B', 'C'), ('C', 'B')]:
            marker = f'direct_{sender}_{d.args.run_id}'
            d.send(sender, marker + ' ASCII 中文 😀')
            d.wait(recipient, marker)
            d.selected_message(recipient, marker)
            d.keys(recipient, 'r')
            d.wait(recipient, 'Reply ')
            reply = marker + '_reply'
            d.paste(recipient, reply)
            d.keys(recipient, 'Enter', 'Escape')
            d.wait(sender, reply)
            d.selected_message(recipient, reply)
            d.keys(recipient, 'e')
            d.clear_input(recipient)
            edited = reply + '_edited'
            d.paste(recipient, edited)
            d.keys(recipient, 'Enter', 'Escape')
            d.wait(sender, edited)
            d.selected_message(sender, edited)
            for choice, emoji in ((1, '👍'), (2, '❤️')):
                d.command(sender, 'reaction ' + str(choice))
                d.wait(sender, emoji)
                d.wait(recipient, emoji)
            d.command(sender, 'reaction 0')
            d.wait(sender, lambda text: '👍' not in text and '❤️' not in text)
            d.keys(recipient, 'i')
            d.paste(recipient, 'typing_' + marker)
            d.wait(sender, '正在输入')
            d.screenshot(sender, 'direct-typing-' + sender)
            d.keys(recipient, 'Escape')
            d.wait(sender, lambda text: '正在输入' not in text)
            # Remove the unsent typing probe before the next compose action.
            d.keys(recipient, 'i')
            d.clear_input(recipient)
            d.keys(recipient, 'Escape')
            d.command(sender, 'search ' + edited)
            d.wait(sender, edited)
            d.screenshot(sender, 'direct-search-' + sender)
            d.keys(sender, 'Escape')
            d.keys(recipient, 'G')
            d.command(recipient, 'read')
            d.keys(sender, 'G')
            d.screenshot(sender, 'direct-read-' + sender)
            d.selected_message(recipient, edited)
            d.keys(recipient, 'd')
            d.confirm(recipient, '删除这条消息')
            d.wait(sender, '消息已删除')
            d.screenshot(sender, 'direct-delete-' + sender)
            d.command(recipient, 'help')
            d.wait(recipient, 'Keyboard help')
            unread_marker = marker + '_unread'
            d.send(sender, unread_marker)
            recipient_id = int(d.manifest['actors'][recipient]['id'])
            sql = ("SELECT m.id, cm.last_read_message_id FROM messages m "
                   "JOIN conversation_members cm ON cm.conversation_id=m.conversation_id "
                   f"WHERE m.body='{unread_marker}' AND cm.user_id={recipient_id}")
            before = read_only_sql(sql)
            assert len(before) == 1 and int(before[0][1]) < int(before[0][0]), before
            d.command(recipient, 'conversations')
            d.screenshot(recipient, 'direct-unread-' + recipient)
            d.open_direct(recipient, sender)
            d.keys(recipient, 'G')
            d.wait(sender, '✓✓')
            deadline = time.monotonic() + 10
            while True:
                after = read_only_sql(sql)
                if len(after) == 1 and int(after[0][1]) >= int(after[0][0]):
                    break
                assert time.monotonic() < deadline, after
                time.sleep(.05)
            d.evidence('direct-read-watermark-' + sender, {'before': before, 'after': after})
            d.screenshot(sender, 'direct-read-receipt-' + sender)
    with d.case('accepted-friend-direct-files', 'Both real TUI directions send/save text, PNG and JPEG with matching SHA256 and Unicode/space filenames'):
        hashes = []
        for sender, recipient in [('B', 'C'), ('C', 'B')]:
            d.open_direct(sender, recipient)
            d.open_direct(recipient, sender)
            for kind in ('text', 'png', 'jpeg'):
                source = files[kind]
                d.command(sender, 'file ' + str(source))
                d.wait(sender, '附件已发送', timeout=40)
                d.wait(recipient, source.name, timeout=40)
                d.selected_message(recipient, source.name)
                target = d.work / (f'direct-{sender}-{recipient}-' + source.name)
                d.command(recipient, 'save ' + str(target))
                d.wait(recipient, lambda text: target.exists() and target.stat().st_size == source.stat().st_size, timeout=40, description='unique direct attachment target fully written')
                d.wait(recipient, '已保存', timeout=40)
                expected = hashlib.sha256(source.read_bytes()).hexdigest()
                actual = hashlib.sha256(target.read_bytes()).hexdigest()
                assert actual == expected
                hashes.append({'sender': sender, 'recipient': recipient, 'type': kind,
                               'bytes': source.stat().st_size, 'sha256': actual})
                d.screenshot(recipient, f'direct-{sender}-{kind}-saved')
        d.evidence('direct-attachment-sha256', hashes)

def stage_removed_friend(d, files):
    with d.case('removed-friend-bilateral-gating', 'Removed friends retain direct history/search/download/read/mute/pin/delete, but neither side can send/reply/edit/react/type/upload'):
        d.open_direct('B', 'C')
        d.open_direct('C', 'B')
        d.send('B', 'retained_history_' + d.args.run_id)
        d.command('B', 'file ' + str(files['png']))
        d.wait('C', files['png'].name)
        d.remove_contact('B', 'C')
        d.find_profile('C', 'B')
        d.wait('C', 'Add friend')
        for actor, peer in [('B', 'C'), ('C', 'B')]:
            d.open_named(actor, d.name(peer))
            d.wait(actor, '双方接受好友申请后可发送消息')
            for action in ('compose', 'reply', 'edit', 'reaction 1', 'file ' + str(files['text'])):
                d.command(actor, action)
                d.wait(actor, '双方接受好友申请后可发送消息')
                assert '正在上传' not in d.capture(actor)
            d.command(actor, 'mute')
            d.command(actor, 'pin')
            d.command(actor, 'read')
            d.command(actor, 'search retained_history_' + d.args.run_id)
            d.wait(actor, 'retained_history_' + d.args.run_id)
            d.screenshot(actor, 'removed-history-search')
            d.keys(actor, 'Escape')
        d.selected_message('C', files['png'].name)
        target = d.work / 'removed-friend-download.png'
        d.command('C', 'save ' + str(target))
        d.wait('C', lambda text: target.exists() and target.stat().st_size == files['png'].stat().st_size, timeout=40, description='removed-friend download complete')
        assert target.read_bytes() == files['png'].read_bytes()
        d.selected_message('B', 'retained_history_' + d.args.run_id)
        d.keys('B', 'd')
        d.confirm('B', '删除这条消息')
        d.wait('C', '消息已删除')
        d.request_friend('B', 'C')
        d.find_profile('C', 'B')
        d.wait('C', 'Incoming friend request')
        assert 'online' not in d.capture('C') and 'Message' not in d.capture('C')
        d.accept_friend('C', 'B')
        d.open_direct('B', 'C')
        d.open_direct('C', 'B')
        d.send('C', 'reaccepted_' + d.args.run_id)
        d.wait('B', 'reaccepted_' + d.args.run_id)


def stage_admin_friend_invite(d):
    with d.case('admin-invites-confirmed-friends', 'Real admin selects only accepted friends; pending/shared-group profiles are excluded; manual invite bypasses join approval'):
        d.request_friend('B', 'D')
        d.accept_friend('D', 'B')
        d.request_friend('B', 'E')
        d.find_profile('E', 'B')
        d.wait('E', 'Incoming friend request')
        assert 'Message' not in d.capture('E')
        d.open_main('D')
        d.open_main('A')
        d.choose_member('A', 'D')
        d.command('A', 'kick')
        d.confirm('A', '移除群成员')
        d.wait('D', lambda text: 'Conversations' in text and d.title not in text)
        d.open_main('B')
        d.command('B', 'group')
        d.wait('B', 'Your role: admin')
        d.keys('B', 'Escape')
        d.command('B', 'invite')
        d.wait('B', 'Choose friends')
        d.wait('B', d.name('D'))
        screen = d.capture('B')
        assert d.name('E') not in screen, 'Pending friend must not be an invitation candidate'
        stranger = next(item for item in d.manifest['sdk'] if item['alias'] == 'S030')
        assert stranger['username'] not in screen, 'Shared-group stranger must not be an invitation candidate'
        d.screenshot('B', 'accepted-candidates-only')
        choose_row(d, 'B', d.name('D'))
        d.keys('B', 'Space')
        d.wait('B', 'Selected: 1')
        d.screenshot('B', 'invite-selected-friend')
        d.keys('B', 'Enter')
        d.wait('B', 'Members (100)')
        d.open_main('D')
        d.command('D', 'members')
        d.wait('D', 'Members (100)')
        requests = d.query('S005', 'get_group_join_requests', conversation=d.group)
        pending = list(requests['requests'])
        while requests.get('next'):
            requests = d.query('S005', 'get_group_join_requests', conversation=d.group, before=requests['next'])
            pending.extend(requests['requests'])
        recipient = int(d.manifest['actors']['D']['id'])
        assert all(item['applicant']['id'] != recipient for item in pending), 'Manual invitation must not create a pending join request'
        d.evidence('manual-invite', {'member_count': 100, 'invited_member': recipient,
                                     'remaining_pending_count': len(pending), 'approval_bypassed': True})
        d.screenshot('D', 'manually-rejoined')
        d.find_profile('E', 'B')
        d.command('E', 'reject-friend')
        d.wait('E', 'Add friend')

def stage_group_management(d):
    with d.case('group-management-roles', 'Fourth administrator rejected; owner demotes/promotes; ordinary member sees no secret invitation actions'):
        d.open_main('A')
        d.choose_member('A', 'C')
        d.command('A', 'admin')
        d.wait('A', '最多可设置 3 位管理员')
        d.choose_member('A', 'B')
        d.command('A', 'admin')
        d.wait('A', '群聊已更新')
        d.open_main('B')
        d.command('B', 'group')
        d.wait('B', 'Your role: member')
        assert 'invitation link' not in d.capture('B')
        d.choose_member('A', 'B')
        d.command('A', 'admin')
        d.open_main('B')
        d.command('B', 'group')
        d.wait('B', 'Your role: admin')
        d.open_main('C')
        d.command('C', 'group')
        d.wait('C', 'Your role: member')
        assert 'invitation link' not in d.capture('C')
        d.screenshot('C', 'member-public-group-profile')
    with d.case('group-announcement-pin', 'Group announcement supports full view/edit/clear; group pin follows edits and deletion'):
        d.open_main('A')
        announcement = '公告_' + d.args.run_id + '_中文😀'
        d.command('A', 'announcement ' + announcement)
        d.wait('C', announcement)
        d.command('C', 'show-announcement')
        d.wait('C', 'Copyable text')
        d.wait('C', announcement)
        d.screenshot('C', 'full-announcement')
        d.command('A', 'announcement')
        d.wait('A', '编辑公告')
        d.clear_input('A')
        d.keys('A', 'Enter')
        d.wait('A', lambda text: announcement not in text)
        marker = 'group_pin_' + d.args.run_id
        d.send('A', marker)
        d.selected_message('A', marker)
        d.command('A', 'pin-message')
        d.wait('A', 'Pinned:')
        d.open_main('C')
        d.selected_message('C', marker)
        d.command('C', 'pinned')
        d.wait('C', 'i: compose')
        d.wait_selected('C', marker)
        d.selected_message('A', marker)
        d.keys('A', 'e')
        d.clear_input('A')
        d.paste('A', marker + '_edited')
        d.keys('A', 'Enter', 'Escape')
        d.wait('C', marker + '_edited')
        d.selected_message('A', marker + '_edited')
        d.keys('A', 'd')
        d.confirm('A', '删除这条消息')
        d.wait('C', lambda text: 'Pinned:' not in text)
    with d.case('removed-group-stale-pages', 'Kicked member loses history/search/members pages; request/rejoin restores fresh membership and read state'):
        for page in ('conversation', 'search', 'members'):
            d.open_main('D')
            if page == 'search':
                d.command('D', 'search ' + d.manifest['search_query'])
                d.wait('D', 'Search:')
            elif page == 'members':
                d.command('D', 'members')
                d.wait('D', 'Members (100)')
            d.open_main('A')
            d.choose_member('A', 'D')
            d.command('A', 'kick')
            d.confirm('A', '移除群成员')
            d.wait('D', lambda text: 'Conversations' in text and d.title not in text)
            d.screenshot('D', 'removed-' + page)
            gap = d.query('S005', 'send_message', conversation=d.group, text='removed_gap_' + page + '_' + d.args.run_id)
            if page == 'conversation':
                d.open_main('A')
                d.selected_message('A', 'read_probe_' + d.args.run_id)
                d.wait('A', '已读 98 人')
                d.screenshot('A', 'reader-left-count98')

            d.command('D', 'join chat://join/' + d.manifest['invite_token'])
            d.wait('D', '等待管理员审批')
            d.command('A', 'requests')
            d.wait('A', 'Join requests')
            choose_row(d, 'A', d.name('D'), pages=True)
            d.keys('A', 'y')
            d.wait('A', '入群申请已处理')
            d.wait('D', d.title)
            member_id = int(d.manifest['actors']['D']['id'])
            boundary = read_only_sql(f'SELECT joined_message_id,last_read_message_id FROM conversation_members WHERE conversation_id={int(d.group)} AND user_id={member_id}')
            assert len(boundary) == 1 and int(boundary[0][0]) >= gap['message_id'] and int(boundary[0][1]) <= int(boundary[0][0]), boundary
            d.command('D', 'help')
            d.wait('D', 'Keyboard help')
            post = d.query('S005', 'send_message', conversation=d.group, text='postjoin_' + page + '_' + d.args.run_id)
            unread_sql = (f'SELECT count(*) FROM messages m JOIN conversation_members cm ON cm.conversation_id=m.conversation_id '
                          f'WHERE cm.conversation_id={int(d.group)} AND cm.user_id={member_id} AND m.id>cm.joined_message_id '
                          f'AND m.id>cm.last_read_message_id AND m.sender_id<>{member_id} AND NOT m.deleted')
            unread = read_only_sql(unread_sql)
            assert unread == [['1']], unread
            d.resize('D', 80,24)
            d.command('D', 'conversations')
            d.wait('D', lambda text: d.title in text and '(1)' in text)
            d.screenshot('D', 'only-postjoin-message-unread-' + page)
            d.evidence('rejoin-watermark-' + page, {'gap_message': gap['message_id'], 'membership': boundary, 'new_message':post['message_id'], 'unread':1})
            d.open_main('D')
            deadline = time.monotonic() + 10
            while read_only_sql(unread_sql) != [['0']]:
                assert time.monotonic() < deadline, 'Latest visible postjoin message not marked read'
                time.sleep(.1)
            d.resize('D', 160,45)
            d.command('D', 'members')
            d.wait('D', 'Members (100)')
        assert d.control('status', verify=True)['member_count'] == 100
    with d.case('conversation-pagination-pin-mute', 'Owner loads 65 authoritative conversations; personal pin and mute stay independent of unread/history'):
        d.logout('A')
        owner_id = d.manifest['actors']['A']['id']
        deadline = time.monotonic() + 10
        while owner_id in d.control('status', verify=True)['online_member_ids']:
            assert time.monotonic() < deadline
            time.sleep(.1)
        d.control('connect', actors=['A'])
        expected = []
        cursor = None
        while True:
            result = d.query('A', 'get_conversations', **({'before': cursor} if cursor else {}))
            expected.extend(result['conversations'])
            cursor = result['next']
            if not cursor:
                break
        assert len(expected) == 65 and len({value['id'] for value in expected}) == 65
        d.evidence('owner-authoritative-conversation-order', expected)
        d.control('disconnect', actors=['A'])
        d.login('A', first=False)
        d.command('A', 'conversations')
        d.keys('A', 'k', repeat=200)
        names = [value['username'] for value in expected]
        seen = []
        for item in expected:
            name = item['username']
            prefix = next((name[:n] for n in range(1,13)
                           if sum(other.startswith(name[:n]) for other in names) == 1), None)
            assert prefix, 'Fixture names must have a unique visible sidebar prefix'
            label = '[' + name[0] + '] ' + prefix
            deadline = time.monotonic() + 10
            while label not in inverse_text(d.capture('A', styled=True)):
                assert time.monotonic() < deadline, 'TUI ordering differs from owner authoritative snapshot'
                d.keys('A', 'j')
                time.sleep(.08)
            d.keys('A', 'Enter')
            d.wait('A', name)
            seen.append(item['id'])
            d.keys('A', 'Escape')
            d.wait('A', 'Conversations')
            d.keys('A', 'j')
            time.sleep(.08)
        assert seen == [value['id'] for value in expected]
        d.evidence('visible-conversation-ids-in-server-order', seen)
        d.screenshot('A', 'conversation-bottom')
        d.open_main('A')
        d.command('A', 'mute')
        d.wait('A', '[mute]')
        owner_id = int(d.manifest['actors']['A']['id'])
        for flag in ('f','t'):
            d.command('A', 'pin')
            deadline = time.monotonic() + 10
            while read_only_sql(f'SELECT pinned FROM conversation_members WHERE conversation_id={int(d.group)} AND user_id={owner_id}') != [[flag]]:
                assert time.monotonic() < deadline, 'Personal pin RPC did not complete'
                time.sleep(.1)
            d.wait('A', lambda text: ('[pin]' in inverse_text(d.capture('A', styled=True))) == (flag == 't'))
        d.command('A', 'conversations')
        d.keys('A', 'k', repeat=150)
        d.barrier('A')
        d.wait_selected('A', d.title[:5])
        d.screenshot('A', 'personal-pinned-first')
        d.keys('A', 'Enter')
        d.command('A', 'mute')
        d.command('A', 'pin')


def stage_create_group(d):
    with d.case('create-group-friend-picker', 'Creation starts from accepted friends, search/multiselect/cancel, then title and explicit final confirmation'):
        d.command('A', 'create-group')
        d.wait('A', 'Choose friends')
        d.command('A', 'filter ' + d.name('B'))
        d.wait('A', d.name('B'))
        d.keys('A', 'Space')
        d.wait('A', 'Selected: 1')
        d.keys('A', 'Space')
        d.wait('A', 'Selected: 0')
        d.keys('A', 'Space', 'Enter')
        d.wait('A', '群名称')
        title = '自建群_' + d.args.run_id
        d.paste('A', title)
        d.keys('A', 'Enter')
        d.confirm('A', '创建群聊')
        d.wait('A', title)
        d.command('B', 'conversations')
        d.resize('B', 80,24)
        d.wait('B', title)
        d.screenshot('B', 'new-group-visible')
        d.resize('B', 160,45)
        d.command('A', 'group')
        d.wait('A', 'Your role: owner')
        d.keys('A', 'Escape')
        d.command('A', 'invite')
        d.wait('A', 'Choose friends')
        d.command('A', 'filter ' + d.name('C'))
        d.keys('A', 'Space', 'Enter')
        d.wait('A', 'Members (3)')
        assert d.name('C') in d.capture('A')
        d.screenshot('A', 'created-and-invited-friends')
        d.command('A', 'rename 重命名_' + d.args.run_id)
        d.wait('A', '群聊已更新')
        # Nonfriend E must not occur in the accepted-only invitation picker.
        d.command('A', 'invite')
        d.command('A', 'filter ' + d.name('E'))
        d.wait('A', lambda text: 'No items' in text or '[ ]' not in text)
        assert d.name('E') not in inverse_text(d.capture('A', styled=True))
        d.keys('A', 'Escape')
        d.open_main('A')


def stage_resize(d):
    with d.case('resize-six-sizes-eight-pages', 'Six terminal sizes preserve active conversation, page, drafts and accessible input across core pages'):
        sizes = ((160,45),(120,40),(100,30),(80,24),(70,20),(35,10))
        for page in ('conversations', 'members', 'conversation', 'search', 'requests', 'account', 'help', 'compose'):
            d.resize('A', 160,45)
            d.open_main('A')
            if page == 'search':
                d.command('A', 'search ' + d.manifest['search_query'])
                d.wait('A', 'Search:')
            elif page == 'compose':
                d.keys('A', 'i')
                d.clear_input('A')
                d.paste('A', 'resize_draft')
            elif page != 'conversation':
                d.command('A', page)
            heading = {'conversations':'Conversations', 'members':'Members (100)',
                       'conversation':d.title, 'search':'Search:', 'requests':'Join requests',
                       'account':'Profile ·', 'help':'Keyboard help', 'compose':'resize_draft'}[page]
            d.wait('A', heading)
            for width,height in sizes:
                d.resize('A', width,height)
                d.wait('A', 'Terminal too small' if width < 40 else heading)
                if width >= 100 and page in ('conversations','conversation','compose'):
                    d.wait('A', 'Conversations')
                d.screenshot('A', f'{page}-{width}x{height}')
            d.resize('A', 160,45)
            d.wait('A', heading)
            d.keys('A', 'Escape')
        for actor in 'BCDE':
            d.resize(actor, 80,24)
            d.open_main(actor)
            d.wait(actor, 'i: compose')
            d.keys(actor, 'Escape')
            d.wait(actor, 'Conversations')
            d.keys(actor, 'Enter')
            d.resize(actor, 160,45)


def stage_fanout(d):
    with d.case('exact-99-recipient-fanout', 'One real owner TUI plus 99 authenticated SDK members: each recipient observes exactly one unique message, no duplicates'):
        for actor in 'BCDE':
            d.logout(actor)
        d.control('connect', actors=list('BCDE'))
        status = d.control('status', verify=True)
        assert status['member_count'] == 100 and status['online_member_count'] == 100
        marker = 'fanout_unique_' + d.args.run_id
        d.control('events_reset', conversation=d.group, marker=marker)
        d.open_main('A')
        ui_started = time.monotonic()
        d.send('A', marker)
        deadline = time.monotonic() + 15
        while True:
            evidence = d.control('events')
            if evidence['recipient_count'] == 99:
                break
            assert time.monotonic() < deadline, evidence
            time.sleep(.1)
        assert evidence['expected_count'] == 99 and evidence['duplicates'] == 0 and not evidence['missing'] and not evidence['unexpected'], evidence
        d.evidence('exact-fanout', evidence)
        d.evidence('ui-send-to-99-recipients-upper-bound', {'elapsed_ms':(time.monotonic()-ui_started)*1000,
                   'measurement':'UI send action (including actual keystrokes/input clear and waits) until first observed 99-recipient snapshot; upper bound, not network-only latency'})
        d.screenshot('A', 'real-owner-send')
        identity = message_id(d, marker)
        d.control('read', message=identity, target_count=99)
        d.wait('A', '已读 99 人')
        d.screenshot('A', 'hundred-online-read99')
        recipients = ['B','C','D','E'] + [item['alias'] for item in d.manifest['sdk'] if item['alias'] != 'S100']
        assert len(recipients) == 99
        distribution = [('👍',25),('❤️',25),('😂',25),('🎉',24)]
        offset = 0
        for emoji,count in distribution:
            for actor in recipients[offset:offset+count]:
                d.query(actor, 'set_message_reaction', conversation=d.group, message=identity, emoji=emoji)
            offset += count
            d.wait('A', emoji + ' ' + str(count))
        result = d.query('S005', 'search_messages', conversation=d.group, query=marker)['messages'][0]
        assert {item['emoji']:len(item['users']) for item in result['reactions']} == dict(distribution)
        users = [user for item in result['reactions'] for user in item['users']]
        assert len(users) == len(set(users)) == 99
        d.evidence('99-reaction-users', result)
        d.screenshot('A', 'reactions-25-25-25-24')
        settled = d.control('events')
        assert settled['recipient_count'] == settled['expected_count'] == 99 and settled['duplicates'] == 0 and not settled['missing'] and not settled['unexpected'], settled
        d.evidence('fanout-after-99-reaction-roundtrips', settled)
        d.control('events_reset', conversation=d.group, message=identity)
        typers = recipients[:10]
        for actor in typers:
            d.query(actor, 'set_typing', conversation=d.group, typing=True)
        d.wait('A', '等 正在输入')
        d.screenshot('A', 'ten-typers')
        deadline = time.monotonic() + 15
        # Each of the ten senders receives nine peer events, other SDK members ten.
        while True:
            events = d.control('events', kind='typing_true')
            counts = {actor:events['actors'].get(actor,{}).get('typing_true',0) for actor in recipients}
            expected = {actor:9 if actor in typers else 10 for actor in recipients}
            assert all(counts[actor] <= expected[actor] for actor in recipients), events
            if counts == expected:
                break
            assert time.monotonic() < deadline, events
            time.sleep(.05)
        d.evidence('ten-typing-fanout', events)
        for actor in typers:
            d.query(actor, 'set_typing', conversation=d.group, typing=False)
        d.wait('A', lambda text: '正在输入' not in text)

        latency = d.sdk('S030', 'send_message', conversation=d.group, text='sdk_latency_' + d.args.run_id)
        d.wait('A', 'sdk_latency_' + d.args.run_id)
        d.evidence('sdk-send-latency', latency)
        d.control('disconnect', actors=list('BCDE'))
        for actor in 'BCDE':
            d.login(actor, first=False)
            d.open_main(actor)
        status = d.control('status', verify=True)
        assert status['online_member_count'] == 100

def stage_restarts(d):
    with d.case('server-restarts', f'{d.args.restarts} actual server restarts preserve five TUI identities, drafts and unique delivery after automatic reconnect'):
        actors = [item['alias'] for item in d.manifest['sdk'] if item['alias'] != 'S100']
        for actor in 'ABCDE':
            d.open_main(actor)
        for iteration in range(d.args.restarts):
            draft = f'reconnect_draft_{iteration}_' + d.args.run_id
            d.keys('E', 'i')
            d.clear_input('E')
            d.paste('E', draft)
            d.wait('E', draft)
            d.keys('E', 'Escape')
            d.control('close')
            d.stop_server()
            for actor in 'ABCDE':
                d.wait(actor, lambda text: '正在重连' in text or 'reconnecting' in text or 'connecting' in text)
            d.start_server()
            d.control('connect', actors=actors, timeout=120)
            for actor in 'ABCDE':
                d.wait(actor, lambda text: 'connected' in text and '正在重连' not in text and 'i: compose' in text, timeout=30)
            d.keys('E', 'i')
            d.wait('E', draft)
            d.screenshot('E', 'restored-draft-' + str(iteration))
            d.clear_input('E')
            d.keys('E', 'Escape')
            marker = f'restart_{iteration:02d}_{d.args.run_id}'
            d.control('events_reset', conversation=d.group, marker=marker)
            d.send('A', marker)
            for actor in 'BCDE':
                d.keys(actor, 'G')
                d.wait(actor, marker)
            deadline = time.monotonic() + 10
            while True:
                events = d.control('events')
                if events['recipient_count'] == 95:
                    break
                assert time.monotonic() < deadline, events
                time.sleep(.1)
            assert events['duplicates'] == 0 and not events['missing'] and not events['unexpected']
            d.screenshot('A', 'restart-' + str(iteration))
            d.observer.sample(checkpoint='restart-' + str(iteration))
        status = d.control('status', verify=True)
        assert status['member_count'] == status['online_member_count'] == 100


def stage_soak(d):
    with d.case('soak', f'Five actual TUI processes plus 95 SDK members stay active for {d.args.soak_seconds}s with periodic real UI actions and resource samples'):
        for actor in 'ABCDE':
            d.open_main(actor)
        start = time.monotonic()
        duration = d.args.soak_seconds
        checkpoints = sorted(set([0, duration] + [n for n in (600,1200,1800) if n <= duration]))
        next_probe = 0
        next_sample = 0
        seen = set()
        probes = 0
        while True:
            elapsed = time.monotonic() - start
            for checkpoint in checkpoints:
                if checkpoint not in seen and elapsed >= checkpoint:
                    sample = d.observer.sample(checkpoint=checkpoint)
                    for actor in 'ABCDE':
                        process = sample['processes']['tui_' + actor]
                        assert process['alive'] and process['executable'].endswith('/chat_tui') and not process['pid_reused'], process
                    d.evidence('resource-' + str(checkpoint), sample)
                    online = d.control('status', verify=True)
                    assert online['member_count'] == online['online_member_count'] == 100 and online['sdk_online_count'] == 95, online
                    d.evidence('online-' + str(checkpoint), online)
                    seen.add(checkpoint)
            if elapsed >= duration:
                break
            if elapsed >= next_sample:
                d.observer.sample()
                next_sample += 60
            if elapsed >= next_probe:
                sender = 'ABCDE'[probes % 5]
                recipient = 'ABCDE'[(probes + 1) % 5]
                marker = f'soak_{probes:03d}_{d.args.run_id}'
                d.open_main(sender)
                d.open_main(recipient)
                d.send(sender, marker)
                d.keys(recipient, 'G')
                d.wait(recipient, marker)
                mode = probes % 6
                if mode == 0:
                    d.command(recipient, 'reaction 1')
                elif mode == 1:
                    d.command(recipient, 'search ' + marker)
                    d.wait(recipient, marker)
                    d.keys(recipient, 'Escape')
                elif mode == 2:
                    d.keys(recipient, 'PPage')
                    d.keys(recipient, 'k', repeat=3)
                    d.wait(recipient, 'Browsing history')
                    d.keys(recipient, 'G')
                elif mode == 3:
                    d.keys(recipient, 'i')
                    d.clear_input(recipient)
                    d.paste(recipient, 'soak_typing')
                    d.wait(sender, '正在输入')
                    d.keys(recipient, 'Escape')
                    d.wait(sender, lambda text: '正在输入' not in text)
                elif mode == 4:
                    d.resize(recipient, 70,20)
                    d.wait(recipient, 'i: compose')
                    d.resize(recipient, 160,45)
                else:
                    d.command(recipient, 'contacts')
                    d.wait(recipient, 'New friends')
                    first = next(value['id'] for value in d.manifest['sdk'] if value['alias'] == 'S030')
                    second = next(value['id'] for value in d.manifest['sdk'] if value['alias'] == 'S031')
                    d.query('S030', 'remove_contact', user='S031')
                    absent_left = d.query('S030', 'get_contacts')
                    absent_right = d.query('S031', 'get_contacts')
                    hidden = d.query('S030', 'get_presence')
                    assert second not in {value['id'] for value in absent_left} and first not in {value['id'] for value in absent_right}
                    assert second not in {value['user'] for value in hidden}
                    d.query('S030', 'send_friend_request', user='S031')
                    d.query('S031', 'respond_friend_request', user='S030', accept=True)
                    friends = d.query('S030', 'get_contacts')
                    restored_right = d.query('S031', 'get_contacts')
                    presence = d.query('S030', 'get_presence')
                    assert second in {value['id'] for value in friends} and first in {value['id'] for value in restored_right}
                    assert any(value['user'] == second and value['online'] for value in presence)
                    assert {value['user'] for value in presence} <= {value['id'] for value in friends}
                    d.evidence('contact-presence-' + str(probes), {'removed_presence':hidden,'contacts': friends, 'presence': presence})
                    d.open_main(recipient)
                d.command(recipient, 'read')
                d.screenshot(recipient, f'probe-{probes:03d}')
                probes += 1
                next_probe += 60
                print(f'SOAK elapsed={int(elapsed)}s probes={probes}', flush=True)
            time.sleep(min(1, max(.01, duration - (time.monotonic()-start))))
        assert seen == set(checkpoints)
        status = d.control('status', verify=True)
        assert status['member_count'] == status['online_member_count'] == 100
        d.evidence('soak-final-presence', status)
        d.evidence('soak-summary', {'requested_seconds': duration, 'elapsed_seconds': time.monotonic()-start,
                                   'probes': probes, 'checkpoints': checkpoints,
                                   'full_duration': duration >= 1800})


def stage_transfer_logout(d):
    with d.case('owner-transfer-leave', 'Owner cannot leave or transfer to ordinary member; explicit transfer to admin enables former owner leave'):
        d.open_main('A')
        d.command('A', 'leave')
        d.wait('A', '群主须先转让')
        d.choose_member('A', 'C')
        d.command('A', 'transfer')
        d.wait('A', '当前管理员')
        d.choose_member('A', 'B')
        d.command('A', 'transfer')
        d.confirm('A', '转让')
        d.open_main('B')
        d.command('B', 'group')
        d.wait('B', 'Your role: owner')
        d.command('A', 'leave')
        d.confirm('A', '退出当前群聊')
        d.wait('A', '已退出群聊')
        d.screenshot('A', 'former-owner-left')
        d.command('B', 'invite')
        d.wait('B', 'Choose friends')
        d.command('B', 'filter ' + d.name('A'))
        d.wait('B', d.name('A'))
        d.keys('B', 'Space', 'Enter')
        d.wait('B', 'Members (100)')
        d.choose_member('B', 'A')
        d.command('B', 'admin')
        d.wait('B', '群聊已更新')
        d.open_main('A')
        d.command('A', 'group')
        d.wait('A', 'Your role: admin')
        final = d.control('status', verify=True)
        assert final['member_count'] == final['online_member_count'] == 100
        assert final['owner_count'] == 1 and final['admin_count'] == 3
        d.evidence('final-hundred-roles', final)

    with d.case('account-logout', 'Self account page requires explicit logout confirmation and all five TUI processes exit safely'):
        for actor in 'ABCDE':
            d.command(actor, 'account')
            d.wait(actor, 'Log out')
            d.command(actor, 'logout')
            d.wait(actor, '退出当前账号')
            d.keys(actor, 'Escape')
            d.wait(actor, 'Profile ·')
            d.logout(actor)
            d.screenshot(actor, 'logged-out')
            d.keys(actor, 'C-c')
        time.sleep(.15)


def run_matrix(d):
    stage_friendships(d)
    stage_onboard(d)
    stage_messages(d)
    stage_terminal_messages(d)
    stage_large_text(d)
    files = stage_files(d)
    stage_friend_direct(d, files)
    stage_removed_friend(d, files)
    stage_admin_friend_invite(d)
    stage_group_management(d)
    from tui_group_edges_smoke import run as group_edges, prepare_reconnect, verify_reconnect
    group_edges(d)
    stage_create_group(d)
    stage_resize(d)
    stage_fanout(d)
    prepare_reconnect(d)
    stage_restarts(d)
    verify_reconnect(d)
    stage_soak(d)
    with d.case('sdk-size-measurements', 'Actual SDK-only groups of 3/10/50/200 each run three member/history/send samples'):
        sizes = d.control('measure_sizes', timeout=600)
        assert [value['size'] for value in sizes['sizes']] == [3,10,50,200]
        for value in sizes['sizes']:
            assert len(value['samples']) == 3
        d.evidence('all-size-samples', sizes)
    from tui_lifecycle_smoke import run as lifecycle
    lifecycle(d)
    stage_transfer_logout(d)
    full = d.args.soak_seconds >= 1800 and d.args.restarts >= 10
    summary = {'result': 'PASS' if full else 'DIAGNOSTIC_PASS', 'full_acceptance': full,
               'cases': d.case_count, 'soak_seconds': d.args.soak_seconds, 'restarts': d.args.restarts,
               'functional_mode': '5 real TUI + 95 SDK members',
               'exact_fanout_mode': '1 real owner TUI + 99 SDK members',
               'database_mutations': 'Only through real chat::client APIs; shell owns isolated database'}
    d.evidence('summary', summary)
    (d.work / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2))
    print(json.dumps(summary, ensure_ascii=False), flush=True)



def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--work-dir', required=True)
    parser.add_argument('--fixture-tool', required=True)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--soak-seconds', type=int, default=1800)
    parser.add_argument('--restarts', type=int, default=10)
    return parser.parse_args()


def main():
    args = parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_]{1,24}', args.run_id):
        raise SystemExit('run-id must be 1–24 ASCII letters/digits/underscores')
    if args.soak_seconds < 0 or args.restarts < 0:
        raise SystemExit('durations/counts must be nonnegative')
    driver = Driver(args)
    def terminate(signum, frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, terminate)
    try:
        with driver.case('setup', 'Isolated fixture: 100 initial group members, five real TUI processes'):
            driver.setup()
        with driver.case('accounts', 'Five actual TUI logins; bad password/Unicode username edge whitespace rejected; internal spaces preserved'):
            for actor in 'ABCDE':
                if actor == 'E':
                    driver.registration_edges(actor)
                driver.login(actor, first=actor != 'E', bad_password=actor == 'A')
                driver.screenshot(actor, 'authenticated')
        # Scenario stages are intentionally independent checkpoints. The SDK
        # fixture is the only protocol implementation used by the harness.
        run_matrix(driver)
    except Exception as error:
        (driver.work / 'failure-traceback.txt').write_text(driver.redact(traceback.format_exc()))
        print(f'FAIL {driver.case_id}: {driver.redact(str(error))}', file=sys.stderr, flush=True)
        return 1
    finally:
        driver.close()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
