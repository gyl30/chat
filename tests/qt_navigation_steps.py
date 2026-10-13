"""Navigation-only native Qt actions sharing qt_x11_smoke's owned fixture and driver."""

def run(c):
    gui = c['gui']
    control, capture, wait, record = c['control'], c['capture'], c['wait'], c['record']
    actor, sql, query_fact = c['actor'], c['sql'], c['query_fact']
    manifest = c['manifest']
    c['login']('A', 0)
    aid, did, eid = (actor(name)['id'] for name in ('A', 'D', 'E'))

    def click(label):
        gui.click(aid, gui.field(aid, label))

    def sdk(who, method, **args):
        reply = control('sdk', actor=who, method=method, **args)
        assert reply['ok'], reply
        return reply['value']

    def presence(online):
        return any(p['user'] == aid and p['online'] == online for p in sdk('S005', 'get_presence'))

    def restored(section):
        markers = {'contacts': '搜索联系人', 'new-friends': '收到的好友申请', 'chats': '搜索聊天'}
        wait(lambda: gui.find(aid, markers[section]), 'Back restores actual ' + section + ' sidebar')

    def back(section):
        click('返回')
        restored(section)

    wait(lambda: presence(True), 'Qt A authenticated')
    gui.navigate(aid, '聊天')
    capture('nav-01-chats-sidebar')
    gui.navigate(aid, '联系人')
    capture('nav-02-accepted-contacts-only')
    expected = {actor('C')['id'], actor('S005')['id']}
    actual = {int(v) for v in sql(f'SELECT contact_id FROM contacts WHERE owner_id={aid}').splitlines()}
    assert actual == expected, actual
    query_fact('nav-accepted-contact-ids', f'SELECT contact_id FROM contacts WHERE owner_id={aid} ORDER BY contact_id')
    expected_names = {actor(name)['username'] for name in ('C', 'S005')}
    gui.wait(lambda: {row.get_name() for row in gui.rows(aid, '联系人列表')} == expected_names,
             'Native contacts show accepted users without pending requests')
    # Search only filters accepted contacts in its independent modal dialog.
    gui.paste(aid, gui.field(aid, '搜索联系人'), actor('D')['username'])
    gui.key('Return')
    gui.wait(lambda: not gui.rows(aid, '联系人列表'), 'Nonfriend search has no accepted result')
    assert gui.find(aid, '搜索用户') is None
    capture('nav-contacts-no-match')
    gui.paste(aid, gui.field(aid, '搜索联系人'), actor('C')['username'].upper())
    gui.key('Return')
    gui.wait(lambda: gui.field(aid, '联系人列表').get_state_set().contains(gui.Atspi.StateType.FOCUSED),
             'Search Enter focuses matched contact list')
    gui.key('Return')
    gui.wait(lambda: gui.dialog(aid, '联系人') is None, 'Contact Enter closes modal')
    gui.wait(lambda: next((n for n in gui.nodes(aid) if n.get_name() == actor('C')['username'] and
             n.get_role() == gui.Atspi.Role.PUSH_BUTTON), None), 'Contact Enter opens exact direct header')
    capture('nav-contacts-enter-direct')
    gui.navigate(aid, '联系人')
    gui.paste(aid, gui.field(aid, '搜索联系人'), '')
    gui.click(aid, gui.wait_row(aid, '联系人列表', actor('C')['username']), x_margin=24)
    gui.field(aid, '关闭资料')
    capture('nav-contacts-avatar-profile')
    click('关闭资料')
    gui.wait(lambda: gui.dialog(aid, '联系人'), 'Profile close restores contact dialog')
    capture('nav-contacts-avatar-close')
    gui.click(aid, gui.wait_row(aid, '联系人列表', actor('C')['username']))
    gui.wait(lambda: gui.dialog(aid, '联系人') is None, 'Contact body opens chat and closes modal')
    gui.field(aid, '消息输入')
    capture('nav-contacts-body-direct')
    gui.navigate(aid, '联系人')
    click('添加联系人')  # The actual plus inside Contacts, not a blocked main menu.
    gui.field(aid, '搜索用户')
    capture('nav-contacts-add-friend')
    gui.key('Escape')
    gui.wait(lambda: gui.dialog(aid, '添加联系人') is None, 'Cancel Add contact dialog')
    gui.wait(lambda: gui.dialog(aid, '联系人'), 'Nested Add cancel restores Contacts')
    capture('nav-contacts-add-back')
    gui.navigate(aid, '新的朋友')
    capture('nav-03-new-friends-incoming')
    gui.navigate(aid, '添加联系人')
    gui.field(aid, '搜索用户')
    capture('nav-new-friends-add-friend')
    gui.key('Escape')
    gui.wait(lambda: gui.dialog(aid, '添加联系人') is None, 'Add cancel restores underlying New friends')
    restored('new-friends')
    capture('nav-new-friends-add-back')
    back('chats')
    capture('nav-new-friends-back-chats')
    gui.navigate(aid, '新的朋友')
    # The initial fixture contains exactly one incoming and one outgoing request.
    gui.click(aid, gui.wait_row(aid, '收到的好友申请', actor('D')['username']))
    gui.key('Return')
    gui.field(aid, '接受')
    capture('nav-04-incoming-profile')
    click('接受')
    relation = f'SELECT count(*) FROM contacts WHERE (owner_id={aid} AND contact_id={did}) OR (owner_id={did} AND contact_id={aid})'
    wait(lambda: sql(relation) == '2', 'Incoming request accepted bilaterally')
    capture('nav-05-accepted-profile')
    click('资料')
    click('移除联系人')
    gui.confirmation_button(aid, '移除联系人')
    capture('nav-remove-confirm')
    gui.click(aid, gui.confirmation_button(aid, '移除联系人'))
    wait(lambda: sql(relation) == '0', 'Remove accepted friend bilaterally')
    gui.key('Escape')
    capture('nav-removed-friend')
    gui.navigate(aid, '新的朋友')
    gui.click(aid, gui.wait_row(aid, '发出的好友申请', actor('E')['username']))
    gui.key('Return')
    gui.field(aid, '撤回申请')
    capture('nav-outgoing-profile')
    click('撤回申请')
    pending = f'SELECT count(*) FROM friend_requests WHERE requester_id={aid} AND recipient_id={eid}'
    wait(lambda: sql(pending) == '0', 'Outgoing request cancelled')
    capture('nav-cancelled-outgoing')
    gui.key('Escape')
    record('qt-contacts-pending-isolation-accept-remove-cancel', [
        'nav-02-accepted-contacts-only.png', 'nav-03-new-friends-incoming.png', 'nav-04-incoming-profile.png',
        'nav-accepted-contact-ids.txt', 'nav-05-accepted-profile.png', 'nav-remove-confirm.png',
        'nav-removed-friend.png', 'nav-outgoing-profile.png', 'nav-cancelled-outgoing.png'])
    gui.navigate(aid, '聊天')
    gui.navigate(aid, '添加联系人')
    gui.field(aid, '搜索用户')
    capture('nav-07-add-friend-search')
    gui.key('Escape')
    gui.wait(lambda: gui.dialog(aid, '添加联系人') is None, 'Add cancel restores underlying Chats')
    restored('chats')
    capture('nav-chats-add-back')
    record('qt-secondary-close-preserves-parent', [
        'nav-contacts-add-back.png', 'nav-new-friends-add-back.png',
        'nav-new-friends-back-chats.png', 'nav-chats-add-back.png'])
    gui.navigate(aid, '加入群组')
    gui.field(aid, '邀请码')
    capture('nav-08-join-action')
    gui.key('Escape')
    gui.navigate(aid, '新建群组')
    gui.field(aid, '选择群成员')
    capture('nav-09-create-select-contacts')
    gui.click(aid, gui.wait_row(aid, '选择群成员', actor('S005')['username']))
    gui.key('space')
    gui.field(aid, actor('S005')['username'] + ' ×')
    capture('nav-10-create-selected')
    click('下一步')
    gui.wait_row(aid, '已选群成员', actor('S005')['username'])
    capture('nav-11-create-name-step')
    title = 'Qt导航创建群_' + manifest['prefix']
    gui.paste(aid, gui.editable(aid, '群名称', '创建群聊'), title)
    capture('nav-11-create-title-filled')
    click('创建')
    def created():
        page = sdk('S005', 'get_conversations')
        return next((v for v in page['conversations'] if v['username'] == title), None)
    conversation = wait(created, 'New group visible to selected member')
    members = sdk('S005', 'get_members', conversation=conversation['id'])
    assert {m['id'] for m in members} == {aid, actor('S005')['id']}, members
    gui.open_room(aid, title)
    capture('nav-12-created-group-open')
    click('聊天信息')
    gui.field(aid, '群资料')
    capture('nav-13-created-group-header-opens-details')
    gui.key('Escape')
    record('qt-chats-three-actions-and-two-step-create', [
        'nav-01-chats-sidebar.png', 'nav-07-add-friend-search.png', 'nav-08-join-action.png',
        'nav-09-create-select-contacts.png', 'nav-11-create-name-step.png', 'nav-11-create-title-filled.png',
        'nav-12-created-group-open.png', 'nav-13-created-group-header-opens-details.png'])
    gui.navigate(aid, '我的资料')
    gui.field(aid, '退出登录')
    capture('nav-14-menu-account')
    click('退出登录')
    gui.confirmation_button(aid, '退出登录')
    capture('nav-15-logout-confirmation')
    # Default Enter still activates the actual safe confirmation default.
    gui.key('Return')
    wait(lambda: gui.find(aid, '退出当前账号？应用会返回登录页。') is None, 'Default Enter cancels logout')
    assert presence(True), 'Cancel must keep authenticated session'
    capture('nav-16-logout-cancelled')
    click('退出登录')
    gui.click(aid, gui.confirmation_button(aid, '退出登录'))
    wait(lambda: presence(False), 'Qt logout authoritative offline')
    assert c['clients']['A'].poll() is None
    gui.field(aid, '登录')
    capture('nav-17-logout-login-page')
    record('qt-menu-account-cancel-and-confirm-logout', [
        'nav-14-menu-account.png', 'nav-15-logout-confirmation.png', 'nav-16-logout-cancelled.png',
        'nav-17-logout-login-page.png'])
    return 4
