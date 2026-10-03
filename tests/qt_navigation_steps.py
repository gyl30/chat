"""Navigation-only X11 actions sharing qt_x11_smoke's owned fixture and driver."""
def run(c):
    control=c['control'];click=c['click'];key=c['key'];paste=c['paste']
    capture=c['capture'];wait=c['wait'];record=c['record'];modal=c['modal'];focus=c['focus']
    actor=c['actor'];sql=c['sql'];query_fact=c['query_fact'];manifest=c['manifest']
    def sdk(who,method,**args):
        reply=control('sdk',actor=who,method=method,**args)
        assert reply['ok'],reply
        return reply['value']
    def presence(online):
        return any(p['user']==actor('A')['id'] and p['online']==online for p in sdk('S005','get_presence'))
    a=c['login']('A',0)
    wait(lambda:presence(True),'Qt A authenticated')
    focus(a);capture('nav-01-chats-sidebar')
    click(41,170);capture('nav-02-accepted-contacts-only')
    aid=actor('A')['id'];did=actor('D')['id'];eid=actor('E')['id']
    expected={actor('C')['id'],actor('S005')['id']}
    actual={int(v) for v in sql(f'SELECT contact_id FROM contacts WHERE owner_id={aid}').splitlines()}
    assert actual==expected,actual
    query_fact('nav-accepted-contact-ids',f'SELECT contact_id FROM contacts WHERE owner_id={aid} ORDER BY contact_id')
    click(230,73);capture('nav-03-new-friends-incoming')
    # The initial fixture contains exactly one incoming and one outgoing request.
    click(230,130);profile=modal('A',actor('D')['username']);capture('nav-04-incoming-profile')
    click(508,366)
    relation=f'SELECT count(*) FROM contacts WHERE (owner_id={aid} AND contact_id={did}) OR (owner_id={did} AND contact_id={aid})'
    wait(lambda:sql(relation)=='2','incoming request accepted bilaterally')
    capture('nav-05-accepted-profile')
    click(580,548);modal('A','移除联系人');capture('nav-remove-confirm')
    key('Left');key('Return');wait(lambda:sql(relation)=='0','remove accepted friend bilaterally')
    focus(a);capture('nav-removed-friend')
    click(230,463);modal('A',actor('E')['username']);capture('nav-outgoing-profile')
    click(580,548)
    pending=f'SELECT count(*) FROM friend_requests WHERE requester_id={aid} AND recipient_id={eid}'
    wait(lambda:sql(pending)=='0','outgoing request cancelled')
    capture('nav-cancelled-outgoing');key('Escape');focus(a)
    record('qt-contacts-pending-isolation-accept-remove-cancel',['nav-02-accepted-contacts-only.png','nav-03-new-friends-incoming.png','nav-04-incoming-profile.png','nav-accepted-contact-ids.txt','nav-05-accepted-profile.png','nav-remove-confirm.png','nav-removed-friend.png','nav-outgoing-profile.png','nav-cancelled-outgoing.png'])
    click(41,95);click(380,37);capture('nav-06-chats-three-actions')
    click(420,69);capture('nav-07-add-friend-search')
    # Re-open Chats then the join entry, cancel its dialog without side effects.
    click(41,95);click(380,37);click(420,133)
    modal('A','加入群聊');capture('nav-08-join-action');key('Escape');focus(a)
    click(380,37);click(420,101)
    modal('A','创建群聊');capture('nav-09-create-select-contacts')
    click(535,90);paste(actor('S005')['username']);click(382,162)
    capture('nav-10-create-selected')
    click(665,593);capture('nav-11-create-name-step')
    title='Qt导航创建群_'+manifest['prefix'];paste(title);click(579,593)
    # The observer is an actual selected member and reads the authoritative result.
    def created():
        page=sdk('S005','get_conversations')
        return next((v for v in page['conversations'] if v['username']==title),None)
    conversation=wait(created,'new group visible to selected member')
    members=sdk('S005','get_members',conversation=conversation['id'])
    assert {m['id'] for m in members}=={aid,actor('S005')['id']},members
    capture('nav-12-created-group-open')
    click(610,24);modal('A','群资料');capture('nav-13-created-group-header-opens-details');key('Escape');focus(a)
    record('qt-chats-three-actions-and-two-step-create',['nav-01-chats-sidebar.png','nav-06-chats-three-actions.png','nav-07-add-friend-search.png','nav-08-join-action.png','nav-09-create-select-contacts.png','nav-11-create-name-step.png','nav-12-created-group-open.png','nav-13-created-group-header-opens-details.png'])
    click(41,724);modal('A',actor('A')['username']);capture('nav-14-bottom-account')
    click(580,596);modal('A','退出登录');capture('nav-15-logout-confirmation')
    click(675,320);assert presence(True),'Cancel must keep authenticated session'
    capture('nav-16-logout-cancelled');click(580,596);modal('A','退出登录');click(600,320)
    wait(lambda:presence(False),'Qt logout authoritative offline')
    assert c['clients']['A'].poll() is None
    capture('nav-17-logout-login-page')
    record('qt-bottom-account-cancel-and-confirm-logout',['nav-14-bottom-account.png','nav-15-logout-confirmation.png','nav-16-logout-cancelled.png','nav-17-logout-login-page.png'])
    return 3
