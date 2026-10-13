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
import shutil
from native_gui_scale import NativeGui
if not os.environ.get('DBUS_SESSION_BUS_ADDRESS'):
    os.execvp('dbus-run-session', ['dbus-run-session', '--', sys.executable, '-B', __file__, *sys.argv[1:]])
owned = []
gui = None

def cleanup_processes():
    global gui
    if gui is not None:
        gui.shutdown()
        gui = None
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
parser.add_argument('--openbox', default=shutil.which('openbox'), help='Verified Openbox binary for the owned X11 display')
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
os.environ['QT_LINUX_ACCESSIBILITY_ALWAYS_ON'] = '1'
os.environ['QT_ACCESSIBILITY'] = '1'
# A private settings home: remembered accounts and appearance must not change focus or colors.
os.environ['XDG_CONFIG_HOME'] = str(work / 'config')
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
# Adopt this driver's existing owned display and clients in the common native
# helper. Menu, label and row input stays on real Qt/XTest; fixture facts remain
# in the original SDK/SQL checks below.
import gi
gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib
S.run(['dbus-update-activation-environment', 'DISPLAY', 'XDG_CONFIG_HOME'], check=True,
      stdout=S.DEVNULL)
for name, argtypes, restype in [
    ('XQueryTree', [C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),
        C.POINTER(C.POINTER(C.c_ulong)),C.POINTER(C.c_uint)], C.c_int),
    ('XGetGeometry', [C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),
        C.POINTER(C.c_int),C.POINTER(C.c_uint),C.POINTER(C.c_uint),
        C.POINTER(C.c_uint),C.POINTER(C.c_uint)], C.c_int),
    ('XTranslateCoordinates', [C.c_void_p,C.c_ulong,C.c_ulong,C.c_int,C.c_int,
        C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_ulong)], C.c_int),
    ('XGetWindowAttributes', [C.c_void_p,C.c_ulong,C.c_void_p], C.c_int),
    ('XInternAtom', [C.c_void_p,C.c_char_p,C.c_int], C.c_ulong),
    ('XGetWindowProperty', [C.c_void_p,C.c_ulong,C.c_ulong,C.c_long,C.c_long,C.c_int,
        C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),C.POINTER(C.c_ulong),
        C.POINTER(C.c_ulong),C.POINTER(C.POINTER(C.c_ubyte))], C.c_int),
    ('XGetInputFocus', [C.c_void_p,C.POINTER(C.c_ulong),C.POINTER(C.c_int)], C.c_int),
    ('XFree', [C.c_void_p], C.c_int),
    ('XCloseDisplay', [C.c_void_p], C.c_int)]:
    function = getattr(x, name)
    function.argtypes, function.restype = argtypes, restype
if not args.openbox:
    raise RuntimeError('Supply a verified --openbox binary for native menu input')
wm_env = os.environ.copy()
wm_env['XDG_DATA_DIRS'] = str(pathlib.Path(args.openbox).resolve().parents[1] / 'share') + ':' + os.environ.get(
    'XDG_DATA_DIRS', '/usr/local/share:/usr/share')
wm = S.Popen([args.openbox, '--sm-disable'], stdout=(work/'openbox.log').open('w'), stderr=S.STDOUT,
             env=wm_env)
owned.append(wm)
gui = NativeGui.__new__(NativeGui)
gui.Atspi, gui.GLib, gui.ImageGrab = Atspi, GLib, ImageGrab
gui.cfg = {'binary':str(build/'qt/chat_qt'), 'actors':list(manifest['actors'].values()) + manifest.get('sdk', [])}
gui.work, gui.lib, gui.xt, gui.native, gui.display = work, x, xt, d, display
gui.processes, gui.clients, gui.apps, gui.rooms, gui.relays = [], {}, {}, {}, {}
gui.clipboard, gui.last_focus, gui.receipt_context = None, None, None
gui.redact_invite_ui = True
Atspi.init()
gui.wait(lambda: gui.property(gui.root(), '_NET_SUPPORTED'), 'Owned Openbox readiness')


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

def capture(label):
    time.sleep(0.2)
    path = work / (label + '.png')
    gui.screenshot(path)
    return str(path)

