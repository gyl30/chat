#!/usr/bin/env python3
"""One native Qt/X11 shard. A coordinator supplies private JSON over stdin.

No SDK, JSON-RPC client, business SQL or in-process Qt hooks are used here.
AT-SPI reads native accessibility state; XTest supplies all product input.
Run inside dbus-run-session. The first stdin line is configuration; after
all actors are ready the second line must be {"command":"start"}.
"""
import argparse
import ctypes as C
import ctypes.util
import json
import os
import pathlib
import re
import selectors
import signal
import tempfile
import subprocess
import sys
import time
import traceback
from urllib.parse import urlsplit

from native_network import Blackhole


_private_values = set()

# Native unread evidence explicitly selects Daylight and reads its installed highlight token.
# Theme colors are external product data, so the driver must not preserve an obsolete palette.
ACCENT_TOLERANCE = 32
# A badge is at least a 19x19 rounded rect, so it contributes far more matching pixels than any
# antialiased edge can. The threshold only separates "one is drawn" from "none is".
BADGE_ACCENT_MIN = 40


def theme_highlight_rgb(binary, theme='daylight'):
    path = pathlib.Path(binary).resolve().parent / 'themes' / (theme + '.json')
    value = json.loads(path.read_text())['colors']['highlight']
    if not isinstance(value, str) or re.fullmatch(r'#[0-9a-fA-F]{6}', value) is None:
        raise ValueError('Installed theme has no valid highlight color: ' + str(path))
    return tuple(int(value[index:index + 2], 16) for index in (1, 3, 5))


def accent_pixels(image, box, rgb):
    crop = image.crop(box)
    return sum(1 for pixel in crop.convert('RGB').getdata()
               if all(abs(channel - wanted) <= ACCENT_TOLERANCE
                      for channel, wanted in zip(pixel, rgb)))


def redact_text(value):
    for secret in _private_values:
        value = value.replace(secret, '[private]')
    return value


