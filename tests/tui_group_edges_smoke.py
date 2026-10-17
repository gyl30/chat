"""Focused real-TUI group edge cases sharing the scale driver's fixture."""
from __future__ import annotations

import time

from tui_scale_observer import read_only_sql
from tui_100_member_smoke import conversation_list_visible


def _eventually(predicate, description, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.08)
    raise AssertionError(description)


def _group(d):
    cursor = None
    while True:
        page = d.query('S005', 'get_conversations', **({'before': cursor} if cursor else {}))
        for value in page['conversations']:
            if value['id'] == d.group:
                return value
        cursor = page['next']
        assert cursor, 'Authoritative main conversation missing'


def _group_is(d, predicate):
    def check():
        value = _group(d)
        return value if predicate(value) else None
    return _eventually(check, 'Group metadata did not reach the expected authoritative state')


def _membership(d):
    owner = int(d.manifest['actors']['A']['id'])
    rows = read_only_sql(
        'SELECT cm.muted::text,cm.last_read_message_id,'
        '(SELECT count(*) FROM messages m WHERE m.conversation_id=cm.conversation_id '
        'AND m.id>cm.last_read_message_id AND m.id>cm.joined_message_id '
        'AND NOT m.deleted AND m.sender_id<>cm.user_id) '
        f'FROM conversation_members cm WHERE cm.conversation_id={int(d.group)} AND cm.user_id={owner}')
    assert len(rows) == 1, 'Owner membership must exist'
    return {'muted': rows[0][0] == 'true', 'last_read': int(rows[0][1]), 'unread': int(rows[0][2])}


