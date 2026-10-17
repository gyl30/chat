#!/usr/bin/env python3
"""Native TUI scale worker: stdin configuration, actual tmux keys, JSON evidence.

No SDK instance, WebSocket client or SQL operation exists in this worker. The
coordinator creates isolated fixtures and checks authoritative database facts.
The first stdin line contains a password kept in memory; output is redacted.
A second line {"command":"start"} synchronizes the independent workers.
"""
from __future__ import annotations

import argparse
import base64
import contextlib
import hashlib
import json
import os
from pathlib import Path
import random
import re
import secrets
import shlex
import signal
import subprocess
import sys
import time
from urllib.parse import urlsplit

sys.dont_write_bytecode = True
# These are terminal-screen helpers only. Do not instantiate the SDK Driver.
from tui_100_member_smoke import CSI, inverse_text, composing, chat_open, prompt_line
from tui_scale_observer import process_sample
from native_network import Blackhole


class Cancelled(Exception):
    pass


def chat_header_matches(screen, title):
    """Check the visible native chat header, never a truncated sidebar prefix."""
    # 顶部首个内容行是标题；取最右栏，排除侧栏同名项。
    row = next((line for line in screen.splitlines() if line.startswith('│')), '')
    cells = row.split('│')
    if len(cells) < 3:
        return False
    header = cells[-2].strip()
    detail = r'(?:在线|离线|最后在线 [-0-9: ]+|\d+ 位成员|\S*(?: \S*)?…)'
    return re.fullmatch(re.escape(title) + r'(?:\s{2,}' + detail + r')?', header) is not None


def sidebar_title_matches(label, title):
    # Match the complete name followed only by the native list's metadata.
    # "Scale 1" must not open "Scale 10" or a different longer username.
    name = label.strip(' │┃╻╹').removeprefix('> ').removeprefix('▸ ').strip()
    presence = r'(?:\s+·\s+(?:在线|离线|最后在线\s+\d{2}-\d{2}\s+\d{2}:\d{2}))?'
    flags = r'(?:\s+(?:置顶|免打扰))*'
    when = r'(?:\s+(?:\d{2}:\d{2}|昨天|周[日一二三四五六]|(?:\d{4}-)?\d{2}-\d{2}))?'
    return re.fullmatch(re.escape(title) + presence + flags + when + r'\s*', name) is not None


def sidebar_step(screen, title):
    # Activity can move a target above the selected conversation. Follow the
    # current visible rows, rather than assuming a stable Down-only order.
    selected = target = None
    for index, line in enumerate(screen.splitlines()):
        cells = line.split('│')
        if len(cells) <= 2:
            continue
        label = cells[1]
        if label.lstrip().startswith('> ') or (selected is None and label.lstrip().startswith('▸ ')):
            selected = index
        if sidebar_title_matches(label, title):
            target = index
    return 'Up' if selected is not None and target is not None and target < selected else 'Down'


def receipt_metadata_matches(line, expected):
    # 时间和回执共用气泡的辅助文字颜色，正文中的“已读”不能冒充回执。
    metadata = r'(?:\d{2}-\d{2} )?\d{2}:\d{2} · ' + re.escape(expected)
    if re.fullmatch(metadata, CSI.sub('', line).strip(' │┃╻╹')) is None:
        return False
    return re.search(r'\x1b\[38;2;126;164;139m\s*' + metadata +
                     r'(?=\s|\x1b|[│┃╻╹]|$)', line) is not None


