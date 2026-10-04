#!/usr/bin/env python3
"""Small real tmux navigation smoke using the existing SDK/terminal driver."""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from tui_100_member_smoke import Driver, inverse_text, choose_row

READONLY='你们目前不是好友'

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
        self.wait(actor,'Login / Register')

    def login(self,actor,first=True):
        self.wait(actor,'Login / Register')
        if first:
            self.keys(actor,'Tab'); self.paste(actor,self.name(actor)); self.keys(actor,'Tab')
        self.paste(actor,self.password); self.keys(actor,'Enter')
        self.wait(actor,lambda s:'connected' in s and ('Chats' in s or 'Conversations' in s))

    def chats(self,actor):
        self.command(actor,'conversations')
        self.wait(actor,lambda s:'Chats' in s or 'Conversations' in s)

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


def readonly_navigation(d):
    marker=d.manifest['navigation']['history_marker']
    with d.case('readonly-enter-escape-navigation','Read-only history opens with Enter; Esc reaches Chats and stays there'):
        d.resize('A',80,24)
        d.select_chat('A',d.name('B'))
        d.wait('A',lambda s:marker in s and READONLY in s)
        d.screenshot('A','narrow-readonly-enter')
        d.keys('A','Escape')
        d.wait('A',lambda s:('Chats' in s or 'Conversations' in s) and READONLY not in s)
        d.screenshot('A','narrow-escape-list')
        # Returning by the top-level Chats action must have the same root
        # semantics. Esc at that root cannot reveal the prior read-only view.
        d.select_chat('A',d.name('B'))
        d.wait('A',READONLY)
        d.chats('A')
        d.keys('A','Escape')
        d.wait('A',lambda s:('Chats' in s or 'Conversations' in s) and READONLY not in s)
        d.screenshot('A','narrow-root-escape-stable')
        d.resize('A',160,45)
        d.select_chat('A',d.name('B'))
        d.wait('A',marker)
        d.keys('A','Escape')
        d.keys('A','j','Enter')
        d.wait('A',lambda s:d.title in s and 'members' in s)
        d.screenshot('A','wide-return-focus-opens-next-chat')


def tab_navigation(d):
    with d.case('tab-navigation-roots','Tab changes focus or request tabs; Esc returns directly to the parent'):
        d.resize('A',80,24)
        d.select_chat('A',d.name('B'))
        d.wait('A',READONLY)
        for _ in range(3):
            d.keys('A','Tab')
            d.wait('A',lambda s:('Chats' in s or 'Conversations' in s) and READONLY not in s)
            d.keys('A','Tab')
            d.wait('A',READONLY)
        d.keys('A','Escape')
        d.wait('A',lambda s:('Chats' in s or 'Conversations' in s) and READONLY not in s)
        d.keys('A','Escape')
        d.wait('A',lambda s:('Chats' in s or 'Conversations' in s) and READONLY not in s)
        d.screenshot('A','tab-chat-root-stable')
        d.keys('A','c'); d.wait('A','New friends (1)')
        d.keys('A','Enter');d.wait('A','New friends · Incoming')
        for _ in range(3):
            d.keys('A','Tab');d.wait('A','New friends · Outgoing')
            d.keys('A','Tab');d.wait('A','New friends · Incoming')
        d.keys('A','Tab');d.wait('A','New friends · Outgoing')
        d.keys('A','Escape');d.wait('A','New friends (1)')
        d.screenshot('A','request-tab-escape-contacts')


def friends_navigation(d):
    with d.case('contacts-and-new-friends','Accepted contacts and requests stay separate; cancel, accept, remove are real actions'):
        d.keys('A','c'); d.wait('A','New friends (1)')
        screen=d.capture('A')
        assert d.name('C') in screen and '观察S005_' in screen
        assert all(d.name(a) not in screen for a in ['B','D','E'])
        d.screenshot('A','accepted-list-only')
        d.control('connect',actors=['D','E','B'])
        d.keys('A','Enter');d.wait('A','New friends · Incoming')
        d.wait('A',d.name('D'));d.keys('A','Tab');d.wait('A','New friends · Outgoing')
        d.wait('A',d.name('E'));d.screenshot('A','outgoing-request')
        d.keys('A','x');d.wait('A','No pending friend requests')
        assert not d.query('E','get_friend_requests')['incoming']
        d.keys('A','Tab');d.wait('A',d.name('D'));d.keys('A','y')
        d.wait('A','No pending friend requests')
        assert any(v['id']==d.manifest['actors']['A']['id'] for v in d.query('D','get_contacts'))
        d.keys('A','Escape');d.wait('A','New friends (0)');d.wait('A',d.name('D'))
        d.command('A','search-users '+d.name('D'));d.wait('A','User search');d.wait('A',d.name('D'))
        d.keys('A','Enter');d.wait('A','Remove friend')
        d.command('A','remove-contact');d.confirm('A','删除好友')
        d.wait('A','Not friends')
        assert not any(v['id']==d.manifest['actors']['A']['id'] for v in d.query('D','get_contacts'))
        d.screenshot('A','accepted-then-removed')
        # The preserved B conversation exercises all three distinct read-only hints.
        d.select_chat('A',d.name('B'));d.wait('A',READONLY)
        d.query('B','send_friend_request',user='A')
        d.wait('A','对方已发送好友申请，确认后可继续聊天')
        d.screenshot('A','readonly-incoming-request')
        d.query('B','cancel_friend_request',user='A');d.wait('A',READONLY)
        d.command('A','profile');d.wait('A','Add friend')
        d.keys('A','j','Enter');d.wait('A','Waiting for acceptance')
        d.keys('A','Escape');d.wait('A','好友申请已发送，等待对方确认')
        d.screenshot('A','readonly-outgoing-request')
        d.command('A','profile');d.wait('A','Cancel friend request');d.keys('A','j','Enter')
        d.wait('A','Not friends')
        assert not d.query('B','get_friend_requests')['incoming']
        d.control('disconnect',actors=['B','D','E'])