def run(d):
    with d.case('mixed-message-history-pages',
                'Edited text, deleted placeholders and deleted reply quotes survive actual older-history pagination'):
        # Seed messages belong to A. Release its real TUI identity before the SDK
        # edits those messages, then hand the same identity back to the real UI.
        d.logout('A')
        owner = d.manifest['actors']['A']['id']
        _eventually(lambda: owner not in d.control('status', verify=True)['online_member_ids'],
                    'Owner session was not released for the SDK fixture action')
        d.control('connect', actors=['A'])
        edited_id = d.manifest['messages'][-2]['id']
        deleted_id = d.manifest['messages'][-1]['id']
        edited_text = 'mixed_edited_' + d.args.run_id
        reply_text = 'mixed_reply_' + d.args.run_id
        try:
            edited = d.query('A', 'edit_message', conversation=d.group, message=edited_id, text=edited_text)
            assert edited['edited_at'] is not None and edited['text'] == edited_text
            reply = d.query('A', 'send_message', conversation=d.group, text=reply_text, reply_to=deleted_id)
            assert reply['reply']['id'] == deleted_id
            deleted = d.query('A', 'delete_message', conversation=d.group, message=deleted_id)
            assert deleted['deleted']
        finally:
            d.control('disconnect', actors=['A'])
            d.login('A', first=False)
        for index in range(60):
            d.query('S005', 'send_message', conversation=d.group,
                    text=f'mixed_page_fill_{index:02d}_{d.args.run_id}')

        cursor = None
        collected = {}
        pages = []
        while True:
            page = d.query('S005', 'get_messages', conversation=d.group,
                           **({'before': cursor} if cursor else {}))
            ids = [item['id'] for item in page['messages']]
            assert ids == sorted(ids) and len(ids) == len(set(ids))
            assert not set(ids).intersection(collected)
            if cursor is not None and ids:
                assert max(ids) < cursor
            collected.update((item['id'], item) for item in page['messages'])
            pages.append(ids)
            if not page['has_more']:
                break
            assert ids
            cursor = min(ids)
        assert collected[edited_id]['text'] == edited_text and collected[edited_id]['edited_at'] is not None
        assert collected[deleted_id]['deleted'] and not collected[deleted_id]['text']
        assert collected[reply['id']]['reply']['id'] == deleted_id
        assert collected[reply['id']]['reply']['deleted']
        assert set(item['id'] for item in d.manifest['messages']).issubset(collected)
        assert all(identity not in pages[0] for identity in (edited_id, deleted_id, reply['id']))
        d.evidence('mixed-history-pages', {'pages': pages, 'edited': collected[edited_id],
                                         'deleted': collected[deleted_id], 'reply': collected[reply['id']]})

        d.open_main('C')
        # The sixty newer messages force these states out of the first page.
        # A uniquely selected body from each older page acknowledges its actual
        # arrival before the next PgUp; an input barrier alone cannot prove RPC
        # completion. Additional earlier smoke actions may have added messages.
        required = max(index for index, ids in enumerate(pages)
                       if any(identity in ids for identity in (edited_id, deleted_id, reply['id'])))
        for index in range(1, required + 1):
            sentinel = next(collected[identity]['text'].split()[0]
                            for identity in reversed(pages[index])
                            if not collected[identity]['deleted'] and collected[identity]['text'])
            d.keys('C', 'PPage')
            d.selected_message('C', sentinel, latest=False)
        d.selected_message('C', reply_text)
        d.wait('C', lambda text: reply_text in text and '↪' in text and '消息已删除' in text)
        d.screenshot('C', 'older-reply-to-deleted-message')
        d.selected_message('C', edited_text)
        d.wait('C', lambda text: edited_text in text and '（已编辑）' in text and '消息已删除' in text)
        d.screenshot('C', 'older-edited-and-deleted')
        d.keys('C', 'G')

    with d.case('announcement-replace-multiline-whitespace',
                'Real owner replaces and clears announcements; a multiline SDK fixture is fully shown and retained by UI confirmation'):
        d.open_main('A')
        first = 'announcement_first_' + d.args.run_id
        second = 'announcement_replaced_' + d.args.run_id
        for content in (first, second):
            d.command('A', 'announcement ' + content)
            _group_is(d, lambda value: value['announcement'] == content)
        d.open_main('C')
        d.command('C', 'show-announcement')
        d.wait('C', second)
        assert first not in d.capture('C')
        d.screenshot('C', 'replacement')
        d.keys('C', 'Escape')
        multiline = '公告第一行_' + d.args.run_id + '\n第二行 中文 😀\nthird line'
        # The terminal composer is single-line: inject the multiline fixture
        # with the existing SDK, then verify full display and an unchanged real
        # UI edit/confirm. This does not claim keyboard multiline entry.
        d.query('S005', 'set_group_announcement', conversation=d.group, text=multiline)
        _group_is(d, lambda value: value['announcement'] == multiline)
        d.wait('A', '公告第一行_' + d.args.run_id)
        d.command('A', 'announcement')
        d.wait('A', '编辑公告')
        d.wait('A', 'third line')
        d.keys('A', 'Enter')
        _group_is(d, lambda value: value['announcement'] == multiline)
        d.evidence('multiline-source', {'source': 'SDK fixture', 'text': multiline,
                                        'ui': 'full view and unchanged edit confirmation'})
        d.command('C', 'show-announcement')
        d.wait('C', lambda text: '公告第一行_' in text and '第二行 中文 😀' in text and 'third line' in text)
        d.screenshot('C', 'multiline-announcement')
        d.keys('C', 'Escape')
        d.command('A', 'announcement')
        d.wait('A', '编辑公告')
        d.clear_input('A')
        d.paste('A', ' \u00a0\u3000 ')
        d.keys('A', 'Enter')
        empty = _group_is(d, lambda value: value['announcement'] == '')
        d.command('C', 'show-announcement')
        d.wait('C', '暂无群公告')
        d.screenshot('C', 'unicode-whitespace-clears')
        d.evidence('cleared-announcement', empty)
        d.keys('C', 'Escape')

    with d.case('group-pin-replace-unpin', 'Real owner replaces the group pin, then explicitly unpins it'):
        d.open_main('A')
        d.open_main('C')
        for marker in ('pin_replace_a_' + d.args.run_id, 'pin_replace_b_' + d.args.run_id):
            d.send('A', marker)
            message = d.query('S005', 'search_messages', conversation=d.group, query=marker)['messages'][0]
            d.selected_message('A', marker)
            d.command('A', 'pin-message')
            pinned = _group_is(d, lambda value: value['pinned_message'] is not None and value['pinned_message']['id'] == message['id'])
            d.wait('C', '置顶：' + marker)
            d.screenshot('C', marker.split('_' + d.args.run_id)[0])
            d.evidence('pin-' + str(message['id']), pinned['pinned_message'])
        d.command('A', 'unpin-message')
        _group_is(d, lambda value: value['pinned_message'] is None)
        d.wait('C', lambda text: '置顶：' not in text)
        d.screenshot('C', 'explicit-unpin')

    with d.case('typing-partial-clear-natural-expiry',
                'Clearing one of two typers preserves the other until its normal expiry without a false notification'):
        d.open_main('A')
        names = {item['alias']: item['username'] for item in d.manifest['sdk']}
        for actor in ('S030', 'S031'):
            d.query(actor, 'set_typing', conversation=d.group, typing=True)
        started = time.monotonic()
        d.wait('A', lambda text: any('正在输入' in line and names['S030'] in line and names['S031'] in line
                                   for line in text.splitlines()))
        d.query('S030', 'set_typing', conversation=d.group, typing=False)
        d.wait('A', lambda text: any('正在输入' in line and names['S031'] in line and names['S030'] not in line
                                   for line in text.splitlines()))
        d.screenshot('A', 'one-typer-remains')
        # There is deliberately no false RPC for S031 before this assertion.
        d.wait('A', lambda text: '正在输入' not in text)
        d.evidence('natural-expiry', {'elapsed_seconds': time.monotonic() - started,
                                      'explicit_false_sent': ['S030'], 'expired_without_false': 'S031'})
        d.screenshot('A', 'natural-expiry')
        d.query('S031', 'set_typing', conversation=d.group, typing=False)
    for actor in 'ABCDE':
        d.open_main(actor)
    state = d.control('status', verify=True)
    assert state['member_count'] == 100 and state['owner_count'] == 1 and state['admin_count'] == 3


