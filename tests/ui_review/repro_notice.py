#!/usr/bin/env python3
"""Reproduce: join-accepted notice is wiped by the list refresh; opening a chat from Contacts keeps the Contacts sidebar.

Isolated database + owned server + Xvfb; reuses the repository SDK fixture.
"""
import ctypes as C, json, os, pathlib, re, secrets, selectors, subprocess as S, sys, time, urllib.request
from PIL import ImageGrab

repo = pathlib.Path(__file__).resolve().parents[2]
build = repo / 'build'
work = pathlib.Path(sys.argv[1]); work.mkdir(parents=True, exist_ok=True)
port = 18893
url = f'ws://127.0.0.1:{port}/ws'
owned = []
database = 'chat_repro_notice_' + time.strftime('%m%d%H%M%S')

def cleanup(drop):
    for p in reversed(owned):
        if p.poll() is None:
            p.terminate()
            try: p.wait(timeout=10)
            except S.TimeoutExpired: p.kill()
    if drop:
        os.environ['PGDATABASE'] = 'chat'
        S.run(['dropdb', '--maintenance-db=chat', database], check=False)

S.run(['createdb', '--maintenance-db=chat', '--template=template0', database], check=True)
os.environ['PGDATABASE'] = database
ok = False
try:
    with (work / 'migrations.log').open('w') as log:
        for path in sorted((repo / 'sql').glob('[0-9][0-9][0-9]_*.sql')):
            S.run(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-f', str(path)], stdout=log, stderr=log, check=True)
    server = S.Popen([str(build / 'chat_server'), str(port), '16', '4'], stdout=(work / 'server.log').open('w'), stderr=S.STDOUT)
    owned.append(server)
    for _ in range(150):
        try: urllib.request.urlopen(f'http://127.0.0.1:{port}/health', timeout=.5).close(); break
        except OSError: time.sleep(.1)
    helper = S.Popen([str(build / 'chat_tui_scale_fixture')], stdin=S.PIPE, stdout=S.PIPE,
                     stderr=(work / 'sdk.log').open('w'), text=True, bufsize=1)
    owned.append(helper)
    seq = 0
    def control(command, timeout=60, **payload):
        global seq
        seq += 1
        helper.stdin.write(json.dumps(dict(id=seq, command=command, **payload), ensure_ascii=False) + '\n'); helper.stdin.flush()
        sel = selectors.DefaultSelector(); sel.register(helper.stdout, selectors.EVENT_READ)
        assert sel.select(timeout), 'fixture timeout'
        r = json.loads(helper.stdout.readline()); assert r['ok'], r
        return r['result']
    def sdk(actor, method, **kw):
        r = control('sdk', actor=actor, method=method, **kw); assert r['ok'], r
        return r['value']
    def sql(q):
        return S.check_output(['psql', '-X', '-A', '-t', '-c', q], text=True).strip()

    password = secrets.token_hex(12)
    control('configure', url=url, prefix='ao' + time.strftime('%H%M%S'), password=password)
    m = control('seed_navigation', timeout=300)
    A, B, Cc = (m['actors'][k] for k in 'ABC')
    group = m['group']['id']
    control('connect', actors=['B', 'C'])

    control('connect', actors=['A', 'D'])
    token = m['invite_token']
    sdk('A', 'set_group_join_approval', conversation=group, required=True)
    dm = sdk('S005', 'open_direct_conversation', user=m['actors']['D']['id'])['conversation']
    sdk('S005', 'send_message', conversation=dm, text='hello-D')
    joined = sdk('D', 'join_group', token=token)
    facts = dict(join_state=joined)
    control('disconnect', actors=['D'])
    xvfb = S.Popen(['Xvfb', '-displayfd', '1', '-screen', '0', '1400x900x24', '-nolisten', 'tcp'], stdout=S.PIPE, stderr=S.DEVNULL, text=True)
    owned.append(xvfb)
    display = ':' + xvfb.stdout.readline().strip()
    os.environ['DISPLAY'] = display; os.environ['QT_QPA_PLATFORM'] = 'xcb'; os.environ['XDG_CONFIG_HOME'] = str(work / 'config')
    x = C.CDLL('libX11.so.6'); xt = C.CDLL('libXtst.so.6')
    x.XOpenDisplay.argtypes = [C.c_char_p]; x.XOpenDisplay.restype = C.c_void_p
    d = x.XOpenDisplay(display.encode())
    for name, a, r in [('XSetInputFocus', [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong], C.c_int), ('XFlush', [C.c_void_p], C.c_int),
                       ('XMoveResizeWindow', [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint], C.c_int),
                       ('XStringToKeysym', [C.c_char_p], C.c_ulong), ('XKeysymToKeycode', [C.c_void_p, C.c_ulong], C.c_uint),
                       ('XRaiseWindow', [C.c_void_p, C.c_ulong], C.c_int)]:
        f = getattr(x, name); f.argtypes = a; f.restype = r
    xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
    xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
    def key(name, ctrl=False):
        ctl = x.XKeysymToKeycode(d, x.XStringToKeysym(b'Control_L'))
        if ctrl: xt.XTestFakeKeyEvent(d, ctl, 1, 0)
        code = x.XKeysymToKeycode(d, x.XStringToKeysym(name.encode()))
        xt.XTestFakeKeyEvent(d, code, 1, 0); xt.XTestFakeKeyEvent(d, code, 0, 0)
        if ctrl: xt.XTestFakeKeyEvent(d, ctl, 0, 0)
        x.XFlush(d)
    def capture(label):
        time.sleep(.3); ImageGrab.grab(xdisplay=display).save(work / f'{label}.png')
    def wait(pred, label, timeout=20):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            v = pred()
            if v: return v
            time.sleep(.1)
        raise AssertionError(label)
    qt = S.Popen([str(build / 'qt/chat_qt'), url], stdout=(work / 'qt-A.log').open('w'), stderr=S.STDOUT)
    owned.append(qt)
    def window():
        for line in S.check_output(['xwininfo', '-root', '-tree'], text=True).splitlines():
            f = re.match(r'\s*(0x[0-9a-f]+) "([^"\n]*)".*? (\d+)x(\d+)', line)
            if f and 'Chat' in f[2] and int(f[3]) >= 100 and S.check_output(['xprop', '-id', f[1], '_NET_WM_PID'], text=True).strip().endswith(str(qt.pid)):
                return int(f[1], 16)
    w = wait(window, 'qt window')
    x.XMoveResizeWindow(d, w, 0, 0, 1180, 760); x.XRaiseWindow(d, w); x.XSetInputFocus(d, w, 2, 0); x.XFlush(d); time.sleep(.8)
    key('a', ctrl=True)
    S.run(['xclip', '-selection', 'clipboard', '-in'], input=m['actors']['D']['username'], text=True, check=True); key('v', True); time.sleep(.2)
    key('Tab'); time.sleep(.1)
    for ch in password: key(ch)
    key('Return')

    time.sleep(5)
    capture('0-D-logged-in')
    sdk('A', 'respond_group_join_request', conversation=group, user=m['actors']['D']['id'], accept=True)
    for i, t in enumerate([0.05, 0.15, 0.3, 0.6, 1.5]):
        time.sleep(t); capture(f'1-after-accept-{i}')
    # Contacts -> open chat
    def click(px, py):
        xt.XTestFakeMotionEvent(d, -1, px, py, 0); xt.XTestFakeButtonEvent(d, 1, 1, 0); xt.XTestFakeButtonEvent(d, 1, 0, 0); x.XFlush(d); time.sleep(.4)
    click(41, 165); capture('2-contacts-page')
    click(250, 180); time.sleep(1.5); capture('3-after-contact-click')
    (work / 'facts.json').write_text(json.dumps(facts, indent=2))
    print(json.dumps(facts, indent=2))
    ok = True
finally:
    cleanup(drop=True)
    print('database dropped' if ok else 'failed; database dropped anyway')
