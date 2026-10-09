#!/usr/bin/env python3
"""Small real tmux navigation smoke using the existing SDK/terminal driver."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shlex
import signal
import socket
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from tui_100_member_smoke import Driver, inverse_text, choose_row, chat_open, composing, prompt_line


class NavigationDriver(Driver):
    def setup(self):
        self.start_server()
        with (self.work / 'fixture-stderr.log').open('ab') as output:
            self.helper = subprocess.Popen([self.args.fixture_tool], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,stderr=output,text=True,bufsize=1)
        self.control('configure',url=self.url,prefix=self.args.run_id,password=self.password)
        self.manifest=self.control('seed_navigation')
        (self.work/'manifest.json').write_text(json.dumps(self.redact(self.manifest),ensure_ascii=False,indent=2))
        self.spawn_tui('A')

    def spawn_tui(self, actor):
        command='exec '+shlex.quote(str(self.args.binary))+' '+shlex.quote(self.url)
        if not self.panes:
            result=self.tmux('new-session','-d','-P','-F','#{pane_id}','-s',self.session,
                             '-n',actor,'-x','160','-y','45',command)
        else:
            result=self.tmux('new-window','-d','-P','-F','#{pane_id}','-t',self.session,'-n',actor,command)
        self.panes[actor]=result.stdout.strip()
        self.wait(actor,'登录 / 注册')

    def login(self,actor,first=True):
        self.wait(actor,'登录 / 注册')
        if first:
            self.paste(actor,self.name(actor)); self.keys(actor,'Tab')
        self.paste(actor,self.password); self.keys(actor,'Enter')
        self.wait(actor,lambda s:'已连接' in s and ('聊天' in s or '聊天' in s))

    def chats(self,actor):
        self.command(actor,'conversations')
        self.wait(actor,lambda s:'聊天' in s or '聊天' in s)

    def select_chat(self,actor,title):
        self.chats(actor)
        self.keys(actor,'k',repeat=20)
        self.barrier(actor)
        for _ in range(12):
            if title.split('_',1)[0] in inverse_text(self.capture(actor,styled=True)):
                self.keys(actor,'Enter')
                return
            self.keys(actor,'j')
            self.barrier(actor)
        raise AssertionError('Expected chat was not selectable: '+title)


def capture_terminal(d, actor, label):
    d.screenshot(actor, label)
    path = d.work / f'{d.case_id}-{actor}-{label}.ansi'
    path.write_text(d.redact(d.capture(actor, styled=True)))
    d.observer.capture_reference(d.case_id, path, label + ' styled terminal')


def late_profile_message(d):
    try:
        _late_profile_message(d)
    except Exception:
        capture_terminal(d, 'A', 'failure')
        raise


def _late_profile_message(d):
    from tui_lifecycle_smoke import _eventually, _server_receive_queue

    with d.case('late-profile-message',
                'A delayed peer Message response cannot replace Account opened on the same profile page'):
        d.control('connect', actors=['C'])
        d.find_profile('A', 'C')
        d.wait('A', '发消息')
        d.barrier('A')
        pid = int(d.tmux('display-message', '-p', '-t', d.panes['A'], '#{pane_pid}').stdout.strip())
        assert d.server is not None and d.server.poll() is None
        server_pid = d.server.pid
        # Only this driver's isolated server is stopped. Receive-Q establishes
        # that the real open-direct request cannot complete before Account.
        os.kill(server_pid, signal.SIGSTOP)
        try:
            _eventually(lambda: any(line.startswith('State:') and line.split()[1] in ('T', 't')
                                    for line in Path(f'/proc/{server_pid}/status').read_text().splitlines()),
                        'The isolated server did not enter the stopped state')
            initial_bytes = _server_receive_queue(pid, d.args.port)
            d.command('A', 'message')
            queued = _eventually(lambda: (size if (size := _server_receive_queue(pid, d.args.port)) > initial_bytes else None),
                                 'The real Message RPC did not reach the paused isolated server')
            d.keys('A', 'u')
            d.wait('A', '账号 ·')
            d.wait('A', d.name('A'))
            d.barrier('A')
            capture_terminal(d, 'A', 'account-before-message-response')
            d.evidence('message-rpc-pending', {'tui_pid': pid, 'isolated_server_pid': server_pid,
                                            'server_receive_queue_before': initial_bytes,
                                            'server_receive_queue_after': queued})
        finally:
            os.kill(server_pid, signal.SIGCONT)

        def peer_direct():
            values = d.query('C', 'get_conversations')['conversations']
            return next((v for v in values if v.get('user') == d.manifest['actors']['A']['id']), None)

        direct = _eventually(peer_direct, 'The paused real Message RPC did not create the peer direct')
        d.evidence('message-rpc-server-completed', {'conversation': direct['id']})
        # Poll real terminal output after the server-side effect, allowing both
        # the delayed open callback and its follow-up list response to run.
        observation_seconds = 3
        started = time.monotonic()
        deadline = started + observation_seconds
        observations = 0
        while time.monotonic() < deadline:
            d.barrier('A')
            screen = d.capture('A')
            observations += 1
            if '账号 ·' not in screen:
                capture_terminal(d, 'A', 'late-message-overrode-account')
                raise AssertionError('A delayed peer Message response replaced the newer Account destination')
            time.sleep(.05)
        capture_terminal(d, 'A', 'account-after-message-response')
        d.evidence('client-observation-window', {'requested_seconds': observation_seconds,
                   'observed_seconds': time.monotonic() - started, 'pty_barrier_observations': observations,
                   'scope': 'Account remained visible during this finite 3-second client observation after server-side direct creation'})



def hidden_direct_restoration(d):
    try:
        _hidden_direct_restoration(d)
    except Exception:
        for actor in ('A', 'C'):
            if actor in d.panes:
                capture_terminal(d, actor, 'failure')
        raise


def _hidden_direct_restoration(d):
    with d.case('hidden-direct-restoration',
                'Two real TUIs close removed direct chats, keep pending chats hidden, and restore the same history and Unicode drafts after acceptance'):
        d.control('connect', actors=['C'])
        direct = d.query('C', 'open_direct_conversation', user='A')['conversation']
        history = 'HIDDEN_HISTORY_' + d.args.run_id + ' é 👩‍💻 1️⃣'
        d.open_direct('A', 'C')
        d.send('A', history)
        original = d.query('C', 'get_messages', conversation=direct)['messages']
        historical = next(v for v in original if v['text'] == history)
        d.evidence('original-direct-history', {'conversation': direct, 'message': historical['id'], 'text': history})
        d.control('disconnect', actors=['C'])
        d.spawn_tui('C')
        d.login('C')
        d.open_direct('C', 'A')
        d.wait('C', 'HIDDEN_HISTORY_')
        drafts = {actor: 'DRAFT_' + actor + '_' + d.args.run_id + ' é 👩‍💻 1️⃣\n第二行 🫩 👨‍👩‍👧‍👦'
                  for actor in ('A', 'C')}
        for actor in ('A', 'C'):
            d.focus_input(actor)
            d.clear_input(actor)
            d.paste(actor, drafts[actor])
            d.focus_messages(actor)
            d.wait(actor, 'DRAFT_' + actor + '_')
            capture_terminal(d, actor, 'open-with-unsent-unicode-draft')

        # The removal and subsequent friendship actions use actual TUI commands.
        # Both clients have this direct open when the removal is confirmed.
        d.command('A', 'remove-contact')
        d.confirm('A', '删除好友')
        for actor, peer in (('A', 'C'), ('C', 'A')):
            d.wait(actor, lambda screen: '聊天' in screen and not chat_open(screen) and '资料 ·' not in screen)
            d.resize(actor, 80, 24)
            d.barrier(actor)
            assert d.name(peer) not in d.capture(actor), 'A removed direct remained in Chats'
            d.keys(actor, 'Tab', 'Escape')
            d.barrier(actor)
            screen = d.capture(actor)
            assert not chat_open(screen) and 'HIDDEN_HISTORY_' not in screen and d.name(peer) not in screen, 'Tab reopened a removed readonly direct'
            capture_terminal(d, actor, 'removed-direct-closed-and-hidden')

        d.request_friend('A', 'C')
        for actor, peer in (('A', 'C'), ('C', 'A')):
            d.chats(actor)
            d.barrier(actor)
            assert d.name(peer) not in d.capture(actor), 'A pending friendship exposed its direct in Chats'
            d.keys(actor, 'Tab')
            d.barrier(actor)
            screen = d.capture(actor)
            assert not chat_open(screen) and 'HIDDEN_HISTORY_' not in screen and d.name(peer) not in screen, 'Tab reopened a pending readonly direct'
            capture_terminal(d, actor, 'pending-direct-still-hidden')

        d.accept_friend('C', 'A')
        for actor, peer in (('A', 'C'), ('C', 'A')):
            d.open_direct(actor, peer)
            d.wait(actor, 'HIDDEN_HISTORY_')
            d.wait(actor, 'DRAFT_' + actor + '_')
            # The selected history message is highlighted only while the messages have the keys.
            d.focus_messages(actor)
            capture_terminal(d, actor, 'accepted-history-and-draft-restored')

        dimensions = [(width, height) for width in (60, 70, 80, 100, 120, 160) for height in (20, 24, 40)]
        for actor in ('A', 'C'):
            for width, height in dimensions:
                d.resize(actor, width, height)
                d.barrier(actor)
                screen = d.capture(actor)
                assert 'DRAFT_' + actor + '_' in screen, 'Resize lost the restored draft preview'
                assert 'HIDDEN_HISTORY_' in inverse_text(d.capture(actor, styled=True)), 'Resize lost the selected historical message'
            capture_terminal(d, actor, 'restored-after-six-widths-three-heights')
        d.evidence('resize-preservation', {'clients': ['A', 'C'], 'dimensions': dimensions,
                                         'checks': ['unsent Unicode draft preview', 'selected original historical message']})

        for actor in ('A', 'C'):
            d.focus_input(actor)
            d.keys(actor, 'Enter')
            marker = 'DRAFT_' + actor + '_'
            d.wait(actor, lambda screen: marker in screen and '消息已发送' in screen and
                   any(prompt_line(line) and marker not in line for line in screen.splitlines()))
        d.wait('A', 'DRAFT_C_')
        d.wait('C', 'DRAFT_A_')
        # Release C's real TUI identity before observing exact DTO bytes through SDK.
        d.command('C', 'logout')
        d.confirm('C', '退出当前账号')
        d.wait('C', '登录 / 注册')
        d.control('connect', actors=['C'])
        metadata = d.query('C', 'get_conversations')['conversations']
        restored = next(v['id'] for v in metadata if v.get('user') == d.manifest['actors']['A']['id'])
        assert restored == direct, 'Acceptance restored a different direct conversation'
        messages = d.query('C', 'get_messages', conversation=restored)['messages']
        assert any(v['id'] == historical['id'] and v['text'] == history for v in messages), 'The original direct history was not preserved'
        for text in drafts.values():
            assert sum(v['text'] == text for v in messages) == 1, 'A restored Unicode draft was lost, altered, or sent twice'
        d.evidence('same-direct-and-exact-unicode', {'original_conversation': direct, 'restored_conversation': restored,
                   'historical_message': historical['id'], 'drafts': drafts, 'exact_message_count_per_draft': 1})


def chat_navigation(d):
    marker='visible_group_'+d.args.run_id
    d.query('S005','send_message',conversation=d.group,text=marker)
    with d.case('chat-enter-escape-navigation','Visible group history opens with Enter; Esc reaches Chats and stays there'):
        d.resize('A',80,24)
        d.select_chat('A',d.title)
        d.wait('A',lambda s:marker in s and chat_open(s))
        d.screenshot('A','narrow-group-enter')
        d.keys('A','Escape')
        d.wait('A',lambda s:('聊天' in s or '聊天' in s) and not chat_open(s))
        d.screenshot('A','narrow-escape-list')
        # Returning by the top-level Chats action must have the same root
        # semantics. Esc at that root cannot reveal the prior message view.
        d.select_chat('A',d.title)
        d.wait('A',chat_open)
        d.chats('A')
        d.keys('A','Escape')
        d.wait('A',lambda s:('聊天' in s or '聊天' in s) and not chat_open(s))
        d.screenshot('A','narrow-root-escape-stable')
        d.resize('A',160,45)
        d.select_chat('A',d.title)
        d.wait('A',marker)
        d.keys('A','Escape')
        d.keys('A','j','Enter')
        d.wait('A',lambda s:d.title in s and '位成员' in s)
        d.screenshot('A','wide-return-focus-opens-next-chat')


def tab_navigation(d):
    with d.case('tab-navigation-roots','Tab changes focus or request tabs; Esc returns directly to the parent'):
        d.resize('A',80,24)
        d.select_chat('A',d.title)
        d.wait('A',chat_open)
        for _ in range(3):
            d.keys('A','Tab')
            d.wait('A',lambda s:chat_open(s) and not composing(s))
            d.keys('A','Tab')
            d.wait('A',composing)
        # Raw keys, without the driver's focus help: shortcut characters are text while typing,
        # Alt+Enter adds a line, and Esc steps back one level at a time.
        d.tmux('send-keys','-l','-t',d.panes['A'],'?:/r')
        d.wait('A',lambda s:any(prompt_line(line) and '?:/r' in line for line in s.splitlines()))
        assert '键盘帮助' not in d.capture('A') and 'Enter 执行' not in d.capture('A')
        d.keys('A','M-Enter')
        d.tmux('send-keys','-l','-t',d.panes['A'],'second')
        d.wait('A',lambda s:'?:/r' in s and 'second' in s and not any('?:/r' in l and 'second' in l for l in s.splitlines()))
        d.screenshot('A','raw-typing-and-alt-enter')
        # Ctrl+K: the palette takes the query, runs help, and Esc returns to the same draft.
        d.keys('A','C-k')
        d.wait('A','命令面板')
        d.tmux('send-keys','-l','-t',d.panes['A'],'帮助')
        d.keys('A','Enter')
        d.wait('A','键盘帮助')
        d.escape_until('A',lambda s:composing(s) and '?:/r' in s and 'second' in s)
        d.keys('A','BTab')
        d.wait('A',lambda s:chat_open(s) and not composing(s))
        d.keys('A','Escape')
        d.wait('A',composing)
        d.keys('A','Home','Up','Home')
        d.keys('A','DC',repeat=32)
        d.keys('A','Escape')
        d.wait('A',lambda s:('聊天' in s or '聊天' in s) and not chat_open(s))
        d.keys('A','Escape')
        d.wait('A',lambda s:('聊天' in s or '聊天' in s) and not chat_open(s))
        d.screenshot('A','tab-chat-root-stable')
        d.keys('A','c'); d.wait('A','新的朋友 (1)')
        d.keys('A','Enter');d.wait('A','新的朋友 · 收到')
        for _ in range(3):
            d.keys('A','Tab');d.wait('A','新的朋友 · 发出')
            d.keys('A','Tab');d.wait('A','新的朋友 · 收到')
        d.keys('A','Tab');d.wait('A','新的朋友 · 发出')
        d.keys('A','Escape');d.wait('A','新的朋友 (1)')
        d.screenshot('A','request-tab-escape-contacts')


def friends_navigation(d):
    with d.case('contacts-and-new-friends','Accepted contacts and requests stay separate; cancel, accept, remove are real actions'):
        d.keys('A','c'); d.wait('A','新的朋友 (1)')
        screen=d.capture('A')
        assert d.name('C') in screen and '观察S005_' in screen
        assert all(d.name(a) not in screen for a in ['B','D','E'])
        d.screenshot('A','accepted-list-only')
        d.control('connect',actors=['D','E','B'])
        d.keys('A','Enter');d.wait('A','新的朋友 · 收到')
        d.wait('A',d.name('D'));d.keys('A','Tab');d.wait('A','新的朋友 · 发出')
        d.wait('A',d.name('E'));d.screenshot('A','outgoing-request')
        d.keys('A','x');d.wait('A','没有待处理的好友申请')
        assert not d.query('E','get_friend_requests')['incoming']
        d.keys('A','Tab');d.wait('A',d.name('D'));d.keys('A','y')
        d.wait('A','没有待处理的好友申请')
        assert any(v['id']==d.manifest['actors']['A']['id'] for v in d.query('D','get_contacts'))
        d.keys('A','Escape');d.wait('A','新的朋友 (0)');d.wait('A',d.name('D'))
        d.command('A','search-users '+d.name('D'));d.wait('A','查找用户');d.wait('A',d.name('D'))
        d.keys('A','Enter');d.wait('A','删除好友')
        d.command('A','remove-contact');d.confirm('A','删除好友')
        d.wait('A','不是好友')
        assert not any(v['id']==d.manifest['actors']['A']['id'] for v in d.query('D','get_contacts'))
        d.screenshot('A','accepted-then-removed')
        direct=d.manifest['navigation']['readonly_conversation']
        d.chats('A');d.barrier('A')
        assert d.name('B') not in d.capture('A')
        assert any(m['id']==d.manifest['navigation']['history_message']
                   for m in d.query('B','get_messages',conversation=direct)['messages'])
        d.query('B','send_friend_request',user='A')
        d.command('A','friend-requests');d.wait('A',d.name('B'))
        d.screenshot('A','hidden-direct-incoming-request')
        d.query('B','cancel_friend_request',user='A');d.wait('A','没有待处理的好友申请')
        d.command('A','search-users '+d.name('B'));d.wait('A',d.name('B'))
        d.keys('A','Enter');d.wait('A','添加好友')
        d.command('A','add');d.wait('A','等待对方确认')
        d.chats('A');d.barrier('A')
        assert d.name('B') not in d.capture('A')
        d.screenshot('A','hidden-direct-outgoing-request')
        d.command('A','search-users '+d.name('B'));d.wait('A',d.name('B'))
        d.keys('A','Enter');d.wait('A','撤回好友申请');d.keys('A','j','Enter')
        d.wait('A','不是好友')
        assert not d.query('B','get_friend_requests')['incoming']
        d.control('disconnect',actors=['B','D','E'])


def contacts_search(d):
    with d.case('contacts-local-search','Local accepted-only substring filtering supports ASCII case, clear, selection and profile'):
        d.keys('A','c','/');d.wait('A','搜索已接受的好友')
        d.paste('A','s005');d.keys('A','Enter')
        d.wait('A',lambda s:'观察S005_' in s and d.name('C') not in s and '新的朋友 (1)' in s)
        assert all(d.name(a) not in d.capture('A') for a in ['B','D','E'])
        d.keys('A','j','j');d.wait_selected('A','观察S005_')
        d.keys('A','Enter');d.wait('A','删除好友');d.screenshot('A','filtered-contact-profile')
        d.keys('A','Escape','/');d.wait('A','搜索已接受的好友')
        d.keys('A','End','BSpace','BSpace','BSpace','BSpace','Enter')
        d.wait('A',lambda s:d.name('C') in s and '观察S005_' in s and '新的朋友 (1)' in s)
        d.screenshot('A','cleared-filter-accepted-only')


def request_selection(d):
    with d.case('request-refresh-preserves-identity','Incoming accept/reject and outgoing cancel target the visually selected user after external refresh'):
        peers=['B','D','E'];d.control('connect',actors=peers)
        aid=d.manifest['actors']['A']['id']
        for peer in peers:d.query(peer,'send_friend_request',user='A')
        d.command('A','friend-requests');d.wait('A',lambda s:all(d.name(p) in s for p in peers))
        order=sorted(peers,key=lambda p:d.capture('A').index(d.name(p)))
        d.keys('A','k',repeat=10);d.keys('A','j');d.wait_selected('A',d.name(order[1]))
        d.query(order[0],'cancel_friend_request',user='A')
        d.wait('A',lambda s:d.name(order[0]) not in s);d.wait_selected('A',d.name(order[1]))
        d.screenshot('A','incoming-after-before-selection-cancel')
        d.keys('A','n');d.wait('A',lambda s:d.name(order[1]) not in s)
        assert not any(r['user']['id']==aid for r in d.query(order[1],'get_friend_requests')['outgoing'])
        assert any(r['user']['id']==aid for r in d.query(order[2],'get_friend_requests')['outgoing'])
        d.keys('A','y');d.wait('A','没有待处理的好友申请')
        assert any(v['id']==aid for v in d.query(order[2],'get_contacts'))
        d.query(order[2],'remove_contact',user='A')
        for peer in peers:
            d.command('A','search-users '+d.name(peer));d.wait('A',d.name(peer))
            d.keys('A','Enter');d.wait('A','添加好友');d.command('A','add');d.wait('A','等待对方确认')
        d.command('A','friend-sent');d.wait('A',lambda s:all(d.name(p) in s for p in peers))
        order=sorted(peers,key=lambda p:d.capture('A').index(d.name(p)))
        d.keys('A','k',repeat=10);d.keys('A','j');d.wait_selected('A',d.name(order[1]))
        d.query(order[0],'respond_friend_request',user='A',accept=False)
        d.wait('A',lambda s:d.name(order[0]) not in s);d.wait_selected('A',d.name(order[1]))
        d.screenshot('A','outgoing-after-before-selection-reject')
        d.keys('A','x');d.wait('A',lambda s:d.name(order[1]) not in s)
        assert not any(r['user']['id']==aid for r in d.query(order[1],'get_friend_requests')['incoming'])
        assert any(r['user']['id']==aid for r in d.query(order[2],'get_friend_requests')['incoming'])
        d.query(order[2],'respond_friend_request',user='A',accept=False)
        d.wait('A','没有待处理的好友申请')
        d.control('disconnect',actors=peers)


def picker_selection(d):
    with d.case('picker-refresh-preserves-identity','Friend removal keeps candidate identity and Space target; selected friend remains cancellable under a filter'):
        d.control('connect',actors=['C'])
        observer=d.manifest['sdk'][0]['username']
        names={'C':d.name('C'),'S005':observer}
        d.command('A','create-group');d.wait('A',lambda s:all(n in s for n in names.values()))
        screen=d.capture('A');order=sorted(names,key=lambda a:screen.index(names[a]))
        d.keys('A','k',repeat=10);d.keys('A','j');d.wait_selected('A',names[order[1]])
        d.query(order[0],'remove_contact',user='A')
        d.wait('A',lambda s:names[order[0]] not in s);d.wait_selected('A',names[order[1]])
        d.keys('A','Space');d.wait('A','已选 1 人');d.screenshot('A','space-target-after-friend-removal')
        d.command('A','filter no-matching-friend');d.wait('A','[x]')
        d.keys('A','Space');d.wait('A',lambda s:'已选 0 人' in s and names[order[1]] not in s)
        d.screenshot('A','filtered-selection-cancelled')
        d.keys('A','Escape');d.wait('A','新的朋友 (1)')
        d.query(order[0],'send_friend_request',user='A')
        d.command('A','friend-requests');d.wait('A',names[order[0]])
        choose_row(d,'A',names[order[0]],limit=3)
        d.keys('A','y');d.wait('A',lambda s:names[order[0]] not in s)
        assert any(v['id']==d.manifest['actors']['A']['id'] for v in d.query(order[0],'get_contacts'))
        d.keys('A','Escape');d.wait('A',lambda s:all(n in s for n in names.values()))
        d.control('disconnect',actors=['C'])


def message_alignment(d):
    with d.case('own-right-peer-left','Real group history distinguishes own and peer messages at narrow and wide terminal sizes'):
        d.control('connect',actors=['C'])
        own='RIGHT_OWN_'+d.args.run_id;peer='LEFT_PEER_'+d.args.run_id
        d.select_chat('A',d.title);d.wait('A',chat_open)
        d.send('A',own)
        d.query('C','send_message',conversation=d.group,text=peer)
        for width in [80,160]:
            d.resize('A',width,30);d.wait('A',lambda s:own in s and peer in s)
            screen=d.capture('A').splitlines()
            own_x=next(line.index(own) for line in screen if own in line)
            peer_x=next(line.index(peer) for line in screen if peer in line)
            assert own_x>peer_x+8,(width,own_x,peer_x)
            d.screenshot('A','message-sides-'+str(width))
        d.control('disconnect',actors=['C'])


def paste_messages(d):
    with d.case('bracketed-multiline-paste','Paste preserves draft and sends exactly one complete message only after Enter'):
        d.spawn_tui('C');d.login('C')
        d.select_chat('C',d.title);d.select_chat('A',d.title)
        for index in range(3):
            marker='PASTE_'+str(index)+'_'+d.args.run_id
            text=marker+'\n第二行 🙂\n\nLAST_'+str(index)+('\n' if index==2 else '')
            d.focus_input('A');d.clear_input('A');d.paste('A',text)
            if index==2: d.keys('A','Up')
            d.wait('A','LAST_'+str(index))
            assert not d.query('S005','search_messages',conversation=d.group,query=marker)['messages']
            d.focus_messages('A')
            d.screenshot('A','unsent-multiline-'+str(index))
            assert not d.query('S005','search_messages',conversation=d.group,query=marker)['messages']
            d.focus_input('A');d.keys('A','Enter');d.wait('A','消息已发送')
            d.wait('C','LAST_'+str(index))
            result=d.query('S005','search_messages',conversation=d.group,query=marker)['messages']
            assert len(result)==1 and result[0]['text']==text,result
            d.screenshot('C','received-multiline-'+str(index))
    with d.case('paste-interrupted-by-reconnect','A disconnected composer keeps its draft and ignores the remainder of the old paste'):
        marker='PASTE_INTERRUPTED_'+d.args.run_id
        d.focus_input('A');d.clear_input('A')
        d.tmux('send-keys','-l','-t',d.panes['A'],'\x1b[200~'+marker)
        d.wait('A',marker)
        d.stop_server();d.control('close');d.wait('A','正在重连')
        d.tmux('send-keys','-l','-t',d.panes['A'],'\nN:quit\x1b[201~')
        d.wait('A','正在重连')
        d.start_server();d.control('connect',actors=['S005'])
        d.wait('A',lambda s:'已连接' in s and chat_open(s))
        assert not d.query('S005','search_messages',conversation=d.group,query=marker)['messages']
        d.focus_input('A');d.wait('A',marker)
        d.screenshot('A','interrupted-paste-draft')
        d.keys('A','Enter');d.wait('A','消息已发送')
        result=d.query('S005','search_messages',conversation=d.group,query=marker)['messages']
        assert len(result)==1 and result[0]['text']==marker,result
        d.wait('C',marker)


def new_actions(d):
    with d.case('new-menu-three-actions','Global New offers add friend, two-step create, and actual join'):
        d.keys('A','h','N');d.wait('A',lambda s: all(t in s for t in ['添加好友','创建群聊','加入群聊']))
        d.screenshot('A','new-three-actions')
        d.keys('A','Enter');d.wait('A','搜索用户（姓名前缀）')
        d.paste('A',d.name('E'));d.keys('A','Enter');d.wait('A','查找用户');d.wait('A',d.name('E'))
        d.keys('A','Enter');d.wait('A','添加好友');d.keys('A','j','Enter');d.wait('A','等待对方确认')
        d.screenshot('A','new-add-friend-pending')
        d.keys('A','h','N','j','Enter');d.wait('A','选择好友')
        # Both accepted friends are selected; pending E must not be an option.
        assert d.name('E') not in d.capture('A')
        d.keys('A','Space','j','Space');d.wait('A','已选 2 人')
        d.screenshot('A','create-select-accepted')
        d.keys('A','Enter');d.wait('A','下一步：群名称')
        title='TUI导航新群_'+d.args.run_id
        d.paste('A',title);d.keys('A','Enter');d.confirm('A','创建群聊')
        d.wait('A',lambda s:title in s and chat_open(s))
        created=next(v for v in d.query('S005','get_conversations')['conversations'] if v['username']==title)
        actual=d.query('S005','get_members',conversation=created['id'])
        assert {v['id'] for v in actual}=={d.manifest['actors'][a]['id'] for a in ['A','C']}|{d.manifest['sdk'][0]['id']}
        d.screenshot('A','created-group-open')
        d.spawn_tui('E');d.login('E');d.keys('E','N','j','j','Enter');d.wait('E','加入群聊')
        d.paste('E',d.manifest['invite_token']);d.keys('E','Enter')
        d.wait('E',lambda s:d.title in s and chat_open(s))
        members=d.query('S005','get_members',conversation=d.group)
        assert len(members)==5 and any(v['id']==d.manifest['actors']['E']['id'] for v in members)
        d.screenshot('E','new-join-open')


def account_logout(d):
    with d.case('account-logout','Account opens explicitly and logout confirmation preserves or closes the real session'):
        d.keys('A','u');d.wait('A','账号 ·');d.wait('A','退出登录')
        d.screenshot('A','account-page')
        d.keys('A','j','j','j','Enter');d.wait('A','退出当前账号')
        d.keys('A','Escape');d.wait('A','账号 ·')
        assert any(p['user']==d.manifest['actors']['A']['id'] and p['online'] for p in d.query('S005','get_presence'))
        d.keys('A','Enter');d.confirm('A','退出当前账号');d.wait('A','登录 / 注册')
        d.screenshot('A','logout-login-page')
        assert any(p['user']==d.manifest['actors']['A']['id'] and not p['online'] for p in d.query('S005','get_presence'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',default='build')
    parser.add_argument('--binary')
    parser.add_argument('--port',type=int,default=18884)
    parser.add_argument('--work-dir',default='/tmp/chat-navigation-'+time.strftime('%m%d%H%M%S'))
    parser.add_argument('--red-only',action='store_true',help='Only Chats/focus regression')
    parser.add_argument('--late-profile-only',action='store_true',help='Only delayed Message versus Account navigation regression')
    parser.add_argument('--hidden-direct-only',action='store_true',help='Only two-client hidden direct and Unicode draft restoration regression')
    parser.add_argument('--keep-database',action='store_true')
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[1]
    args.build_dir=str(Path(args.build_dir).resolve())
    args.binary=Path(args.binary).resolve() if args.binary else Path(args.build_dir)/'chat_tui'
    args.fixture_tool=str(Path(args.build_dir)/'chat_tui_scale_fixture')
    args.run_id='nav'+time.strftime('%m%d%H%M%S')+'_'+str(os.getpid())
    work=Path(args.work_dir).resolve();work.mkdir(mode=0o700,parents=True)
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);probe.bind(('127.0.0.1',args.port))
    maintenance=os.environ.get('PGDATABASE','postgres');database='chat_'+args.run_id
    subprocess.run(['createdb','--maintenance-db='+maintenance,'--template=template0','--encoding=UTF8',database],check=True)
    os.environ['PGDATABASE']=database
    (work/'database.txt').write_text(database+'\n')
    (work/'head.txt').write_text(subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True))
    driver=None;success=False
    try:
        with (work/'migrations.log').open('wb') as log:
            for migration in sorted((repo/'sql').glob('[0-9][0-9][0-9]_*.sql')):
                subprocess.run(['psql','-X','-v','ON_ERROR_STOP=1','-f',str(migration)],check=True,stdout=log,stderr=subprocess.STDOUT)
        print('Evidence:',work,flush=True)
        driver=NavigationDriver(args);driver.setup();driver.login('A')
        if args.late_profile_only:
            late_profile_message(driver)
        elif args.hidden_direct_only:
            hidden_direct_restoration(driver)
        else:
            chat_navigation(driver)
            tab_navigation(driver)
            if not args.red_only:
                contacts_search(driver)
                picker_selection(driver)
                friends_navigation(driver)
                request_selection(driver)
                message_alignment(driver)
                paste_messages(driver)
                new_actions(driver)
                account_logout(driver)
        success=True
        (work/'result.json').write_text(json.dumps({'status':'PASS','cases':driver.case_count,'mode':'navigation'},indent=2))
    finally:
        if driver: driver.close()
        if success and not args.keep_database:
            subprocess.run(['dropdb','--maintenance-db='+maintenance,database],check=True)
            (work/'database-cleanup.txt').write_text('dropped\n')
        else:
            (work/'database-cleanup.txt').write_text('retained: '+database+'\n')
        print('Evidence retained:',work,'success:',success,flush=True)

if __name__=='__main__':main()