def prepare_reconnect(d):
    with d.case('muted-realtime-unread',
                'Muted conversation receives a live message and increments unread while the owner browses the list'):
        d.open_main('A')
        _eventually(lambda: _membership(d)['unread'] == 0, 'Open latest conversation did not become read')
        if not _membership(d)['muted']:
            d.command('A', 'mute')
        _eventually(lambda: _membership(d)['muted'], 'Real UI mute did not reach the server')
        d.resize('A', 80, 30)
        d.command('A', 'conversations')
        d.wait('A', conversation_list_visible)
        d.barrier('A')
        before = _membership(d)
        assert before['muted']
        marker = 'mute_edge_' + d.args.run_id
        sent = d.query('S030', 'send_message', conversation=d.group, text=marker)
        def unread_without_read():
            value = _membership(d)
            return value if (value['muted'] and value['unread'] == 1 and
                             value['last_read'] == before['last_read']) else None
        after = _eventually(unread_without_read, 'Muted live message must remain unread without advancing last_read')
        assert conversation_list_visible(d.capture('A')), 'Owner must remain on the conversation list'
        d.screenshot('A', 'muted-live-unread')
        d.evidence('muted-no-auto-read', {'before': before, 'after': after, 'message': sent['id']})
        d._group_edge_mute = {'message': sent['id'], 'marker': marker}
        d.resize('A', 160, 45)


def verify_reconnect(d):
    with d.case('muted-reconnect-read',
                'Mute survives real server restarts; realtime history and visible-read semantics still work'):
        saved = d._group_edge_mute
        after = _membership(d)
        assert after['muted'], 'Server reconnect lost the personal mute'
        # 返回历史前先确认仍在列表，静音状态由数据库验证。
        d.resize('A', 80, 30)
        d.command('A', 'conversations')
        d.wait('A', conversation_list_visible)
        d.screenshot('A', 'mute-survived-restarts-list')
        d.resize('A', 160, 45)
        d.open_main('A')
        d.selected_message('A', saved['marker'])
        _eventually(lambda: _membership(d)['last_read'] >= saved['message'], 'Mute prevented marking visible latest history read')
        d.screenshot('A', 'mute-survived-restarts')
        d.evidence('muted-after-reconnect', _membership(d))
        d.keys('A', 'G')
        d.command('A', 'mute')
        _eventually(lambda: not _membership(d)['muted'], 'Failed to restore unmuted conversation')



