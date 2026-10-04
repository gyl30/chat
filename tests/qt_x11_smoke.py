#!/usr/bin/env python3
"""Run real chat_qt processes under Xvfb/XTest with SDK-created fixtures.

From the repository root after sourcing the normal PostgreSQL test environment:
    cmake --build build --target chat_qt chat_server chat_tui_scale_fixture
    python3 -B tests/qt_x11_smoke.py

Requires Xvfb, xwininfo, xprop, xclip, libX11, libXtst and Pillow. Creates an
isolated database, applies migrations, and starts an owned server (port 18881
by default). Success drops that database unless --keep-db; failures retain it
and captures for diagnosis. Credentials stay in memory. No SQL business writes.

This explicit diagnostic uses a fixed 2400x1000 X11 display and two 1180x760
windows; it is not a CTest or a replacement for qt_ui assertions. Screenshots
support human visual review, without OCR. SDK/SQL facts verify business results.
"""
import argparse
import re
import socket
import hashlib
import atexit
import signal
import ctypes as C, json, os, pathlib, secrets, selectors, subprocess as S, sys, time, urllib.request
from PIL import ImageGrab
owned = []

def cleanup_processes():
    for proc in reversed(owned):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=15)
            except S.TimeoutExpired:
                proc.kill()
                proc.wait()
atexit.register(cleanup_processes)

def interrupted(signum, frame):
    # Let finally/atexit reap owned children even if another signal arrives.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    raise SystemExit(128 + signum)

signal.signal(signal.SIGINT, interrupted)
signal.signal(signal.SIGTERM, interrupted)
parser = argparse.ArgumentParser(description='Real xcb chat_qt smoke via Xvfb/XTest; requires a disposable DB creation role, xclip, Pillow and the SDK scale fixture target. No OCR or offscreen widget driver.')
parser.add_argument('--build', default='build')
parser.add_argument('--port', type=int, default=18881)
parser.add_argument('--output', default='/tmp/chat-qt-x11-' + time.strftime('%Y%m%d-%H%M%S'))
parser.add_argument('--keep-db', action='store_true')
parser.add_argument('--navigation-only', action='store_true', help='Six-user navigation regression instead of the scale scenario')
args = parser.parse_args()
work = pathlib.Path(args.output).resolve()
work.mkdir(mode=448, parents=True)
repo = pathlib.Path(__file__).resolve().parents[1]
build = pathlib.Path(args.build).resolve()
for binary in ['chat_server', 'chat_tui_scale_fixture', 'qt/chat_qt']:
    if not (build / binary).is_file():
        raise RuntimeError('Build required target: ' + binary)
url = 'ws://127.0.0.1:' + str(args.port) + '/ws'
with socket.socket() as available:
    available.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
    available.bind(('127.0.0.1', args.port))