def launch(alias):
    clients[alias] = S.Popen([str(build / 'qt/chat_qt'), url], stdout=(work / ('qt-' + alias + '.log')).open('w'), stderr=S.STDOUT)
    owned.append(clients[alias])
    gui.clients[actor(alias)['id']] = clients[alias]
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

def login(alias, offset):
    # Password uses hex key events, never clipboard or process arguments.
    launch(alias)
    w = wait(lambda : window(clients[alias].pid), 'main window')
    x.XMoveResizeWindow(d, w, offset, 0, 1180, 760)
    x.XFlush(d)
    focus(w)
    gui.authenticate_existing(actor(alias), password)
    return w

def members(conversation):
    result = control('sdk', actor='S006', method='get_members', conversation=conversation)
    assert result['ok'], result
    return result['value']
def run_scale():
    login('A', 0)
    login('S030', 1200)
    aid, bid = actor('A')['id'], actor('S030')['id']

    def native_click(identity, label):
        gui.click(identity, gui.field(identity, label))

    def open_friend_chat(identity, username):
        gui.navigate(identity, '联系人')
        gui.paste(identity, gui.field(identity, '搜索联系人'), username)
        gui.click(identity, gui.wait_row(identity, '联系人列表', username))
        gui.wait(lambda: next((n for n in gui.nodes(identity) if n.get_name() == username
                 and n.get_role() == gui.Atspi.Role.PUSH_BUTTON), None),
                 'Native contact card opened exact direct conversation')
        gui.field(identity, '消息输入')

    def online_both():
        result = control('sdk', actor='S005', method='get_presence')
        return result.get('ok') and all(any(p['user'] == actor(alias)['id'] and p['online']
                                           for p in result['value']) for alias in ['A', 'S030'])
    wait(online_both, 'Both Qt users online')
    capture('01-two-qt-online')
    gui.open_room(aid, manifest['group']['title'])
    native_click(aid, '聊天信息')
    gui.field(aid, '100 位成员')
    capture('02-group-overview')
    assert len(members(manifest['group']['id'])) == 100
    native_click(aid, '成员')
    capture('03-all-members')
    rows = [node for node in gui.walk(gui.field(aid, '群成员'))
            if node.get_role() in (gui.Atspi.Role.LIST_ITEM, gui.Atspi.Role.TABLE_CELL)]
    target_name = actor('S030')['username'] + ' · 成员'
    target_index = next(index for index, row in enumerate(rows) if row.get_name() == target_name)
    gui.click(aid, gui.field(aid, '群成员'), x_margin=2)
    gui.key('Home')
    for _ in range(target_index):
        gui.key('Down')
    gui.wait(lambda: gui.wait_row(aid, '群成员', target_name).get_state_set().contains(
        gui.Atspi.StateType.SELECTED), 'Native keyboard selected the intended group member')
    gui.key('Menu')
    rectangle = gui.wait(lambda: gui.popup(aid), 'Native member operation menu')
    # Existing owner menu has Personal profile, Make admin, Remove member.
    click(rectangle['x'] + rectangle['width']//2,
          round(rectangle['y'] + 7 + .5*(rectangle['height']-14)/3))
    gui.field(aid, '添加好友')
    capture('04-stranger-profile')
    relation = f'SELECT count(*) FROM contacts WHERE owner_id={aid} AND contact_id={bid} OR owner_id={bid} AND contact_id={aid}'
    assert sql(relation) == '0'
    native_click(aid, '添加好友')
    pending = f'SELECT count(*) FROM friend_requests WHERE requester_id={aid} AND recipient_id={bid}'
    wait(lambda: sql(pending) == '1', 'Friend request recorded')
    query_fact('pending-contact-count', relation)
    capture('05-outgoing-pending')
    record('hundred-member-overview-and-stranger-request', [
        '02-group-overview.png', '03-all-members.png', '04-stranger-profile.png',
        '05-outgoing-pending.png', 'pending-contact-count.txt'])
    gui.incoming(bid, actor('A'))
    capture('06-readable-new-friends-entry')
    capture('07-incoming-and-outgoing')
    capture('08-incoming-profile')
    native_click(bid, '接受')
    wait(lambda: sql(relation) == '2' and sql(pending) == '0', 'Mutual acceptance')
    query_fact('accepted-contact-count', relation)
    capture('09-accepted-profile')
    native_click(bid, '发消息')
    gui.escape(aid)
    gui.escape(aid)
    open_friend_chat(aid, actor('S030')['username'])
    gui.paste(aid, gui.field(aid, '消息输入'), 'X11 Alice to friend 中文')
    gui.key('Return')
    open_friend_chat(bid, actor('A')['username'])
    gui.paste(bid, gui.field(bid, '消息输入'), 'X11 friend to Alice 收到')
    gui.key('Return')
    messages = "SELECT sender_id,conversation_id,body FROM messages WHERE body IN ('X11 Alice to friend 中文','X11 friend to Alice 收到') ORDER BY id"
    wait(lambda: len(sql(messages).splitlines()) == 2, 'Bidirectional messages')
    query_fact('bidirectional-message-facts', messages)
    capture('10-bidirectional-chat')
    record('request-accept-and-bidirectional-chat', [
        '06-readable-new-friends-entry.png', '07-incoming-and-outgoing.png', '08-incoming-profile.png',
        'accepted-contact-count.txt', '10-bidirectional-chat.png', 'bidirectional-message-facts.txt'])
    gui.navigate(aid, '新建群组')
    gui.field(aid, '选择群成员')
    capture('11-contact-picker')

    def picker_search():
        def target():
            dialog = next((node for node in gui.nodes(aid) if node.get_name() == '创建群聊'
                           and node.get_role() == gui.Atspi.Role.DIALOG), None)
            return next((node for node in gui.walk(dialog) if gui.visible(node)
                and node.get_role() in (gui.Atspi.Role.ENTRY, gui.Atspi.Role.TEXT)
                and node.get_state_set().contains(gui.Atspi.StateType.EDITABLE)), None)
        return gui.wait(target, 'Native create-group dialog contact search')
    def pick(alias):
        gui.paste(aid, picker_search(), actor(alias)['username'])
        gui.click(aid, gui.wait_row(aid, '选择群成员', actor(alias)['username']))
        gui.key('space')
    pick('S005')
    pick('S006')
    capture('12-multiple-selected')
    native_click(aid, actor('S005')['username'] + ' ×')
    pick('S007')
    capture('13-search-with-selected-chips')
    gui.paste(aid, picker_search(), '')
    native_click(aid, '下一步')
    capture('14-next-group-name')
    gui.paste(aid, gui.editable(aid, '群名称', '创建群聊'), 'Qt X11 验证群')
    native_click(aid, '创建')
    created = wait(lambda: sql("SELECT id FROM conversations WHERE title='Qt X11 验证群'"), 'Group created')
    actual = members(int(created))
    assert {m['id'] for m in actual} == {aid, actor('S006')['id'], actor('S007')['id']}, actual
    gui.open_room(aid, 'Qt X11 验证群')
    capture('15-created-group')
    record('two-step-group-multiselect-search-remove', [
        '11-contact-picker.png', '12-multiple-selected.png', '13-search-with-selected-chips.png',
        '14-next-group-name.png', '15-created-group.png'])
    gui.navigate(aid, '我的资料')
    gui.field(aid, '退出登录')
    capture('16-self-profile')
    native_click(aid, '退出登录')
    gui.confirmation_button(aid, '退出登录')
    capture('17-logout-confirmation')
    gui.click(aid, gui.confirmation_button(aid, '退出登录', cancel=True))
    capture('18-logout-cancelled')
    assert clients['A'].poll() is None
    assert online_both(), 'Cancelling logout must retain the authenticated session'
    native_click(aid, '退出登录')
    gui.click(aid, gui.confirmation_button(aid, '退出登录'))

    def offline():
        result = control('sdk', actor='S005', method='get_presence')
        return result.get('ok') and any(p['user'] == aid and not p['online'] for p in result['value'])
    wait(offline, 'Qt logout authoritative offline')
    assert clients['A'].poll() is None
    gui.field(aid, '登录')
    capture('19-logout-login-page')
    record('account-profile-confirmed-logout-keeps-app', [
        '16-self-profile.png', '17-logout-confirmation.png', '18-logout-cancelled.png', '19-logout-login-page.png'])
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