def contacts_search(d):
    with d.case('contacts-local-search','Local accepted-only substring filtering supports ASCII case, clear, selection and profile'):
        d.keys('A','c','/');d.wait('A','搜索已接受的好友')
        d.paste('A','s005');d.keys('A','Enter')
        d.wait('A',lambda s:'观察S005_' in s and d.name('C') not in s and 'New friends (1)' in s)
        assert all(d.name(a) not in d.capture('A') for a in ['B','D','E'])
        d.keys('A','j','j');d.wait_selected('A','观察S005_')
        d.keys('A','Enter');d.wait('A','Remove friend');d.screenshot('A','filtered-contact-profile')
        d.keys('A','Escape','/');d.wait('A','搜索已接受的好友')
        d.keys('A','End','BSpace','BSpace','BSpace','BSpace','Enter')
        d.wait('A',lambda s:d.name('C') in s and '观察S005_' in s and 'New friends (1)' in s)
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
        d.keys('A','y');d.wait('A','No pending friend requests')
        assert any(v['id']==aid for v in d.query(order[2],'get_contacts'))
        d.query(order[2],'remove_contact',user='A')
        for peer in peers:
            d.command('A','search-users '+d.name(peer));d.wait('A',d.name(peer))
            d.keys('A','Enter');d.wait('A','Add friend');d.command('A','add');d.wait('A','Waiting for acceptance')
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
        d.wait('A','No pending friend requests')
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
        d.keys('A','Space');d.wait('A','Selected: 1');d.screenshot('A','space-target-after-friend-removal')
        d.command('A','filter no-matching-friend');d.wait('A','[x]')
        d.keys('A','Space');d.wait('A',lambda s:'Selected: 0' in s and names[order[1]] not in s)
        d.screenshot('A','filtered-selection-cancelled')
        d.keys('A','Escape');d.wait('A','New friends (1)')
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
        d.select_chat('A',d.title);d.wait('A','i: compose')
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


def new_actions(d):
    with d.case('new-menu-three-actions','Global New offers add friend, two-step create, and actual join'):
        d.keys('A','h','N');d.wait('A',lambda s: all(t in s for t in ['Add friend','Create group','Join group']))
        d.screenshot('A','new-three-actions')
        d.keys('A','Enter');d.wait('A','搜索用户（姓名前缀）')
        d.paste('A',d.name('E'));d.keys('A','Enter');d.wait('A','User search');d.wait('A',d.name('E'))
        d.keys('A','Enter');d.wait('A','Add friend');d.keys('A','j','Enter');d.wait('A','Waiting for acceptance')
        d.screenshot('A','new-add-friend-pending')
        d.keys('A','h','N','j','Enter');d.wait('A','Choose friends')
        # Both accepted friends are selected; pending E must not be an option.
        assert d.name('E') not in d.capture('A')
        d.keys('A','Space','j','Space');d.wait('A','Selected: 2')
        d.screenshot('A','create-select-accepted')
        d.keys('A','Enter');d.wait('A','下一步：群名称')
        title='TUI导航新群_'+d.args.run_id
        d.paste('A',title);d.keys('A','Enter');d.confirm('A','创建群聊')
        d.wait('A',lambda s:title in s and 'i: compose' in s)
        created=next(v for v in d.query('S005','get_conversations')['conversations'] if v['username']==title)
        actual=d.query('S005','get_members',conversation=created['id'])
        assert {v['id'] for v in actual}=={d.manifest['actors'][a]['id'] for a in ['A','C']}|{d.manifest['sdk'][0]['id']}
        d.screenshot('A','created-group-open')
        d.spawn_tui('E');d.login('E');d.keys('E','N','j','j','Enter');d.wait('E','加入群聊')
        d.paste('E','chat://join/'+d.manifest['invite_token']);d.keys('E','Enter')
        d.wait('E',lambda s:d.title in s and 'i: compose' in s)
        members=d.query('S005','get_members',conversation=d.group)
        assert len(members)==5 and any(v['id']==d.manifest['actors']['E']['id'] for v in members)
        d.screenshot('E','new-join-open')


def account_logout(d):
    with d.case('account-logout','Account opens explicitly and logout confirmation preserves or closes the real session'):
        d.keys('A','u');d.wait('A','Account ·');d.wait('A','Log out')
        d.screenshot('A','account-page')
        d.keys('A','j','j','j','Enter');d.wait('A','退出当前账号')
        d.keys('A','Escape');d.wait('A','Account ·')
        assert any(p['user']==d.manifest['actors']['A']['id'] and p['online'] for p in d.query('S005','get_presence'))
        d.keys('A','Enter');d.confirm('A','退出当前账号');d.wait('A','Login / Register')
        d.screenshot('A','logout-login-page')
        assert any(p['user']==d.manifest['actors']['A']['id'] and not p['online'] for p in d.query('S005','get_presence'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',default='build')
    parser.add_argument('--binary')
    parser.add_argument('--port',type=int,default=18884)
    parser.add_argument('--work-dir',default='/tmp/chat-navigation-'+time.strftime('%m%d%H%M%S'))
    parser.add_argument('--red-only',action='store_true',help='Only readonly/focus regression')
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
        readonly_navigation(driver)
        tab_navigation(driver)
        if not args.red_only:
            contacts_search(driver)
            picker_selection(driver)
            friends_navigation(driver)
            request_selection(driver)
            message_alignment(driver)
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