def verify_join_request_rejection(d):
    """Two real TUIs: selected applicant profiles and both admission decisions."""
    from tui_100_member_smoke import choose_row

    applicant = int(d.manifest['actors']['D']['id'])
    other = int(d.manifest['actors']['E']['id'])
    group = int(d.group)

    def pending(user):
        return read_only_sql(
            f'SELECT count(*) FROM group_join_requests WHERE conversation_id={group} AND user_id={user}'
        )[0][0] == '1'

    d.open_main('A')
    if not _group(d)['join_approval']:
        d.command('A', 'approval')
    _group_is(d, lambda value: value['join_approval'])
    d.command('D', 'join ' + d.manifest['invite_token'])
    d.wait('D', '申请已提交，等待管理员审批')
    _eventually(lambda: pending(applicant), 'The real applicant request was not persisted')
    d.screenshot('D', 'awaiting-approval')
    # The second applicant is fixture-only. Requests are sorted by user ID
    # descending; the real applicant D has the lower ID and must be second,
    # so resetting selection to zero cannot pass the return check.
    assert other > applicant, 'The fixture must place the real applicant on a non-first row'
    d.control('connect', actors=['E'])
    try:
        d.query('E', 'join_group', token=d.manifest['invite_token'])
        _eventually(lambda: pending(other), 'The second fixture applicant was not persisted')
    finally:
        d.control('disconnect', actors=['E'])

    with d.case('join-request-profile-return',
                'Enter opens the selected second applicant; Escape restores the same pending row and group'):
        d.command('A', 'requests')
        d.wait('A', lambda text: d.name('D') in text and d.name('E') in text)
        screen = d.capture('A')
        assert screen.index(d.name('E')) < screen.index(d.name('D')), 'The real applicant must be non-first'
        choose_row(d, 'A', d.name('D'), limit=12)
        d.screenshot('A', 'selected-second-applicant')
        d.keys('A', 'Enter')
        d.wait('A', lambda text: any('资料 · ' in line and d.name('D') in line
                                    for line in text.splitlines()))
        assert not any('资料 · ' in line and d.name('A') in line
                       for line in d.capture('A').splitlines()), 'Enter must not open the owner profile'
        d.screenshot('A', 'selected-applicant-profile')
        d.keys('A', 'Escape')
        d.wait('A', '待处理的入群申请')
        d.wait_selected('A', d.name('D'))
        d.screenshot('A', 'same-applicant-after-profile')
        d.evidence('profile-return', {'selected_applicant': applicant, 'other_applicant': other,
                   'group': group, 'selected_nonfirst': True, 'identity_after_escape': applicant})

    with d.case('join-request-rejected-feedback',
                'A real TUI applicant sees rejection after a real owner rejects its pending request'):
        d.command('A', 'reject')
        d.wait('A', '入群申请已处理')
        _eventually(lambda: not pending(applicant), 'The real owner rejection did not remove the pending request')
        assert pending(other), 'Reject must leave the other applicant pending'
        members = d.query('S005', 'get_members', conversation=d.group)
        assert all(member['id'] != applicant for member in members), 'Rejection must not grant membership'
        presence = d.query('S005', 'get_presence')
        assert any(item['user'] == applicant and item['online'] for item in presence), 'Applicant must remain connected'
        d.evidence('authoritative-rejection', {'applicant': applicant, 'group': group,
                   'pending_request_exists': False, 'other_pending_exists': True,
                   'membership_exists': False, 'applicant_online': True,
                   'decision_actor': 'real owner TUI', 'submission_actor': 'real applicant TUI'})
        d.screenshot('A', 'owner-rejected')
        d.wait('D', '入群申请被拒绝')
        d.screenshot('D', 'rejection-notice')
        d.command('A', 'group')
        d.wait('A', '群聊 · ' + d.title)
        assert _group(d)['id'] == group, 'Reject/profile return must keep the same active group'

    with d.case('join-request-accepted-return',
                'A second real request is accepted; the applicant opens the same group and owner retains it'):
        d.command('D', 'join ' + d.manifest['invite_token'])
        d.wait('D', '申请已提交，等待管理员审批')
        _eventually(lambda: pending(applicant), 'The real repeated applicant request was not persisted')
        d.command('A', 'requests')
        d.wait('A', d.name('D'))
        choose_row(d, 'A', d.name('D'), limit=12)
        d.command('A', 'accept')
        d.wait('A', '入群申请已处理')
        _eventually(lambda: not pending(applicant), 'Acceptance did not remove the repeated pending request')
        _eventually(lambda: any(item['id'] == applicant for item in
                    d.query('S005', 'get_members', conversation=d.group)), 'Acceptance did not grant membership')
        assert pending(other), 'Accept must leave the other applicant pending'
        d.wait('D', lambda text: d.title in text and '位成员' in text)
        d.screenshot('D', 'accepted-group-opened')
        d.command('A', 'group')
        d.wait('A', '群聊 · ' + d.title)
        d.screenshot('A', 'same-owner-group-after-accept')
        d.evidence('accepted-return', {'applicant': applicant, 'group': group,
                   'pending_request_exists': False, 'other_pending_exists': True,
                   'membership_exists': True, 'same_owner_group': True})