class Worker:
    def __init__(self, config):
        self.config = config
        self.password = config['password']
        self.private_tokens = []
        self.actors = config['actors']
        if not 1 <= len(self.actors) <= 40:
            raise ValueError('Each TUI worker supports 1–40 independent clients')
        self.binary = Path(config['binary']).resolve()
        self.work = Path(config['work']).resolve()
        self.work.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.socket = 'chat_native_' + secrets.token_hex(8)
        if config.get('daily_pilot', False) and len(self.actors) != 2:
            raise ValueError('Daily pilot requires exactly two native actors')
        if config.get('job_control_pilot', False) and len(self.actors) != 2:
            raise ValueError('Job control pilot requires exactly two native actors')
        self.panes = {}
        self.relays = {}
        self.shells = {}
        self.processes = {}
        self.ttys = {}
        self.sequence = {}
        self.seen = set()
        self.rng = random.Random(config['seed'])
        self.tag = hashlib.sha256(str(config['seed']).encode()).hexdigest()[:8]
        self.by_id = {int(a['id']): a for a in self.actors}
        self.active = {}
        self.started_at = None
        self.steady_started_at = None
        self.steady_finished_at = None
        self.operations = 0
        self.failures = 0
        self.running = False
        self.clean = True

    def safe(self, value):
        if isinstance(value, str):
            value = value.replace(self.password, '[REDACTED_PASSWORD]')
            for token in self.private_tokens:
                value = value.replace(token, '[REDACTED_INVITE]')
            if self.config.get('daily_pilot', False):
                # Also protect failure frames captured before a newly displayed
                # invite has been parsed; the code remains memory-only.
                value = re.sub(r'[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}-[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}',
                               '[REDACTED_INVITE]', value)
            return value
        if isinstance(value, dict):
            return {key: self.safe(item) for key, item in value.items()}
        if isinstance(value, list):
            return [self.safe(item) for item in value]
        return value

    def emit(self, event, **value):
        print(json.dumps(self.safe({'event': event, 'monotonic': time.monotonic(),
                                    'utc_ns': time.time_ns(), **value}), ensure_ascii=False), flush=True)

    def tmux(self, *arguments, input=None, check=True):
        result = subprocess.run(['tmux', '-f', '/dev/null', '-L', self.socket, *map(str, arguments)],
                                input=input, text=True, capture_output=True, timeout=15)
        if check and result.returncode:
            raise RuntimeError('Owned tmux operation failed: ' + self.safe(result.stderr.strip()))
        return result

    def pane(self, actor):
        return self.panes[int(actor['id'])]

    def key(self, actor, *keys, repeat=1):
        self.tmux('send-keys', '-t', self.pane(actor), '-N', repeat, *keys)
        if 'Escape' in keys:
            # A distinct Escape followed immediately by a character encodes Alt.
            # Separate physical keys as in the existing native tmux driver;
            # this does not wait for a server operation or mask a race.
            time.sleep(.15)

    def paste(self, actor, text):
        name = 'paste_' + str(actor['id'])
        self.tmux('load-buffer', '-b', name, '-', input=text)
        self.tmux('paste-buffer', '-p', '-b', name, '-d', '-t', self.pane(actor))

    def capture(self, actor, styled=False):
        text = self.tmux('capture-pane', '-p', *(['-e'] if styled else []),
                         '-t', self.pane(actor)).stdout
        if self.password in text:
            raise AssertionError('Password appeared in native terminal output')
        return text

    def screenshot(self, actor, name):
        path = self.work / ('actor-' + str(actor['id']) + '-' + name + '.ansi')
        path.write_text(self.safe(self.capture(actor, True)))
        self.emit('capture', actor=actor['id'], path=str(path))

    def wait(self, actor, predicate, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            text = self.capture(actor)
            if predicate(text) if callable(predicate) else predicate in text:
                return text
            state = process_sample(self.processes[int(actor['id'])]['pid'])
            if not state.get('alive'):
                raise RuntimeError('Native TUI exited while waiting for a frame')
            time.sleep(.06)
        self.screenshot(actor, 'deadline-' + str(time.time_ns()))
        raise TimeoutError('Native TUI did not reach expected frame: ' + str(predicate))

    def shell_prompt(self, actor):
        return 'native-shell-' + self.tag + '-' + str(actor['id']) + '$ '

    def wait_shell(self, actor, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            text = self.capture(actor)
            if text.rstrip().endswith(self.shell_prompt(actor).rstrip()):
                return text
            original = self.shells[int(actor['id'])]
            current = process_sample(original['pid'])
            if not current.get('alive') or current.get('start_ticks') != original['start_ticks']:
                raise AssertionError('Owned interactive shell ended before its job was checked')
            time.sleep(.04)
        self.screenshot(actor, 'shell-deadline')
        raise TimeoutError('Owned interactive shell did not regain the terminal')

    def wait_file(self, path, timeout=5):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if path.is_file() and path.read_text().strip():
                return path.read_text().strip()
            time.sleep(.03)
        raise TimeoutError('Native shell did not produce its terminal evidence')

    def focus_messages(self, actor):
        if composing(self.capture(actor)):
            self.key(actor, 'BTab')
            self.wait(actor, lambda text: not composing(text))

    def focus_compose(self, actor):
        if not composing(self.capture(actor)):
            # Tab is a native focus transition, never a printable fallback that
            # could become message text if the previous frame was stale.
            self.key(actor, 'Tab')
            self.wait(actor, composing)

    def command(self, actor, command):
        self.focus_messages(actor)
        self.key(actor, ':')
        self.wait(actor, 'Enter 执行 · Esc 取消')
        self.paste(actor, command)
        self.key(actor, 'Enter')
        self.wait(actor, lambda text: 'Enter 执行 · Esc 取消' not in text)

    def palette(self, actor, label, expected):
        self.key(actor, 'C-k')
        self.wait(actor, '输入命令或关键字')
        self.paste(actor, label)
        self.wait(actor, lambda text: label in text)
        self.key(actor, 'Enter')
        return self.wait(actor, expected)

    def conversation_context(self, conversation):
        # New groups have no user-visible ID. The coordinator resolves their
        # unique final title through read-only DB evidence after native actions.
        return {'conversation_title': conversation} if isinstance(conversation, str) else {'conversation': conversation}

    def operation(self, actor, kind, action, marker=None, conversation=None):
        start = time.monotonic()
        try:
            result = action()
        except Exception as error:
            self.failures += 1
            # Capture before notifying the fail-fast coordinator, which may
            # immediately request cleanup of these owned native processes.
            with contextlib.suppress(Exception):
                self.screenshot(actor, 'operation-failure-' + str(time.time_ns()))
            self.emit('operation', kind=kind, actor=actor['id'], marker=marker,
                      **self.conversation_context(conversation), elapsed_ms=(time.monotonic()-start)*1000,
                      ok=False, error=str(error))
            raise
        self.operations += 1
        self.emit('operation', kind=kind, actor=actor['id'], marker=marker,
                  **self.conversation_context(conversation), elapsed_ms=(time.monotonic()-start)*1000, ok=True)
        return result

    def url_for(self, actor):
        return actor.get('relay_url', self.config['server_url'])

    def launch(self):
        if self.config.get('blackhole_pilot', False):
            # One relay per client so a single native client can lose its link while the
            # others keep their direct connection to the server.
            target = urlsplit(self.config['server_url'])
            for actor in self.actors:
                relay = Blackhole(target.hostname, target.port)
                self.relays[int(actor['id'])] = relay
                actor['relay_url'] = relay.url
        for actor in self.actors:
            identity = int(actor['id'])
            directory = self.work / ('actor-' + str(identity))
            directory.mkdir(exist_ok=True, mode=0o700)
            baseline = directory / 'terminal-before.txt'
            restored_path = directory / 'terminal-after.txt'
            # Passwords never cross argv. This shell records terminal state
            # before and after the real client, before tmux closes the master.
            # Only its independently verified chat_tui child is counted.
            command = ('stty -g > ' + shlex.quote(str(baseline)) + '; env XDG_CONFIG_HOME=' +
                       shlex.quote(str(directory / 'config')) + ' ' + shlex.quote(str(self.binary)) +
                       ' ' + shlex.quote(self.url_for(actor)) +
                       '; native_exit_status=$?; stty -g > ' + shlex.quote(str(restored_path)) +
                       '; exit "$native_exit_status"')
            native_command = ('env XDG_CONFIG_HOME=' + shlex.quote(str(directory / 'config')) + ' ' +
                              shlex.quote(str(self.binary)) + ' ' + shlex.quote(self.url_for(actor)))
            if self.config.get('job_control_pilot', False):
                # A real interactive shell supplies a controlling terminal and
                # job control; fg is typed into it, never simulated by SIGCONT.
                command = ('exec env HISTFILE=/dev/null PS1=' +
                           shlex.quote(self.shell_prompt(actor)) + ' bash --noprofile --norc -i')
            if not self.panes:
                pane = self.tmux('new-session', '-d', '-P', '-F', '#{pane_id}',
                                 '-s', 'clients', '-n', str(identity), '-x', '120', '-y', '40', command)
                server_pid = int(self.tmux('display-message', '-p', '#{pid}').stdout)
                server = process_sample(server_pid)
                if not server.get('alive') or Path(server.get('executable', '')).name != 'tmux':
                    raise AssertionError('Private tmux server identity cannot be verified')
                self.emit('resource-started', kind='tmux-server', pid=server_pid,
                          starttime=server['start_ticks'], executable=server['executable'])
            else:
                pane = self.tmux('new-window', '-d', '-P', '-F', '#{pane_id}',
                                 '-t', 'clients', '-n', str(identity), command)
            self.panes[identity] = pane.stdout.strip()
            self.tmux('set-window-option', '-t', self.pane(actor), 'remain-on-exit', 'on')
            pane_pid = int(self.tmux('display-message', '-p', '-t', self.pane(actor), '#{pane_pid}').stdout)
            if self.config.get('job_control_pilot', False):
                self.shells[identity] = process_sample(pane_pid)
                self.wait_shell(actor)
                self.shells[identity] = process_sample(pane_pid)
                self.emit('resource-started', kind='job-control-shell', actor=identity, pid=pane_pid,
                          starttime=self.shells[identity]['start_ticks'],
                          executable=self.shells[identity]['executable'])
                self.paste(actor, 'stty -g > ' + shlex.quote(str(baseline)))
                self.key(actor, 'Enter')
                self.wait_file(baseline)
                self.wait_shell(actor)
                self.paste(actor, native_command)
                self.key(actor, 'Enter')
            deadline = time.monotonic() + 20
            state = None
            while time.monotonic() < deadline:
                child_file = Path(f'/proc/{pane_pid}/task/{pane_pid}/children')
                children = child_file.read_text().split() if child_file.exists() else []
                candidates = [process_sample(int(pid)) for pid in children]
                live = [value for value in candidates if value.get('alive') and
                        Path(value.get('executable', '')).resolve() == self.binary]
                if len(live) == 1:
                    state = live[0]
                    break
                time.sleep(.03)
            if state is None:
                raise RuntimeError('Pane has no independently verified native TUI child')
            pid = state['pid']
            self.processes[identity] = state
            self.ttys[identity] = (baseline, restored_path)
            self.wait(actor, '注册账号')
            self.emit('started', actor=identity, pid=pid, starttime=state['start_ticks'],
                      executable=state['executable'])
            start = time.monotonic()
            self.paste(actor, actor['username'])
            self.key(actor, 'Tab')
            self.paste(actor, self.password)
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: '已连接' in text and 'Esc 返回' in text, timeout=45)
            self.emit('authenticated', actor=identity, login_ms=(time.monotonic()-start)*1000,
                      pid=pid, starttime=state['start_ticks'])
        self.screenshot(self.actors[0], 'authenticated')
        self.emit('ready', actors=[a['id'] for a in self.actors])

    def key_barrier(self, actor):
        # Reuse the existing smoke's visible nonce acknowledgement. The palette
        # takes keys after the whole preceding FIFO stream and Esc restores the
        # same page; no application hooks, RPCs or fixed network sleeps.
        nonce = 'ui_ack_' + secrets.token_hex(4)
        self.key(actor, 'C-k')
        self.wait(actor, '命令面板')
        self.paste(actor, nonce)
        self.wait(actor, nonce)
        self.key(actor, 'Escape')
        self.wait(actor, lambda text: '命令面板' not in text and nonce not in text)

    def open_chat(self, actor, title, conversation):
        def action():
            self.command(actor, 'chats')
            self.key(actor, 'Up', repeat=25)
            self.key_barrier(actor)
            for _ in range(18):
                frame = self.capture(actor)
                selected = inverse_text(self.capture(actor, True))
                label = next((line.split('│')[1] for prefix in ('> ', '▸ ')
                              for line in frame.splitlines() if len(line.split('│')) > 2
                              and line.split('│')[1].lstrip().startswith(prefix)), '')
                if sidebar_title_matches(label, title):
                    # Keep the original fast path for fully visible fixture
                    # names; verify the full header rather than a body match.
                    self.key(actor, 'Enter')
                    self.wait(actor, lambda text: chat_header_matches(text, title) and composing(text))
                    self.active[int(actor['id'])] = conversation
                    return
                # Only a truncated name needs opening for identification. The
                # summary may also truncate, so inspect the marked title row.
                if '…' in label:
                    self.key(actor, 'Enter')
                    self.key_barrier(actor)
                    if chat_header_matches(self.capture(actor), title):
                        self.focus_compose(actor)
                        self.wait(actor, lambda text: chat_header_matches(text, title) and composing(text))
                        self.active[int(actor['id'])] = conversation
                        return
                    self.command(actor, 'chats')
                    self.key_barrier(actor)
                selected = inverse_text(self.capture(actor, True))
                self.key(actor, sidebar_step(self.capture(actor), title))
                self.wait(actor, lambda text: inverse_text(self.capture(actor, True)) != selected, timeout=3)
            raise AssertionError('Expected chat cannot be selected through the native list: ' + title)
        return self.operation(actor, 'open-conversation', action, conversation=conversation)

    def next_marker(self, actor):
        identity = int(actor['id'])
        number = self.sequence.get(identity, 0) + 1
        self.sequence[identity] = number
        return f'NS_TUI_{self.tag}_{identity:05d}_{number:06d}'

    def history_text(self, actor, styled=False):
        # tmux already decodes terminal cells. Exclude wide sidebar summaries and
        # the composer/status so a stale draft is never counted as delivery.
        lines = []
        for line in self.capture(actor, styled).splitlines():
            if prompt_line(line):
                continue
            cells = line.split('│')
            lines.append('│'.join(cells[2:]) if len(cells) > 3 else line)
        return '\n'.join(lines)

    def body_marker_visible(self, actor, marker):
        # Require the whole native body token, not a substring of a malformed
        # driver draft, sidebar summary, or a later sequence number.
        return re.search(r'(?:^|[│\s])' + re.escape(marker) +
                         r'(?=[\s│┃]|$)', self.history_text(actor), re.M) is not None

    def receive(self, actor, marker, sender=None, timeout=20):
        start = time.monotonic()
        self.wait(actor, lambda text: self.body_marker_visible(actor, marker), timeout=timeout)
        key = (int(actor['id']), marker)
        if key not in self.seen:
            self.seen.add(key)
            self.emit('screen-received', actor=actor['id'], marker=marker, sender=sender,
                      observed_wait_ms=(time.monotonic()-start)*1000)

    def send(self, actor, extra='ASCII 中文 😀 ? : / a r e d', kind='send-message'):
        marker = self.next_marker(actor)
        conversation = self.active[int(actor['id'])]
        def action():
            self.focus_compose(actor)
            self.paste(actor, marker + ' ' + extra)
            self.emit('send-start', actor=actor['id'], marker=marker, **self.conversation_context(conversation))
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: self.body_marker_visible(actor, marker) and
                      '消息已发送' in text and not any(marker in line for line in text.splitlines() if prompt_line(line)))
        self.operation(actor, kind, action, marker, conversation)
        peer = self.by_id.get(int(actor['peer_id']))
        if peer and self.active.get(int(peer['id'])) == conversation:
            self.receive(peer, marker, actor['id'])
        return marker

    def select_latest(self, actor):
        self.focus_messages(actor)
        self.key(actor, 'G')
        self.wait(actor, lambda text: chat_open(text) and not composing(text))

    def profile(self, actor, target):
        self.command(actor, 'search-users ' + target['username'])
        self.wait(actor, lambda text: '查找用户' in text and target['username'] in text and
                  '没有找到匹配' not in text)
        self.key(actor, 'Enter')
        self.wait(actor, lambda text: '资料 · ' in text and target['username'] in text)

    def friendship_checks(self, actor, peer, history_marker):
        self.profile(actor, peer)
        def remove():
            self.command(actor, 'remove-contact')
            self.wait(actor, '输入 y 后按 Enter 确认')
            self.paste(actor, 'y')
            self.key(actor, 'Enter')
            for current, target in ((actor, peer), (peer, actor)):
                self.wait(current, lambda text: not chat_open(text))
                self.command(current, 'chats')
                # Controlled fixture: the only direct is this peer; group
                # summaries contain markers, never account names.
                self.wait(current, lambda text, name=target['username']: name not in text)
                self.screenshot(current, 'removed-direct-hidden')
        self.operation(actor, 'remove-friend', remove, conversation=actor['direct_id'])

        def request():
            self.profile(actor, peer)
            self.command(actor, 'add')
            self.wait(actor, '等待对方确认')
            self.profile(peer, actor)
            self.wait(peer, '接受好友申请')
            for current in (actor, peer):
                frame = self.capture(current)
                if '发消息' in frame or '在线' in '\n'.join(frame.splitlines()[2:-2]):
                    raise AssertionError('Pending profile exposed Message/presence')
        self.operation(actor, 'friend-request', request)
        self.operation(peer, 'reject-friend', lambda: (self.command(peer, 'reject-friend'),
                       self.wait(peer, '添加好友'), self.wait(actor, '添加好友')))
        self.operation(actor, 'friend-request', request)
        self.operation(actor, 'cancel-friend', lambda: (self.command(actor, 'cancel-friend'),
                       self.wait(actor, '添加好友'), self.wait(peer, '添加好友')))
        self.operation(actor, 'friend-request', request)
        self.operation(peer, 'accept-friend', lambda: (self.command(peer, 'accept-friend'),
                       self.wait(peer, '删除好友'), self.wait(actor, '删除好友')))
        for current, target in ((actor, peer), (peer, actor)):
            self.open_chat(current, target['username'], current['direct_id'])
            self.operation(current, 'restored-direct-history', lambda a=current:
                           (self.command(a, 'search ' + history_marker),
                            self.wait(a, lambda text: '搜索：' in text and history_marker in text)),
                           conversation=current['direct_id'])
            self.screenshot(current, 'reaccepted-history')
            self.key(current, 'Escape')
            self.open_chat(current, target['username'], current['direct_id'])
        self.send(actor, extra='reaccepted 双向 😀')
        self.send(peer, extra='reaccepted 回信 😀')

    def image_attachments(self, actor, peer):
        header = (Path(__file__).parent / 'avatar_fixture.hpp').read_text()
        for media, suffix in (('png', 'png'), ('jpeg', 'jpg')):
            definition = re.search(r'avatar_' + media + r'_base64\s*=\s*(.*?);', header, re.S)
            if not definition:
                raise AssertionError('Existing image fixture is missing')
            encoded = ''.join(re.findall(r'"([^"\n]*)"', definition.group(1)))
            payload = base64.b64decode(encoded)
            source = self.work / ('图像 ' + str(actor['id']) + '.' + suffix)
            target = self.work / ('saved-image-' + str(peer['id']) + '.' + suffix)
            source.write_bytes(payload)
            if target.exists():
                raise AssertionError('Image download destination is not new')
            self.operation(actor, media + '-send', lambda:
                           (self.command(actor, 'file ' + str(source)), self.wait(actor, '附件已发送')),
                           conversation=actor['direct_id'])
            self.wait(peer, lambda text: '[图片]' in text and source.name in text)
            self.select_latest(peer)
            self.operation(peer, media + '-download', lambda:
                           (self.command(peer, 'save ' + str(target)), self.wait(peer, '附件已保存：')),
                           conversation=peer['direct_id'])
            if target.read_bytes() != payload:
                raise AssertionError('Native image attachment content mismatch')
            self.emit('attachment-verified', actor=peer['id'], media=media,
                      sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload),
                      source=str(source), target=str(target))
            self.screenshot(peer, media + '-downloaded')
            self.focus_compose(peer)

    def choose_member(self, actor, target):
        self.command(actor, 'members')
        self.wait(actor, '群成员 (')
        self.key(actor, 'Up', repeat=25)
        self.key_barrier(actor)
        for _ in range(18):
            if target['username'] in inverse_text(self.capture(actor, True)):
                return
            self.key(actor, 'Down')
            time.sleep(.04)
        raise AssertionError('Expected group member is not keyboard reachable')

    def contact_search(self, actor, peer):
        self.command(actor, 'contacts')
        self.wait(actor, lambda text: '联系人' in text and '新的朋友' in text)
        self.operation(actor, 'contact-search-match', lambda:
                       (self.command(actor, 'filter ' + peer['username']),
                        self.wait(actor, '/ 搜索 · ' + peer['username']),
                        self.key(actor, 'Down'),
                        self.wait(actor, lambda text: peer['username'] in inverse_text(self.capture(actor, True)))))
        self.screenshot(actor, 'contact-search-match')
        absent = 'none_' + self.tag
        self.operation(actor, 'contact-search-no-match', lambda:
                       (self.command(actor, 'filter ' + absent),
                        self.wait(actor, lambda text: '没有匹配的好友' in text and peer['username'] not in text)))
        def clear():
            self.command(actor, 'filter')
            self.wait(actor, '搜索已接受的好友 · 留空显示全部')
            self.key(actor, 'Home')
            self.key(actor, 'DC', repeat=512)
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: peer['username'] in text and absent not in text)
        self.operation(actor, 'contact-search-clear', clear)

    def group_remove_invite(self, actor, peer, marker):
        total = int(actor['member_count'])
        self.open_chat(peer, peer['group_title'], peer['group_id'])
        self.wait(peer, lambda text: self.body_marker_visible(peer, marker))
        self.choose_member(actor, peer)
        if peer['role'] == 'admin':
            self.operation(actor, 'demote-before-remove', lambda:
                           (self.command(actor, 'admin'), self.wait(actor, lambda text:
                            peer['username'] in inverse_text(self.capture(actor, True)) and
                            '管理员' not in inverse_text(self.capture(actor, True)))),
                           conversation=actor['group_id'])
        def remove():
            self.command(actor, 'kick')
            self.wait(actor, '移除群成员 ' + peer['username'])
            self.wait(actor, '输入 y 后按 Enter 确认')
            self.paste(actor, 'y')
            self.key(actor, 'Enter')
            self.wait(actor, '群成员 (' + str(total - 1) + ')')
            self.wait(peer, lambda text: not chat_open(text))
            self.command(peer, 'chats')
            self.wait(peer, lambda text: peer['group_title'] not in text)
            self.screenshot(peer, 'removed-group-hidden')
        self.operation(actor, 'group-remove-member', remove, conversation=actor['group_id'])
        self.open_chat(actor, actor['group_title'], actor['group_id'])
        self.wait(actor, str(total - 1) + ' 位成员')
        def invite():
            self.command(actor, 'invite')
            self.wait(actor, '选择好友')
            self.command(actor, 'filter ' + peer['username'])
            self.wait(actor, lambda text: peer['username'] in inverse_text(self.capture(actor, True)))
            self.key(actor, 'Space')
            self.wait(actor, lambda text: '[x]' in inverse_text(self.capture(actor, True)) and
                      peer['username'] in inverse_text(self.capture(actor, True)))
            self.key(actor, 'Enter')
            self.wait(actor, '群成员 (' + str(total) + ')')
            self.wait(peer, peer['group_title'])
        self.operation(actor, 'group-invite-friend', invite, conversation=actor['group_id'])
        self.open_chat(peer, peer['group_title'], peer['group_id'])
        self.wait(peer, str(total) + ' 位成员')
        self.operation(peer, 'rejoined-group-history', lambda:
                       (self.command(peer, 'search ' + marker),
                        self.wait(peer, lambda text: '搜索：' in text and self.body_marker_visible(peer, marker))),
                       conversation=peer['group_id'])
        self.screenshot(peer, 'rejoined-group-history')
        self.key(peer, 'Escape')
        self.open_chat(actor, actor['group_title'], actor['group_id'])
        self.wait(actor, str(total) + ' 位成员')
        if peer['role'] == 'admin':
            self.choose_member(actor, peer)
            self.operation(actor, 'restore-admin-after-rejoin', lambda:
                           (self.command(actor, 'admin'), self.wait(actor, lambda text:
                            peer['username'] in inverse_text(self.capture(actor, True)) and
                            '管理员' in inverse_text(self.capture(actor, True)))),
                           conversation=actor['group_id'])
            self.screenshot(actor, 'rejoined-admin-restored')

    def group_mutations(self, actor, peer):
        if actor['role'] != 'owner' or peer['group_id'] != actor['group_id']:
            self.emit('not-verified', kinds=['group-pin', 'announcement', 'group-admin'],
                      reason='This shard has no local owner/peer pair')
            return
        self.open_chat(actor, actor['group_title'], actor['group_id'])
        marker = self.send(actor, extra='pinned 群消息 😀', kind='group-message')
        self.select_latest(actor)
        self.operation(actor, 'group-pin', lambda:
                       (self.command(actor, 'pin-message'), self.wait(actor, '置顶：' + marker)),
                       conversation=actor['group_id'])
        self.command(actor, 'group')
        self.wait(actor, '置顶：' + marker)
        self.screenshot(actor, 'group-pin-details-consistent')
        self.operation(actor, 'group-unpin', lambda:
                       (self.command(actor, 'unpin-message'), self.wait(actor, lambda text: '置顶：' + marker not in text)),
                       conversation=actor['group_id'])
        notice = 'Native 公告 中文 😀'
        self.operation(actor, 'group-announcement', lambda:
                       (self.command(actor, 'announcement ' + notice), self.wait(actor, '公告：' + notice)),
                       conversation=actor['group_id'])
        self.screenshot(actor, 'group-announcement')
        def clear():
            self.command(actor, 'announcement')
            self.wait(actor, '编辑公告 · 留空清除')
            self.key(actor, 'Home')
            self.key(actor, 'DC', repeat=512)
            self.key(actor, 'Enter')
            self.wait(actor, '公告：（无）')
        self.operation(actor, 'group-announcement-clear', clear, conversation=actor['group_id'])
        if peer['role'] == 'admin':
            self.choose_member(actor, peer)
            self.operation(actor, 'demote-admin', lambda:
                           (self.command(actor, 'admin'), self.wait(actor, lambda text:
                            peer['username'] in inverse_text(self.capture(actor, True)) and
                            '管理员' not in inverse_text(self.capture(actor, True)))),
                           conversation=actor['group_id'])
            self.choose_member(actor, peer)
            selected = inverse_text(self.capture(actor, True))
            if '管理员' in selected:
                raise AssertionError('Native member row did not demote admin')
            self.operation(actor, 'promote-admin', lambda:
                           (self.command(actor, 'admin'), self.wait(actor, lambda text:
                            peer['username'] in inverse_text(self.capture(actor, True)) and
                            '管理员' in inverse_text(self.capture(actor, True)))),
                           conversation=actor['group_id'])
            self.choose_member(actor, peer)
            self.wait(actor, lambda text: '管理员' in inverse_text(self.capture(actor, True)))
            self.screenshot(actor, 'admin-restored')
        self.group_remove_invite(actor, peer, marker)
        for current, target in ((actor, peer), (peer, actor)):
            self.open_chat(current, target['username'], current['direct_id'])

    def deep_checks(self):
        actor = self.actors[0]
        peer = self.by_id.get(int(actor['peer_id']))
        self.palette(actor, '联系人', lambda text: '联系人' in text and '新的朋友' in text)
        self.wait(actor, actor['peer_username'])
        self.screenshot(actor, 'contacts')
        if peer:
            self.contact_search(actor, peer)
        self.operation(actor, 'friend-requests-view', lambda: self.palette(actor, '新的朋友', '收到 ('))
        self.operation(actor, 'account-menu', lambda: self.palette(actor, '账号', '退出登录'))
        self.key(actor, 'Escape')
        self.open_chat(actor, actor['group_title'], actor['group_id'])
        self.operation(actor, 'members-view', lambda: self.palette(actor, '群成员', '群成员 ('))
        self.screenshot(actor, 'group-members')
        self.key(actor, 'Escape')
        self.command(actor, 'group')
        self.wait(actor, '我的身份：')
        self.screenshot(actor, 'group-details')
        self.key(actor, 'Escape')
        self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        original = self.send(actor, extra='deep ASCII 中文 😀')
        self.select_latest(actor)
        self.key(actor, 'Enter')
        self.wait(actor, lambda text: '编辑' in text and '删除' in text)
        self.screenshot(actor, 'message-menu')
        self.key(actor, 'e')
        self.wait(actor, '正在编辑消息')
        edited = original + '_EDIT'
        def edit():
            self.key(actor, 'Home')
            self.key(actor, 'DC', repeat=512)
            self.paste(actor, edited + ' 编辑中文 😀')
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: edited in self.history_text(actor) and '正在编辑消息' not in text)
        self.operation(actor, 'edit', edit, marker=original, conversation=actor['direct_id'])
        if peer:
            self.receive(peer, edited, actor['id'])
        self.select_latest(actor)
        self.key(actor, 'Enter')
        self.wait(actor, '回复')
        self.key(actor, 'r')
        # The open menu itself contains '回复'; await the actual composer
        # after r before deciding which control should receive the draft.
        self.wait(actor, lambda text: composing(text) and '回复 ' in text)
        self.send(actor, extra='回复中文 😀', kind='reply-message')
        self.select_latest(actor)
        self.operation(actor, 'reaction', lambda: (self.command(actor, 'reaction 1'), self.wait(actor, '回应已更新')),
                       conversation=actor['direct_id'])
        if peer:
            self.wait(peer, '👍')
        self.operation(actor, 'search-messages', lambda: (self.command(actor, 'search ' + original),
                       self.wait(actor, lambda text: '搜索：' in text and edited in text)), conversation=actor['direct_id'])
        self.screenshot(actor, 'message-search')
        self.operation(actor, 'message-search-copy', lambda:
                       (self.key(actor, 'y'), self.wait(actor, lambda text:
                        '可复制文本' in text and edited in text)), conversation=actor['direct_id'])
        self.screenshot(actor, 'message-search-copy')
        self.key(actor, 'Escape')
        self.wait(actor, lambda text: '搜索：' in text and edited in text)
        self.key(actor, 'Escape')
        self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        deletion = self.send(actor, extra='DELETE fixture')
        self.select_latest(actor)
        self.key(actor, 'Enter')
        self.wait(actor, '删除')
        self.key(actor, 'd')
        self.wait(actor, '输入 y 后按 Enter 确认')
        def delete():
            self.paste(actor, 'y')
            self.key(actor, 'Enter')
            self.wait(actor, '消息已删除')
        self.operation(actor, 'delete', delete, marker=deletion, conversation=actor['direct_id'])
        self.screenshot(actor, 'after-delete')
        if peer:
            source = self.work / ('附件 ' + str(actor['id']) + '.txt')
            payload = b'Native TUI attachment\n' + '中文 😀'.encode()
            source.write_bytes(payload)
            target = self.work / ('saved-' + str(peer['id']) + '.txt')
            if target.exists():
                raise AssertionError('This native download target already exists')
            self.operation(actor, 'attachment-send', lambda: (self.command(actor, 'file ' + str(source)),
                           self.wait(actor, '附件已发送')), conversation=actor['direct_id'])
            self.wait(peer, source.name)
            self.select_latest(peer)
            self.operation(peer, 'attachment-download', lambda: (self.command(peer, 'save ' + str(target)),
                           self.wait(peer, '附件已保存：')), conversation=peer['direct_id'])
            if target.read_bytes() != payload:
                raise AssertionError('Native TUI attachment SHA256/content mismatch')
            self.emit('attachment-verified', actor=peer['id'],
                      sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload),
                      source=str(source), target=str(target))
            self.operation(peer, 'attachment-refuse-overwrite', lambda: (self.command(peer, 'save ' + str(target)),
                           self.wait(peer, '已有文件不会覆盖')), conversation=peer['direct_id'])
            if target.read_bytes() != payload:
                raise AssertionError('Existing native download was overwritten')
            self.focus_compose(peer)
        if peer:
            self.image_attachments(actor, peer)
            self.friendship_checks(actor, peer, edited)
            self.group_mutations(actor, peer)
        # Keep claims narrow. These extra scenarios require coordinator-planned
        # actors and server outage windows, not synthetic RPCs inside a worker.
        covered = {'daily_pilot': ['duplicate-login'], 'blackhole_pilot': ['network-loss'],
                   'job_control_pilot': ['Ctrl+Z/fg'], 'lifecycle_pilot': ['server-restart']}
        kinds = ['group-owner-transfer', 'conversation-search', 'search-result-location']
        kinds += [kind for scenario, scope in covered.items() if not self.config.get(scenario, False)
                  for kind in scope]
        self.emit('not-verified', kinds=kinds,
                  reason='Not part of this worker path; requires separately scheduled native UI scenario')

    def proof(self, kind, **context):
        self.emit('proof-request', kind=kind, **context)
        answer = json.loads(sys.stdin.readline())
        if answer.get('command') != 'proof' or answer.get('kind') != kind:
            raise RuntimeError('Unexpected answer to a native proof request')
        return answer

    def unread_row(self, actor, username, label):
        # 列表仅显示名称；由协调器通过现有只读数据库连接确认未读。
        self.key(actor, 'Up', repeat=25)
        self.key_barrier(actor)
        for _ in range(18):
            selected = inverse_text(self.capture(actor, True))
            if any(sidebar_title_matches(line.strip(' │'), username) for line in selected.splitlines()):
                proof = self.proof('read-position', conversation=actor['direct_id'], member=actor['id'])
                assert proof['unread'] > 0, 'Selected conversation must have authoritative unread messages'
                self.emit('unread-observed', actor=actor['id'], conversation=actor['direct_id'],
                          read_position=proof['read_position'], unread=proof['unread'])
                self.screenshot(actor, label)
                return
            self.key(actor, 'Down')
            self.wait(actor, lambda text: inverse_text(self.capture(actor, True)) != selected, timeout=3)
        raise AssertionError('Unread direct row was not keyboard reachable: ' + username)

    def daily_read_typing(self, actor, peer):
        conversation = actor['direct_id']
        for current in (actor, peer):
            self.open_chat(current, current['peer_username'], conversation)
        self.command(peer, 'chats')
        self.active[int(peer['id'])] = None
        marker = self.send(actor, extra='unread observation 中文 😀')
        self.operation(peer, 'direct-unread-visible',
                       lambda: self.unread_row(peer, actor['username'], 'daily-unread-chat-row'),
                       marker=marker, conversation=conversation)
        self.open_chat(peer, peer['peer_username'], conversation)
        self.receive(peer, marker, actor['id'])
        def receipt(value, expected):
            lines = self.history_text(actor, True).splitlines()
            for index, line in enumerate(lines):
                if re.search(r'(?:^|[│\s])' + re.escape(value) + r'(?=[\s│┃]|$)', CSI.sub('', line)):
                    return any(receipt_metadata_matches(candidate, expected) for candidate in lines[index+1:index+4])
            return False
        self.operation(actor, 'direct-read-visible', lambda:
                       self.wait(actor, lambda text: receipt(marker, '已读')),
                       marker=marker, conversation=conversation)
        self.screenshot(actor, 'daily-direct-message-read')
        self.emit('read-observed', actor=actor['id'], reader=peer['id'], marker=marker,
                  conversation=conversation, native_state='已读')
        self.focus_messages(peer)
        self.key(peer, 'PPage')
        self.wait(peer, '正在浏览历史')
        self.active[int(peer['id'])] = None
        older_marker = self.send(actor, extra='history remains unread 中文 😀')
        self.operation(actor, 'old-history-new-message-unread', lambda:
                       self.wait(actor, lambda text: receipt(older_marker, '已发送')),
                       marker=older_marker, conversation=conversation)
        self.screenshot(peer, 'daily-browsing-history')
        self.select_latest(peer)
        self.receive(peer, older_marker, actor['id'])
        self.active[int(peer['id'])] = conversation
        self.operation(actor, 'history-G-read-visible', lambda:
                       self.wait(actor, lambda text: receipt(older_marker, '已读')),
                       marker=older_marker, conversation=conversation)
        self.emit('read-observed', actor=actor['id'], reader=peer['id'], marker=older_marker,
                  conversation=conversation, native_state='已读', after_history_G=True)
        self.focus_compose(actor)
        draft = 'typing draft 中文 😀'
        def typing_start():
            self.paste(actor, draft)
            self.wait(peer, actor['username'] + ' 正在输入…')
            self.screenshot(peer, 'daily-typing-visible')
        self.operation(actor, 'typing-visible-to-peer', typing_start, conversation=conversation)
        self.operation(actor, 'typing-Escape-clears-preserves-draft', lambda:
                       (self.key(actor, 'Escape'), self.wait(actor, draft),
                        self.wait(peer, lambda text: '正在输入…' not in text)), conversation=conversation)
        # Esc returns to Chats; wide Tab would stop at message selection first.
        # Reopen through the native list and verify the preserved draft before
        # editing it again, rather than changing the ordinary focus helper.
        self.open_chat(actor, actor['peer_username'], conversation)
        self.operation(actor, 'typing-Escape-reopen-preserves-draft', lambda:
                       self.wait(actor, lambda text: composing(text) and any(draft in line
                                 for line in text.splitlines() if prompt_line(line))), conversation=conversation)
        self.key(actor, 'End')
        self.paste(actor, ' idle')
        self.wait(peer, actor['username'] + ' 正在输入…')
        self.operation(actor, 'typing-idle-expiry', lambda:
                       self.wait(peer, lambda text: '正在输入…' not in text, timeout=10), conversation=conversation)
        self.key(actor, 'Home')
        self.key(actor, 'DC', repeat=512)
        self.wait(actor, lambda text: not any(draft in line for line in text.splitlines() if prompt_line(line)))

    def daily_avatar_transfer(self, actor, peer):
        self.command(actor, 'my-profile')
        self.wait(actor, '头像：默认')
        header = (Path(__file__).parent / 'avatar_fixture.hpp').read_text()
        for media, suffix in (('png', 'png'), ('jpeg', 'jpg')):
            definition = re.search(r'avatar_' + media + r'_base64\s*=\s*(.*?);', header, re.S)
            if not definition:
                raise AssertionError('Existing avatar fixture is missing')
            payload = base64.b64decode(''.join(re.findall(r'"([^"\n]*)"', definition.group(1))))
            path = self.work / ('daily-avatar.' + suffix)
            path.write_bytes(payload)
            self.operation(actor, 'avatar-' + media + '-set', lambda p=path:
                           (self.command(actor, 'avatar ' + str(p)), self.wait(actor, '头像已更新'),
                            self.wait(actor, '头像：已设置')))
            self.screenshot(actor, 'daily-avatar-' + media)
        self.operation(actor, 'avatar-clear', lambda:
                       (self.command(actor, 'avatar-clear'), self.wait(actor, '头像已清除'),
                        self.wait(actor, '头像：默认')))
        self.screenshot(actor, 'daily-avatar-cleared')
        if actor['role'] != 'owner' or peer['role'] != 'admin' or actor['group_id'] != peer['group_id']:
            self.emit('not-verified', kinds=['group-owner-transfer'], reason='Daily pilot has no owner/admin pair')
            return False
        for owner, target, kind in ((actor, peer, 'owner-transfer-to-admin'),
                                    (peer, actor, 'owner-transfer-restore')):
            self.open_chat(owner, owner['group_title'], owner['group_id'])
            self.choose_member(owner, target)
            self.wait(owner, lambda text: target['username'] in inverse_text(self.capture(owner, True)) and
                      '管理员' in inverse_text(self.capture(owner, True)))
            def transfer(current=owner, destination=target):
                self.command(current, 'transfer')
                self.wait(current, '将群主身份转让给 ' + destination['username'] + '？')
                self.wait(current, '输入 y 后按 Enter 确认')
                self.paste(current, 'y')
                self.key(current, 'Enter')
                self.choose_member(current, destination)
                self.wait(current, lambda text: destination['username'] in inverse_text(self.capture(current, True)) and
                          '群主' in inverse_text(self.capture(current, True)))
                self.screenshot(current, 'daily-' + kind)
            self.operation(owner, kind, transfer, conversation=owner['group_id'])
        for current in (actor, peer):
            self.open_chat(current, current['peer_username'], current['direct_id'])
        self.send(actor, extra='avatar and owner roles restored 中文 😀')
        return True

    def daily_pilot(self):
        actor, peer = self.actors
        self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        self.open_chat(peer, peer['peer_username'], peer['direct_id'])
        original = self.processes[int(actor['id'])]
        def logout_prompt():
            self.command(actor, 'account')
            self.wait(actor, lambda text: '账号 · ' in text and '退出登录' in text)
            self.key(actor, 'l')
            self.wait(actor, '退出当前账号？')
            self.wait(actor, '输入 y 后按 Enter 确认')
        def cancel_logout():
            logout_prompt()
            self.key(actor, 'Escape')
            self.wait(actor, lambda text: '退出当前账号？' not in text and '已连接' in text)
        self.operation(actor, 'account-logout-cancel', cancel_logout)
        def logout():
            logout_prompt()
            self.paste(actor, 'y')
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: '注册账号' in text and actor['username'] in text and '已连接' not in text)
            self.screenshot(actor, 'account-logged-out')
        self.operation(actor, 'account-logout-confirm', logout)
        def rejected_login(expected, evidence):
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: '注册账号' in text and expected in text and
                      '已连接' not in text, timeout=45)
            current = process_sample(original['pid'])
            if not current.get('alive') or current.get('start_ticks') != original['start_ticks']:
                raise AssertionError('Rejected login replaced the native client process')
            self.wait(peer, lambda text: '已连接' in text and peer['username'] in text)
            self.screenshot(actor, evidence)
        def wrong_password():
            # Password focus is retained by the actual login form after logout;
            # failures clear the password and keep the form available for retry.
            self.paste(actor, self.password + '-invalid')
            rejected_login('用户名或密码错误', 'account-wrong-password')
        self.operation(actor, 'account-wrong-password-rejected', wrong_password)
        def duplicate_login():
            self.key(actor, 'BTab')
            self.key(actor, 'Home')
            self.key(actor, 'DC', repeat=512)
            self.paste(actor, peer['username'])
            self.key(actor, 'Tab')
            self.paste(actor, self.password)
            rejected_login('该账号已在其他地方登录', 'account-duplicate-login-rejected')
        self.operation(actor, 'account-duplicate-login-rejected', duplicate_login)
        def login_again():
            # Return to the original account after the explicit negative cases;
            # neither rejected attempt emits a new authenticated-client event.
            self.key(actor, 'BTab')
            self.key(actor, 'Home')
            self.key(actor, 'DC', repeat=512)
            self.paste(actor, actor['username'])
            self.key(actor, 'Tab')
            # The real input keeps its username/password focus across logout,
            # as verified by the existing native login smoke.
            self.paste(actor, self.password)
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: '已连接' in text and 'Esc 返回' in text, timeout=45)
            current = process_sample(original['pid'])
            if (not current.get('alive') or current.get('start_ticks') != original['start_ticks']):
                raise AssertionError('Logout/login replaced the native client process')
        self.operation(actor, 'account-login-again', login_again)
        self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        self.send(actor, extra='logout login direct 中文 😀')
        for current in self.actors:
            self.open_chat(current, current['group_title'], current['group_id'])
            self.wait(current, str(current['member_count']) + ' 位成员')
        for current in self.actors:
            self.send(current, extra='logout login group 中文 😀', kind='group-message')
        self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        for command, flag in (('mute', '免打扰'), ('pin', '置顶')):
            for enabled in (True, False):
                def toggle(name=command, label=flag, expected=enabled):
                    self.command(actor, name)
                    self.command(actor, 'chats')
                    self.wait(actor, lambda text: (label in text) == expected)
                self.operation(actor, 'personal-' + command + ('-enable' if enabled else '-restore'),
                               toggle, conversation=actor['direct_id'])
                self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        initial = 'Draft ' + self.tag
        final = 'Daily ' + self.tag
        def create():
            self.command(actor, 'create-group')
            self.wait(actor, '选择好友')
            self.command(actor, 'filter ' + peer['username'])
            self.wait(actor, lambda text: peer['username'] in inverse_text(self.capture(actor, True)))
            self.key(actor, 'Space')
            self.wait(actor, lambda text: '[x]' in inverse_text(self.capture(actor, True)))
            self.screenshot(actor, 'daily-create-friend-selected')
            self.key(actor, 'Enter')
            self.wait(actor, '创建群聊 · 下一步：群名称')
            self.paste(actor, initial)
            self.wait(actor, lambda text: '创建群聊 · 下一步：群名称' in text and initial in text)
            self.screenshot(actor, 'daily-create-title-input')
            self.key(actor, 'Enter')
            self.wait(actor, '创建群聊「' + initial + '」')
            self.wait(actor, '2 位成员？')
            self.wait(actor, '输入 y 后按 Enter 确认')
            self.paste(actor, 'y')
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: initial in text and composing(text) and '2 位成员' in text)
            self.screenshot(actor, 'daily-created-group')
        self.operation(actor, 'create-group-two-step', create)
        self.operation(actor, 'rename-created-group', lambda:
                       (self.command(actor, 'rename ' + final),
                        self.wait(actor, lambda text: final in text and initial not in text)), conversation=final)
        self.active[int(actor['id'])] = final
        self.emit('daily-group-created', actor=actor['id'], peer_id=peer['id'],
                  conversation_title=final, member_count=2)
        self.open_chat(peer, final, final)
        self.open_chat(actor, final, final)
        self.send(actor, extra='新建群 owner 中文 😀', kind='group-message')
        self.send(peer, extra='新建群 peer 中文 😀', kind='group-message')
        self.command(actor, 'group')
        notice = 'Daily 公告 中文 😀'
        self.operation(actor, 'daily-announcement-set', lambda:
                       (self.command(actor, 'announcement ' + notice), self.wait(actor, '公告：' + notice)),
                       conversation=final)
        def clear():
            self.command(actor, 'announcement')
            self.wait(actor, '编辑公告 · 留空清除')
            self.key(actor, 'Home')
            self.key(actor, 'DC', repeat=512)
            self.key(actor, 'Enter')
            self.wait(actor, '公告：（无）')
        self.operation(actor, 'daily-announcement-clear', clear, conversation=final)
        self.command(actor, 'link-create')
        frame = self.wait(actor, lambda text: '可复制文本' in text and
                          re.search(r'[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}-[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}', text))
        code = re.search(r'[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}-[23456789ABCDEFGHJKMNPQRSTUVWXYZ]{4}', frame).group()
        self.private_tokens.extend([code, code.replace('-', '')])
        self.emit('operation', kind='daily-invite-generated', actor=actor['id'],
                  conversation_title=final, ok=True)
        self.operations += 1
        self.key(actor, 'Escape')
        def leave():
            self.command(peer, 'leave')
            self.wait(peer, '退出当前群聊？')
            self.wait(peer, '输入 y 后按 Enter 确认')
            self.paste(peer, 'y')
            self.key(peer, 'Enter')
            self.wait(peer, lambda text: not chat_open(text) and final not in text)
            self.screenshot(peer, 'daily-left-group')
        self.operation(peer, 'daily-leave-group', leave, conversation=final)
        self.open_chat(actor, final, final)
        self.wait(actor, '1 位成员')
        def join():
            self.command(peer, 'join')
            self.wait(peer, '加入群聊 · 输入邀请码')
            self.paste(peer, code)
            self.key(peer, 'Enter')
            self.wait(peer, lambda text: final in text and composing(text) and '2 位成员' in text)
            self.active[int(peer['id'])] = final
            self.screenshot(peer, 'daily-invite-rejoined')
        self.operation(peer, 'daily-join-invite', join, conversation=final)
        self.wait(actor, '2 位成员')
        self.send(actor, extra='invite rejoined owner 中文 😀', kind='group-message')
        self.send(peer, extra='invite rejoined peer 中文 😀', kind='group-message')
        for current in self.actors:
            self.open_chat(current, current['peer_username'], current['direct_id'])
        self.daily_read_typing(actor, peer)
        transferred = self.daily_avatar_transfer(actor, peer)
        self.emit('daily-restored', actors=[current['id'] for current in self.actors],
                  checks=['account-logout-cancel', 'account-logout-login-same-process',
                          'account-wrong-password-rejected', 'account-duplicate-login-rejected', 'mute-pin-restored',
                          'create-group-two-step', 'rename', 'announcement-set-clear', 'leave-invite-join',
                          'direct-read-unread', 'typing-Escape-idle', 'avatar-PNG-JPEG-clear'] +
                         (['owner-transfer-restored'] if transferred else []),
                  created_group_title=final, invite_persisted=False)

    def job_control_pilot(self):
        actor, peer = self.actors
        self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        self.open_chat(peer, peer['peer_username'], peer['direct_id'])
        marker = self.next_marker(actor)
        draft = marker + ' CtrlZ 草稿 中文 😀'
        self.focus_compose(actor)
        self.paste(actor, draft)
        self.wait(actor, lambda text: any(marker in line for line in text.splitlines() if prompt_line(line)))
        self.focus_messages(actor)
        original = self.processes[int(actor['id'])]
        def suspend_and_fg():
            self.key(actor, 'C-z')
            end = time.monotonic() + 5
            while True:
                current = process_sample(original['pid'])
                if current.get('state') == 'T' and current.get('start_ticks') == original['start_ticks']:
                    break
                if not current.get('alive') or time.monotonic() >= end:
                    raise AssertionError('Native Ctrl+Z did not suspend the same client')
                time.sleep(.03)
            self.wait_shell(actor)
            self.paste(actor, 'jobs -l')
            self.key(actor, 'Enter')
            end = time.monotonic() + 5
            while True:
                frame = self.capture(actor)
                current = process_sample(original['pid'])
                owned_job = re.search(r'\[1\][+-]?\s+' + re.escape(str(original['pid'])) + r'\s+', frame)
                if (owned_job and current.get('state') == 'T' and
                        current.get('start_ticks') == original['start_ticks']):
                    break
                if time.monotonic() >= end:
                    raise AssertionError('Interactive jobs output did not identify the stopped client')
                time.sleep(.03)
            stopped = self.work / ('actor-' + str(actor['id'])) / 'terminal-suspended.txt'
            self.paste(actor, 'stty -g > ' + shlex.quote(str(stopped)))
            self.key(actor, 'Enter')
            terminal = self.wait_file(stopped)
            before = self.ttys[int(actor['id'])][0].read_text().strip()
            if not before or terminal != before:
                raise AssertionError('Ctrl+Z did not restore the shell terminal attributes')
            self.screenshot(actor, 'ctrl-z-shell-jobs')
            self.emit('job-control-stopped', actor=actor['id'], pid=original['pid'],
                      starttime=original['start_ticks'], process_state='T', terminal_restored=True)
            self.wait_shell(actor)
            self.paste(actor, 'fg %1')
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: chat_open(text) and marker in text)
            current = process_sample(original['pid'])
            if (not current.get('alive') or current.get('state') == 'T' or
                    current.get('start_ticks') != original['start_ticks']):
                raise AssertionError('fg did not resume the same native client')
            self.key_barrier(actor)
            self.focus_compose(actor)
            self.wait(actor, lambda text: any(marker in line for line in text.splitlines() if prompt_line(line)))
            self.screenshot(actor, 'fg-draft-preserved')
        self.operation(actor, 'native-ctrl-z-fg', suspend_and_fg, conversation=actor['direct_id'])
        def send_preserved():
            self.emit('send-start', actor=actor['id'], marker=marker, conversation=actor['direct_id'])
            self.key(actor, 'Enter')
            self.wait(actor, lambda text: self.body_marker_visible(actor, marker) and
                      not any(marker in line for line in text.splitlines() if prompt_line(line)))
        self.operation(actor, 'send-message', send_preserved, marker=marker, conversation=actor['direct_id'])
        self.receive(peer, marker, actor['id'])
        self.emit('job-control-restored', actor=actor['id'], pid=original['pid'],
                  starttime=original['start_ticks'], draft_preserved=True, peer_received=True,
                  resume_action='native-shell fg %1')

    def lifecycle_pilot(self):
        if len(self.actors) != 2:
            raise ValueError('Lifecycle pilot requires exactly two native actors')
        self.emit('lifecycle-ready', actors=[actor['id'] for actor in self.actors],
                  processes=[{'actor': actor['id'], 'pid': self.processes[int(actor['id'])]['pid'],
                              'starttime': self.processes[int(actor['id'])]['start_ticks']}
                             for actor in self.actors])
        instruction = json.loads(sys.stdin.readline())
        if instruction.get('command') != 'recover':
            raise ValueError('Expected coordinator recovery barrier')
        # The coordinator owns the server restart. These same native processes
        # must recover through the app's existing reconnect and authority refresh.
        for actor in self.actors:
            original = self.processes[int(actor['id'])]
            current = process_sample(original['pid'])
            if (not current.get('alive') or current.get('start_ticks') != original['start_ticks'] or
                    current.get('executable') != str(self.binary)):
                raise AssertionError('Restart replaced or ended a native client process')
            self.wait(actor, '已连接', timeout=60)
            self.open_chat(actor, actor['group_title'], actor['group_id'])
            self.operation(actor, 'reconnect-group-header', lambda a=actor:
                           self.wait(a, str(a['member_count']) + ' 位成员'),
                           conversation=actor['group_id'])
            self.screenshot(actor, 'reconnected-group')
        for actor in self.actors:
            self.send(actor, extra='重连恢复 group 中文 😀', kind='group-message')
        for actor in self.actors:
            self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        for actor in self.actors:
            self.send(actor, extra='重连恢复 direct 中文 😀')
        actor = self.actors[0]
        original = self.processes[int(actor['id'])]
        def pause_and_resume():
            current = process_sample(original['pid'])
            if (not current.get('alive') or current.get('start_ticks') != original['start_ticks'] or
                    current.get('executable') != str(self.binary)):
                raise AssertionError('Refuse to signal an unverified native process')
            try:
                os.kill(original['pid'], signal.SIGSTOP)
                deadline = time.monotonic() + 5
                while True:
                    current = process_sample(original['pid'])
                    if current.get('state') == 'T' and current.get('start_ticks') == original['start_ticks']:
                        self.emit('native-paused', actor=actor['id'], pid=original['pid'],
                                  starttime=original['start_ticks'], process_state=current['state'])
                        self.screenshot(actor, 'sigstop')
                        break
                    if not current.get('alive') or time.monotonic() >= deadline:
                        raise AssertionError('SIGSTOP did not stop the verified native process')
                    time.sleep(.03)
            finally:
                current = process_sample(original['pid'])
                if (current.get('alive') and current.get('start_ticks') == original['start_ticks'] and
                        current.get('executable') == str(self.binary)):
                    os.kill(original['pid'], signal.SIGCONT)
            self.key_barrier(actor)
            self.screenshot(actor, 'sigcont-ui-responsive')
        self.operation(actor, 'native-sigstop-sigcont', pause_and_resume, conversation=actor['direct_id'])
        self.send(actor, extra='SIGCONT resumed 中文 😀')
        self.emit('lifecycle-restored', actors=[item['id'] for item in self.actors],
                  checks=['server-restart', 'same-native-process', 'group-header',
                          'group-message-and-receipt', 'direct-message-and-receipt', 'SIGSTOP/SIGCONT'],
                  not_verified=['Ctrl+Z/fg', 'network-blackhole'])

    def role_pilot(self):
        """A group role comes only from the member list, so opening a group must not claim one."""
        owner, manager, ordinary = self.actors[:3]
        group, title = int(owner['group_id']), owner['group_title']
        if not all(int(actor['group_id']) == group for actor in self.actors[:3]):
            raise AssertionError('Role pilot needs one shared fixture group')
        waiting, refused, prompt = '正在加载群成员', '仅群主和管理员可执行该操作', '修改群名称'
        observed = {'waiting': 0, 'ready': 0, 'leave-confirm': 0, 'leave-waiting': 0,
                    'leave-transfer': 0, 'page-waiting': 0, 'page-loaded': 0}

        def answer(actor, label):
            # The first answer the client shows is the observation: the loading text only exists
            # while the member request is still in flight, so it is polled, never forced.
            for _ in range(60):
                frame = self.capture(actor)
                for word, name in ((waiting, 'waiting'), (prompt, 'prompt'), (refused, 'refused')):
                    if word in frame:
                        self.screenshot(actor, f'{label}-{name}')
                        return name, frame
                time.sleep(.05)
            raise AssertionError('The group management command produced no answer')

        def ask(actor, label, manager_role):
            # Leaving and re-entering the group is what clears the member list. The whole gesture
            # then goes out in one send-keys, so the command can run before the new member
            # response arrives. Waiting or the real prompt are both right for a manager; being
            # told it has no permission is not, because the role has not been learned yet.
            self.open_chat(actor, actor['peer_username'], actor['direct_id'])
            self.open_chat(actor, title, group)
            self.key(actor, 'BTab', ':', 'r', 'e', 'n', 'a', 'm', 'e', 'Enter')
            name, frame = answer(actor, 'role-' + label)
            self.key(actor, 'Escape')
            if refused in frame and manager_role:
                raise AssertionError('A manager was told it has no permission while its role was unknown')
            if name == 'waiting':
                observed['waiting'] += 1
            else:
                observed['ready'] += 1

        def identity(actor, expected, label):
            self.command(actor, 'group')
            self.wait(actor, '我的身份：' + expected)
            self.screenshot(actor, 'identity-' + label)
            self.key(actor, 'Escape')

        for round_ in range(3):
            for actor, label, manager_role in ((owner, 'owner', True), (manager, 'admin', True),
                                              (ordinary, 'member', False)):
                self.operation(actor, 'role-command-while-unknown',
                               lambda a=actor, l=label, m=manager_role: ask(a, f'{l}-{round_}', m))
        for actor, expected, label in ((owner, '群主', 'owner'), (manager, '管理员', 'admin'),
                                       (ordinary, '成员', 'member')):
            self.operation(actor, 'role-authoritative-identity',
                           lambda a=actor, e=expected, l=label: identity(a, e, l), conversation=group)

        def refuses(actor, label):
            self.command(actor, 'rename')
            self.wait(actor, refused)
            self.screenshot(actor, label)
            self.key(actor, 'Escape')

        def allows(actor, label):
            self.command(actor, 'rename')
            self.wait(actor, prompt)
            self.screenshot(actor, label)
            self.key(actor, 'Escape')

        self.operation(ordinary, 'role-refuses-ordinary-member',
                       lambda: refuses(ordinary, 'rename-refused-member'), conversation=group)
        self.operation(owner, 'role-allows-owner', lambda: allows(owner, 'rename-prompt-owner'), conversation=group)

        def first_answer(actor, label, outcomes):
            # The first answer the client shows is the observation: a waiting line only exists
            # while the member request is still in flight, so it is polled, never forced.
            for _ in range(60):
                frame = self.capture(actor)
                for word, name in outcomes:
                    if word in frame:
                        self.screenshot(actor, f'{label}-{name}')
                        return name, frame
                time.sleep(.05)
            raise AssertionError('The native command produced no answer: ' + label)

        def leave_request(actor, label, owner_role):
            # Leaving is the permission question the owner must never be asked, so a confirmation
            # here while the role is unknown is the defect.
            self.open_chat(actor, actor['peer_username'], actor['direct_id'])
            self.open_chat(actor, title, group)
            self.key(actor, 'BTab', ':', 'l', 'e', 'a', 'v', 'e', 'Enter')
            name, _ = first_answer(actor, 'leave-' + label, (('退出当前群聊？', 'confirm'),
                                                             ('正在加载群成员', 'waiting'),
                                                             ('群主须先转让', 'transfer')))
            self.key(actor, 'Escape')
            if name == 'confirm' and owner_role:
                raise AssertionError('The owner was asked to confirm leaving while its role was unknown')
            observed['leave-' + name] += 1

        def group_page(actor, label, expected):
            # The group page is where a member count, a subset preview and a leave action would
            # appear, so only the state the frame actually reached is asserted against.
            self.open_chat(actor, actor['peer_username'], actor['direct_id'])
            self.open_chat(actor, title, group)
            self.key(actor, 'BTab', ':', 'g', 'r', 'o', 'u', 'p', 'Enter')
            name, frame = first_answer(actor, 'group-page-' + label,
                                       (('群成员信息加载中', 'waiting'), ('我的身份：', 'loaded')))
            self.key(actor, 'Escape')
            if name == 'waiting':
                if '成员 0 人' in frame or '退出群聊' in frame:
                    raise AssertionError('The group page showed a member count or a leave action it cannot know')
                observed['page-waiting'] += 1
            elif '我的身份：' + expected not in frame:
                raise AssertionError('The group page did not show the real role for ' + label)
            else:
                observed['page-loaded'] += 1

        for actor, label, owner_role, expected in ((owner, 'owner', True, '群主'),
                                                   (manager, 'admin', False, '管理员'),
                                                   (ordinary, 'member', False, '成员')):
            self.operation(actor, 'role-leave-while-unknown',
                           lambda a=actor, l=label, o=owner_role: leave_request(a, l, o), conversation=group)
            self.operation(actor, 'role-group-page-while-unknown',
                           lambda a=actor, l=label, e=expected: group_page(a, l, e), conversation=group)

        def leave_as_owner():
            self.command(owner, 'leave')
            self.wait(owner, '群主须先转让')
            self.screenshot(owner, 'leave-owner-refused')
        self.operation(owner, 'role-owner-cannot-leave', leave_as_owner, conversation=group)

        def leave_as_manager(actor, label):
            self.command(actor, 'leave')
            self.wait(actor, '退出当前群聊？')
            self.screenshot(actor, label)
            self.key(actor, 'Escape')
        self.operation(manager, 'role-admin-can-leave',
                       lambda: leave_as_manager(manager, 'leave-admin-confirms'), conversation=group)
        self.operation(ordinary, 'role-member-can-leave',
                       lambda: leave_as_manager(ordinary, 'leave-member-confirms'), conversation=group)

        def demote():
            self.choose_member(owner, manager)
            self.command(owner, 'admin')
            self.key_barrier(owner)
            self.screenshot(owner, 'demoted-admin')
        self.operation(owner, 'role-owner-demotes-admin', demote, conversation=group)

        def demoted():
            self.open_chat(manager, manager['peer_username'], manager['direct_id'])
            self.open_chat(manager, title, group)
            self.command(manager, 'group')
            self.wait(manager, '我的身份：成员')
            self.screenshot(manager, 'identity-admin-after-demotion')
            self.key(manager, 'Escape')
            refuses(manager, 'rename-refused-after-demotion')
        self.operation(manager, 'role-demoted-member-refused', demoted, conversation=group)

        def restore():
            self.choose_member(owner, manager)
            self.command(owner, 'admin')
            self.key_barrier(owner)
            self.screenshot(owner, 'role-restored-admin')
        self.operation(owner, 'role-owner-restores-admin', restore, conversation=group)

        def restored():
            self.open_chat(manager, manager['peer_username'], manager['direct_id'])
            self.open_chat(manager, title, group)
            self.command(manager, 'group')
            self.wait(manager, '我的身份：管理员')
            self.screenshot(manager, 'identity-admin-restored')
            self.key(manager, 'Escape')
        self.operation(manager, 'role-restored-admin-identity', restored, conversation=group)

        def narrow():
            # A narrow terminal is where the group page would have to truncate a count or an
            # identity line, so the real window is resized rather than a state fixture.
            self.tmux('resize-window', '-t', self.pane(owner), '-x', '60', '-y', '30')
            self.key_barrier(owner)
            self.command(owner, 'group')
            self.wait(owner, '我的身份：群主')
            self.screenshot(owner, 'group-page-60-columns')
            self.key(owner, 'Escape')
            self.tmux('resize-window', '-t', self.pane(owner), '-x', '120', '-y', '40')
            self.key_barrier(owner)
        self.operation(owner, 'role-group-page-narrow', narrow, conversation=group)
        # Hand the three clients back to the shared flow in the state it expects.
        for current in self.actors[:3]:
            self.open_chat(current, current['peer_username'], current['direct_id'])
        self.emit('role-pilot-observed', group=group, unknown_window=observed['waiting'],
                  already_loaded=observed['ready'], observations=observed)

    def blackhole_pilot(self):
        """One client silently loses its link; the peer keeps working and both catch up."""
        isolated, peer = self.actors
        relay = self.relays[int(isolated['id'])]
        conversation = isolated['direct_id']
        lost, online = '正在重连', '● 已连接'
        draft = 'blackhole draft 中文 😀'

        def baseline():
            marker = self.send(peer, extra='blackhole baseline 中文 😀')
            self.receive(isolated, marker, peer['id'])
        self.operation(isolated, 'blackhole-baseline', baseline, conversation=conversation)

        def drop():
            relay.pause()
            # Nothing is closed, so only the client's own probe can notice. The wait is bounded
            # by the heartbeat interval plus its grace, never by a fixed sleep.
            self.wait(isolated, lost, timeout=90)
            self.screenshot(isolated, 'blackhole-link-lost')
        self.operation(isolated, 'blackhole-silent-link-loss', drop, conversation=conversation)

        def while_offline():
            self.command(isolated, 'chats')
            self.active[int(isolated['id'])] = None
            return [self.send(peer, extra=f'blackhole missed {index} 中文 😀') for index in (1, 2)]
        missed = self.operation(peer, 'blackhole-peer-sends-while-offline', while_offline,
                                conversation=conversation)
        def restore():
            relay.resume()
            self.wait(isolated, online, timeout=45)
            self.screenshot(isolated, 'blackhole-link-restored')
        self.operation(isolated, 'blackhole-link-restored', restore, conversation=conversation)
        self.operation(isolated, 'blackhole-unread-after-reconnect',
                       lambda: self.unread_row(isolated, peer['username'], 'blackhole-unread-row'),
                       conversation=conversation)
        self.operation(isolated, 'blackhole-open-after-reconnect',
                       lambda: self.open_chat(isolated, peer['username'], conversation),
                       conversation=conversation)
        for marker in missed:
            self.operation(isolated, 'blackhole-missed-message',
                           lambda value=marker: self.receive(isolated, value, peer['id']),
                           marker=marker, conversation=conversation)

        def typing():
            self.focus_compose(isolated)
            self.paste(isolated, draft)
            self.wait(peer, isolated['username'] + ' 正在输入…')
            self.screenshot(peer, 'blackhole-typing-visible')
        self.operation(isolated, 'blackhole-typing-visible', typing, conversation=conversation)
        self.operation(isolated, 'blackhole-draft-cleared', lambda: (
            self.key(isolated, 'Home'), self.key(isolated, 'DC', repeat=512),
            self.wait(isolated, lambda text: not any(draft in line for line in text.splitlines()
                                                     if prompt_line(line)))), conversation=conversation)
        marker = self.send(isolated, extra='blackhole after reconnect 中文 😀')
        self.operation(peer, 'blackhole-peer-receives-after-reconnect',
                       lambda: self.receive(peer, marker, isolated['id']), marker=marker,
                       conversation=conversation)
        self.emit('blackhole-pilot-observed', isolated=isolated['id'], peer=peer['id'],
                  conversation=conversation, missed=missed, relayed_bytes=relay.relayed)

    def steady(self):
        self.started_at = time.monotonic()
        self.running = True
        # Every independent UI visits a group and its direct; no idle client is
        # counted merely because a process or socket exists.
        for actor in self.actors:
            self.open_chat(actor, actor['group_title'], actor['group_id'])
            self.operation(actor, 'group-header', lambda a=actor: self.wait(a, str(a['member_count']) + ' 位成员'),
                           conversation=actor['group_id'])
        groups = {}
        for actor in self.actors:
            groups.setdefault(actor['group_id'], []).append(actor)
        for conversation, members in groups.items():
            sender = members[0]
            marker = self.send(sender, extra='group 中文 😀', kind='group-message')
            for receiver in members[1:]:
                self.receive(receiver, marker, sender['id'])
        medium_groups = {}
        for actor in self.actors:
            if all(actor.get(key) is not None for key in ('medium_id', 'medium_title', 'medium_member_count')):
                self.open_chat(actor, actor['medium_title'], actor['medium_id'])
                self.operation(actor, 'medium-group-header', lambda a=actor:
                               self.wait(a, str(a['medium_member_count']) + ' 位成员'),
                               conversation=actor['medium_id'])
                medium_groups.setdefault(actor['medium_id'], []).append(actor)
        for conversation, members in medium_groups.items():
            sender = members[0]
            marker = self.send(sender, extra='medium 群 中文 😀', kind='group-message')
            for receiver in members[1:]:
                self.receive(receiver, marker, sender['id'])
        for actor in self.actors:
            self.open_chat(actor, actor['peer_username'], actor['direct_id'])
        for index, actor in enumerate(self.actors):
            self.send(actor, 'ASCII 中文 😀 ? : / a r e d' if index % 2 else '多行首行\n第二行中文 😀')
        if self.config.get('deep_checks', True):
            self.deep_checks()
        if self.config.get('daily_pilot', False):
            self.daily_pilot()
        if self.config.get('role_pilot', False):
            self.role_pilot()
        if self.config.get('blackhole_pilot', False):
            self.blackhole_pilot()
        if self.config.get('job_control_pilot', False):
            self.job_control_pilot()
        if self.config.get('lifecycle_pilot', False):
            self.lifecycle_pilot()
        if self.config.get('steady_gate', False):
            self.emit('steady-ready', actors=[a['id'] for a in self.actors],
                      warmup_seconds=time.monotonic()-self.started_at)
            instruction = json.loads(sys.stdin.readline())
            if instruction.get('command') != 'steady':
                raise ValueError('Expected coordinator steady barrier')
        self.steady_started_at = time.monotonic()
        self.emit('steady-start', actors=[a['id'] for a in self.actors],
                  warmup_seconds=self.steady_started_at-self.started_at)
        next_send = {int(a['id']): time.monotonic() for a in self.actors}
        duration = float(self.config['steady_seconds'])
        while time.monotonic() - self.steady_started_at < duration:
            for index, actor in enumerate(self.actors):
                identity = int(actor['id'])
                now = time.monotonic()
                # 20% active, 40% low-frequency, 40% receive-mostly, all with
                # mandatory initial native sends and navigation above.
                interval = 20 if index % 5 == 0 else 60 if index % 5 in (1, 2) else 180
                if now >= next_send[identity]:
                    self.send(actor)
                    next_send[identity] = now + interval + self.rng.uniform(0, interval/10)
                if time.monotonic() - self.steady_started_at >= duration:
                    break
            time.sleep(.15)
        self.steady_finished_at = time.monotonic()
        self.screenshot(self.actors[0], 'steady-final')
        self.running = False
        if self.config.get('finish_gate', False):
            self.emit('steady-finished', actors=[a['id'] for a in self.actors],
                      steady_seconds=self.steady_finished_at-self.steady_started_at,
                      operations=self.operations, failed_operations=self.failures)
            instruction = json.loads(sys.stdin.readline())
            if instruction.get('command') != 'finish':
                raise ValueError('Expected coordinator finish barrier')

    def close(self):
        for actor in self.actors:
            if int(actor['id']) in self.panes:
                with contextlib.suppress(Exception):
                    original = self.processes.get(int(actor['id']))
                    if self.config.get('job_control_pilot', False) and original:
                        current = process_sample(original['pid'])
                        if current.get('state') == 'T' and current.get('start_ticks') == original['start_ticks']:
                            self.wait_shell(actor)
                            self.paste(actor, 'fg %1')
                            self.key(actor, 'Enter')
                            self.wait(actor, chat_open)
                    self.key(actor, 'C-c')
        deadline = time.monotonic() + 10
        remaining = dict(self.processes)
        while remaining and time.monotonic() < deadline:
            for identity, original in list(remaining.items()):
                state = process_sample(original['pid'])
                if not state.get('alive') or state.get('start_ticks') != original.get('start_ticks'):
                    remaining.pop(identity)
            if remaining:
                time.sleep(.08)
        for identity, original in remaining.items():
            state = process_sample(original['pid'])
            # No generic process-name kill; verify both PID generation and exe.
            if (state.get('alive') and state.get('start_ticks') == original.get('start_ticks') and
                    state.get('executable') == str(self.binary)):
                self.clean = False
                os.kill(original['pid'], signal.SIGTERM)
                self.emit('failure', actor=identity, kind='clean-exit', error='Ctrl+C did not stop owned native client')
        for actor in self.actors:
            identity = int(actor['id'])
            if identity not in self.panes:
                continue
            if self.config.get('job_control_pilot', False) and identity in self.shells:
                try:
                    self.wait_shell(actor)
                    after = self.ttys[identity][1]
                    command = ('native_exit_status=$?; stty -g > ' + shlex.quote(str(after)) +
                               "; printf 'native-exit-%s\\n' \"$native_exit_status\"; exit \"$native_exit_status\"")
                    self.paste(actor, command)
                    self.key(actor, 'Enter')
                    self.wait_file(after)
                except Exception as error:
                    self.clean = False
                    self.emit('failure', actor=identity, kind='job-control-exit', error=str(error))
            with contextlib.suppress(Exception):
                deadline = time.monotonic() + 5
                while True:
                    status = self.tmux('display-message', '-p', '-t', self.pane(actor),
                                       '#{pane_dead},#{pane_dead_status},#{pane_dead_signal}').stdout.strip()
                    if status.startswith('1,') or time.monotonic() >= deadline:
                        break
                    time.sleep(.04)
                self.emit('exited', actor=identity, pane_status=status)
                if status != '1,0,':
                    self.clean = False
            if identity in self.ttys:
                baseline, restored_path = self.ttys[identity]
                try:
                    before = baseline.read_text().strip()
                    after = restored_path.read_text().strip()
                    restored = bool(before) and before == after
                    self.emit('terminal-restored', actor=identity, ok=restored,
                              before=before, after=after, measurement='stty before PTY master closes')
                    self.clean = self.clean and restored
                except Exception as error:
                    self.clean = False
                    self.emit('terminal-restored', actor=identity, ok=False, error=str(error))
        self.tmux('kill-server', check=False)
        for relay in self.relays.values():
            relay.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    config = json.loads(sys.stdin.readline())
    worker = Worker(config)
    def cancelled(_signal, _frame):
        raise Cancelled('Coordinator requested worker cleanup')
    signal.signal(signal.SIGTERM, cancelled)
    signal.signal(signal.SIGINT, cancelled)
    ok = False
    try:
        worker.launch()
        start = json.loads(sys.stdin.readline())
        if start.get('command') != 'start':
            raise ValueError('Expected coordinator start barrier')
        worker.steady()
        ok = True
    except Exception as error:
        worker.emit('failure', kind=type(error).__name__, error=str(error))
        with contextlib.suppress(Exception):
            for actor in worker.actors:
                if int(actor['id']) in worker.panes:
                    worker.screenshot(actor, 'failure')
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        worker.close()
    worker.emit('completed', ok=ok and worker.clean, operations=worker.operations,
                failed_operations=worker.failures, elapsed_seconds=time.monotonic()-worker.started_at if worker.started_at else 0,
                steady_seconds=(worker.steady_finished_at-worker.steady_started_at) if worker.steady_finished_at else 0,
                clean_exit=worker.clean)
    return 0 if ok and worker.clean else 1


if __name__ == '__main__':
    raise SystemExit(main())