(work / 'head.txt').write_text(S.check_output(['git', 'rev-parse', 'HEAD'], cwd=repo, text=True))
database = 'chat_qtx11_' + time.strftime('%m%d%H%M%S') + '_' + str(os.getpid())
maintenance = os.environ.get('PGDATABASE', 'postgres')
S.run(['createdb', '--maintenance-db=' + maintenance, '--template=template0', database], check=True)
os.environ['PGDATABASE'] = database
(work / 'database.txt').write_text(database)
with (work / 'migrations.log').open('w') as log:
    for path in sorted((repo / 'sql').glob('[0-9][0-9][0-9]_*.sql')):
        S.run(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-f', str(path)], stdout=log, stderr=log, check=True)
server = S.Popen([str(build / 'chat_server'), str(args.port), '256', '16'], stdout=(work / 'server.log').open('w'), stderr=S.STDOUT)
owned.append(server)
for _ in range(150):
    try:
        if server.poll() is not None:
            raise RuntimeError('Owned server exited before readiness')
        urllib.request.urlopen('http://127.0.0.1:' + str(args.port) + '/health', timeout=0.5).close()
        break
    except OSError:
        time.sleep(0.1)
else:
    raise RuntimeError('server health unavailable')
helper = S.Popen([str(build / 'chat_tui_scale_fixture')], stdin=S.PIPE, stdout=S.PIPE, stderr=(work / 'sdk-stderr.log').open('w'), text=True, bufsize=1)
owned.append(helper)
seq = 0

def control(command, timeout=30, **payload):
    global seq
    seq += 1
    helper.stdin.write(json.dumps(dict(id=seq, command=command, **payload), ensure_ascii=False) + '\n')
    helper.stdin.flush()
    sel = selectors.DefaultSelector()
    sel.register(helper.stdout, selectors.EVENT_READ)
    assert sel.select(timeout), 'SDK helper timeout'
    sel.close()
    result = json.loads(helper.stdout.readline())
    assert result['ok'], result
    if command != 'configure':
        (work / ('sdk-%04d-%s.json' % (seq, command))).write_text(json.dumps(result, ensure_ascii=False, indent=2))
    return result['result']
password = secrets.token_hex(16)
control('configure', url=url, prefix='qtx11_' + time.strftime('%H%M%S'), password=password)
print(json.dumps(dict(stage='seeding', work=str(work), database=database)), flush=True)
manifest = control('seed_navigation' if args.navigation_only else 'seed', timeout=600, **({} if args.navigation_only else {'pending':0}))
(work / 'manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2))
if not args.navigation_only:
    control('disconnect', actors=['S030'])
xvfb = S.Popen(['Xvfb', '-displayfd', '1', '-screen', '0', '2400x1000x24', '-nolisten', 'tcp'], stdout=S.PIPE, stderr=(work / 'xvfb.log').open('w'), text=True)
owned.append(xvfb)
display = ':' + xvfb.stdout.readline().strip()
os.environ['DISPLAY'] = display
os.environ['QT_QPA_PLATFORM'] = 'xcb'
x = C.CDLL('libX11.so.6')
xt = C.CDLL('libXtst.so.6')
x.XOpenDisplay.argtypes = [C.c_char_p]
x.XOpenDisplay.restype = C.c_void_p
d = x.XOpenDisplay(display.encode())
assert d
for (name, argtypes, ret) in [('XDefaultRootWindow', [C.c_void_p], C.c_ulong), ('XSetInputFocus', [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong], C.c_int), ('XFlush', [C.c_void_p], C.c_int), ('XMoveResizeWindow', [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint], C.c_int), ('XStringToKeysym', [C.c_char_p], C.c_ulong), ('XKeysymToKeycode', [C.c_void_p, C.c_ulong], C.c_uint), ('XRaiseWindow', [C.c_void_p, C.c_ulong], C.c_int)]:
    f = getattr(x, name)
    f.argtypes = argtypes
    f.restype = ret
xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
clients = {}

def actor(alias):
    return manifest['actors'].get(alias) or next((a for a in manifest['sdk'] if a['alias'] == alias))

def windows():
    return S.check_output(['xwininfo', '-root', '-tree'], text=True)

def focus(w):
    wait(lambda:'Map State: IsViewable' in S.check_output(['xwininfo','-id',hex(w)],text=True),
         'window is mapped before input focus')
    x.XRaiseWindow(d, w)
    x.XSetInputFocus(d, w, 2, 0)
    x.XFlush(d)
    time.sleep(0.12)

def key(name, ctrl=False):
    if ctrl:
        xt.XTestFakeKeyEvent(d, x.XKeysymToKeycode(d, x.XStringToKeysym(b'Control_L')), 1, 0)
    code = x.XKeysymToKeycode(d, x.XStringToKeysym(name.encode()))
    xt.XTestFakeKeyEvent(d, code, 1, 0)
    xt.XTestFakeKeyEvent(d, code, 0, 0)
    if ctrl:
        xt.XTestFakeKeyEvent(d, x.XKeysymToKeycode(d, x.XStringToKeysym(b'Control_L')), 0, 0)
    x.XFlush(d)

def paste(text):
    S.run(['xclip', '-selection', 'clipboard', '-in'], input=text, text=True, stdout=S.DEVNULL, stderr=S.DEVNULL, check=True)
    key('v', True)
    time.sleep(0.15)

def capture(label):
    time.sleep(0.2)
    path = work / (label + '.png')
    ImageGrab.grab(xdisplay=display).save(path)
    return str(path)

def launch(alias):
    clients[alias] = S.Popen([str(build / 'qt/chat_qt'), url], stdout=(work / ('qt-' + alias + '.log')).open('w'), stderr=S.STDOUT)
    owned.append(clients[alias])
    return clients[alias].pid

def click(px, py):
    # Brief X11 layout/input settling only; business outcomes use wait predicates.
    xt.XTestFakeMotionEvent(d, -1, px, py, 0)
    xt.XTestFakeButtonEvent(d, 1, 1, 0)
    xt.XTestFakeButtonEvent(d, 1, 0, 0)
    x.XFlush(d)
    time.sleep(0.15)

def wait(predicate, label, timeout=15):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = predicate()
        if result:
            return result
        time.sleep(0.1)
    raise AssertionError(label)

def sql(statement):
    # Only constant statements and fixture integer IDs reach this function.
    environment = os.environ.copy()
    environment['PGOPTIONS'] = '-c default_transaction_read_only=on'
    return S.check_output(['psql', '-X', '-A', '-t', '-v', 'ON_ERROR_STOP=1', '-c', statement], env=environment, text=True).strip()

def query_fact(label, statement):
    result = sql(statement)
    (work / (label + '.txt')).write_text(result + '\n')
    return result

def record(name, evidence):
    with (work / 'cases.jsonl').open('a') as stream:
        stream.write(json.dumps(dict(case=name, status='PASS', evidence=evidence), ensure_ascii=False) + '\n')
    print(json.dumps(dict(case=name, status='PASS')), flush=True)

def window(pid, title='Chat'):
    for line in windows().splitlines():
        found = re.match('\\s*(0x[0-9a-f]+) "([^"\\n]*)".*? (\\d+)x(\\d+)\\+(-?\\d+)\\+(-?\\d+)', line)
        if not found or title not in found[2] or int(found[3]) < 100:
            continue
        prop = S.check_output(['xprop', '-id', found[1], '_NET_WM_PID'], text=True)
        if prop.strip().endswith('= ' + str(pid)):
            return int(found[1], 16)
    return None

def modal(alias, contains):
    return wait(lambda : window(clients[alias].pid, contains), 'modal ' + contains)

def login(alias, offset):
    # Password uses hex key events, never clipboard or process arguments.
    launch(alias)
    w = wait(lambda : window(clients[alias].pid), 'main window')
    x.XMoveResizeWindow(d, w, offset, 0, 1180, 760)
    x.XFlush(d)
    focus(w)
    click(offset + 600, 358)
    paste(actor(alias)['username'])
    key('Tab')
    time.sleep(0.1)
    for char in password:
        key(char)
    key('Return')
    time.sleep(0.2)
    return w

def members(conversation):
    result = control('sdk', actor='S006', method='get_members', conversation=conversation)
    assert result['ok'], result
    return result['value']
def run_scale():
    a = login('A', 0)
    b = login('S030', 1200)

    def online_both():
        result = control('sdk', actor='S005', method='get_presence')
        return result.get('ok') and all((any((p['user'] == actor(alias)['id'] and p['online'] for p in result['value'])) for alias in ['A', 'S030']))
    wait(online_both, 'both Qt users online')
    capture('01-two-qt-online')
    focus(a)
    click(610, 24)
    group = modal('A', '群资料')
    capture('02-group-overview')
    assert len(members(manifest['group']['id'])) == 100
    click(580, 480)
    capture('03-all-members')
    click(480, 87)
    for char in 's030':
        key(char)
    time.sleep(0.2)
    key('Menu')
    time.sleep(0.2)
    key('Down')
    key('Return')
    profile = modal('A', actor('S030')['username'])
    capture('04-stranger-profile')
    aid = actor('A')['id']
    bid = actor('S030')['id']
    relation = f'SELECT count(*) FROM contacts WHERE owner_id={aid} AND contact_id={bid} OR owner_id={bid} AND contact_id={aid}'
    assert sql(relation) == '0'
    click(510, 382)
    pending = f'SELECT count(*) FROM friend_requests WHERE requester_id={aid} AND recipient_id={bid}'
    wait(lambda : sql(pending) == '1', 'friend request recorded')
    query_fact('pending-contact-count', relation)
    capture('05-outgoing-pending')
    record('hundred-member-overview-and-stranger-request', ['02-group-overview.png', '03-all-members.png', '04-stranger-profile.png', '05-outgoing-pending.png', 'pending-contact-count.txt'])
    focus(b)
    click(1240, 170)
    capture('06-readable-new-friends-entry')
    click(1430, 73)
    capture('07-incoming-and-outgoing')
    click(1430, 130)
    peer_profile = modal('S030', actor('A')['username'])
    capture('08-incoming-profile')
    click(1708, 366)
    wait(lambda : sql(relation) == '2' and sql(pending) == '0', 'mutual acceptance')
    query_fact('accepted-contact-count', relation)
    capture('09-accepted-profile')
    click(1708, 366)
    focus(profile)
    click(510, 382)
    focus(group)
    key('Escape')
    time.sleep(0.15)
    focus(a)
    click(620, 732)
    paste('X11 Alice to friend 中文')
    key('Return')
    focus(b)
    click(1800, 732)
    paste('X11 friend to Alice 收到')
    key('Return')
    messages = "SELECT sender_id,conversation_id,body FROM messages WHERE body IN ('X11 Alice to friend 中文','X11 friend to Alice 收到') ORDER BY id"
    wait(lambda : len(sql(messages).splitlines()) == 2, 'bidirectional messages')
    query_fact('bidirectional-message-facts', messages)
    capture('10-bidirectional-chat')
    record('request-accept-and-bidirectional-chat', ['06-readable-new-friends-entry.png', '07-incoming-and-outgoing.png', '08-incoming-profile.png', 'accepted-contact-count.txt', '10-bidirectional-chat.png', 'bidirectional-message-facts.txt'])
    focus(a)
    click(41, 95)
    click(380, 37)
    click(420, 101)
    wizard = modal('A', '创建群聊')
    capture('11-contact-picker')

    def pick(name, first=False):
        click(535, 90)
        key('a', True)
        paste(name)
        click(382, 162 if first else 280)
    pick('S005', True)
    pick('S006')
    capture('12-multiple-selected')
    click(450, 153)
    pick('S007')
    capture('13-search-with-selected-chips')
    click(535, 90)
    key('a', True)
    key('BackSpace')
    click(745, 593)
    capture('14-next-group-name')
    paste('Qt X11 验证群')
    click(745, 593)
    created = wait(lambda : sql("SELECT id FROM conversations WHERE title='Qt X11 验证群'"), 'group created')
    actual = members(int(created))
    assert {m['id'] for m in actual} == {aid, actor('S006')['id'], actor('S007')['id']}, actual
    capture('15-created-group')
    record('two-step-group-multiselect-search-remove', ['11-contact-picker.png', '12-multiple-selected.png', '13-search-with-selected-chips.png', '14-next-group-name.png', '15-created-group.png'])
    focus(a)
    click(41, 724)
    modal('A', actor('A')['username'])
    capture('16-self-profile')
    click(580, 596)
    modal('A', '退出登录')
    capture('17-logout-confirmation')
    click(600, 320)
    capture('18-logout-cancelled')
    assert clients['A'].poll() is None
    assert online_both(), 'Cancelling logout must retain the authenticated session'
    click(580, 596)
    modal('A', '退出登录')
    click(675, 320)

    def offline():
        result = control('sdk', actor='S005', method='get_presence')
        return result.get('ok') and any((p['user'] == aid and (not p['online']) for p in result['value']))
    wait(offline, 'Qt logout authoritative offline')
    assert clients['A'].poll() is None
    capture('19-logout-login-page')
    record('account-profile-confirmed-logout-keeps-app', ['16-self-profile.png', '17-logout-confirmation.png', '18-logout-cancelled.png', '19-logout-login-page.png'])
    return 4

try:
    if args.navigation_only:
        from qt_navigation_steps import run
        case_count = run(globals())
    else:
        case_count = run_scale()
    query_fact('final-user-count', 'SELECT count(*) FROM users')
    (work / 'capture-sha256.json').write_text(json.dumps({path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(work.glob('*.png'))}, indent=2))
    (work / 'result.json').write_text(json.dumps(dict(status='PASS', database=database, head=(work / 'head.txt').read_text().strip(), cases=case_count), indent=2))
    print(json.dumps(dict(status='PASS', evidence=str(work))), flush=True)
except BaseException:
    capture('failure')
    raise
finally:
    cleanup_processes()
    if not args.keep_db and (work / 'result.json').exists():
        S.run(['dropdb', '--maintenance-db=' + maintenance, database], check=True)
    else:
        print(json.dumps(dict(database_retained=database, evidence=str(work))), flush=True)