def main():
    """Bounded standalone mode; reuse the small navigation fixture and driver."""
    import argparse
    import hashlib
    import json
    import os
    from pathlib import Path
    import socket
    import subprocess
    import traceback
    from tui_navigation_smoke import NavigationDriver
    from tui_scale_observer import process_sample

    parser = argparse.ArgumentParser(description='Two real TUI join-request rejection feedback regression')
    parser.add_argument('--rejection-only', action='store_true', required=True)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--binary')
    parser.add_argument('--work-dir', required=True)
    parser.add_argument('--port', type=int, default=18887)
    args = parser.parse_args()
    os.umask(0o077)
    repo = Path(__file__).resolve().parents[1]
    args.build_dir = str(Path(args.build_dir).resolve())
    args.binary = Path(args.binary).resolve() if args.binary else Path(args.build_dir) / 'chat_tui'
    args.fixture_tool = str(Path(args.build_dir) / 'chat_tui_scale_fixture')
    args.run_id = 'jrej' + time.strftime('%m%d%H%M%S') + '_' + str(os.getpid())
    work = Path(args.work_dir).resolve()
    work.mkdir(mode=0o700, parents=True, exist_ok=True)
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', args.port))
    maintenance = os.environ.get('PGDATABASE', 'postgres')
    database = 'chat_' + args.run_id
    subprocess.run(['createdb', '--maintenance-db=' + maintenance,
                    '--template=template0', '--encoding=UTF8', database], check=True)
    os.environ['PGDATABASE'] = database
    (work / 'database.txt').write_text(database + '\n')
    inputs = ['tui/src/app.cpp', 'tui/src/profile.cpp', 'tui/src/groups.cpp', 'tui/src/ui.cpp',
              'tests/tui_group_edges_smoke.py', 'tests/tui_navigation_smoke.py', 'tests/tui_scale_fixture.cpp']
    (work / 'inputs.json').write_text(json.dumps({
        'head': subprocess.check_output(['git', '-C', str(repo), 'rev-parse', 'HEAD'], text=True).strip(),
        'input_sha256': {name: hashlib.sha256((repo / name).read_bytes()).hexdigest() for name in inputs},
        'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
        'scope': 'Two real TUI processes, real server, existing six-account navigation SDK fixture; no SQL business writes'
    }, indent=2))
    driver = None
    result = {'status': 'FAIL', 'database': database, 'port': args.port}
    try:
        with (work / 'migrations.log').open('wb') as log:
            for migration in sorted((repo / 'sql').glob('[0-9][0-9][0-9]_*.sql')):
                subprocess.run(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-f', str(migration)],
                               check=True, stdout=log, stderr=subprocess.STDOUT)
        driver = NavigationDriver(args)
        driver.setup()
        driver.spawn_tui('D')
        started = {'server': process_sample(driver.server.pid),
                   'fixture': process_sample(driver.helper.pid)}
        for actor, pane in driver.panes.items():
            pid = int(driver.tmux('display-message', '-p', '-t', pane, '#{pane_pid}').stdout.strip())
            started['tui_' + actor] = process_sample(pid)
        (work / 'resources-started.json').write_text(json.dumps(started, indent=2))
        driver.login('A')
        driver.login('D')
        verify_join_request_rejection(driver)
        result['status'] = 'PASS'
        print('PASS real TUI applicant rejection feedback', flush=True)
    except Exception as error:
        safe = driver.redact if driver else str
        (work / 'failure-traceback.txt').write_text(safe(traceback.format_exc()))
        result['error'] = safe(str(error))
        print('FAIL real TUI applicant rejection feedback: ' + result['error'], flush=True)
    finally:
        if driver:
            driver.close()
        cleanup = {'owned_driver_closed': driver is not None}
        environment = os.environ.copy()
        environment['PGDATABASE'] = maintenance
        dropped = subprocess.run(['dropdb', '--maintenance-db=' + maintenance, database],
                                 env=environment, capture_output=True, text=True)
        cleanup['owned_database_dropped'] = dropped.returncode == 0
        with socket.socket() as probe:
            try:
                probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                probe.bind(('127.0.0.1', args.port))
                cleanup['owned_port_closed'] = True
            except OSError:
                cleanup['owned_port_closed'] = False
        if (work / 'resources-started.json').exists():
            started = json.loads((work / 'resources-started.json').read_text())
            cleanup['owned_processes_stopped'] = all(
                not (current['alive'] and current.get('start_ticks') == original.get('start_ticks'))
                for original in started.values()
                for current in [process_sample(original['pid'])])
        if not all(cleanup.values()):
            result['status'] = 'FAIL'
            result['cleanup_incomplete'] = True
        (work / 'cleanup.json').write_text(json.dumps(cleanup, indent=2))
        (work / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