def redact_fields(value):
    if isinstance(value, str):
        return redact_text(value)
    if isinstance(value, dict):
        return {key: redact_fields(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [redact_fields(item) for item in value]
    return value


def emit(event, **fields):
    payload = redact_fields(dict(event=event, timestamp=time.time(), utc_ns=time.time_ns(), **fields))
    print(json.dumps(payload, ensure_ascii=False), flush=True)


def starttime(pid):
    return pathlib.Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[19]


def is_transient_window_property_error(error, window):
    # A window from XQueryTree may disappear before this read-only request.
    return (error['error_code'] == 3 and error['request_code'] == 20 and
            error['resource_id'] == window)


def is_transient_popup_geometry_error(error, window):
    # Only an already owned popup disappearing during these read-only queries:
    # BadWindow/BadDrawable from GetWindowAttributes (3), GetGeometry (14) or
    # QueryTree (15), and BadWindow from TranslateCoordinates (40).
    return (error['resource_id'] == window and
            ((error['error_code'] in (3, 9) and error['request_code'] in (3, 14, 15)) or
             (error['error_code'] == 3 and error['request_code'] == 40)))


# XWindowAttributes.map_state counted in c_int units, and IsViewable. The layout is
# cross-checked against XGetGeometry below, so a wrong offset cannot pass silently.
MAP_STATE_INDEX = 23
MAP_VIEWABLE = 2


def message_name_matches(name, marker):
    """Identify the body of a timestamped message in this native fixture.

    message_model's AccessibleTextRole joins sender, timestamp, optional reply,
    body and metadata with a Chinese semicolon. Our generated bodies/quotes do
    not contain that separator, and all fixture messages have timestamps.
    A quoted marker or a longer marker is not a receipt of the requested body.
    """
    parts = name.split('；')
    if len(parts) < 3:
        return False
    body_index = 3 if parts[2].startswith('回复 ') else 2
    if len(parts) <= body_index:
        return False
    body = parts[body_index]
    if body.startswith(('文件：', '图片：')):
        return body.split('\n', 1)[0].split('：', 1)[1] == marker
    tokens = body.split(None, 1)
    return bool(tokens) and tokens[0] == marker


class NativeGui:
    def __init__(self, config):
        import gi
        gi.require_version('Atspi', '2.0')
        from gi.repository import Atspi, GLib
        from PIL import ImageGrab
        self.Atspi, self.GLib, self.ImageGrab = Atspi, GLib, ImageGrab
        self.cfg = config
        self.work = pathlib.Path(config['work']).resolve()
        self.work.mkdir(parents=True, exist_ok=True, mode=0o700)
        self.processes, self.clients, self.apps = [], {}, {}
        self.relays = {}
        self.rooms = {}
        self.last_focus = None
        self.receipt_context = None
        self.badge_rgb = None
        self.clipboard = None
        self.display = None
        self.native = None
        self.private_home = pathlib.Path(tempfile.mkdtemp(prefix='chat-native-gui-'))
        for folder in ['runtime', 'data', 'cache']:
            (self.private_home / folder).mkdir(mode=0o700)
        (self.work / 'config').mkdir(exist_ok=True, mode=0o700)
        # Unix-domain socket paths have a small fixed limit; evidence paths may be long.
        os.environ.update(XDG_CONFIG_HOME=str(self.work / 'config'),
                          XDG_RUNTIME_DIR=str(self.private_home / 'runtime'),
                          XDG_DATA_HOME=str(self.private_home / 'data'),
                          XDG_CACHE_HOME=str(self.private_home / 'cache'))
        emit('private-runtime', path=str(self.private_home))
        read_fd, write_fd = os.pipe()
        try:
            self.spawn(['Xvfb', '-displayfd', str(write_fd), '-screen', '0',
                        '1600x1000x24', '-maxclients', '256', '-nolisten', 'tcp'],
                       'xvfb', pass_fds=(write_fd,))
        finally:
            os.close(write_fd)
        ready = selectors.DefaultSelector()
        ready.register(read_fd, selectors.EVENT_READ)
        try:
            if not ready.select(15):
                raise RuntimeError('Xvfb display readiness deadline')
            self.display = ':' + os.read(read_fd, 32).decode().strip()
        finally:
            ready.close()
            os.close(read_fd)
        os.environ.update(DISPLAY=self.display, QT_QPA_PLATFORM='xcb',
                          QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1', QT_ACCESSIBILITY='1')
        subprocess.run(['dbus-update-activation-environment', 'DISPLAY', 'XDG_RUNTIME_DIR',
                        'XDG_DATA_HOME', 'XDG_CONFIG_HOME', 'XDG_CACHE_HOME'], check=True,
                       stdout=sys.stderr)
        self.lib = C.CDLL(C.util.find_library('X11'))
        self.xt = C.CDLL(C.util.find_library('Xtst'))
        for name, argtypes, restype in [
            ('XOpenDisplay', [C.c_char_p], C.c_void_p),
            ('XQueryTree', [C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_ulong),
                C.POINTER(C.POINTER(C.c_ulong)),C.POINTER(C.c_uint)], C.c_int),
            ('XGetGeometry', [C.c_void_p,C.c_ulong,C.POINTER(C.c_ulong),C.POINTER(C.c_int),
                C.POINTER(C.c_int),C.POINTER(C.c_uint),C.POINTER(C.c_uint),C.POINTER(C.c_uint),
                C.POINTER(C.c_uint)], C.c_int),
            ('XTranslateCoordinates', [C.c_void_p,C.c_ulong,C.c_ulong,C.c_int,C.c_int,
                C.POINTER(C.c_int),C.POINTER(C.c_int),C.POINTER(C.c_ulong)], C.c_int),
            ('XGetWindowAttributes', [C.c_void_p,C.c_ulong,C.c_void_p], C.c_int),
            ('XDefaultRootWindow', [C.c_void_p], C.c_ulong),
            ('XInternAtom', [C.c_void_p, C.c_char_p, C.c_int], C.c_ulong),
            ('XGetWindowProperty', [C.c_void_p, C.c_ulong, C.c_ulong, C.c_long,
                C.c_long, C.c_int, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_int),
                C.POINTER(C.c_ulong), C.POINTER(C.c_ulong), C.POINTER(C.POINTER(C.c_ubyte))], C.c_int),
            ('XStringToKeysym', [C.c_char_p], C.c_ulong),
            ('XKeysymToKeycode', [C.c_void_p, C.c_ulong], C.c_uint),
            ('XRaiseWindow', [C.c_void_p, C.c_ulong], C.c_int),
            ('XSetInputFocus', [C.c_void_p, C.c_ulong, C.c_int, C.c_ulong], C.c_int),
            ('XGetInputFocus', [C.c_void_p, C.POINTER(C.c_ulong), C.POINTER(C.c_int)], C.c_int),
            ('XMoveResizeWindow', [C.c_void_p, C.c_ulong, C.c_int, C.c_int, C.c_uint, C.c_uint], C.c_int),
            ('XFlush', [C.c_void_p], C.c_int),
            ('XFree', [C.c_void_p], C.c_int),
            ('XCloseDisplay', [C.c_void_p], C.c_int)]:
            func = getattr(self.lib, name)
            func.argtypes, func.restype = argtypes, restype
        self.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
        self.native = self.lib.XOpenDisplay(self.display.encode())
        if not self.native:
            raise RuntimeError('Cannot connect to owned X display')
        openbox = config.get('openbox')
        if not openbox:
            raise RuntimeError('A verified Openbox binary is required')
        wm = self.work / 'openbox.xml'
        wm.write_text('<openbox_config xmlns="http://openbox.org/3.4/rc"><theme><name>Clearlooks</name></theme><desktops><number>1</number></desktops></openbox_config>')
        env = os.environ.copy()
        if config.get('openbox_data'):
            env['XDG_DATA_DIRS'] = config['openbox_data'] + ':/usr/share'
        self.spawn([openbox, '--sm-disable', '--config-file', str(wm)], 'openbox', env=env)
        self.Atspi.init()
        self.wait(lambda: self.property(self.root(), '_NET_SUPPORTED'), 'Owned WM readiness')

    def spawn(self, command, label, **kwargs):
        with (self.work / (label + '.log')).open('wb') as log:
            process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, **kwargs)
        identity = starttime(process.pid)
        self.processes.append((process, identity))
        emit('resource-started', kind=label, pid=process.pid, starttime=identity)
        return process

    def root(self):
        return self.lib.XDefaultRootWindow(self.native)

    def property(self, window, name):
        # QMenu destruction can race the PID/type reads after XQueryTree.
        # Limit the handler to this synchronous read and this exact resource;
        # unrelated X errors must still fail instead of terminating in C.
        class XErrorEvent(C.Structure):
            _fields_ = [('type', C.c_int), ('display', C.c_void_p),
                ('resourceid', C.c_ulong), ('serial', C.c_ulong),
                ('error_code', C.c_ubyte), ('request_code', C.c_ubyte), ('minor_code', C.c_ubyte)]
        errors = []
        callback_type = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(XErrorEvent))
        def record_error(display, pointer):
            error = pointer.contents
            errors.append({'display': error.display, 'resource_id': error.resourceid,
                'serial': error.serial, 'error_code': error.error_code,
                'request_code': error.request_code, 'minor_code': error.minor_code})
            return 0
        handler = callback_type(record_error)
        atom = self.lib.XInternAtom(self.native, name.encode(), False)
        self.lib.XSetErrorHandler.argtypes = [C.c_void_p]
        self.lib.XSetErrorHandler.restype = C.c_void_p
        previous = self.lib.XSetErrorHandler(C.cast(handler, C.c_void_p))
        actual, fmt, count, remaining = C.c_ulong(), C.c_int(), C.c_ulong(), C.c_ulong()
        data = C.POINTER(C.c_ubyte)()
        try:
            result = self.lib.XGetWindowProperty(self.native, window,
                atom, 0, 16384, False, 0, C.byref(actual), C.byref(fmt),
                C.byref(count), C.byref(remaining), C.byref(data))
        finally:
            self.lib.XSetErrorHandler(previous)
        values = []
        try:
            if result == 0 and data and not errors and fmt.value == 32:
                values = list(C.cast(data, C.POINTER(C.c_ulong))[:count.value])
        finally:
            if data:
                self.lib.XFree(data)
        for error in errors:
            if error['display'] != self.native or not is_transient_window_property_error(error, window):
                emit('native-x-error', window=window, property=name, error=error)
                raise RuntimeError('Unexpected X11 error while reading window property: ' + str(error))
            emit('native-window-gone', window=window, property=name, error=error,
                 reason='window destroyed during GetProperty')
        return [] if errors else values

    def pump(self):
        context = self.GLib.MainContext.default()
        # Drain the current batch, without a self-maintaining busy loop.
        for _ in range(256):
            if not context.pending():
                break
            context.iteration(False)

    def wait(self, predicate, description, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.pump()
            try:
                result = predicate()
            except self.GLib.GError:
                # Qt recycles view accessibles; an object that vanished between
                # enumeration and reading is the same not-settled condition the
                # caller already polls for, not a failed assertion.
                result = None
            if result:
                return result
            for actor, process in self.clients.items():
                if process.poll() is not None:
                    raise RuntimeError(f'Native actor {actor} exited {process.returncode}')
            time.sleep(.08)
        raise TimeoutError(description)

    def walk(self, node):
        if node is None:
            return
        yield node
        try:
            children = node.get_child_count()
        except self.GLib.GError:
            return
        for index in range(children):
            try:
                child = node.get_child_at_index(index)
            except self.GLib.GError:
                continue
            if child is not None:
                yield from self.walk(child)

    def app(self, actor):
        self.pump()
        process = self.clients[actor]
        if actor in self.apps:
            return self.apps[actor]
        desktop = self.Atspi.get_desktop(0)
        # Qt's GTK3 platform theme loads atk-bridge into the same process, which
        # registers a second, empty AT-SPI application. Only the Qt registration
        # exposes the widget tree, so require a child instead of the first match.
        for index in range(desktop.get_child_count()):
            app = desktop.get_child_at_index(index)
            if app is not None and app.get_process_id() == process.pid and app.get_child_count():
                self.apps[actor] = app
                return app

    def visible(self, node):
        try:
            return node.get_state_set().contains(self.Atspi.StateType.SHOWING)
        except self.GLib.GError:
            return False

    def find(self, actor, name, under=None):
        root = self.app(actor)
        if root is None:
            return None
        if under:
            root = next((n for n in self.walk(root) if self.visible(n) and n.get_name() == under), None)
        if root is None:
            return None
        matches = [n for n in self.walk(root) if self.visible(n) and n.get_name() == name]
        if name in ('用户名', '密码', '确认密码', '服务器地址'):
            matches = [n for n in matches if n.get_role() in
                (self.Atspi.Role.ENTRY, self.Atspi.Role.PASSWORD_TEXT, self.Atspi.Role.TEXT)]
        return matches[-1] if matches else None

    def field(self, actor, name, under=None):
        return self.wait(lambda: self.find(actor, name, under), 'Missing native control ' + name)

    def nodes(self, actor):
        return [n for n in self.walk(self.app(actor)) if self.visible(n)]

    def text(self, node):
        if node.get_role() == self.Atspi.Role.PASSWORD_TEXT:
            raise RuntimeError('Password text inspection is forbidden')
        text = node.get_text_iface()
        return text.get_text(0, text.get_character_count())

    def focus(self, actor):
        self.last_focus = actor
        windows = [w for w in self.property(self.root(), '_NET_CLIENT_LIST')
                   if self.property(w, '_NET_WM_PID') == [self.clients[actor].pid]]
        modal = self.lib.XInternAtom(self.native, b'_NET_WM_STATE_MODAL', False)
        targets = [w for w in windows if modal in self.property(w, '_NET_WM_STATE')] or windows
        if not targets:
            raise RuntimeError('No owned native window for actor')
        self.last_focus_window = targets[-1]
        self.lib.XRaiseWindow(self.native, targets[-1])
        self.lib.XSetInputFocus(self.native, targets[-1], 1, 0)
        self.lib.XFlush(self.native)
        time.sleep(.04)

    def key(self, name, modifiers=()):
        def edge(key, down):
            code = self.lib.XKeysymToKeycode(self.native, self.lib.XStringToKeysym(key.encode()))
            if not code or not self.xt.XTestFakeKeyEvent(self.native, code, down, 0):
                raise RuntimeError('Unsupported native key')
        for modifier in modifiers:
            edge(modifier, True)
        edge(name, True)
        edge(name, False)
        for modifier in reversed(modifiers):
            edge(modifier, False)
        self.lib.XFlush(self.native)

    def click(self, actor, node, button=1, x_margin=None):
        name, role = node.get_name(), node.get_role()
        parent = node.get_parent()
        list_scope = parent.get_name() if parent is not None and parent.get_role() in (
            self.Atspi.Role.LIST, self.Atspi.Role.TREE_TABLE, self.Atspi.Role.TABLE) else None
        self.focus(actor)
        def target_geometry():
            # Raising another client's window can cause its list model to
            # refresh. Resolve the named native control after focus instead of
            # asking a destroyed accessibility proxy for its component.
            candidates = [n for n in self.nodes(actor) if n.get_name() == name and n.get_role() == role
                and (not list_scope or (n.get_parent() is not None and n.get_parent().get_name() == list_scope))]
            for candidate in reversed(candidates):
                component = candidate.get_component_iface()
                if component is None:
                    continue
                rect = component.get_extents(self.Atspi.CoordType.SCREEN)
                if rect.width > 0 and rect.height > 0:
                    return rect
        rect = self.wait(target_geometry, 'Native target unavailable after focus: ' + name)
        if list_scope:
            emit('input-target', actor=actor, name=name, list_scope=list_scope,
                 rectangle=[rect.x, rect.y, rect.width, rect.height])
        x = rect.x + rect.width // 2 if x_margin is None else rect.x + x_margin
        self.xt.XTestFakeMotionEvent(self.native, -1, x, rect.y + rect.height // 2, 0)
        self.xt.XTestFakeButtonEvent(self.native, button, True, 0)
        self.xt.XTestFakeButtonEvent(self.native, button, False, 0)
        self.lib.XFlush(self.native)
        time.sleep(.05)

    def paste(self, actor, node, value):
        if self.clipboard is not None:
            self.clipboard.terminate()
            self.clipboard.wait(timeout=3)
        self.clipboard = subprocess.Popen(['xclip', '-quiet', '-selection', 'clipboard'],
                                          stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL)
        self.processes.append((self.clipboard, starttime(self.clipboard.pid)))
        self.clipboard.stdin.write(value.encode())
        self.clipboard.stdin.close()
        self.click(actor, node)
        self.key('a', ('Control_L',))
        self.key('v', ('Control_L',))
        self.wait(lambda: self.text(node) == value, 'Native clipboard paste receipt')

    def screenshot(self, path):
        image = self.ImageGrab.grab(xdisplay=self.display)
        regions = []
        if getattr(self, 'redact_invite_ui', False):
            # Preserve the actual X11 capture, masking only semantic invite-code
            # controls. Check even before reading a new secret: a failed AT-SPI
            # read must not expose a code which the native UI already displayed.
            for actor, process in self.clients.items():
                if process.poll() is not None:
                    continue
                nodes = self.nodes(actor)
                if not nodes:
                    raise RuntimeError('Cannot inspect private native UI for screenshot redaction')
                for node in nodes:
                    name = node.get_name()
                    if not (name == '群邀请码' or
                            (name == '邀请码' and node.get_role() in (self.Atspi.Role.ENTRY, self.Atspi.Role.TEXT)) or
                            (name.startswith('邀请码 · ') and not name.endswith('尚未生成'))):
                        continue
                    component = node.get_component_iface()
                    if component is None:
                        raise RuntimeError('Cannot locate private native invite control for screenshot redaction')
                    rect = component.get_extents(self.Atspi.CoordType.SCREEN)
                    left, top = max(0, rect.x), max(0, rect.y)
                    right, bottom = min(image.width, rect.x+rect.width), min(image.height, rect.y+rect.height)
                    if right > left and bottom > top:
                        image.paste('#142737', (left, top, right, bottom))
                        regions.append([left, top, right, bottom])
        image.save(path)
        if regions:
            emit('screenshot-redaction', path=str(path), reason='Private invitation-code regions masked', regions=regions)

    def capture(self, actor, label):
        self.focus(actor)
        time.sleep(.08)
        path = self.work / (label + '.png')
        self.screenshot(path)
        emit('capture', actor=actor, path=str(path))

    def rows(self, actor, view):
        node = self.field(actor, view)
        return [n for n in self.walk(node) if self.visible(n) and
                n.get_role() in (self.Atspi.Role.LIST_ITEM, self.Atspi.Role.TABLE_CELL)]

    def message(self, actor, marker):
        view = self.find(actor, '消息记录')
        if view is None or view.get_component_iface() is None:
            return None
        bounds = view.get_component_iface().get_extents(self.Atspi.CoordType.SCREEN)
        for node in self.walk(view):
            if not self.visible(node) or not message_name_matches(node.get_name(), marker):
                continue
            component = node.get_component_iface()
            if component is None:
                continue
            rect = component.get_extents(self.Atspi.CoordType.SCREEN)
            # Qt may mark virtual list rows SHOWING even when they are clipped
            # outside the viewport. Count a screen receipt only for an actual
            # visible row, and never right-click coordinates in the composer.
            if rect.height > 0 and bounds.y <= rect.y + rect.height/2 < bounds.y + bounds.height:
                return node

    def expect_message(self, actor, marker, description, sender=None, timeout=15):
        self.receipt_context = {'actor': actor, 'sender': sender, 'marker': marker}
        try:
            result = self.wait(lambda: self.message(actor, marker), description, timeout=timeout)
        except TimeoutError:
            def observation():
                view = self.find(actor, '消息记录')
                component = view.get_component_iface() if view else None
                bounds = component.get_extents(self.Atspi.CoordType.SCREEN) if component else None
                rows = []
                for node in self.walk(view):
                    if marker not in node.get_name():
                        continue
                    component = node.get_component_iface()
                    rect = component.get_extents(self.Atspi.CoordType.SCREEN) if component else None
                    rows.append({'name': node.get_name(), 'role': node.get_role_name(),
                                 'exact_body': message_name_matches(node.get_name(), marker),
                                 'showing': self.visible(node),
                                 'rectangle': [rect.x, rect.y, rect.width, rect.height] if rect else None})
                nodes = self.nodes(actor)
                headers = [{'name': n.get_name(), 'role': n.get_role_name()} for n in nodes
                    if n.get_role() == self.Atspi.Role.PUSH_BUTTON and n.get_name() in
                    (self.cfg_actor(actor)['peer_username'], self.cfg_actor(actor)['group_title'],
                     self.cfg_actor(actor).get('medium_title'))]
                focused = [{'name': n.get_name(), 'role': n.get_role_name()} for n in nodes
                    if n.get_state_set().contains(self.Atspi.StateType.FOCUSED)]
                native_focus = self.focus_observation(actor)
                return dict(room=self.rooms.get(actor), headers=headers, matching_rows=rows,
                     history_rectangle=[bounds.x,bounds.y,bounds.width,bounds.height] if bounds else None,
                     focused_controls=focused, native_focus=native_focus,
                     visible_history_names=[n.get_name() for n in self.rows(actor, '消息记录')][-12:])
            before = observation()
            emit('receipt-diagnostic', actor=actor, sender=sender, marker=marker, **before)
            self.capture(actor, 'receipt-failure-' + str(actor))
            # Failure-only investigation: once the requested row demonstrably
            # exists, issue one real End and record its effect. This does not
            # retry the business assertion or convert the failed run to PASS.
            if any(row['exact_body'] for row in before['matching_rows']):
                try:
                    self.latest(actor)
                    try:
                        self.wait(lambda: self.message(actor, marker),
                                  'Failure-only End observation', timeout=2)
                    except TimeoutError:
                        pass
                    emit('receipt-end-diagnostic', actor=actor, sender=sender, marker=marker,
                         after_native_end_visible=bool(self.message(actor, marker)), **observation())
                    self.capture(actor, 'receipt-after-end-' + str(actor))
                except Exception as diagnostic_error:
                    emit('receipt-end-diagnostic-failure', actor=actor,
                         error=type(diagnostic_error).__name__ + ': ' + str(diagnostic_error))
            if sender is not None and sender != actor:
                self.capture(sender, 'receipt-sender-' + str(sender))
            raise
        self.receipt_context = None
        return result

    def focus_observation(self, actor):
        input_window, revert = C.c_ulong(), C.c_int()
        self.lib.XGetInputFocus(self.native, C.byref(input_window), C.byref(revert))
        active = self.property(self.root(), '_NET_ACTIVE_WINDOW')
        return dict(input_window=input_window.value,
            input_pid=self.property(input_window.value, '_NET_WM_PID') if input_window.value > 1 else [],
            wm_active_window=active,
            wm_active_pid=self.property(active[0], '_NET_WM_PID') if active else [],
            intended_actor=getattr(self, 'last_focus', None),
            intended_window=getattr(self, 'last_focus_window', None),
            actor_windows=[w for w in self.property(self.root(), '_NET_CLIENT_LIST')
                if self.property(w, '_NET_WM_PID') == [self.clients[actor].pid]])

    def history_observation(self, actor, phase):
        # Only the explicit two-actor diagnostic pilot uses this observation.
        # AT-SPI calls themselves take time: record that cost rather than treat
        # a before/after sample as an atomic snapshot of the Qt event loop.
        began = time.monotonic()
        try:
            view = self.find(actor, '消息记录')
            component = view.get_component_iface() if view else None
            bounds = component.get_extents(self.Atspi.CoordType.SCREEN) if component else None
            children = list(self.walk(view))
            rows = [n for n in children if n.get_role() in
                    (self.Atspi.Role.LIST_ITEM, self.Atspi.Role.TABLE_CELL)]
            def row_info(node):
                component = node.get_component_iface()
                rect = component.get_extents(self.Atspi.CoordType.SCREEN) if component else None
                return {'name': node.get_name(), 'showing': self.visible(node),
                        'focused': node.get_state_set().contains(self.Atspi.StateType.FOCUSED),
                        'rectangle': [rect.x, rect.y, rect.width, rect.height] if rect else None}
            scrollbars = []
            for node in children:
                if node.get_role() != self.Atspi.Role.SCROLL_BAR:
                    continue
                value = node.get_value_iface()
                scrollbars.append({'name': node.get_name(), 'showing': self.visible(node),
                    'minimum': value.get_minimum_value() if value else None,
                    'maximum': value.get_maximum_value() if value else None,
                    'current': value.get_current_value() if value else None})
            controls = self.nodes(actor)
            emit('native-history-end-observation', actor=actor, sample_phase=phase,
                 room=self.rooms.get(actor), accessible_row_count=len(rows),
                 history_list_focused=view.get_state_set().contains(self.Atspi.StateType.FOCUSED) if view else None,
                 history_rectangle=[bounds.x, bounds.y, bounds.width, bounds.height] if bounds else None,
                 first_row=row_info(rows[0]) if rows else None,
                 last_row=row_info(rows[-1]) if rows else None,
                 focused_rows=[row_info(n) for n in rows if n.get_state_set().contains(self.Atspi.StateType.FOCUSED)],
                 scrollbars=scrollbars,
                 loading_label_visible=any(n.get_name() == '正在加载消息…' for n in controls),
                 focused_controls=[{'name': n.get_name(), 'role': n.get_role_name()} for n in controls
                     if n.get_state_set().contains(self.Atspi.StateType.FOCUSED)],
                 native_focus=self.focus_observation(actor),
                 observation_ms=(time.monotonic()-began)*1000)
        except Exception as error:
            # A diagnostic read failure does not add a business retry or hide
            # the missing evidence; the original native End still executes.
            emit('native-history-end-observation-error', actor=actor, sample_phase=phase,
                 error=type(error).__name__ + ': ' + str(error))

    def earliest(self, actor):
        # A group burst legitimately keeps the newest row at the bottom, so an
        # earlier peer row is read with the same real scroll-up a user performs.
        self.click(actor, self.field(actor, '消息记录'), x_margin=2)
        self.key('Home')

    def latest(self, actor):
        # The row's centre may be an image with a native left-click preview.
        # The history viewport's two-pixel gutter is outside every bubble and
        # avatar; it focuses the list without opening message content.
        self.click(actor, self.field(actor, '消息记录'), x_margin=2)
        if self.cfg.get('history_diagnostic', False):
            self.history_observation(actor, 'before-end')
        # QListView initializes an invalid current index to its first row even
        # for End. Home establishes that index before End navigates to the last
        # row; use the same native sequence every time, not a failed-read retry.
        self.key('Home')
        self.key('End')
        if self.cfg.get('history_diagnostic', False):
            self.history_observation(actor, 'after-end')

    def dialog(self, actor, label):
        for node in self.nodes(actor):
            try:
                if node.get_role() == self.Atspi.Role.DIALOG and node.get_name() == label and self.visible(node):
                    return node
            except self.GLib.GError:
                continue
        return None

    def close_contact_dialogs(self, actor):
        # Only these two persistent contact dialogs may be dismissed before a
        # main-menu action. A nested profile must be completed by its caller.
        for label in ('添加联系人', '联系人'):
            if self.dialog(actor, label) is None:
                continue
            if self.find(actor, '关闭资料') is not None:
                raise RuntimeError('Finish the active profile before main-menu navigation')
            self.focus(actor)
            self.key('Escape')
            self.wait(lambda: self.dialog(actor, label) is None,
                      'Native contact dialog did not close: ' + label)

    def navigate(self, actor, label):
        actions = ['我的资料', '聊天', '联系人', '新的朋友', '添加联系人', '新建群组', '加入群组']
        if label not in actions:
            raise ValueError('Unknown native main-menu action: ' + label)
        self.close_contact_dialogs(actor)
        self.click(actor, self.field(actor, '主菜单'))
        rectangle = self.wait(lambda: self.popup(actor), 'Owned native main navigation menu unavailable')
        # QMenu exports differ between Qt platform integrations. When Qt exposes
        # visible items, verify their stable labels; input still uses the actual
        # owned popup geometry: seven equal rows with one separator after the first four.
        labels = []
        for node in self.nodes(actor):
            try:
                if node.get_role() == self.Atspi.Role.MENU_ITEM:
                    labels.append(node.get_name())
            except self.GLib.GError:
                # Qt can remove a previous transient accessible while opening
                # this popup. Its owned X11 geometry remains the input target.
                continue
        if labels and not all(any(name == action or name.startswith(action + ' ') or
                                 name.startswith(action + '（') or name.startswith(action + '(')
                                 for name in labels) for action in actions):
            raise RuntimeError('Native main menu labels differ from its seven actions: ' + str(labels))
        index = actions.index(label)
        px = rectangle['x'] + rectangle['width'] // 2
        scale = float(os.environ.get('QT_SCALE_FACTOR', '1'))
        padding, separator = 7 * scale, 9 * scale
        row_height = (rectangle['height'] - 2 * padding - separator) / len(actions)
        py = round(rectangle['y'] + padding + (index + .5) * row_height +
                   (separator if index >= 4 else 0))
        self.xt.XTestFakeMotionEvent(self.native, -1, px, py, 0)
        self.xt.XTestFakeButtonEvent(self.native, 1, True, 0)
        self.xt.XTestFakeButtonEvent(self.native, 1, False, 0)
        self.lib.XFlush(self.native)
        emit('main-menu-route', actor=actor, action=label, index=index,
             context_actions=actions, exported_labels=labels, rectangle=rectangle, target=[px, py])
        self.wait(lambda: self.popup(actor) is None, 'Native main menu remained open after destination click')

    def open_room(self, actor, title):
        self.navigate(actor, '聊天')
        row = self.wait(lambda: next((n for n in self.rows(actor, '会话列表')
                        if n.get_name() == title), None), 'Conversation unavailable ' + title)
        self.click(actor, row)
        self.key('Return')
        # The chat identity is an actual QPushButton (not the similarly named
        # sidebar row). Wait for that header before attributing any new input.
        self.wait(lambda: next((n for n in self.nodes(actor) if n.get_name() == title and
            n.get_role() == self.Atspi.Role.PUSH_BUTTON and
            n.get_state_set().contains(self.Atspi.StateType.SENSITIVE)), None),
            'Native conversation header did not switch to ' + title)
        self.field(actor, '消息输入')
        self.rooms[actor] = title
        self.latest(actor)

    def send(self, actor, marker, body=None):
        editor = self.field(actor, '消息输入')
        context = self.cfg_actor(actor)
        title = self.rooms.get(actor)
        if not title:
            raise RuntimeError('Message input has no verified native conversation header')
        if title == context['group_title']:
            message_context = {'conversation': context['group_id']}
        elif title == context['peer_username']:
            message_context = {'conversation': context['direct_id']}
        elif context.get('medium_title') == title:
            message_context = {'conversation': context['medium_id']}
        else:
            # A newly created group's ID is deliberately not exposed in native
            # UI. The coordinator resolves this unique title using read-only DB.
            message_context = {'conversation_title': title}
        begin = time.monotonic()
        emit('send-start', actor=actor, marker=marker, **message_context)
        self.paste(actor, editor, body or marker)
        self.key('Return')
        self.wait(lambda: self.text(editor) == '', 'Send acknowledgement did not clear native draft')
        self.latest(actor)
        node = self.expect_message(actor, marker, 'Sent text not present in native history', sender=actor)
        emit('ui-message', actor=actor, marker=marker, name=node.get_name(),
             description=node.get_description(), role=node.get_role_name())
        emit('screen-received', actor=actor, marker=marker)
        emit('send-confirmed', actor=actor, marker=marker, **message_context,
             elapsed_ms=(time.monotonic()-begin)*1000)

    def popup(self, actor):
        root, parent = C.c_ulong(), C.c_ulong()
        children, count = C.POINTER(C.c_ulong)(), C.c_uint()
        self.lib.XQueryTree(self.native, self.root(), C.byref(root), C.byref(parent),
                            C.byref(children), C.byref(count))
        try:
            windows = list(children[:count.value])
        finally:
            if children:
                self.lib.XFree(children)
        popup_type = self.lib.XInternAtom(self.native, b'_NET_WM_WINDOW_TYPE_POPUP_MENU', False)
        for window in reversed(windows):
            if self.property(window, '_NET_WM_PID') != [self.clients[actor].pid]:
                continue
            if popup_type not in self.property(window, '_NET_WM_WINDOW_TYPE'):
                continue
            rectangle = self.popup_geometry(actor, window)
            if rectangle is not None:
                return rectangle

    def popup_geometry(self, actor, window):
        # popup() has just verified this window's actor PID and popup type.
        # Closing a QMenu can destroy it between XQueryTree and GetGeometry;
        # libX11's default error handler would exit before Python cleanup.
        class XErrorEvent(C.Structure):
            _fields_ = [('type', C.c_int), ('display', C.c_void_p),
                ('resourceid', C.c_ulong), ('serial', C.c_ulong),
                ('error_code', C.c_ubyte), ('request_code', C.c_ubyte), ('minor_code', C.c_ubyte)]
        errors = []
        callback_type = C.CFUNCTYPE(C.c_int, C.c_void_p, C.POINTER(XErrorEvent))
        def record_error(display, pointer):
            error = pointer.contents
            # Never raise or print through a C callback. Decide in Python after
            # the synchronous request returns and restore the prior handler.
            errors.append({'display': error.display, 'resource_id': error.resourceid,
                'serial': error.serial, 'error_code': error.error_code,
                'request_code': error.request_code, 'minor_code': error.minor_code})
            return 0
        handler = callback_type(record_error)
        self.lib.XSetErrorHandler.argtypes = [C.c_void_p]
        self.lib.XSetErrorHandler.restype = C.c_void_p
        previous = self.lib.XSetErrorHandler(C.cast(handler, C.c_void_p))
        root, child = C.c_ulong(), C.c_ulong()
        x,y,w,h,border,depth = C.c_int(),C.c_int(),C.c_uint(),C.c_uint(),C.c_uint(),C.c_uint()
        attributes = C.create_string_buffer(192)
        map_state = None
        try:
            valid = self.lib.XGetGeometry(self.native, window, C.byref(root), C.byref(x), C.byref(y),
                C.byref(w), C.byref(h), C.byref(border), C.byref(depth))
            # A closed QMenu keeps its native window, and XGetGeometry still answers for
            # it, so only a viewable window can receive the native click this rectangle
            # is used for.
            if valid and not errors:
                if self.lib.XGetWindowAttributes(self.native, window, attributes):
                    found = C.cast(attributes, C.POINTER(C.c_int))
                    # Both queries report the inside geometry, so they must agree. This is
                    # what makes a wrong MAP_STATE_INDEX impossible to miss.
                    if (found[2], found[3]) != (w.value, h.value):
                        raise RuntimeError('XWindowAttributes layout differs from XGetGeometry: '
                                           + str([found[i] for i in range(4)]))
                    map_state = found[MAP_STATE_INDEX]
                else:
                    valid = 0
            if valid and not errors:
                valid = self.lib.XTranslateCoordinates(self.native, window, self.root(), 0, 0,
                    C.byref(x), C.byref(y), C.byref(child))
        finally:
            self.lib.XSetErrorHandler(previous)
        for error in errors:
            if error['display'] != self.native or not is_transient_popup_geometry_error(error, window):
                emit('native-x-error', actor=actor, window=window, error=error)
                raise RuntimeError('Unexpected X11 error while querying owned popup geometry: ' + str(error))
            emit('native-window-gone', actor=actor, window=window, error=error,
                 reason='owned popup destroyed during GetGeometry or TranslateCoordinates')
        if errors or not valid or map_state != MAP_VIEWABLE or w.value <= 0 or h.value <= 0:
            return None
        return {'window': window, 'x': x.value, 'y': y.value, 'width': w.value, 'height': h.value}

    def menu(self, actor, marker, action):
        row = self.expect_message(actor, marker, 'Message operation target not visible')
        self.click(actor, row, 3)
        # Qt popup menu items are not consistently exported as SHOWING by AT-SPI.
        # Locate the actual owned popup window before supplying native mouse input.
        context = self.cfg_actor(actor)
        group = self.rooms.get(actor) == context['group_title']
        attachment = '文件：' in row.get_name() or '图片：' in row.get_name()
        own = row.get_name().startswith('你')
        # This fixture uploads files without a text caption. Qt offers Copy only
        # for a nonempty message body, so attachment menus have one fewer row.
        actions = ['回复'] if attachment else ['复制', '回复']
        if group and context['role'] in ('owner', 'admin'):
            actions.append('置顶消息')
        actions.append('表情回应')
        if group and own:
            actions.append('已读详情')
        if attachment:
            actions.append('下载文件')
            if '图片：' in row.get_name():
                actions.append('查看图片')
        if own and not attachment:
            actions.append('编辑')
        if own:
            actions.append('删除')
        if action not in actions:
            raise RuntimeError('Requested native menu action unavailable for fixture context')
        rectangle = self.wait(lambda: self.popup(actor), 'Owned native popup menu unavailable')
        # Existing QMenu has six pixels of padding and a one-pixel border.
        padding = 7
        px = rectangle['x'] + rectangle['width']//2
        py = round(rectangle['y'] + padding + (actions.index(action)+.5)*
                   (rectangle['height']-2*padding)/len(actions))
        emit('menu-route',actor=actor,marker=marker,action=action,index=actions.index(action),
             context_actions=actions,rectangle=rectangle,target=[px,py])
        self.xt.XTestFakeMotionEvent(self.native,-1,px,py,0)
        self.lib.XFlush(self.native)
        time.sleep(.08)
        self.screenshot(self.work / ('menu-'+str(actor)+'-'+action+'.png'))
        self.xt.XTestFakeButtonEvent(self.native,1,True,0)
        self.xt.XTestFakeButtonEvent(self.native,1,False,0)
        self.lib.XFlush(self.native)
        if action == '回复':
            self.field(actor, '取消回复')
        elif action == '编辑':
            self.field(actor, '消息内容')
        elif action == '下载文件':
            self.field(actor, '保存文件')
        elif action == '删除':
            def confirmation():
                # QMessageBox is exposed as ALERT by Qt's accessibility bridge;
                # its role is different from a normal form dialog.
                for node in self.nodes(actor):
                    if node.get_role() not in (self.Atspi.Role.ALERT, self.Atspi.Role.DIALOG):
                        continue
                    children = list(self.walk(node))
                    buttons = {n.get_name() for n in children if self.visible(n)
                               and n.get_role() == self.Atspi.Role.PUSH_BUTTON}
                    if {'删除', '取消'} <= buttons and any('确认删除' in n.get_name() for n in children):
                        return node
            self.wait(confirmation, 'Delete confirmation did not open')
        return rectangle

    def reaction_pick(self, actor, marker, choice):
        parent = self.menu(actor, marker, '表情回应')
        def child_popup():
            rectangle = self.popup(actor)
            return rectangle if rectangle and rectangle['window'] != parent['window'] else None
        rectangle = self.wait(child_popup, 'Native reaction submenu unavailable')
        px = rectangle['x'] + rectangle['width']//2
        py = round(rectangle['y'] + 7 + (choice+.5)*(rectangle['height']-14)/6)
        self.xt.XTestFakeMotionEvent(self.native,-1,px,py,0)
        self.xt.XTestFakeButtonEvent(self.native,1,True,0)
        self.xt.XTestFakeButtonEvent(self.native,1,False,0)
        self.lib.XFlush(self.native)

    def escape(self, actor):
        self.focus(actor)
        self.key('Escape')
        time.sleep(.08)

    def wait_row(self, actor, view, name):
        return self.wait(lambda: next((n for n in self.rows(actor, view)
                        if n.get_name() == name), None), 'Native list row unavailable ' + name)

    def click_prefix(self, actor, prefix):
        node = self.wait(lambda: next((n for n in self.nodes(actor)
            if n.get_name().startswith(prefix) and n.get_role() in
            (self.Atspi.Role.PUSH_BUTTON, self.Atspi.Role.TOGGLE_BUTTON)), None),
            'Native action unavailable ' + prefix)
        self.click(actor, node)

    def friend_request(self, actor, peer):
        self.navigate(actor, '添加联系人')
        self.paste(actor, self.field(actor, '搜索用户'), peer['username'])
        self.key('Return')
        self.click(actor, self.wait_row(actor, '用户搜索结果', peer['username']))
        self.field(actor, '关闭资料')
        self.click(actor, self.field(actor, '添加好友'))
        self.field(actor, '等待验证')

    def incoming(self, actor, requester):
        self.navigate(actor, '新的朋友')
        self.click(actor, self.wait_row(actor, '收到的好友申请', requester['username']))
        self.key('Return')
        self.field(actor, '接受')

    def remove_friend(self, actor, peer):
        self.navigate(actor, '联系人')
        self.click(actor, self.wait_row(actor, '联系人列表', peer['username']), x_margin=24)
        self.field(actor, '关闭资料')
        self.click(actor, self.field(actor, '移除联系人'))
        self.click(actor, self.confirmation_button(actor, '移除联系人'))
        self.wait(lambda: not any(n.get_name() == peer['username'] for n in self.rows(actor, '联系人列表')),
                  'Removed friend remains in contacts')
        if not self.rows(actor, '联系人列表'):
            self.capture(actor, 'friendship-empty-contacts-' + str(actor))
            emit('friendship-empty-contacts', actor=actor, accepted_rows=0)
        for owner, target in [(actor, peer), (peer['id'], {'username': self.cfg_actor(actor)['username']})]:
            self.navigate(owner, '聊天')
            self.wait(lambda o=owner,t=target: not any(n.get_name() == t['username'] for n in self.rows(o, '会话列表')),
                      'Nonfriend direct remains visible')

    def cfg_actor(self, actor):
        return next(a for a in self.cfg['actors'] if a['id'] == actor)

    def friendship(self, actor, marker):
        identity = actor['id']
        peer = next((a for a in self.cfg['actors'] if a['id'] == actor['peer_id']), None)
        if peer is None:
            raise RuntimeError('Friendship scenario requires both native peers in the shard')
        self.open_room(identity, peer['username'])
        self.send(identity, marker + '-before')
        self.remove_friend(identity, peer)
        emit('friendship-state', actor=identity, peer=peer['id'], state='removed-hidden')
        self.friend_request(identity, peer)
        self.incoming(peer['id'], actor)
        self.click(peer['id'], self.field(peer['id'], '拒绝'))
        self.field(identity, '添加好友')
        emit('friendship-state', actor=identity, peer=peer['id'], state='rejected')
        self.click(identity, self.field(identity, '添加好友'))
        self.field(identity, '等待验证')
        self.click(identity, self.field(identity, '取消申请'))
        self.field(identity, '添加好友')
        # The UI deliberately retains a handled rejection row for feedback.
        # Verify the relationship action returns to stranger, not absence of that row.
        self.field(peer['id'], '添加到通讯录')
        emit('friendship-state', actor=identity, peer=peer['id'], state='cancelled')
        self.click(identity, self.field(identity, '添加好友'))
        self.field(identity, '等待验证')
        self.incoming(peer['id'], actor)
        self.click(peer['id'], self.field(peer['id'], '接受'))
        self.field(identity, '消息')
        self.click(identity, self.field(identity, '关闭资料'))
        for current, target in [(actor, peer), (peer, actor)]:
            self.navigate(current['id'], '联系人')
            self.wait_row(current['id'], '联系人列表', target['username'])
            self.open_room(current['id'], target['username'])
            self.expect_message(current['id'], marker + '-before',
                                'Accepted friend original direct history not restored', sender=identity)
        self.send(identity, marker + '-after')
        self.expect_message(peer['id'], marker + '-after', 'Reaccepted direct delivery absent', sender=identity)
        emit('screen-received', actor=peer['id'], marker=marker + '-after', from_actor=identity)
        emit('friendship-state', actor=identity, peer=peer['id'], state='reaccepted-original-history')
        self.capture(identity, 'friendship-restored-' + str(identity))

    def file_path(self, actor, path):
        def entry():
            dialogs = [n for n in self.nodes(actor) if n.get_role() in
                (self.Atspi.Role.DIALOG, self.Atspi.Role.FILE_CHOOSER) and
                n.get_name() in ('发送文件或图片', '保存文件', '更换头像')]
            choices = [n for dialog in dialogs for n in self.walk(dialog) if self.visible(n)
                and n.get_role() in (self.Atspi.Role.ENTRY, self.Atspi.Role.TEXT) and
                n.get_state_set().contains(self.Atspi.StateType.EDITABLE)]
            preferred = [n for n in choices if '文件名' in n.get_name() or 'File name' in n.get_name()]
            return preferred[-1] if preferred else choices[-1] if choices else None
        try:
            node = self.wait(entry, 'Native file chooser filename entry unavailable')
        except TimeoutError:
            screenshot = self.work / ('native-chooser-failure-' + str(actor) + '.png')
            self.screenshot(screenshot)
            emit('capture', actor=actor, path=str(screenshot))
            desktop = self.Atspi.get_desktop(0)
            for index in range(desktop.get_child_count()):
                app = desktop.get_child_at_index(index)
                if app is None:
                    continue
                controls = [{'name': n.get_name(), 'role': n.get_role_name()} for n in self.walk(app)
                    if self.visible(n) and n.get_role() in (self.Atspi.Role.DIALOG, self.Atspi.Role.FILE_CHOOSER,
                        self.Atspi.Role.ENTRY, self.Atspi.Role.TEXT, self.Atspi.Role.PUSH_BUTTON)]
                emit('native-picker-tree', actor=actor, pid=app.get_process_id(), name=app.get_name(), controls=controls)
            raise
        self.paste(actor, node, str(path))
        self.key('Return')

    def attachments(self, actor, marker):
        import hashlib
        from PIL import Image
        identity = actor['id']
        peer = next((a for a in self.cfg['actors'] if a['id'] == actor['peer_id']), None)
        if peer is None:
            raise RuntimeError('Attachment scenario requires both native peers in the shard')
        files = self.work / 'attachment-fixture'
        files.mkdir(exist_ok=True)
        binary = files / (marker + '-file 中文 文件.bin')
        binary.write_bytes(bytes(range(256)) * 16)
        png = files / (marker + '.png')
        jpeg = files / (marker + '.jpg')
        Image.new('RGB', (32, 32), (61, 132, 117)).save(png)
        Image.new('RGB', (32, 32), (121, 161, 182)).save(jpeg)
        for path in [binary, png, jpeg]:
            self.open_room(identity, peer['username'])
            self.click(identity, self.field(identity, '发送文件或图片'))
            self.file_path(identity, path)
            self.expect_message(identity, path.name, 'Uploaded attachment absent in sender native UI', sender=identity)
            self.open_room(peer['id'], actor['username'])
            self.expect_message(peer['id'], path.name, 'Attachment absent in peer native UI', sender=identity)
            self.menu(peer['id'], path.name, '下载文件')
            save = self.wait(lambda: self.find(peer['id'], '保存文件') if self.find(peer['id'], '保存文件') and
                self.find(peer['id'], '保存文件').get_state_set().contains(self.Atspi.StateType.SENSITIVE) else None,
                'Downloaded attachment did not enable save')
            self.click(peer['id'], save)
            output = files / ('saved-' + str(peer['id']) + '-' + path.name)
            if output.exists():
                raise RuntimeError('Fresh attachment destination already exists')
            self.file_path(peer['id'], output)
            self.wait(output.exists, 'Native attachment save did not create destination')
            original_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            saved_hash = hashlib.sha256(output.read_bytes()).hexdigest()
            if saved_hash != original_hash:
                raise RuntimeError('Native attachment SHA256 differs')
            emit('attachment-verified', actor=identity, peer=peer['id'], filename=path.name,
                 size=path.stat().st_size, original_sha256=original_hash, saved_sha256=saved_hash)
            self.escape(peer['id'])
        self.capture(peer['id'], 'attachments-' + str(identity))

    def login(self, actor, password):
        identity = actor['id']
        env = os.environ.copy()
        private = self.work / 'config' / str(identity)
        private.mkdir(parents=True, exist_ok=True)
        env['XDG_CONFIG_HOME'] = str(private)
        process = self.spawn([self.cfg['binary'], actor.get('relay_url', self.cfg['server_url'])],
                             'qt-' + str(identity), env=env)
        self.clients[identity] = process
        emit('started', actor=identity, pid=process.pid, starttime=starttime(process.pid), display=self.display)
        self.authenticate_existing(actor, password)

    def authenticate_existing(self, actor, password, reauthentication=False, expected_error=None):
        identity = actor['id']
        process = self.clients[identity]
        self.field(identity, '注册账号')
        self.paste(identity, self.field(identity, '用户名'), actor['username'])
        password_field = self.field(identity, '密码')
        self.click(identity, password_field)
        self.key('a', ('Control_L',))
        self.key('BackSpace')
        if not password or any(c not in '0123456789abcdef' for c in password):
            raise RuntimeError('Coordinator must supply a hex ASCII private password')
        for character in password:
            self.key(character)
        self.wait(lambda: password_field.get_text_iface().get_character_count() == len(password),
                  'Password input length did not arrive')
        before = time.monotonic()
        self.click(identity, self.field(identity, '登录'))
        if expected_error is not None:
            self.field(identity, expected_error)
            self.wait(lambda: self.field(identity, '登录').get_state_set().contains(
                self.Atspi.StateType.SENSITIVE) and self.find(identity, '主菜单') is None,
                'Rejected native login did not remain on the usable login page')
            emit('login-rejected', actor=identity, pid=process.pid,
                 reason=expected_error, elapsed_ms=(time.monotonic() - before) * 1000)
            return
        self.field(identity, '主菜单')
        emit('reauthenticated' if reauthentication else 'authenticated', actor=identity, pid=process.pid,
             login_ms=(time.monotonic() - before) * 1000)

    def editable(self, actor, name, dialog=None):
        def target():
            root = self.app(actor)
            if dialog:
                root = next((n for n in self.walk(root) if self.visible(n) and
                    n.get_name() == dialog and n.get_role() == self.Atspi.Role.DIALOG), None)
            matches = [n for n in self.walk(root) if self.visible(n) and n.get_name() == name and
                n.get_role() in (self.Atspi.Role.ENTRY, self.Atspi.Role.TEXT) and
                n.get_state_set().contains(self.Atspi.StateType.EDITABLE)]
            return matches[-1] if matches else None
        return self.wait(target, 'Native editable control unavailable: ' + name)

    def confirmation_button(self, actor, action, cancel=False):
        def target():
            for dialog in self.nodes(actor):
                if dialog.get_role() not in (self.Atspi.Role.ALERT, self.Atspi.Role.DIALOG):
                    continue
                buttons = [n for n in self.walk(dialog) if self.visible(n) and
                           n.get_role() == self.Atspi.Role.PUSH_BUTTON]
                names = {n.get_name() for n in buttons}
                if action in names and '取消' in names:
                    return next(n for n in buttons if n.get_name() == ('取消' if cancel else action))
        return self.wait(target, 'Native confirmation unavailable: ' + action)

    def keyboard_focus(self, actor, name):
        # Native Tab navigation also scrolls the management QScrollArea to the
        # focused widget; SHOWING alone does not establish a visible hit target.
        self.focus(actor)
        for _ in range(48):
            focused = [n for n in self.nodes(actor) if n.get_name() == name and
                n.get_state_set().contains(self.Atspi.StateType.FOCUSED)]
            if focused:
                return focused[-1]
            self.key('Tab')
            self.pump()
            time.sleep(.04)
        raise RuntimeError('Native Tab could not reach ' + name)

    def window_box(self, actor):
        # The on-screen box of the window click() has just raised. It bounds where a
        # native button event can actually arrive.
        attributes = C.create_string_buffer(192)
        if not self.lib.XGetWindowAttributes(self.native, self.last_focus_window, attributes):
            return None
        found = C.cast(attributes, C.POINTER(C.c_int))
        x, y, child = C.c_int(), C.c_int(), C.c_ulong()
        if not self.lib.XTranslateCoordinates(self.native, self.last_focus_window, self.root(),
                                             0, 0, C.byref(x), C.byref(y), C.byref(child)):
            return None
        return {'x': x.value, 'y': y.value, 'width': found[2], 'height': found[3]}

    def wheel_down(self, actor, box, clicks=2):
        self.xt.XTestFakeMotionEvent(self.native, -1, box['x'] + box['width']//2,
                                     box['y'] + box['height']//2, 0)
        for _ in range(clicks):
            self.xt.XTestFakeButtonEvent(self.native, 5, True, 0)
            self.xt.XTestFakeButtonEvent(self.native, 5, False, 0)
        self.lib.XFlush(self.native)
        time.sleep(.1)

    def reachable(self, actor, name, under=None):
        # QPlainTextEdit keeps Tab for indentation, so the focus chain cannot pass the group
        # announcement editor; widgets below it are scrolled into view with a native wheel
        # gesture and only then clicked. A widget clipped by its scroll area still reports
        # extents outside the window, which is how "visible" is decided here.
        self.focus(actor)
        box = self.window_box(actor)
        if box is None:
            raise RuntimeError('Owned native window box unavailable')
        scroll_box = clip_box = box
        for scrollbar in self.nodes(actor):
            try:
                if scrollbar.get_role() != self.Atspi.Role.SCROLL_BAR:
                    continue
                parent = scrollbar.get_parent()
                if parent.get_role() != self.Atspi.Role.FILLER:
                    continue  # QPlainTextEdit's inner scrollbar belongs to TEXT.
                rect = scrollbar.get_component_iface().get_extents(self.Atspi.CoordType.SCREEN)
                viewport = parent.get_component_iface().get_extents(self.Atspi.CoordType.SCREEN)
                if 0 < rect.width < rect.height and box['x'] <= viewport.x and box['y'] <= viewport.y and \
                        viewport.x + viewport.width <= box['x'] + box['width'] and \
                        viewport.y + viewport.height <= box['y'] + box['height']:
                    scroll_box = dict(x=rect.x, y=rect.y, width=rect.width, height=rect.height)
                    clip_box = dict(x=viewport.x, y=viewport.y, width=viewport.width, height=viewport.height)
                    emit('native-scroll-target', actor=actor, action=name, role='vertical-scrollbar',
                         parent_role=parent.get_role_name(), rectangle=scroll_box, viewport=clip_box)
                    break
            except self.GLib.GError:
                # A role update can dispose a proxy after walk() enumerates it.
                continue
        for _ in range(10):
            node = self.find(actor, name, under)
            if node is None:
                self.wheel_down(actor, scroll_box)
                continue
            component = node.get_component_iface()
            rect = component.get_extents(self.Atspi.CoordType.SCREEN) if component else None
            if rect is not None and rect.width > 0 and rect.height > 0 and \
                    box['x'] <= rect.x and rect.y >= clip_box['y'] and \
                    rect.x + rect.width <= box['x'] + box['width'] and \
                    rect.y + rect.height <= clip_box['y'] + clip_box['height']:
                return node
            self.wheel_down(actor, scroll_box)
        raise RuntimeError('Native wheel did not bring the target into the window: ' + name)


    def preference_toggle(self, actor, title, index, expected_action):
        self.open_room(actor, title)
        self.click(actor, self.wait_row(actor, '会话列表', title), 3)
        rectangle = self.wait(lambda: self.popup(actor), 'Conversation preferences menu unavailable')
        # Daily pilot 01 proved this real two-row popup can be absent from
        # the AT-SPI app tree despite being drawn correctly. Preserve the real
        # screenshot and do not claim unavailable accessible text was verified.
        action_names = [n.get_name() for n in self.walk(self.app(actor))
                        if n.get_role() == self.Atspi.Role.MENU_ITEM]
        row = self.wait_row(actor, '会话列表', title)
        emit('native-preference-menu-observed', actor=actor, conversation_title=title,
             expected_action=expected_action, rectangle=rectangle,
             accessible_action_names=action_names,
             accessible_label_verified=expected_action in action_names,
             row_accessible_description=row.get_description(),
             proof='owned native popup screenshot; intermediate label needs visual review if inaccessible')
        path = self.work / ('preference-' + str(actor) + '-' + expected_action + '.png')
        # Do not focus the main window while its modal popup is open.
        self.screenshot(path)
        emit('capture', actor=actor, path=str(path))
        px = rectangle['x'] + rectangle['width']//2
        py = round(rectangle['y'] + 7 + (index+.5)*(rectangle['height']-14)/2)
        self.xt.XTestFakeMotionEvent(self.native, -1, px, py, 0)
        self.xt.XTestFakeButtonEvent(self.native, 1, True, 0)
        self.xt.XTestFakeButtonEvent(self.native, 1, False, 0)
        self.lib.XFlush(self.native)
        self.wait(lambda: not self.popup(actor), 'Preference popup remained open')
        context = self.cfg_actor(actor)
        if title != context['group_title'] or index not in (0, 1):
            raise RuntimeError('Preference observation requires the fixture group and a known menu row')
        field = 'pinned' if index == 0 else 'muted'
        expected = expected_action in ('置顶', '静音')
        self.capture(actor, 'preference-applied-' + field + '-' + str(expected).lower())
        emit('preference-observe', actor=actor, conversation=context['group_id'],
             field=field, expected=expected)
        instruction = json.loads(sys.stdin.readline())
        if instruction.get('command') != 'preference-observed':
            raise RuntimeError('Expected coordinator read-only preference observation barrier')

    def proof(self, kind, **context):
        # Ask the coordinator for a fact the screen cannot state: it owns the server observer and
        # the read-only database connection, and it records the same values in its own event log.
        emit('proof-request', kind=kind, **context)
        answer = json.loads(sys.stdin.readline())
        if answer.get('command') != 'proof' or answer.get('kind') != kind:
            raise RuntimeError('Unexpected answer to a native proof request')
        return answer

    def daylight_unread_palette(self, actor):
        self.navigate(actor, '我的资料')
        self.field(actor, '关闭资料')
        mode = self.wait(lambda: next((node for node in self.nodes(actor)
            if node.get_role() == self.Atspi.Role.COMBO_BOX and
            node.get_name() in ('外观模式', '跟随系统', 'Daylight', 'Midnight')), None),
            'Native appearance selector for unread evidence')
        self.click(actor, mode)
        self.key('Home')
        self.key('Down')
        self.key('Return')
        self.wait(lambda: any(node.get_role() == self.Atspi.Role.COMBO_BOX and
            node.get_name() == 'Daylight' for node in self.nodes(actor)), 'Daylight selection receipt')
        self.click(actor, self.field(actor, '关闭资料'))
        self.badge_rgb = theme_highlight_rgb(self.cfg['binary'])
        emit('unread-palette', actor=actor, theme='daylight', highlight_rgb=self.badge_rgb,
             source=str(pathlib.Path(self.cfg['binary']).resolve().parent / 'themes' / 'daylight.json'))

    def unread_badge(self, actor, title, label=None):
        # The unread number is painted by the conversation delegate and never reaches the
        # accessibility tree, so the only honest automated observation is of the real window
        # pixels. A badge is a filled rounded rect in the theme's accent colour while the row's own
        # text is neutral grey, so the accent colour separates them. Whether the number itself reads
        # 1 or 3 stays a human judgement, which is why the full window and the badge crop are kept.
        self.focus(actor)
        time.sleep(.08)
        image = self.ImageGrab.grab(xdisplay=self.display)
        row = next((n for n in self.rows(actor, '会话列表') if n.get_name() == title), None)
        if row is None:
            raise AssertionError('The conversation row disappeared from the native list: ' + title)
        rect = row.get_component_iface().get_extents(self.Atspi.CoordType.SCREEN)
        box = (max(0, rect.x + rect.width - 72), rect.y + 28, max(0, rect.x + rect.width - 4), rect.y + 58)
        crop = image.crop(box)
        if self.badge_rgb is None:
            raise RuntimeError('Unread pixels require an observed Daylight selection and installed palette')
        accent = accent_pixels(image, box, self.badge_rgb)
        if label is not None:
            image.save(self.work / (label + '.png'))
            crop.save(self.work / (label + '-badge.png'))
            emit('unread-badge-capture', actor=actor, conversation_title=title, label=label,
                 path=str(self.work / (label + '.png')), crop=str(self.work / (label + '-badge.png')),
                 region=list(box), highlight_rgb=self.badge_rgb, accent_pixels=accent, badge_visible=accent > BADGE_ACCENT_MIN)
        return accent > BADGE_ACCENT_MIN

    def heartbeat_pilot(self, seed):
        # The link stays paused longer than the client's own heartbeat may go unanswered, so this
        # is the scenario where the client has to notice by itself and come back by itself. Nothing
        # here closes a socket, restarts a process, clicks retry or logs in again.
        actors = self.cfg['actors']
        if len(actors) != 2:
            raise RuntimeError('heartbeat_pilot requires exactly two native peers')
        isolated, peer = sorted(actors, key=lambda actor: int(actor['id']))
        sender_id, direct = isolated['id'], isolated['peer_username']
        relay = self.relays.get(int(sender_id))
        if relay is None:
            raise RuntimeError('Heartbeat pilot has no relay for the isolated client')
        draft = f'qt-heartbeat-draft-{seed} 中文 \U0001f642 '
        # The three texts the chat header may carry while the client itself believes the link is
        # down; an empty header means the client considers the connection healthy again.
        BREAK_STATES = ('正在连接…', '正在恢复会话…', '连接中断')
        # Staying down past the client's own detection keeps the scenario a heartbeat timeout
        # instead of a blip: the client has to sit in its retry loop and come back on its own.
        EXTRA_DOWN_SECONDS = 25
        states, seen = [], set()
        missed = f'native-{seed}-heartbeat-missed'
        timeline = {}
        observed = {}
        operations = 0

        def step(kind, action, conversation=None):
            nonlocal operations
            operations += 1
            begin = time.monotonic()
            try:
                result = action()
            except Exception as error:
                self.capture(sender_id, 'heartbeat-' + kind + '-failure')
                emit('operation', kind='heartbeat-' + kind, actor=sender_id, marker='heartbeat',
                     conversation=conversation, elapsed_ms=(time.monotonic() - begin) * 1000, ok=False,
                     error=str(error)[:240])
                raise
            emit('operation', kind='heartbeat-' + kind, actor=sender_id, marker='heartbeat',
                 conversation=conversation, elapsed_ms=(time.monotonic() - begin) * 1000, ok=True)
            return result

        def window_of(actor):
            return [w for w in self.property(self.root(), '_NET_CLIENT_LIST')
                    if self.property(w, '_NET_WM_PID') == [self.clients[actor].pid]]

        def offline_notice():
            # The chat header carries a connection button only while the client itself believes the
            # link is down: an attempt in flight, a scheduled retry, a session being restored. Its
            # text is the client's own statement, never something the test asserts about it.
            for node in self.nodes(sender_id):
                name = node.get_name()
                if any(name == state or name.startswith(state) for state in BREAK_STATES):
                    return name
            return None

        def editor_text():
            return self.text(self.field(sender_id, '消息输入'))

        def occurrences(marker):
            view = self.field(sender_id, '消息记录')
            return sum(1 for node in self.walk(view)
                       if self.visible(node) and message_name_matches(node.get_name(), marker))

        def absent_down(marker):
            # A link that is really down must not show the message yet; without this check the
            # later catch-up could be satisfied by something that was never actually missed.
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if occurrences(marker):
                    raise AssertionError('The paused relay delivered a message anyway')
                time.sleep(.2)

        def peer_sends(marker):
            self.open_room(peer['id'], peer['peer_username'])
            self.send(peer['id'], marker)

        def baseline():
            self.open_room(sender_id, direct)
            self.send(sender_id, f'native-{seed}-heartbeat-first')
            self.open_room(peer['id'], peer['peer_username'])
            self.expect_message(peer['id'], f'native-{seed}-heartbeat-first', 'The peer never saw the baseline message')
            self.send(peer['id'], f'native-{seed}-heartbeat-second')
            self.open_room(sender_id, direct)
            self.expect_message(sender_id, f'native-{seed}-heartbeat-second', 'The isolated client never saw the peer reply')

        def note_state(name):
            # Recording each state the client shows, in order, is what separates "the network was
            # paused" from "the client noticed, retried, re-authenticated and resumed".
            if name and (not states or states[-1][0] != name):
                states.append([name, round(time.monotonic() - timeline['paused_at'], 2)])
                if name not in seen:
                    seen.add(name)
                    observed.setdefault('ui_states', []).append(name)
                    self.screenshot(self.work / f'heartbeat-ui-state-{len(seen)}-{sender_id}.png')

        def visible_words():
            # What the client's own window actually says at the moment the scenario gives up. The
            # walk is the same one every other check uses; it runs once, only to explain a failure.
            found = []
            for node in self.nodes(sender_id):
                name = node.get_name()
                if name and len(name) < 70:
                    found.append(name)
                if len(found) >= 40:
                    break
            return found

        def deactivation():
            # Two independent consequences of the same timeout: the server stops counting the old
            # session, and the client's own heartbeat gives up and draws its notice in the chat
            # header. Both are observed, neither is assumed from elapsed time. The link then stays
            # down well past that point, so recovery cannot be the first attempt after a blip.
            deadline = time.monotonic() + 150
            released = noticed = probed = None
            while True:
                if time.monotonic() > deadline:
                    observed['window'] = visible_words()
                    raise AssertionError('No heartbeat deactivation became observable within the budget: '
                                         + json.dumps(observed, ensure_ascii=False)[:1500])
                if released is None and (probed is None or time.monotonic() - probed >= 3):
                    probed = time.monotonic()
                    if self.proof('online', stage='heartbeat-release')['online'] == 1:
                        released = time.monotonic()
                        timeline['server_released_seconds'] = round(released - timeline['paused_at'], 2)
                drawn = offline_notice()
                if noticed is None and drawn:
                    noticed = time.monotonic()
                    self.capture(sender_id, 'heartbeat-ui-offline')
                    note_state(drawn)
                    observed['banner'] = drawn
                    timeline['ui_offline_seconds'] = round(noticed - timeline['paused_at'], 2)
                    emit('heartbeat-ui-offline', actor=sender_id, conversation=isolated['direct_id'],
                         wording=drawn)
                if released is not None and noticed is not None and time.monotonic() - noticed >= EXTRA_DOWN_SECONDS:
                    if not offline_notice():
                        raise AssertionError('The connection banner went away before the link was released')
                    if occurrences(missed):
                        raise AssertionError('The paused relay delivered a message during the hold')
                    timeline['held_down_seconds'] = round(time.monotonic() - noticed, 2)
                    return
                time.sleep(1)

        def unchanged():
            process = self.clients[sender_id]
            if process.pid != pid or process.poll() is not None:
                raise AssertionError('The isolated Qt process changed or exited during the blackhole')
            if window_of(sender_id) != window:
                raise AssertionError('The isolated Qt window was recreated during the blackhole')
            if editor_text() != draft:
                raise AssertionError('The unsent draft did not survive the silent link')

        def recovery():
            # A message arriving on its own proves the client reconnected, re-authenticated and
            # caught up without any test-side action, and the header must then stop drawing the
            # banner it drew while the link was down.
            deadline = time.monotonic() + 120
            while not occurrences(missed):
                if time.monotonic() > deadline:
                    raise AssertionError('The isolated client never recovered its link by itself')
                time.sleep(.5)
            timeline['caught_up_seconds'] = round(time.monotonic() - timeline['resumed_at'], 2)
            if offline_notice():
                raise AssertionError('The client caught up but still shows a connection banner')
            if occurrences(missed) != 1:
                raise AssertionError('The message sent while the link was down arrived more than once')

        def both_directions():
            for marker, sender in ((f'native-{seed}-heartbeat-fourth', isolated), (f'native-{seed}-heartbeat-fifth', peer)):
                if sender['id'] == sender_id:
                    self.open_room(sender_id, direct)
                    self.send(sender_id, marker)
                    self.open_room(peer['id'], peer['peer_username'])
                    self.expect_message(peer['id'], marker, 'The peer never saw the reconnected client again')
                else:
                    self.open_room(peer['id'], peer['peer_username'])
                    self.send(peer['id'], marker)
                    self.open_room(sender_id, direct)
                    self.expect_message(sender_id, marker, 'The reconnected client never saw the peer again')

        pid = self.clients[sender_id].pid
        window = window_of(sender_id)
        step('baseline-exchange', baseline, isolated['direct_id'])
        baseline_online = self.proof('online', stage='heartbeat-baseline')
        if sorted(baseline_online['users'] or []) != sorted(int(a['id']) for a in actors):
            raise AssertionError('Heartbeat baseline does not have both native clients online')
        step('draft', lambda: (self.paste(sender_id, self.field(sender_id, '消息输入'), draft), self.key('End')),
             isolated['direct_id'])
        if editor_text() != draft:
            raise AssertionError('The native composer did not accept the heartbeat draft')
        step('pause', relay.pause, isolated['direct_id'])
        timeline['paused_at'] = time.monotonic()
        step('peer-message-while-down', lambda: peer_sends(missed), isolated['direct_id'])
        step('message-still-absent', lambda: absent_down(missed), isolated['direct_id'])
        step('heartbeat-deactivation', deactivation, isolated['direct_id'])
        step('same-process-and-draft', unchanged, isolated['direct_id'])
        step('resume', relay.resume, isolated['direct_id'])
        timeline['resumed_at'] = time.monotonic()
        timeline['paused_seconds'] = round(timeline['resumed_at'] - timeline['paused_at'], 2)
        step('automatic-recovery', recovery, isolated['direct_id'])
        recovered_online = self.proof('online', stage='heartbeat-recovered')
        if sorted(recovered_online['users'] or []) != sorted(int(a['id']) for a in actors):
            raise AssertionError('The reconnected server sessions differ from the intended native clients')
        timeline['recovered_online'] = recovered_online['online']
        step('draft-after-recovery', unchanged, isolated['direct_id'])
        step('both-directions-after-recovery', both_directions, isolated['direct_id'])
        self.capture(sender_id, 'heartbeat-pilot-complete')
        emit('heartbeat-pilot-timeline', actor=sender_id, pid=pid, window=window, timeline=timeline,
             ui_states=states, relayed_bytes=relay.relayed, observations=observed)
        return operations

    def blackhole_pilot(self, seed):
        # A silent TCP blackhole is the one link failure the client cannot see in its own socket.
        # What matters is what the native window still owes the user while the link is down: an
        # unsent draft, the messages the server accepted meanwhile, and the unread counts.
        actors = self.cfg['actors']
        if len(actors) != 2:
            raise RuntimeError('blackhole_pilot requires exactly two native peers')
        isolated, peer = sorted(actors, key=lambda actor: int(actor['id']))
        sender_id, group, direct = isolated['id'], isolated['group_title'], isolated['peer_username']
        relay = self.relays.get(int(isolated['id']))
        if relay is None:
            raise RuntimeError('Blackhole pilot has no relay for the isolated client')
        observed = {}
        operations = 0

        def step(kind, action, conversation):
            nonlocal operations
            operations += 1
            begin = time.monotonic()
            try:
                result = action()
            except Exception as error:
                self.capture(sender_id, 'blackhole-' + kind + '-failure')
                emit('operation', kind='blackhole-' + kind, actor=sender_id, marker='blackhole',
                     conversation=conversation, elapsed_ms=(time.monotonic() - begin) * 1000, ok=False,
                     error=str(error)[:240])
                raise
            emit('operation', kind='blackhole-' + kind, actor=sender_id, marker='blackhole',
                 conversation=conversation, elapsed_ms=(time.monotonic() - begin) * 1000, ok=True)
            return result

        def draft(value=None):
            editor = self.field(sender_id, '消息输入')
            if value is None:
                return self.text(editor)
            self.paste(sender_id, editor, value)
            self.key('End')

        def accepted(sender, title, marker):
            # The sender's own native history is the proof the server accepted the message; the
            # client behind the blackhole is never asked to confirm what it cannot have received.
            # A direct room is titled with the other person's name, so each side opens its own.
            self.open_room(sender['id'], title)
            self.send(sender['id'], marker)

        def absent(marker):
            # A link that is really down must not show the message yet; without this check the
            # later catch-up assertion could be satisfied by a message that never left.
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                if self.message(sender_id, marker) is not None:
                    raise AssertionError('A blackholed client received a message through the paused relay')
                time.sleep(.2)

        def read_receipt(marker):
            # The read receipt is part of the sender's own message row, so the peer's window
            # states authoritatively that this client has read the message it just caught up on.
            self.open_room(peer['id'], group)
            row = self.expect_message(peer['id'], marker, 'The peer lost its own group message')
            self.wait(lambda: re.search(r'已读 [1-9]', row.get_name()) is not None,
                      'The peer never saw the reconnected client read the message', timeout=20)
            observed['read_receipt_name'] = row.get_name()

        def send_while_blackholed():
            marker = f'native-{seed}-blackhole-pending'
            draft(marker)
            self.key('Return')
            cleared = True
            try:
                self.wait(lambda: draft() == '', 'A blackholed client cleared its draft', timeout=5)
            except Exception:
                cleared = False
            observed['blackholed_send_cleared_draft'] = cleared
            if cleared:
                raise AssertionError('A blackholed client sent a message without a server acknowledgement')
            return marker

        def check_draft():
            current = draft()
            observed['draft_preserved'] = current == pending
            if current != pending:
                raise AssertionError('The native draft did not survive the blackhole: ' + repr(current))

        def send_pending():
            # Recovery is proven by the preserved draft actually reaching the server, not by a
            # status label.  A client that already re-sent on its own has an empty editor.
            if draft() == pending:
                self.key('Return')
            self.wait(lambda: draft() == '', 'The preserved draft did not send after recovery', timeout=20)
            self.expect_message(sender_id, pending, 'The draft sent after recovery never reached the native history')

        def catch_up(group_marker, direct_marker):
            # The missing rows arriving is the only proof of recovery; the unread badge cannot
            # speak for it because Qt paints it inside a tool button, outside the a11y tree.
            self.open_room(sender_id, group)
            self.expect_message(sender_id, group_marker, 'Reconnected native history lost the group message')
            self.open_room(sender_id, direct)
            self.expect_message(sender_id, direct_marker, 'Reconnected native history lost the direct message')

        def send_after_recovery():
            # A usable link is proven in both directions: the peer must see what this client
            # sends after it reconnected, not only the other way round.
            marker = f'native-{seed}-blackhole-after-recovery'
            self.open_room(sender_id, group)
            self.send(sender_id, marker)
            self.open_room(peer['id'], group)
            self.expect_message(peer['id'], marker, 'The peer never saw the reconnected client')
            return marker

        def relogin():
            process = self.clients.pop(sender_id)
            process.terminate()
            self.wait(lambda: process.poll() is not None, 'Blackhole pilot Qt client did not stop')
            # The old process owns the cached AT-SPI application and window mapping; a new one
            # has to be discovered again by its own pid.
            self.apps.pop(sender_id, None)
            self.rooms.pop(sender_id, None)
            self.login(isolated, self.cfg['password'])
            self.open_room(sender_id, group)
            self.expect_message(sender_id, group_marker, 'Re-logged-in native history lost the group message')
            self.capture(sender_id, 'blackhole-relogin')

        group_marker = f'native-{seed}-blackhole-group'
        direct_marker = f'native-{seed}-blackhole-direct'
        pending = f'native-{seed}-blackhole-pending'
        step('open-group', lambda: self.open_room(sender_id, group), isolated['group_id'])
        step('cut', relay.pause, isolated['group_id'])
        step('open-direct', lambda: self.open_room(sender_id, direct), isolated['direct_id'])
        step('draft', lambda: draft(pending), isolated['direct_id'])
        step('group-message-while-blackholed', lambda: accepted(peer, group, group_marker), isolated['group_id'])
        step('direct-message-while-blackholed',
             lambda: accepted(peer, peer['peer_username'], direct_marker), isolated['direct_id'])
        step('direct-message-still-absent', lambda: absent(direct_marker), isolated['direct_id'])
        step('draft-survives-blackhole', check_draft, isolated['direct_id'])
        step('send-while-blackholed', send_while_blackholed, isolated['direct_id'])
        step('restore', relay.resume, isolated['group_id'])
        step('pending-draft-sends-after-recovery', send_pending, isolated['direct_id'])
        step('catch-up', lambda: catch_up(group_marker, direct_marker), isolated['group_id'])
        step('read-receipt-after-recovery', lambda: read_receipt(group_marker), isolated['group_id'])
        step('send-after-recovery', send_after_recovery, isolated['group_id'])
        step('relogin-after-recovery', relogin, isolated['group_id'])
        self.capture(sender_id, 'blackhole-pilot-complete')
        emit('blackhole-pilot-observed', actor=sender_id, group=isolated['group_id'],
             relayed_bytes=relay.relayed, relay_paused=relay.paused, observations=observed)
        return operations

    def read_pilot(self, seed):
        # A conversation that is not open must not be reported as read, and a receipt must not
        # move twice.  Only the sender's own message row states that authoritatively, because Qt
        # paints unread counts inside the conversation row without any accessible object.
        actors = self.cfg['actors']
        if len(actors) != 2:
            raise RuntimeError('read_pilot requires exactly two native peers')
        watcher, writer = sorted(actors, key=lambda actor: int(actor['id']))
        self.daylight_unread_palette(watcher['id'])
        group = watcher['group_title']
        relay = self.relays.get(int(watcher['id']))
        if relay is None:
            raise RuntimeError('Read pilot has no relay for the watching client')
        observed = {}
        operations = 0

        def step(kind, action, conversation):
            nonlocal operations
            operations += 1
            begin = time.monotonic()
            try:
                result = action()
            except Exception as error:
                self.capture(watcher['id'], 'read-' + kind + '-failure')
                emit('operation', kind='read-' + kind, actor=watcher['id'], marker='read',
                     conversation=conversation, elapsed_ms=(time.monotonic() - begin) * 1000, ok=False,
                     error=str(error)[:240])
                raise
            emit('operation', kind='read-' + kind, actor=watcher['id'], marker='read',
                 conversation=conversation, elapsed_ms=(time.monotonic() - begin) * 1000, ok=True)
            return result

        def receipt(marker):
            row = self.expect_message(writer['id'], marker, 'The sender lost its own message')
            found = re.search(r'已读 (\d+) 人', row.get_name())
            if found is None:
                raise AssertionError('The native message row carries no read count: ' + row.get_name())
            return int(found.group(1))

        def occurrences(actor, marker):
            view = self.field(actor, '消息记录')
            return sum(1 for node in self.walk(view)
                       if self.visible(node) and message_name_matches(node.get_name(), marker))

        def row_top(actor, marker):
            view = self.field(actor, '消息记录')
            rows = [node for node in self.walk(view)
                    if self.visible(node) and message_name_matches(node.get_name(), marker)]
            if not rows:
                raise AssertionError('Message absent from the native history: ' + marker)
            return min(row.get_component_iface().get_extents(self.Atspi.CoordType.WINDOW).y for row in rows)

        first = f'native-{seed}-read-first'
        second = f'native-{seed}-read-second'
        third = f'native-{seed}-read-third'

        def send_both():
            self.open_room(writer['id'], group)
            self.send(writer['id'], first)
            self.send(writer['id'], second)

        def unread_state():
            # The count cannot move while the other client keeps the conversation closed.
            self.open_room(writer['id'], group)
            before = {marker: receipt(marker) for marker in (first, second)}
            if any(before[m] != receipt(m) for m in (first, second)):
                raise AssertionError('A closed native conversation was reported as read')
            return before

        def open_group():
            self.open_room(watcher['id'], group)
            for marker in (first, second):
                self.expect_message(watcher['id'], marker, 'The opened conversation lost a message')

        def receipt_advances(before):
            self.open_room(writer['id'], group)
            for marker in (first, second):
                self.wait(lambda marker=marker: receipt(marker) == before[marker] + 1,
                          'The native read receipt did not advance by exactly one reader', timeout=20)

        def switch_back_and_forth():
            # Repeated switching must not duplicate, reorder or drop a row, and a draft belongs
            # to its own conversation only.
            draft = f'native-{seed}-read-draft'
            self.open_room(watcher['id'], watcher['peer_username'])
            editor = self.field(watcher['id'], '消息输入')
            self.paste(watcher['id'], editor, draft)
            for _ in range(2):
                self.open_room(watcher['id'], group)
                self.open_room(watcher['id'], watcher['peer_username'])
            if self.text(editor) != draft:
                raise AssertionError('A native draft did not stay with its own conversation')
            self.open_room(watcher['id'], group)
            if self.text(editor):
                raise AssertionError('A native draft leaked into another conversation')
            for marker in (first, second):
                if occurrences(watcher['id'], marker) != 1:
                    raise AssertionError('Switching conversations duplicated a native message row: ' + marker)
            if row_top(watcher['id'], first) >= row_top(watcher['id'], second):
                raise AssertionError('Switching conversations reordered the native history')
            self.capture(watcher['id'], 'read-history-after-switching')

        def blip():
            # A silent link that returns must leave the same history and the same receipt behind;
            # the conversation switch below is what keeps the link down long enough to matter.
            relay.pause()
            self.open_room(watcher['id'], watcher['peer_username'])
            self.open_room(watcher['id'], group)
            relay.resume()
            # A reconnect may rebuild the model, so the rows are waited for; what must never
            # change is that each of them exists exactly once.
            for marker in (first, second):
                self.expect_message(watcher['id'], marker, 'A reconnect lost a native message')
            for marker in (first, second):
                if occurrences(watcher['id'], marker) != 1:
                    raise AssertionError('A reconnect duplicated a native message row: ' + marker)

        def receipt_after_reconnect(before):
            self.open_room(writer['id'], group)
            for marker in (first, second):
                if receipt(marker) != before[marker] + 1:
                    raise AssertionError('A reconnect changed the native read receipt')
            self.send(writer['id'], third)
            self.expect_message(watcher['id'], third, 'The conversation open across a reconnect lost a message')
            # A client only marks read what it really shows: viewing_latest() requires the active
            # window scrolled to the last row, so the reading side has to come to the foreground.
            self.open_room(watcher['id'], group)
            self.wait(lambda: receipt(third) == before[first] + 1,
                      'A message shown in the active conversation was not read', timeout=20)

        step('watcher-keeps-group-closed', lambda: self.open_room(watcher['id'], watcher['peer_username']),
             watcher['direct_id'])
        step('writer-sends-two', send_both, watcher['group_id'])
        before = step('unread-while-closed', unread_state, watcher['group_id'])
        step('watcher-opens-group', open_group, watcher['group_id'])
        step('receipt-advances-once', lambda: receipt_advances(before), watcher['group_id'])
        step('switching-preserves-state', switch_back_and_forth, watcher['group_id'])
        step('receipt-stable-after-switching', lambda: receipt_advances(before), watcher['group_id'])
        step('link-blip', blip, watcher['group_id'])
        step('state-after-reconnect', lambda: receipt_after_reconnect(before), watcher['group_id'])

        # The unread number itself. The database states the authoritative read position and the
        # screenshot states what the native window shows; neither is allowed to stand in for the
        # other, and the digit itself is left to the saved pixels.
        direct = watcher['peer_username']
        unread_markers = [f'native-{seed}-unread-{index}' for index in range(1, 5)]

        def writer_sends(marker):
            self.open_room(writer['id'], writer['peer_username'])
            self.send(writer['id'], marker)

        def badge_state(label, expected, marker=None):
            proof = self.proof('read-position', conversation=watcher['direct_id'], member=watcher['id'])
            visible = self.unread_badge(watcher['id'], direct, label)
            observed[label] = {'expected': expected, 'database_unread': proof['unread'],
                               'read_position': proof['read_position'], 'badge_visible': visible}
            if proof['unread'] != expected:
                raise AssertionError(f'The database read position for {label} is not {expected}')
            if visible != (expected > 0):
                raise AssertionError(f'The native unread badge for {label} does not match {expected} unread')
            emit('unread-evidence', actor=watcher['id'], conversation=watcher['direct_id'], label=label,
                 marker=marker, expected_unread=expected, database_unread=proof['unread'],
                 read_position=proof['read_position'], badge_visible=visible,
                 screenshot=str(self.work / (label + '.png')))

        def other_conversation_untouched():
            self.proof('read-position', conversation=watcher['group_id'], member=watcher['id'])
            if self.unread_badge(watcher['id'], group, 'unread-3-other-room'):
                raise AssertionError('The group row shows a badge the direct test did not create')
            self.open_room(watcher['id'], group)
            proof = self.proof('read-position', conversation=watcher['direct_id'], member=watcher['id'])
            if proof['unread'] != 3:
                raise AssertionError('Opening another conversation cleared the direct unread')

        def read_marked(marker):
            # A direct conversation reports "已读" without a headcount, so the row is asked whether
            # it states a read at all, and the exact text is kept as evidence.
            row = self.expect_message(writer['id'], marker, 'The sender lost its own direct message')
            observed['direct_read_row'] = row.get_name()
            return '已读' in row.get_name()

        def clear_by_opening():
            self.open_room(watcher['id'], direct)
            for marker in unread_markers[:3]:
                self.expect_message(watcher['id'], marker, 'Opening the conversation lost an unread message')
            self.wait(lambda: not self.unread_badge(watcher['id'], direct),
                      'The native unread badge never cleared after the conversation was opened')
            badge_state('unread-0-after-read', 0, unread_markers[2])
            self.open_room(writer['id'], writer['peer_username'])
            self.wait(lambda: read_marked(unread_markers[2]),
                      'The sender never saw the reader read the unread messages', timeout=20)

        def unread_again():
            self.open_room(watcher['id'], group)
            writer_sends(unread_markers[3])
            badge_state('unread-1-again', 1, unread_markers[3])
            self.open_room(watcher['id'], direct)
            # Opening a room acknowledges its selection before asynchronous history and its
            # read marker arrive. Observe the actual last row, then require the authoritative
            # read position to reach zero within the existing receipt deadline.
            self.expect_message(watcher['id'], unread_markers[3],
                                'The reopened conversation lost its last unread message')
            self.wait(lambda: self.proof('read-position', conversation=watcher['direct_id'],
                                        member=watcher['id'])['unread'] == 0,
                      'The final native read position never cleared after its message was shown', timeout=20)
            self.wait(lambda: not self.unread_badge(watcher['id'], direct),
                      'The final native unread badge never cleared after its message was shown', timeout=20)
            badge_state('unread-0-final', 0, unread_markers[3])

        step('badge-baseline', lambda: (self.open_room(watcher['id'], direct),
                                        self.open_room(watcher['id'], group)), watcher['direct_id'])
        step('badge-zero-before', lambda: badge_state('unread-0-before', 0), watcher['direct_id'])
        step('badge-one-message', lambda: writer_sends(unread_markers[0]), watcher['direct_id'])
        step('badge-one-checked', lambda: badge_state('unread-1', 1, unread_markers[0]), watcher['direct_id'])
        step('badge-more-messages', lambda: (writer_sends(unread_markers[1]),
                                             writer_sends(unread_markers[2])), watcher['direct_id'])
        step('badge-three-checked', lambda: badge_state('unread-3', 3, unread_markers[2]), watcher['direct_id'])
        step('badge-other-conversation', other_conversation_untouched, watcher['direct_id'])
        step('badge-cleared-on-open', clear_by_opening, watcher['direct_id'])
        step('badge-again-and-cleared', unread_again, watcher['direct_id'])
        # What the tool can decide and what it cannot are stated separately: presence of a badge is
        # observed in real pixels, the digit inside it is not machine-readable at all.
        emit('unread-badge-verdict', digits='visual-review-required',
             reason='The unread number is painted by the delegate and never reaches the accessibility '
                   'tree; only badge presence is asserted, the digit needs a visual review',
             evidence=[str(self.work / (label + '.png')) for label in
                       ('unread-0-before', 'unread-1', 'unread-3', 'unread-0-after-read',
                        'unread-1-again', 'unread-0-final')])
        self.capture(watcher['id'], 'read-pilot-complete')
        emit('read-pilot-observed', actor=watcher['id'], group=watcher['group_id'],
             relayed_bytes=relay.relayed, observations=observed)
        return operations

    def daily_pilot(self, password, seed):
        actors = self.cfg['actors']
        if len(actors) != 2 or actors[0]['peer_id'] != actors[1]['id']:
            raise RuntimeError('daily_pilot requires exactly two accepted native peers')
        owner, peer = actors
        identity, receiver = owner['id'], peer['id']
        completed = 0
        def case(kind, action):
            nonlocal completed
            begin = time.monotonic()
            try:
                evidence = action() or {}
                emit('operation', actor=identity, kind='daily-' + kind, ok=True,
                     elapsed_ms=(time.monotonic()-begin)*1000, evidence=evidence)
                completed += 1
            except Exception as error:
                emit('operation', actor=identity, kind='daily-' + kind, ok=False,
                     elapsed_ms=(time.monotonic()-begin)*1000,
                     error=type(error).__name__ + ': ' + str(error))
                self.capture(identity, 'daily-failure-' + kind)
                raise

        def preferences():
            title = owner['group_title']
            # Initial fixture has neither preference. The inverse menu label
            # must appear after the authoritative async response before restore.
            for index, before, after in [(0, '置顶', '取消置顶'), (1, '静音', '取消静音')]:
                self.preference_toggle(identity, title, index, before)
                self.preference_toggle(identity, title, index, after)
            self.open_room(identity, title)
            return {'conversation': owner['group_id'], 'expected_final_pinned': False,
                    'expected_final_muted': False, 'native_menu_inputs_completed': True,
                    'intermediate_state_asserted': True,
                    'proof': 'native forward/inverse popup screenshots; coordinator read-only DB acknowledged every toggle and restore'}
        case('personal-preferences', preferences)

        def clipboard_copy():
            marker = f'native-{seed}-{identity}-daily-copy'
            body = marker + ' 中文 🙂\n复制第二行'
            self.open_room(identity, owner['group_title'])
            self.send(identity, marker, body)
            self.menu(identity, marker, '复制')
            copied = subprocess.run(['xclip', '-o', '-selection', 'clipboard'],
                capture_output=True, check=True, timeout=5).stdout.decode('utf-8')
            if copied != body:
                raise RuntimeError('Native message clipboard differs from the actual body')
            return {'clipboard': 'X11 CLIPBOARD', 'exact_body': True}
        case('clipboard-copy', clipboard_copy)

        def typing():
            for actor, target in [(owner, peer), (peer, owner)]:
                self.open_room(actor['id'], target['username'])
            editor = self.field(identity, '消息输入')
            self.paste(identity, editor, f'native-{seed}-{identity}-typing-draft 中文')
            hint = owner['username'] + '正在输入…'
            self.field(receiver, hint)
            self.capture(receiver, 'daily-typing')
            self.click(identity, self.field(identity, '消息输入'))
            self.key('a', ('Control_L',))
            self.key('BackSpace')
            self.wait(lambda: self.text(self.field(identity, '消息输入')) == '',
                      'Native typing draft did not clear')
            self.wait(lambda: not self.find(receiver, hint), 'Peer typing did not clear with the empty draft')
            return {'typing_seen': True, 'empty_draft_cleared_hint': True,
                    'unsent_draft': True, 'esc_not_a_qt_typing_action': True}
        case('typing-draft-clear', typing)

        def avatars():
            import hashlib
            from PIL import Image
            files = self.work / 'avatar-fixture'
            files.mkdir(exist_ok=True)
            png, jpeg = files / 'daily-avatar.png', files / 'daily-avatar.jpg'
            Image.new('RGB', (32, 32), (61, 132, 117)).save(png)
            Image.new('RGB', (32, 32), (121, 161, 182)).save(jpeg)
            self.navigate(identity, '我的资料')
            hashes = []
            for path in (png, jpeg):
                self.click(identity, self.field(identity, '更换头像'))
                self.file_path(identity, path)
                self.wait(lambda: self.field(identity, '更换头像').get_state_set().contains(
                    self.Atspi.StateType.SENSITIVE) and self.field(identity, '移除头像').get_state_set().contains(
                    self.Atspi.StateType.SENSITIVE) and not any('正在更新头像' in n.get_name()
                    for n in self.nodes(identity)), 'Native avatar upload did not finish')
                digest = hashlib.sha256(path.read_bytes()).hexdigest()
                hashes.append({'format': path.suffix, 'sha256': digest})
                emit('avatar-ui-uploaded', actor=identity, format=path.suffix,
                     sha256=digest, path=str(path), proof='native avatar action completed; coordinator checks DB')
                self.capture(identity, 'daily-avatar-' + path.suffix[1:])
            self.click(identity, self.field(identity, '移除头像'))
            self.wait(lambda: self.field(identity, '更换头像').get_state_set().contains(
                self.Atspi.StateType.SENSITIVE) and not self.field(identity, '移除头像').get_state_set().contains(
                self.Atspi.StateType.SENSITIVE), 'Native avatar clear did not finish')
            self.capture(identity, 'daily-avatar-cleared')
            self.escape(identity)
            emit('avatar-ui-cleared', actor=identity)
            return {'uploads': hashes, 'cleared': True, 'bytes_need_coordinator_check': True}
        case('avatar-management', avatars)

        def logout():
            process = self.clients[identity]
            pid = process.pid
            self.navigate(identity, '我的资料')
            self.click(identity, self.field(identity, '退出登录'))
            self.click(identity, self.confirmation_button(identity, '退出登录', cancel=True))
            self.field(identity, '关闭资料')
            self.click(identity, self.field(identity, '退出登录'))
            self.click(identity, self.confirmation_button(identity, '退出登录'))
            self.field(identity, '注册账号')
            wrong_password = ('0' if password[0] != '0' else '1') + password[1:]
            _private_values.add(wrong_password)
            def rejected_login(actor, attempted_password, error):
                self.authenticate_existing(actor, attempted_password, expected_error=error)
                if self.clients[identity].pid != pid or process.poll() is not None:
                    raise RuntimeError('Rejected login replaced or exited the native process')
                self.capture(identity, 'daily-login-wrong-password' if attempted_password != password else
                             'daily-login-duplicate-rejected')
                return {'same_native_process': True, 'actual_error': error,
                        'login_page_usable': True, 'not_authenticated': True}
            case('account-wrong-password-rejected', lambda: rejected_login(
                owner, wrong_password, '用户名或密码错误'))
            duplicate_actor = dict(owner, username=peer['username'])
            case('account-duplicate-login-rejected', lambda: rejected_login(
                duplicate_actor, password, '该账号已在其他地方登录'))
            self.authenticate_existing(owner, password, reauthentication=True)
            if self.clients[identity].pid != pid or process.poll() is not None:
                raise RuntimeError('Logout replaced or exited the native process')
            self.open_room(identity, owner['group_title'])
            self.open_room(identity, peer['username'])
            marker = f'native-{seed}-{identity}-daily-reauth'
            self.send(identity, marker)
            self.open_room(receiver, owner['username'])
            self.expect_message(receiver, marker, 'Relogin direct not delivered', sender=identity)
            emit('screen-received', actor=receiver, marker=marker, from_actor=identity)
            return {'pid': pid, 'same_native_process': True, 'cancel_confirm': True}
        case('account-logout-login', logout)

        group_title = f'Native daily {seed}'
        renamed = group_title + ' 改名'
        announcement = f'Native notice {seed}\n中文公告 🙂'

        def create():
            self.navigate(identity, '新建群组')
            self.field(identity, '创建群聊')
            self.click(identity, self.wait_row(identity, '选择群成员', peer['username']))
            self.key('space')
            self.field(identity, '已选 1 位好友 · 点击姓名取消')
            self.field(identity, peer['username'] + ' ×')
            self.click(identity, self.field(identity, '下一步'))
            self.wait_row(identity, '已选群成员', peer['username'])
            self.paste(identity, self.editable(identity, '群名称', '创建群聊'), group_title)
            self.click(identity, self.field(identity, '创建'))
            self.open_room(identity, group_title)
            self.open_room(receiver, group_title)
            self.click(identity, self.field(identity, '聊天信息'))
            self.field(identity, '2 位成员')
            self.capture(identity, 'daily-created-group')
            self.escape(identity)
            emit('daily-group-picker-completed', actor=identity,
                 selected_peers=1, member_count=2)
            return {'title': group_title, 'member_count': 2, 'two_step_picker': True}
        case('create-group', create)

        def metadata():
            self.open_room(identity, group_title)
            self.click(identity, self.field(identity, '聊天信息'))
            self.click(identity, self.reachable(identity, '群管理'))
            self.paste(identity, self.editable(identity, '群名称'), renamed)
            self.click(identity, self.field(identity, '保存群名'))
            self.wait(lambda: self.text(self.editable(identity, '群名称')) == renamed and
                      not self.field(identity, '保存群名').get_state_set().contains(self.Atspi.StateType.SENSITIVE),
                      'Rename response did not finish')
            self.keyboard_focus(identity, '群公告')
            self.paste(identity, self.editable(identity, '群公告'), announcement)
            # QPlainTextEdit keeps Tab for indentation, so the focus chain cannot pass the
            # announcement editor; the buttons below it are reached with the native pointer.
            self.click(identity, self.reachable(identity, '保存公告'))
            self.escape(identity)
            self.open_room(identity, renamed)
            self.click(identity, self.field(identity, '聊天信息'))
            self.field(identity, announcement)
            self.capture(identity, 'daily-announcement')
            self.click(identity, self.reachable(identity, '群管理'))
            self.capture(identity, 'daily-management-before-clear')
            self.click(identity, self.reachable(identity, '清空公告'))
            self.escape(identity)
            self.open_room(identity, renamed)
            self.click(identity, self.field(identity, '聊天信息'))
            self.wait(lambda: not any(n.get_name() == announcement for n in self.nodes(identity)),
                      'Cleared announcement retained in group profile')
            self.capture(identity, 'daily-announcement-cleared')
            self.escape(identity)
            emit('daily-group-created', actor=identity, peer_id=receiver,
                 conversation_title=renamed, expected_owner=identity,
                 expected_members=[identity, receiver], announcement_cleared=True)
            return {'title': renamed, 'announcement_set_clear': True}
        case('rename-announcement', metadata)

        def roles():
            def members(actor):
                self.open_room(actor, renamed)
                self.click(actor, self.field(actor, '聊天信息'))
                self.click(actor, self.reachable(actor, '查看全部成员'))
                self.field(actor, '群成员')

            def member_action(actor, name, action, actions):
                if action not in actions:
                    raise RuntimeError('Requested member action unavailable')
                row = self.wait_row(actor, '群成员', name)
                self.click(actor, row, 3)
                # Qt does not reliably export QMenu rows through AT-SPI, so a row is addressed
                # by its position inside the owned popup window, exactly as menu() does; the
                # member list after the click is what the scenario asserts on.
                rectangle = self.wait(lambda: self.popup(actor), 'Native member actions popup unavailable')
                padding = 7
                px = rectangle['x'] + rectangle['width']//2
                py = round(rectangle['y'] + padding + (actions.index(action)+.5)*
                           (rectangle['height']-2*padding)/len(actions))
                emit('menu-route', actor=actor, action=action, context_actions=actions,
                     rectangle=rectangle, target=[px, py])
                self.xt.XTestFakeMotionEvent(self.native, -1, px, py, 0)
                self.xt.XTestFakeButtonEvent(self.native, 1, True, 0)
                self.xt.XTestFakeButtonEvent(self.native, 1, False, 0)
                self.lib.XFlush(self.native)
                if action == '转让群主':
                    self.click(actor, self.confirmation_button(actor, action))

            member_actions = ['个人资料', '设为管理员', '移除成员']
            admin_actions = ['个人资料', '取消管理员', '转让群主', '移除成员']
            members(identity)
            member_action(identity, peer['username'] + ' · 成员', '设为管理员', member_actions)
            self.wait_row(identity, '群成员', peer['username'] + ' · 管理员')
            member_action(identity, peer['username'] + ' · 管理员', '转让群主', admin_actions)
            self.wait_row(identity, '群成员', peer['username'] + ' · 群主')
            self.wait_row(identity, '群成员', owner['username'] + ' · 管理员 · 你')
            self.capture(identity, 'daily-owner-transferred')
            self.escape(identity)
            members(receiver)
            self.wait_row(receiver, '群成员', peer['username'] + ' · 群主 · 你')
            member_action(receiver, owner['username'] + ' · 管理员', '转让群主', admin_actions)
            self.wait_row(receiver, '群成员', owner['username'] + ' · 群主')
            self.wait_row(receiver, '群成员', peer['username'] + ' · 管理员 · 你')
            self.escape(receiver)
            members(identity)
            member_action(identity, peer['username'] + ' · 管理员', '取消管理员', admin_actions)
            self.wait_row(identity, '群成员', peer['username'] + ' · 成员')
            self.wait_row(identity, '群成员', owner['username'] + ' · 群主 · 你')
            self.capture(identity, 'daily-owner-restored')
            self.escape(identity)
            emit('daily-group-roles-restored', actor=identity, peer_id=receiver,
                 conversation_title=renamed, expected_owner=identity, expected_admins=0)
            return {'promote': True, 'transfer_both_directions': True, 'demote': True,
                    'final_owner': identity, 'final_peer_role': 'member'}
        case('roles-owner-transfer-restore', roles)

        invite_code = None
        def invite():
            nonlocal invite_code
            self.open_room(identity, renamed)
            self.click(identity, self.field(identity, '聊天信息'))
            self.click(identity, self.reachable(identity, '群管理'))
            self.redact_invite_ui = True
            self.click(identity, self.reachable(identity, '生成邀请码'))
            invite_code = self.wait(lambda: self.text(self.field(identity, '群邀请码')) or None,
                                    'Native invitation code not generated')
            _private_values.add(invite_code)
            _private_values.add(invite_code.replace('-', ''))
            # Invitation secret remains in memory and is not included in logs.
            self.escape(identity)
            return {'title': renamed, 'code_generated': True}
        case('invite-code', invite)

        def leave_join():
            self.open_room(receiver, renamed)
            self.click(receiver, self.field(receiver, '聊天信息'))
            self.click(receiver, self.reachable(receiver, '退出群聊'))
            self.click(receiver, self.confirmation_button(receiver, '退出群聊'))
            self.wait(lambda: not self.find(receiver, '退出群聊'), 'Left group profile remained open')
            self.navigate(receiver, '聊天')
            self.wait(lambda: not any(n.get_name() == renamed for n in self.rows(receiver, '会话列表')),
                      'Left group remained in Chats')
            self.open_room(identity, renamed)
            self.click(identity, self.field(identity, '聊天信息'))
            self.field(identity, '1 位成员')
            self.escape(identity)
            self.navigate(receiver, '加入群组')
            self.paste(receiver, self.editable(receiver, '邀请码', '加入群聊'), invite_code)
            self.click(receiver, self.field(receiver, '加入'))
            self.open_room(receiver, renamed)
            self.open_room(identity, renamed)
            self.click(identity, self.field(identity, '聊天信息'))
            self.field(identity, '2 位成员')
            self.escape(identity)
            self.capture(receiver, 'daily-rejoined-group')
            for actor, other in [(owner, peer), (peer, owner)]:
                marker = f'native-{seed}-{actor["id"]}-daily-created-group'
                self.open_room(actor['id'], renamed)
                self.send(actor['id'], marker)
                self.open_room(other['id'], renamed)
                self.expect_message(other['id'], marker, 'Created/rejoined group message not delivered',
                                    sender=actor['id'])
                emit('screen-received', actor=other['id'], marker=marker, from_actor=actor['id'])
            emit('daily-group-rejoined', actor=receiver, title=renamed,
                 expected_members=[identity, receiver], expected_count=2)
            return {'title': renamed, 'count_after_leave': 1, 'count_after_join': 2}
        case('leave-join-group', leave_join)
        for actor in actors:
            self.open_room(actor['id'], actor['group_title'])
        emit('daily-pilot-completed', operations=completed,
             implemented=['personal-preferences', 'clipboard-copy', 'typing-draft-clear',
                          'avatar-management', 'account-logout-login', 'create-group',
                          'rename-announcement', 'roles-owner-transfer-restore',
                          'invite-code', 'leave-join-group'],
             unverified=['manual-invite-remove', 'join-approval', 'offline-friendship',
                         'typing-expiry-switch', 'authoritative-read-unread'])
        return completed

    def lifecycle_pilot(self, seed):
        actors = self.cfg['actors']
        if len(actors) != 2:
            raise RuntimeError('GUI lifecycle pilot requires exactly two native actors')
        originals = {a['id']: {'pid': self.clients[a['id']].pid,
            'starttime': starttime(self.clients[a['id']].pid)} for a in actors}
        binary = pathlib.Path(self.cfg['binary']).resolve()
        completed = 0

        def verified(actor):
            identity = originals[actor]
            pid = identity['pid']
            if (self.clients[actor].poll() is not None or starttime(pid) != identity['starttime'] or
                    pathlib.Path(f'/proc/{pid}/exe').resolve() != binary):
                raise RuntimeError('Lifecycle operation refuses a changed or exited native process')
            return identity

        def operation(actor, kind, action, conversation=None, marker=None):
            nonlocal completed
            begin = time.monotonic()
            context = {'conversation': conversation} if conversation is not None else {}
            if marker is not None:
                context['marker'] = marker
            try:
                evidence = action() or {}
                emit('operation', actor=actor, kind=kind, ok=True,
                     elapsed_ms=(time.monotonic()-begin)*1000, evidence=evidence, **context)
                completed += 1
            except Exception as error:
                emit('operation', actor=actor, kind=kind, ok=False,
                     elapsed_ms=(time.monotonic()-begin)*1000,
                     error=type(error).__name__ + ': ' + str(error), **context)
                self.capture(actor, 'lifecycle-failure-' + kind)
                raise

        emit('lifecycle-ready', actors=[a['id'] for a in actors],
             processes=[dict(actor=a['id'], **originals[a['id']]) for a in actors])
        instruction = json.loads(sys.stdin.readline())
        if instruction.get('command') != 'recover':
            raise RuntimeError('Expected coordinator server recovery barrier')
        for actor in actors:
            identity = actor['id']
            def recovered(a=actor):
                verified(a['id'])
                # A status toast can disappear before this actor receives focus.
                # Recovered composer permission and a real member-page response
                # provide durable native state, then actual messages test it.
                self.wait(lambda: self.find(a['id'], '消息输入') and self.find(a['id'], '消息输入').
                    get_state_set().contains(self.Atspi.StateType.SENSITIVE),
                    'Qt composer did not recover after server restart', timeout=60)
                self.open_room(a['id'], a['group_title'])
                self.click(a['id'], self.field(a['id'], '聊天信息'))
                self.field(a['id'], str(a['member_count']) + ' 位成员')
                self.capture(a['id'], 'lifecycle-recovered-group-' + str(a['id']))
                self.escape(a['id'])
                self.navigate(a['id'], '联系人')
                self.wait_row(a['id'], '联系人列表', a['peer_username'])
                self.open_room(a['id'], a['group_title'])
                return {'pid': originals[a['id']]['pid'], 'same_process': True,
                        'member_count': a['member_count'], 'accepted_peer_visible': True}
            operation(identity, 'reconnect-group-header', recovered, actor['group_id'])

        def send_to_peer(actor, target, kind, suffix):
            marker = f'native-{seed}-{actor["id"]}-lifecycle-{suffix}'
            conversation = actor['group_id'] if target == actor['group_title'] else actor['direct_id']
            peer = next(a for a in actors if a['id'] == actor['peer_id'])
            def send():
                verified(actor['id'])
                self.open_room(actor['id'], target)
                self.send(actor['id'], marker, marker + ' 重连中文 🙂')
                peer_title = target if conversation == actor['group_id'] else actor['username']
                self.open_room(peer['id'], peer_title)
                self.expect_message(peer['id'], marker, 'Lifecycle message absent from peer native UI',
                                    sender=actor['id'])
                emit('screen-received', actor=peer['id'], marker=marker,
                     from_actor=actor['id'], conversation=conversation)
                return {'same_native_process': True, 'peer_received': True}
            operation(actor['id'], kind, send, conversation, marker)
        for actor in actors:
            send_to_peer(actor, actor['group_title'], 'reconnect-group-send', 'group')
            send_to_peer(actor, actor['peer_username'], 'reconnect-direct-send', 'direct')

        actor = actors[0]
        identity = actor['id']
        def minimize_restore():
            verified(identity)
            self.focus(identity)
            window = self.last_focus_window
            hidden = self.lib.XInternAtom(self.native, b'_NET_WM_STATE_HIDDEN', False)
            self.click(identity, self.field(identity, '最小化'))
            self.wait(lambda: hidden in self.property(window, '_NET_WM_STATE'),
                      'Native minimize button did not minimize the owned window')
            emit('native-minimized', actor=identity, pid=originals[identity]['pid'], window=window)
            # This is the standard EWMH window-manager restore action, not an
            # in-process Qt hook. xdotool/wmctrl are not required dependencies.
            class ClientMessage(C.Structure):
                _fields_ = [('type', C.c_int), ('serial', C.c_ulong), ('send_event', C.c_int),
                    ('display', C.c_void_p), ('window', C.c_ulong), ('message_type', C.c_ulong),
                    ('format', C.c_int), ('data', C.c_long * 5)]
            event = (C.c_long * 24)()
            message = ClientMessage.from_buffer(event)
            message.type, message.display, message.window = 33, self.native, window
            message.message_type = self.lib.XInternAtom(self.native, b'_NET_ACTIVE_WINDOW', False)
            message.format = 32
            message.data[0], message.data[1] = 2, 0
            self.lib.XSendEvent.argtypes = [C.c_void_p, C.c_ulong, C.c_int, C.c_long, C.c_void_p]
            self.lib.XSendEvent.restype = C.c_int
            if not self.lib.XSendEvent(self.native, self.root(), False, (1 << 20) | (1 << 19),
                                      C.byref(event)):
                raise RuntimeError('Native WM restore event was not sent')
            self.lib.XFlush(self.native)
            self.wait(lambda: hidden not in self.property(window, '_NET_WM_STATE') and
                      self.property(self.root(), '_NET_ACTIVE_WINDOW') == [window],
                      'Window manager did not restore the owned Qt window')
            self.open_room(identity, actor['peer_username'])
            self.capture(identity, 'lifecycle-minimize-restored')
            return {'pid': originals[identity]['pid'], 'native_minimize_button': True,
                    'restore_action': 'EWMH _NET_ACTIVE_WINDOW', 'same_process': True}
        operation(identity, 'native-minimize-restore', minimize_restore, actor['direct_id'])

        def pause_resume():
            original = verified(identity)
            try:
                os.kill(original['pid'], signal.SIGSTOP)
                deadline = time.monotonic() + 5
                while True:
                    verified(identity)
                    state = pathlib.Path(f'/proc/{original["pid"]}/stat').read_text().rsplit(')', 1)[1].split()[0]
                    if state == 'T':
                        emit('native-paused', actor=identity, process_state=state, **original)
                        break
                    if time.monotonic() >= deadline:
                        raise RuntimeError('SIGSTOP did not stop the verified owned Qt process')
                    time.sleep(.03)
            finally:
                original = verified(identity)
                os.kill(original['pid'], signal.SIGCONT)
            self.open_room(identity, actor['peer_username'])
            self.capture(identity, 'lifecycle-sigcont-responsive')
            return {'pid': original['pid'], 'starttime': original['starttime'],
                    'signals': ['SIGSTOP', 'SIGCONT'], 'same_process': True}
        operation(identity, 'native-sigstop-sigcont', pause_resume, actor['direct_id'])
        send_to_peer(actor, actor['peer_username'], 'resumed-direct-send', 'resumed')
        emit('lifecycle-restored', actors=[a['id'] for a in actors],
             checks=['server-restart', 'same-native-process', 'group-header-and-count',
                     'accepted-contacts', 'bidirectional-group-direct-receipts',
                     'native-minimize-restore', 'SIGSTOP/SIGCONT'],
             not_verified=['network-blackhole', 'crash-during-transfer', 'TLS-specific-faults'])
        return completed

    def shutdown(self):
        exits = []
        for actor, process in self.clients.items():
            normal_close = process.poll() == 0
            if process.poll() is None:
                try:
                    modal_atom = self.lib.XInternAtom(self.native, b'_NET_WM_STATE_MODAL', False)
                    for _ in range(4):
                        modals = [w for w in self.property(self.root(), '_NET_CLIENT_LIST')
                            if self.property(w, '_NET_WM_PID') == [process.pid] and
                            modal_atom in self.property(w, '_NET_WM_STATE')]
                        if not modals:
                            break
                        self.focus(actor)
                        self.key('Escape')
                        self.wait(lambda w=modals[-1]: w not in self.property(self.root(), '_NET_CLIENT_LIST'),
                                  'Native modal did not cancel during cleanup', timeout=3)
                    close = self.find(actor, '关闭')
                    if close is None:
                        raise RuntimeError('Native close control unavailable during cleanup')
                    self.click(actor, close)
                    process.wait(timeout=6)
                    normal_close = process.returncode == 0
                except Exception:
                    process.terminate()
            exits.append(dict(actor=actor, pid=process.pid,
                              exit_status=process.poll(), native_close=normal_close))
        # Release the driver's own X connection while the owned X server is
        # alive. Xlib's fatal I/O handler exits the interpreter if XCloseDisplay
        # is called only after terminating Xvfb.
        if self.native:
            self.lib.XCloseDisplay(self.native)
            self.native = None
        for process, identity in reversed(self.processes):
            if process.poll() is None:
                try:
                    if starttime(process.pid) != identity:
                        continue
                    process.terminate()
                    process.wait(timeout=6)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                except FileNotFoundError:
                    pass
        for record in exits:
            record['exit_status'] = self.clients[record['actor']].poll()
        # D-Bus activated document portals can still own a FUSE mount inside
        # XDG_RUNTIME_DIR. The coordinator removes this unique directory only
        # after this worker's dbus-run-session has exited and unmounted it.
        if hasattr(self, 'private_home'):
            emit('cleanup-directory-deferred', path=str(self.private_home))
        ok = all(record['native_close'] and record['exit_status'] == 0 for record in exits)
        emit('closed', clients=exits, ok=ok)
        return ok


def run(config):
    actors = config['actors']
    if not actors or len(actors) > 40:
        raise ValueError('A native GUI shard requires 1–40 actors')
    seed = int(config.get('seed', 1))
    gui = NativeGui.__new__(NativeGui)
    # Partly initialized driver can still clean children after startup failures.
    gui.processes, gui.clients, gui.native, gui.relays = [], {}, None, {}
    completed = False
    operation_count = 0
    try:
        gui.__init__(config)
        if config.get('blackhole_pilot', False) or config.get('read_pilot', False) or config.get('heartbeat_pilot', False):
            if len(actors) != 2:
                raise ValueError('A link pilot needs exactly two native GUI clients')
            # One relay per client so a single native client can lose its link while the other
            # keeps its direct connection to the server.
            target = urlsplit(config['server_url'])
            for actor in actors:
                relay = Blackhole(target.hostname, target.port)
                gui.relays[int(actor['id'])] = relay
                actor['relay_url'] = relay.url
        for actor in actors:
            gui.login(actor, config['password'])
        for actor in actors:
            identity = actor['id']
            gui.open_room(identity, actor['group_title'])
            marker = f'native-{seed}-{identity}-initial'
            begin = time.monotonic()
            gui.send(identity, marker)
            emit('operation', kind='initial-group-send', actor=identity, marker=marker,
                 conversation=actor['group_id'], elapsed_ms=(time.monotonic() - begin) * 1000, ok=True)
            operation_count += 1
        # Read a peer's independently sent text, not just the actor's own echo.
        for actor in actors:
            peer = next((a for a in actors if a['id'] == actor['peer_id']), None)
            if peer and peer['group_id'] == actor['group_id']:
                marker = f'native-{seed}-{peer["id"]}-initial'
                description = 'Peer initial group text absent from native UI'
                try:
                    # A short grace still proves the row arrived in the open view.
                    gui.expect_message(actor['id'], marker, description, sender=peer['id'], timeout=3)
                except TimeoutError:
                    # Ten one-line rows are taller than the history viewport and
                    # staying attached to the newest row is correct, so locate the
                    # same peer row by scrolling up. The screen-receipt assertion
                    # itself is unchanged.
                    emit('receipt-scroll-up', actor=actor['id'], marker=marker, sender=peer['id'])
                    gui.earliest(actor['id'])
                    gui.expect_message(actor['id'], marker, description + ' after scrolling up',
                                       sender=peer['id'])
                emit('screen-received', actor=actor['id'], marker=marker, from_actor=peer['id'])
        gui.capture(actors[0]['id'], 'ready-native-chat')
        emit('ready', actors=len(actors), operations=operation_count)
        gate = json.loads(sys.stdin.readline())
        if gate.get('command') != 'start':
            raise RuntimeError('Expected coordinator steady-state start gate')
        def deep_check(actor, kind):
            nonlocal operation_count
            identity = actor['id']
            marker = f'native-{seed}-{identity}-deep-{kind}'
            gui.open_room(identity, actor['group_title'])
            gui.send(identity, marker)
            begin = time.monotonic()
            try:
                evidence = None
                if kind == 'reply':
                    gui.menu(identity, marker, '回复')
                    gui.send(identity, marker + '-response')
                    gui.wait(lambda: gui.message(identity, marker + '-response') and
                        '回复' in gui.message(identity, marker + '-response').get_name(),
                             'Reply quote not visible')
                elif kind == 'edit':
                    gui.menu(identity, marker, '编辑')
                    gui.paste(identity, gui.field(identity, '消息内容'), marker + '-edited')
                    gui.click(identity, gui.field(identity, '保存'))
                    gui.wait(lambda: gui.message(identity, marker + '-edited') and
                        '已编辑' in gui.message(identity, marker + '-edited').get_name(), 'Edited state not visible')
                elif kind == 'reaction':
                    gui.reaction_pick(identity, marker, 0)
                    gui.wait(lambda: gui.message(identity, marker) and
                        '反应 👍' in gui.message(identity, marker).get_name(), 'Reaction not visible')
                    gui.reaction_pick(identity, marker, 1)
                    gui.wait(lambda: '反应 ❤️' in gui.message(identity, marker).get_name() and
                        '反应 👍' not in gui.message(identity, marker).get_name(), 'Reaction replacement not visible')
                    gui.reaction_pick(identity, marker, 1)
                    gui.wait(lambda: '反应' not in gui.message(identity, marker).get_name(),
                             'Reaction removal not visible')
                elif kind == 'search':
                    gui.focus(identity)
                    gui.key('f', ('Control_L',))
                    gui.paste(identity, gui.field(identity, '消息搜索关键词'), marker)
                    gui.click(identity, gui.field(identity, '搜索'))
                    gui.wait(lambda: any(marker in n.get_name() for n in gui.rows(identity, '消息搜索结果')),
                             'Search result not present')
                    gui.escape(identity)
                elif kind == 'group-details':
                    gui.click(identity, gui.field(identity, '聊天信息'))
                    nodes = gui.wait(lambda: [n.get_name() for n in gui.nodes(identity)
                        if '位成员' in n.get_name()], 'Group member count not visible')
                    pin = [n.get_name() for n in gui.nodes(identity) if '置顶消息' in n.get_name()]
                    evidence = {'member_labels': nodes, 'pin_labels': pin}
                    expected = actor.get('member_count')
                    if expected and not any(str(expected) + ' 位成员' in n for n in nodes):
                        raise RuntimeError('Group member count differs from fixture')
                    gui.capture(identity, 'group-details-' + str(identity))
                    gui.escape(identity)
                elif kind == 'list-search':
                    gui.navigate(identity, '聊天')
                    search = gui.field(identity, '搜索聊天')
                    gui.paste(identity, search, actor['group_title'])
                    gui.wait_row(identity, '会话列表', actor['group_title'])
                    gui.wait(lambda: not any(n.get_name() == actor['peer_username']
                        for n in gui.rows(identity, '会话列表')), 'Conversation search failed to filter peer')
                    gui.paste(identity, search, '')
                    gui.wait_row(identity, '会话列表', actor['peer_username'])
                    gui.navigate(identity, '联系人')
                    search = gui.field(identity, '搜索联系人')
                    gui.paste(identity, search, actor['peer_username'])
                    gui.wait_row(identity, '联系人列表', actor['peer_username'])
                    gui.paste(identity, search, 'no-native-contact-' + marker)
                    gui.wait(lambda: not gui.rows(identity, '联系人列表'), 'Missing contact query retained rows')
                    gui.paste(identity, search, '')
                    gui.wait_row(identity, '联系人列表', actor['peer_username'])
                    gui.open_room(identity, actor['group_title'])
                elif kind == 'group-pin':
                    if actor['role'] in ('owner', 'admin'):
                        gui.menu(identity, marker, '置顶消息')
                        gui.wait(lambda: any(n.get_name().startswith('置顶消息 ·') and marker in n.get_name()
                            for n in gui.nodes(identity)), 'Pinned chat header did not show native target')
                        gui.click(identity, gui.field(identity, '聊天信息'))
                        gui.wait(lambda: any('置顶消息' in n.get_name() and marker in n.get_name()
                            for n in gui.nodes(identity)), 'Group details initial pinned summary differs')
                        gui.capture(identity, 'group-pin-' + str(identity))
                        gui.escape(identity)
                        gui.click(identity, gui.field(identity, '取消置顶消息'))
                        gui.wait(lambda: not gui.find(identity, '取消置顶消息'), 'Group unpin control remained visible')
                        gui.click(identity, gui.field(identity, '聊天信息'))
                        gui.field(identity, '暂无置顶消息')
                        gui.escape(identity)
                    else:
                        if gui.find(identity, '取消置顶消息'):
                            raise RuntimeError('Ordinary member exposes group unpin action')
                        evidence = {'permission': 'member cannot manage group pin'}
                elif kind == 'profile':
                    gui.navigate(identity, '我的资料')
                    gui.field(identity, '关闭资料')
                    gui.capture(identity, 'account-' + str(identity))
                    gui.click(identity, gui.field(identity, '关闭资料'))
                elif kind == 'friendship':
                    gui.friendship(actor, marker)
                elif kind == 'attachment':
                    gui.attachments(actor, marker)
                elif kind == 'delete':
                    target = gui.message(identity, marker)
                    gui.menu(identity, marker, '删除')
                    gui.click(identity, gui.field(identity, '删除'))
                    gui.wait(lambda: not gui.message(identity, marker) and '消息已删除' in target.get_name(),
                             'Target deleted placeholder not present')
                emit('operation', kind=kind, actor=identity, marker=marker,
                     conversation=actor['group_id'],
                     action_conversation=actor['direct_id'] if kind in ('attachment', 'friendship') else actor['group_id'],
                     elapsed_ms=(time.monotonic() - begin) * 1000,
                     ok=True, evidence=evidence)
                operation_count += 1
            except Exception as error:
                emit('operation', kind=kind, actor=identity, marker=marker,
                     conversation=actor['group_id'],
                     action_conversation=actor['direct_id'] if kind in ('attachment', 'friendship') else actor['group_id'],
                     elapsed_ms=(time.monotonic() - begin) * 1000,
                     ok=False, error=type(error).__name__ + ': ' + str(error))
                gui.capture(identity, 'failure-' + kind + '-' + str(identity))
                gui.escape(identity)
                raise
        steady = float(config['steady_seconds'])
        prewarm_started = time.monotonic()
        distribution = {a['id']: ('active' if i % 5 < 2 else 'low-frequency' if i % 5 == 2 else 'receiver')
                        for i, a in enumerate(actors)}
        emit('workload', steady_seconds=steady, seed=seed, distribution=distribution,
             input_method='XTest', response_measurement='Driver-observed native UI includes polling')
        deep = config.get('deep_kinds', ['reply', 'edit', 'reaction', 'search',
            'group-details', 'profile', 'delete', 'attachment', 'friendship', 'list-search', 'group-pin'])
        unsupported = ['group-create-and-join', 'group-invite-remove-rename-announcement',
            'group-roles-and-approval', 'contact-and-nonfriend-profile-actions', 'avatar-management',
            'clipboard-copy', 'read-unread-authoritative-checks', 'typing-authoritative-checks',
            'duplicate-and-concentrated-login', 'network-and-process-lifecycle']
        if config.get('daily_pilot', False):
            unsupported.remove('group-create-and-join')
            unsupported.remove('group-invite-remove-rename-announcement')
            unsupported.append('manual-group-invite-remove')
            unsupported.remove('avatar-management')
            unsupported.remove('clipboard-copy')
            unsupported.remove('typing-authoritative-checks')
            unsupported.remove('group-roles-and-approval')
            unsupported.remove('duplicate-and-concentrated-login')
            unsupported.extend(['group-join-approval', 'typing-idle-expiry-and-switch',
                                'concentrated-login-peak'])
        if config.get('lifecycle_pilot', False):
            unsupported.remove('network-and-process-lifecycle')
            unsupported.extend(['network-blackhole', 'crash-during-transfer', 'TLS-specific-faults'])
        if config.get('blackhole_pilot', False):
            # The relay covers the silent link, and the pilot restarts the client afterwards.
            unsupported.remove('network-and-process-lifecycle')
            unsupported.remove('read-unread-authoritative-checks')
            unsupported.extend(['crash-during-transfer', 'TLS-specific-faults'])
        if config.get('read_pilot', False):
            unsupported.remove('read-unread-authoritative-checks')
            unsupported.extend(['network-blackhole', 'unread-badge-digit-recognition'])
        if config.get('heartbeat_pilot', False):
            # The client is never restarted here, so this is the automatic-recovery scenario; the
            # unread number itself stays out of the accessibility tree and is not claimed.
            unsupported.remove('network-and-process-lifecycle')
            unsupported.extend(['crash-during-transfer', 'TLS-specific-faults', 'manual-process-restart'])
        for kind in unsupported:
            emit('unsupported', kind=kind, reason='Not covered by the configured native scenario')
        emit('prewarm-start', actors=len(actors))
        for actor in actors:
            identity = actor['id']
            marker = f'native-{seed}-{identity}-direct-warm'
            gui.open_room(identity, actor['peer_username'])
            begin = time.monotonic()
            gui.send(identity, marker, marker + ' 中文 🙂\n预热第二行')
            emit('operation', kind='prewarm-direct-send', actor=identity, marker=marker,
                 conversation=actor['direct_id'], elapsed_ms=(time.monotonic()-begin)*1000, ok=True)
            operation_count += 1
        for actor in actors:
            peer = next((a for a in actors if a['id'] == actor['peer_id']), None)
            if peer:
                marker = f'native-{seed}-{peer["id"]}-direct-warm'
                gui.expect_message(actor['id'], marker, 'Prewarm direct peer message absent',
                                   sender=peer['id'])
                emit('screen-received', actor=actor['id'], marker=marker, from_actor=peer['id'])
        deep_targets = list(actors[:min(int(config.get('deep_actor_count',3)), len(actors))]) if config.get('deep_checks', True) else []
        deep_cursor = 0
        for kind in deep:
            for actor in deep_targets:
                deep_check(actor, kind)
                deep_cursor += 1
        if config.get('daily_pilot', False):
            operation_count += gui.daily_pilot(config['password'], seed)
        if config.get('blackhole_pilot', False):
            operation_count += gui.blackhole_pilot(seed)
        if config.get('read_pilot', False):
            operation_count += gui.read_pilot(seed)
        if config.get('heartbeat_pilot', False):
            operation_count += gui.heartbeat_pilot(seed)
        if config.get('lifecycle_pilot', False):
            operation_count += gui.lifecycle_pilot(seed)
        medium_groups = {}
        for actor in actors:
            if actor.get('medium_id') is not None:
                medium_groups.setdefault(actor['medium_id'], []).append(actor)
        for medium_id, participants in medium_groups.items():
            if len(participants) < 2:
                raise RuntimeError('Medium fixture requires at least two local native actors')
            title = participants[0]['medium_title']
            count = participants[0]['medium_member_count']
            if any(a['medium_title'] != title or a['medium_member_count'] != count for a in participants):
                raise RuntimeError('Medium fixture metadata differs within the native shard')
            for actor in participants:
                identity = actor['id']
                begin = time.monotonic()
                gui.open_room(identity, title)
                gui.click(identity, gui.field(identity, '聊天信息'))
                gui.field(identity, str(count) + ' 位成员')
                if actor == participants[0]:
                    gui.capture(identity, 'medium-group-' + str(medium_id))
                gui.escape(identity)
                emit('operation', actor=identity, kind='medium-group-metadata',
                     conversation=medium_id, ok=True, elapsed_ms=(time.monotonic()-begin)*1000,
                     evidence={'title': title, 'member_count': count})
                operation_count += 1
            sender = participants[0]['id']
            marker = f'native-{seed}-{sender}-medium-{medium_id}-initial'
            begin = time.monotonic()
            gui.send(sender, marker, marker + ' 中文中型群 🙂')
            emit('operation', actor=sender, kind='medium-group-send', marker=marker,
                 conversation=medium_id, ok=True, elapsed_ms=(time.monotonic()-begin)*1000)
            operation_count += 1
            for actor in participants:
                identity = actor['id']
                gui.open_room(identity, title)
                gui.expect_message(identity, marker, 'Medium group native receipt absent', sender=sender)
                emit('screen-received', actor=identity, marker=marker, from_actor=sender,
                     conversation=medium_id)
        prewarm_seconds = time.monotonic() - prewarm_started
        if config.get('steady_gate', False):
            emit('steady-ready', actors=len(actors), prewarm_seconds=prewarm_seconds)
            gate = json.loads(sys.stdin.readline())
            if gate.get('command') != 'steady':
                raise RuntimeError('Expected coordinator global steady-state gate')
        start = time.monotonic()
        deadline = start + steady
        emit('steady-start', actors=len(actors), prewarm_seconds=prewarm_seconds,
             steady_seconds=steady, deep_cases_completed=deep_cursor)
        round_number = 0
        recent_group_messages = {}
        next_wave = start
        while time.monotonic() < deadline:
            if time.monotonic() < next_wave:
                time.sleep(min(.2, next_wave - time.monotonic(), max(0, deadline - time.monotonic())))
                gui.pump()
                continue
            for actor in actors:
                if time.monotonic() >= deadline:
                    break
                identity = actor['id']
                if distribution[identity] == 'receiver':
                    begin = time.monotonic()
                    gui.open_room(identity, actor['group_title'])
                    gui.field(identity, '消息记录')
                    latest_group = recent_group_messages.get(actor['group_id'])
                    if latest_group is not None:
                        marker, sender = latest_group
                        gui.expect_message(identity, marker, 'Receiver group text absent', sender=sender)
                        emit('screen-received', actor=identity, marker=marker, from_actor=sender,
                             conversation=actor['group_id'])
                    emit('operation', kind='receive-and-select', actor=identity, conversation=actor['group_id'],
                         message_observed=latest_group is not None,
                         elapsed_ms=(time.monotonic()-begin)*1000, ok=True)
                    operation_count += 1
                    continue
                if distribution[identity] == 'low-frequency' and round_number % 3:
                    continue
                direct = round_number % 2 == 0
                title = actor['peer_username'] if direct else actor['group_title']
                gui.open_room(identity, title)
                marker = f'native-{seed}-{identity}-wave-{round_number}'
                body = marker + (' 中文消息 🙂\n第二行' if round_number % 3 == 0 else ' hello')
                begin = time.monotonic()
                gui.send(identity, marker, body)
                if not direct:
                    recent_group_messages[actor['group_id']] = (marker, identity)
                emit('operation', kind='direct-send' if direct else 'group-send', actor=identity, marker=marker,
                     conversation=actor['direct_id'] if direct else actor['group_id'],
                     elapsed_ms=(time.monotonic() - begin) * 1000, ok=True)
                operation_count += 1
                peer = next((a for a in actors if a['id'] == actor['peer_id']), None)
                if direct and peer:
                    gui.open_room(peer['id'], actor['username'])
                    received = gui.expect_message(peer['id'], marker, 'Peer direct text absent', sender=identity)
                    if received:
                        emit('screen-received', actor=peer['id'], marker=marker, from_actor=identity,
                             elapsed_ms=(time.monotonic() - begin) * 1000)
            round_number += 1
            next_wave = time.monotonic() + float(config.get('wave_interval', 20))
        measured_steady = time.monotonic() - start
        gui.capture(actors[0]['id'], 'final-native-chat')
        if config.get('finish_gate', False):
            emit('steady-finished', actors=len(actors), steady_seconds=measured_steady)
            gate = json.loads(sys.stdin.readline())
            if gate.get('command') != 'finish':
                raise RuntimeError('Expected coordinator verified finish gate')
        completed = True
        emit('completed', actors=len(actors), operations=operation_count,
             steady_seconds=measured_steady, prewarm_seconds=prewarm_seconds,
             deep_cases_completed=deep_cursor,
             deep_cases_planned=len(deep) * len(deep_targets),
             coverage_complete=deep_cursor == len(deep) * len(deep_targets) and not unsupported)
    except Exception:
        failed_ids = [getattr(gui, 'last_focus', None)]
        context = getattr(gui, 'receipt_context', None)
        if context:
            failed_ids.extend([context['actor'], context.get('sender')])
        failed_ids.extend(actor['id'] for actor in actors[:2])
        for identity in dict.fromkeys(failed_ids):
            if identity is None:
                continue
            actor = next(a for a in actors if a['id'] == identity)
            process = gui.clients.get(actor['id'])
            if process is None or process.poll() is not None:
                continue
            try:
                gui.capture(actor['id'], 'worker-failure-' + str(actor['id']))
            except Exception as snapshot_error:
                emit('capture-failure', actor=actor['id'], error=type(snapshot_error).__name__)
        raise
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        cleanup_ok = gui.shutdown()
    return 0 if completed and cleanup_ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    config = json.loads(sys.stdin.readline())
    if config.get('password'):
        _private_values.add(config['password'])
    emit('driver-started', pid=os.getpid(), starttime=starttime(os.getpid()),
         executable=str(pathlib.Path('/proc/self/exe').resolve()))
    def interrupted(signum, frame):
        emit('interrupted', signal=signum)
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    try:
        return run(config)
    except Exception as error:
        print(redact_text(traceback.format_exc()), file=sys.stderr)
        emit('failure', error=type(error).__name__ + ': ' + str(error))
        return 1


if __name__ == '__main__':
    sys.exit(main())
