"""Focused real-TUI group edge cases sharing the scale driver's fixture."""
from __future__ import annotations

import time

from tui_scale_observer import read_only_sql


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
        assert collected[reply['message_id']]['reply']['id'] == deleted_id
        assert collected[reply['message_id']]['reply']['deleted']
        assert set(item['id'] for item in d.manifest['messages']).issubset(collected)
        assert all(identity not in pages[0] for identity in (edited_id, deleted_id, reply['message_id']))
        d.evidence('mixed-history-pages', {'pages': pages, 'edited': collected[edited_id],
                                         'deleted': collected[deleted_id], 'reply': collected[reply['message_id']]})

        d.open_main('C')
        # The sixty newer messages force these states out of the first page.
        # A uniquely selected body from each older page acknowledges its actual
        # arrival before the next PgUp; an input barrier alone cannot prove RPC
        # completion. Additional earlier smoke actions may have added messages.
        required = max(index for index, ids in enumerate(pages)
                       if any(identity in ids for identity in (edited_id, deleted_id, reply['message_id'])))
        for index in range(1, required + 1):
            sentinel = next(collected[identity]['text'].split()[0]
                            for identity in reversed(pages[index])
                            if not collected[identity]['deleted'] and collected[identity]['text'])
            d.keys('C', 'PPage')
            d.selected_message('C', sentinel)
        d.selected_message('C', reply_text)
        d.wait('C', lambda text: reply_text in text and '↪' in text and '消息已删除' in text)
        d.screenshot('C', 'older-reply-to-deleted-message')
        d.selected_message('C', edited_text)
        d.wait('C', lambda text: edited_text in text and '(edited)' in text and '消息已删除' in text)
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
            d.wait('C', 'Pinned: ' + marker)
            d.screenshot('C', marker.split('_' + d.args.run_id)[0])
            d.evidence('pin-' + str(message['id']), pinned['pinned_message'])
        d.command('A', 'unpin-message')
        _group_is(d, lambda value: value['pinned_message'] is None)
        d.wait('C', lambda text: 'Pinned:' not in text)
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
        d.wait('A', lambda text: 'Conversations' in text and '[mute]' in text)
        d.barrier('A')
        before = _membership(d)
        assert before['muted']
        marker = 'mute_edge_' + d.args.run_id
        sent = d.query('S030', 'send_message', conversation=d.group, text=marker)
        d.wait('A', lambda text: marker in text and '[mute]' in text and '(1)' in text)
        after = _membership(d)
        assert after['muted'] and after['unread'] == 1 and after['last_read'] == before['last_read']
        d.screenshot('A', 'muted-live-unread')
        d.evidence('muted-no-auto-read', {'before': before, 'after': after, 'message': sent['message_id']})
        d._group_edge_mute = {'message': sent['message_id'], 'marker': marker}
        d.resize('A', 160, 45)


def verify_reconnect(d):
    with d.case('muted-reconnect-read',
                'Mute survives real server restarts; realtime history and visible-read semantics still work'):
        saved = d._group_edge_mute
        after = _membership(d)
        assert after['muted'], 'Server reconnect lost the personal mute'
        # Inspect the full-width conversation list before returning to the
        # selected history. Wide-sidebar badge layout has separate coverage.
        d.resize('A', 80, 30)
        d.command('A', 'conversations')
        d.wait('A', lambda text: 'Conversations' in text and '[mute]' in text)
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

