#!/usr/bin/env python3
"""Temporary four-client native JSONL executor; no automatic product PASS.
Prepared only. Root must review exact file SHA before --execute. Runtime business
is XTest/tmux input only; SDK is prelogin seed only, SQL observation is SELECT.
"""
import argparse, ctypes as C, fcntl, gzip, hashlib, json, os, re, secrets
import selectors, shlex, signal, struct, subprocess as S, sys, termios, time
from pathlib import Path
from urllib.request import urlopen
sys.dont_write_bytecode=True
REPO=Path('/home/gyl/example/chat')
HEAD='c86b0936b2dbeb82ff3730e9225baf1d9e4614fa'
SOURCES=Path(__file__).with_name('source206.json')
SOURCES_SHA='ee689d03564e483db9ab045eb0c5fa751bcaaa70c15264b570808212cbfbd181'
GATE=REPO/'docs/images/experience/member-role-original-verify-proof.json'
GATE_SHA='1c6f0e2224397ad1dff4f4432eddf37658d4dfe61ada20421106ddef3cdaa5cf'
QT_PROOF=REPO/'docs/images/experience/qt-private-tabs-evidence/native-after-actual-pass/run/runtime-A-authenticated.json.gz'
QT_PROOF_SHA='517d9e42220acfc0ccf4ac4ccc9f11bacb8e8afadf99eb2274882a5f9fb45b82'
COMPACT=REPO/'docs/images/experience/qt-login-compact/proof.json'
COMPACT_SHA='daec1c8ba2a0bb60cfd1944b592aed3021a4d9019d79e2556c5f1785e421bd0f'
CTEST=REPO/'docs/images/experience/qt-login-compact/ctest.log.gz'
CTEST_SHA='dd36521f84748b4bc31227f923a1222f814d1fb247f165467cba1bc77344b400'
PHASES=['login','friends','direct-group','draft-switch','search-actions','attachments','profile','reconnect','logout']
BINS={'build/qt/chat_qt':'3f171e52073b910b3deac61e95e9c2e8683fa0c6300f4fc879e8194099c04eb2',
      'build/chat_tui':'1ecdf579af21099c08210e0d14b5383cef97342fc0a5ae0540f8877dac030350',
      'build/chat_server':'f2c9288cbc32817183e399e72a4a3af8e45adfdfbd54435af43aae86da36bf5b',
      'build/chat_tui_scale_fixture':'b624a131f2170164bd4f9b2c9ac2cc732fd6b62c23819f3a8b9d7fc0b4d021fe'}
NATIVE={'/usr/bin/tmux':'baa7e6be444a751339bb52de4006fb192187d40b9d17548a9fcb60c8ed7ad495',
        '/usr/bin/wezterm':'dcbb87b05cc765677830c06af88b0b7893351dd85bd99e648aa100560d6a2aee',
        '/usr/bin/dbus-daemon':'9e5cf3260e8b0d68a88732ebc4386970ff5977f4a9bcb9022a13ab905f832bbc',
        '/usr/bin/Xvfb':'cf71d78eab8321f7c568271053af449f966a75422140117ea1f244bf72a7e81e',
        '/usr/bin/xclip':'7bd2a1bb6ee2c881d5ef5c1162bf7dc275df58a029466fc07ecc82f2046bc127',
        '/usr/bin/dbus-update-activation-environment':'f912bf470bffbc981fdf0d7be2b208320ef720ebd0ab93332307356de5d75005'}
PROTECTED={2876288:'521400965',3225:'17583',29935:'248258'}
IMAGE_FORMATS={
    '/tmp/chat-qt-dependency-compare.dmJWg7/install/plugins/imageformats/libqico.so':'cf835de8aa0b9d95acfccd64eadb62faeac79d1f34a6b2e17cde022f8fa70280',
    '/tmp/chat-qt-dependency-compare.dmJWg7/install/plugins/imageformats/libqgif.so':'8cda9e49ef1276f6d8ea43d995e920e7217f117364a7f500c099d83109a7efab',
    '/tmp/chat-qt-dependency-compare.dmJWg7/install/plugins/imageformats/libqjpeg.so':'f71f30b6ab7fa064faffe8d36712ef532a4de8874afb1bb485976c6cbc9f199b',
    '/tmp/chat-qt-dependency-compare.dmJWg7/install/plugins/imageformats/libqsvg.so':'530ee0466a2d4a5f443dba337b5ac7bf9a5ab79501fd571625820807e83d20d5',
    '/tmp/chat-qt-dependency-compare.dmJWg7/install/plugins/iconengines/libqsvgicon.so':'80c3956aae4197afbf911fb3db52e93219e2dff88459826b666b0ef88e0e85ca'}
def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def pins(text): return {str(Path(p).resolve()):h for h,p in (l.split(None,1) for l in text.splitlines() if l)}
def identity(pid):
    try:
        p=Path('/proc')/str(pid)
        return {'pid':pid,'ticks':p.joinpath('stat').read_text().rsplit(')',1)[1].split()[19],
                'exe':str(p.joinpath('exe').resolve()),'sha':sha(p/'exe'),'cmd_sha':sha(p/'cmdline')}
    except OSError: return None
def wait(test,label):
    end=time.monotonic()+15
    while time.monotonic()<end:
        result=test()
        if result: return result
        time.sleep(.08)
    raise AssertionError('Original 15s deadline: '+label)

class Daily:
    def __init__(self,args):
        self.args=args; self.made=False; self.owned=[]; self.qt={}; self.panes={}; self.original={}
        self.server=None; self.helper=None; self.db_attempt=False; self.tmux_owned=False; self.tmux_identity=None
        self.events=[]; self.shots=[]; self.completed=False; self.protected={}
        self.w=Path(args.scope).resolve(); self.socket=str(self.w/'tmux.sock')
        assert self.w.parent==Path('/tmp') and self.w.name.startswith('chat-daily-native-') and not self.w.exists()
        assert args.reviewed_sha==sha(__file__), 'Review exact driver before execution'
        self.env=os.environ.copy()
        for key in ('LD_LIBRARY_PATH','LD_PRELOAD','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH'):
            assert not self.env.get(key),'Clean system loader required: '+key
            self.env.pop(key,None)
        for key in ('DISPLAY','WAYLAND_DISPLAY','TMUX','DBUS_SESSION_BUS_ADDRESS','DBUS_STARTER_ADDRESS','DBUS_STARTER_BUS_TYPE',
                    'AT_SPI_BUS_ADDRESS','IBUS_ADDRESS','IBUS_ADDRESS_FILE','QT_IM_MODULE','QT_SCALE_FACTOR','I3SOCK','XAUTHORITY'):
            self.env.pop(key,None); os.environ.pop(key,None)
        self.env.update(GTK_IM_MODULE='none',XMODIFIERS='@im=none'); os.environ.update(GTK_IM_MODULE='none',XMODIFIERS='@im=none')
        self.maintenance=self.env.get('PGDATABASE','postgres'); self.database='chat_daily_'+secrets.token_hex(8)
        self.env['PGDATABASE']=self.database; self.password=secrets.token_hex(16)
        self.url='ws://127.0.0.1:'+str(args.port)+'/ws'
        assert sha(SOURCES)==SOURCES_SHA; self.sources=json.loads(SOURCES.read_text())
        assert self.sources['head']==HEAD and self.sources['count']==len(self.sources['inputs'])==206
        assert sha(GATE)==GATE_SHA and sha(QT_PROOF)==QT_PROOF_SHA and sha(COMPACT)==COMPACT_SHA and sha(CTEST)==CTEST_SHA
        self.gate=json.loads(GATE.read_text()); self.compact=json.loads(COMPACT.read_text())
        assert self.compact['qt_binary_sha256']==BINS['build/qt/chat_qt']
        normal=self.compact['normal_ctest']; assert normal['exit']==0 and normal['passed']==normal['total']==23 and not normal['timeouts_changed']
        assert not normal['asan_ubsan_rerun'], 'Do not label historical69 as current69'
        self.runtime=pins(self.gate['private_qt_runtime_pins']); self.runtime.update(IMAGE_FORMATS)
        self.prefix=Path(self.gate['private_qt_prefix'])
        historic_runtime=json.loads(gzip.decompress(QT_PROOF.read_bytes()))
        self.required_qt=set()
        for item in historic_runtime['loaded_qt']:
            path=str(Path(item['path']).resolve()); assert path not in self.runtime or self.runtime[path]==item['sha256']
            self.runtime[path]=item['sha256']; self.required_qt.add(path)
        self.check()
        self.protected={str(pid):identity(pid) for pid in PROTECTED}
        assert all(self.protected[str(pid)] and self.protected[str(pid)]['ticks']==tick for pid,tick in PROTECTED.items())
        self.w.mkdir(mode=0o700); self.made=True
        self.save('freeze.json',{'head':HEAD,'sources':self.sources,'source_manifest_sha':SOURCES_SHA,'bins':BINS,'native':NATIVE,
            'qt_runtime':self.runtime,'current_normal_23':normal,'current69':False,'historical69_gate_sha':GATE_SHA,
            'historical_runtime_proof_sha':QT_PROOF_SHA,'compact_proof_sha':COMPACT_SHA,'ctest_sha':CTEST_SHA,'protected':self.protected})
    def run(self,cmd,**kw):
        kw.setdefault('timeout',15)
        return S.run(cmd,env=kw.pop('env',self.env),text=True,check=True,stdout=S.PIPE,stderr=S.PIPE,**kw)
    def spawn(self,name,cmd,**kw):
        output=kw.pop('stdout',None)
        if output is None: output=(self.w/(name+'.log')).open('ab')
        p=S.Popen(cmd,env=kw.pop('env',self.env),stdout=output,stderr=kw.pop('stderr',S.STDOUT),**kw)
        self.owned.append((name,p)); return p
    def check(self):
        assert self.run(['git','-C',str(REPO),'rev-parse','HEAD']).stdout.strip()==HEAD
        assert not self.run(['git','-C',str(REPO),'status','--porcelain']).stdout.strip(), 'Repository changed'
        for p,h in [(SOURCES,SOURCES_SHA),(GATE,GATE_SHA),(QT_PROOF,QT_PROOF_SHA),(COMPACT,COMPACT_SHA),(CTEST,CTEST_SHA)]: assert sha(p)==h,str(p)
        for item in self.sources['inputs']:
            p=Path(item['path']); assert sha(p if p.is_absolute() else REPO/p)==item['sha256'],item['path']
        for p,h in {**{str(REPO/p):h for p,h in BINS.items()},**NATIVE,**self.runtime}.items(): assert sha(p)==h,p
        if self.protected: assert self.protected=={str(pid):identity(pid) for pid in PROTECTED}
        for actor,original in self.original.items(): assert identity(original['pid'])==original,actor+' original PID/exe changed'
    def save(self,name,data):
        text=json.dumps(data,ensure_ascii=False,indent=2); assert self.password not in text
        (self.w/name).write_text(text)
    def sql(self,statement):
        assert statement.lstrip().lower().startswith('select ')
        env=self.env.copy(); env['PGOPTIONS']='-c default_transaction_read_only=on'
        return self.run(['psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-c',statement],env=env).stdout.strip()
    def start_server(self):
        assert self.server is None or self.server.poll() is not None
        self.server=self.spawn('server',[str(REPO/'build/chat_server'),str(self.args.port),'256','16'])
        def ready():
            assert self.server.poll() is None
            try: return urlopen('http://127.0.0.1:'+str(self.args.port)+'/health',timeout=.5).status==200
            except OSError: return False
        wait(ready,'own server health')
    def setup(self):
        import socket
        with socket.socket() as probe: probe.bind(('127.0.0.1',self.args.port))
        self.db_attempt=True
        self.run(['createdb','--maintenance-db='+self.maintenance,'--template=template0','--encoding=UTF8',self.database])
        for migration in sorted((REPO/'sql').glob('[0-9][0-9][0-9]_*.sql')): self.run(['psql','-X','-v','ON_ERROR_STOP=1','-f',str(migration)])
        self.start_server()
        self.helper=self.spawn('sdk-prelogin',[str(REPO/'build/chat_tui_scale_fixture')],stdin=S.PIPE,stdout=S.PIPE,stderr=(self.w/'sdk-prelogin.stderr').open('ab'),text=True,bufsize=1)
        def seed(number,command,**payload):
            assert not self.qt and not self.panes
            self.helper.stdin.write(json.dumps({'id':number,'command':command,**payload},ensure_ascii=False)+'\n'); self.helper.stdin.flush()
            reply=json.loads(self.line(self.helper,'SDK '+command))
            assert reply['id']==number and reply['ok']; return reply['result']
        seed(1,'configure',url=self.url,prefix='day_'+secrets.token_hex(6),password=self.password)
        self.manifest=seed(2,'seed_navigation'); seed(3,'close'); self.helper.stdin.close()
        assert self.helper.wait(timeout=15)==0 and identity(self.helper.pid) is None
        self.save('manifest.json',{k:v for k,v in self.manifest.items() if k!='invite_token'})
        self.save('sdk-closed-before-login.json',{'pid':self.helper.pid,'actual_exit':0})
        for key,directory in [('XDG_CONFIG_HOME','config'),('XDG_CACHE_HOME','cache'),('XDG_DATA_HOME','data'),('XDG_RUNTIME_DIR','runtime')]:
            path=self.w/directory; path.mkdir(mode=0o700); self.env[key]=str(path); os.environ[key]=str(path)
        xv=self.spawn('xvfb',['/usr/bin/Xvfb','-displayfd','1','-screen','0','2400x1600x24','-nolisten','tcp'],stdout=S.PIPE,stderr=(self.w/'xvfb.stderr').open('ab'),text=True)
        self.display=':'+self.line(xv,'Xvfb DISPLAY')
        native=dict(DISPLAY=self.display,QT_QPA_PLATFORM='xcb',QT_ACCESSIBILITY='1',QT_LINUX_ACCESSIBILITY_ALWAYS_ON='1',QT_FONT_DPI='96',QT_AUTO_SCREEN_SCALE_FACTOR='0',QT_ENABLE_HIGHDPI_SCALING='0')
        self.env.update(native); os.environ.update(native)
        bus=self.spawn('dbus',['/usr/bin/dbus-daemon','--session','--nofork','--print-address=1','--nosyslog'],stdout=S.PIPE,stderr=(self.w/'dbus.stderr').open('ab'),text=True)
        address=self.line(bus,'private DBus'); assert address.startswith('unix:')
        self.env['DBUS_SESSION_BUS_ADDRESS']=address; os.environ['DBUS_SESSION_BUS_ADDRESS']=address
        sys.path.append('/usr/lib/python3/dist-packages')
        import dbus,pyatspi
        self.at=pyatspi
        iface=dbus.Interface(dbus.bus.BusConnection(address).get_object('org.freedesktop.DBus','/org/freedesktop/DBus',introspect=False),'org.freedesktop.DBus')
        assert int(iface.GetConnectionUnixProcessID('org.freedesktop.DBus'))==bus.pid
        names=['DISPLAY','XDG_RUNTIME_DIR','XDG_CONFIG_HOME','XDG_CACHE_HOME','XDG_DATA_HOME','QT_LINUX_ACCESSIBILITY_ALWAYS_ON','QT_ACCESSIBILITY','QT_QPA_PLATFORM']
        activation=self.run(['/usr/bin/dbus-update-activation-environment','--verbose',*names]); (self.w/'activation.log').write_text(activation.stdout+activation.stderr)
        self.x=C.CDLL('libX11.so.6'); self.xt=C.CDLL('libXtst.so.6')
        for name,args,result in [('XOpenDisplay',[C.c_char_p],C.c_void_p),('XSetInputFocus',[C.c_void_p,C.c_ulong,C.c_int,C.c_ulong],C.c_int),('XRaiseWindow',[C.c_void_p,C.c_ulong],C.c_int),('XFlush',[C.c_void_p],C.c_int),('XStringToKeysym',[C.c_char_p],C.c_ulong),('XKeysymToKeycode',[C.c_void_p,C.c_ulong],C.c_uint),('XMoveWindow',[C.c_void_p,C.c_ulong,C.c_int,C.c_int],C.c_int)]:
            f=getattr(self.x,name); f.argtypes=args; f.restype=result
        self.d=self.x.XOpenDisplay(self.display.encode()); assert self.d
        self.xt.XTestFakeKeyEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
        self.xt.XTestFakeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
        (self.w/'tmux.conf').write_text('set -g default-terminal tmux-256color\nset -g status off\nset -g exit-empty on\nset -s variation-selector-always-wide off\n')
        (self.w/'wezterm.lua').write_text("local w=require 'wezterm'\nreturn {front_end='Software',enable_wayland=false,unicode_version=9,initial_cols=120,initial_rows=40,enable_tab_bar=false,enable_scroll_bar=false,font=w.font_with_fallback({'DejaVu Sans Mono','Lily Han Sans HW SC','Noto Color Emoji'}),font_size=12,dpi=96,window_padding={left=0,right=0,top=0,bottom=0},window_close_confirmation='NeverPrompt'}\n")
        (self.w/'qt-text.txt').write_bytes('Native attachment 中文\n'.encode())
        from PIL import Image
        Image.new('RGB',(2,2),(32,120,80)).save(self.w/'tui-image.png')
    def line(self,process,label):
        with selectors.DefaultSelector() as sel:
            sel.register(process.stdout,selectors.EVENT_READ); assert sel.select(15),label
            line=process.stdout.readline().strip(); assert line; return line
    def tmux(self,*args,**kw): return self.run(['/usr/bin/tmux','-S',self.socket,*args],**kw).stdout.strip()
    def pane(self,actor,styled=False):
        text=self.tmux('capture-pane','-p',*(['-e'] if styled else []),'-t',self.panes[actor]); assert self.password not in text; return text
    def keys(self,actor,names):
        if actor in self.panes:
            for name in names:
                self.tmux('send-keys','-t',self.panes[actor],name)
                if name=='Escape': time.sleep(.15)
        else:
            assert actor in self.qt
            for name in names:
                parts=name.split('+'); mods=[m for token,m in [('ctrl','Control_L'),('shift','Shift_L'),('alt','Alt_L')] if token in parts[:-1]]
                for key,down in [(m,1) for m in mods]+[(parts[-1],1),(parts[-1],0)]+[(m,0) for m in reversed(mods)]:
                    code=self.x.XKeysymToKeycode(self.d,self.x.XStringToKeysym(key.encode())); assert code
                    self.xt.XTestFakeKeyEvent(self.d,code,down,0)
                self.x.XFlush(self.d); time.sleep(.1)
    def paste(self,actor,text):
        if actor in self.panes:
            self.tmux('load-buffer','-b',actor,'-',input=text); self.tmux('paste-buffer','-p','-b',actor,'-d','-t',self.panes[actor])
        else:
            p=self.spawn('clipboard',['/usr/bin/xclip','-selection','clipboard','-quiet'],stdin=S.PIPE)
            p.stdin.write(text.encode()); p.stdin.close()
            wait(lambda:self.run(['/usr/bin/xclip','-selection','clipboard','-o']).stdout==text,'actual clipboard ready')
            self.keys(actor,['ctrl+v'])
    def focus(self,actor,title='Chat'):
        def window():
            found=[]
            for line in self.run(['xwininfo','-root','-tree']).stdout.splitlines():
                m=re.match(r'\s*(0x[0-9a-f]+) "([^"\n]*)".*? (\d+)x(\d+)\+(-?\d+)\+(-?\d+)',line)
                if m and title in m[2] and int(m[3])>100 and self.run(['xprop','-id',m[1],'_NET_WM_PID']).stdout.strip().endswith('= '+str(self.qt[actor].pid)): found.append(int(m[1],16))
            assert len(found)<=1,'Ambiguous owned window'; return found[0] if found else None
        wid=wait(window,'owned Qt window '+title); self.x.XRaiseWindow(self.d,wid); self.x.XSetInputFocus(self.d,wid,2,0); self.x.XFlush(self.d); time.sleep(.12); return wid
    def geometry(self,wid):
        info=self.run(['xwininfo','-id',hex(wid)]).stdout
        return [int(re.search(r'Width:\s*(\d+)',info)[1]),int(re.search(r'Height:\s*(\d+)',info)[1])]
    def nodes(self,actor):
        output=[]; desktop=self.at.Registry.getDesktop(0)
        def visit(node,path):
            if node is None: return
            try:
                state=node.getState()
                if state.contains(self.at.STATE_DEFUNCT): return
                info={'path':path,'name':node.name,'role':node.getRoleName(),'showing':state.contains(self.at.STATE_SHOWING)}
                try:
                    r=node.queryComponent().getExtents(self.at.DESKTOP_COORDS); info['rect']=[r.x,r.y,r.width,r.height]
                except Exception: pass
                if info['role']!='password text':
                    try: info['text']=node.queryText().getText(0,-1)
                    except Exception: pass
                output.append((node,info))
                for i in range(node.childCount):
                    try: child=node.getChildAtIndex(i)
                    except Exception: continue
                    visit(child,path+[i])
            except Exception: return
        if desktop is not None:
            for i in range(desktop.childCount):
                try:
                    app=desktop.getChildAtIndex(i)
                    if app is not None and not app.getState().contains(self.at.STATE_DEFUNCT) and app.get_process_id()==self.qt[actor].pid: visit(app,[])
                except Exception: continue
        return output
    def target(self,actor,selector):
        matches=[(n,i) for n,i in self.nodes(actor) if i['showing'] and i.get('rect',[0,0,0,0])[2]>0 and all(i.get(k)==v for k,v in selector.items())]
        assert len(matches)<=1,'Ambiguous exact selector: inspect path/role/name'; return matches[0] if matches else None
    def click(self,actor,selector,button=1):
        node,info=wait(lambda:self.target(actor,selector),'exact Qt visible selector'); x,y,w,h=info['rect']
        self.xt.XTestFakeMotionEvent(self.d,-1,x+w//2,y+h//2,0); self.xt.XTestFakeButtonEvent(self.d,button,1,0); self.xt.XTestFakeButtonEvent(self.d,button,0,0); self.x.XFlush(self.d); time.sleep(.15); return info
    def login(self):
        assert self.helper.poll()==0
        for actor,offset in [('A',0),('D',1200)]:
            env=self.env.copy(); env.update(LD_LIBRARY_PATH=str(self.prefix/'lib'),QT_PLUGIN_PATH=str(self.prefix/'plugins'),QT_QPA_PLATFORM_PLUGIN_PATH=str(self.prefix/'plugins/platforms'),QT_SCALE_FACTOR='1',QT_IM_MODULE='compose')
            self.qt[actor]=self.spawn('qt-'+actor,[str(REPO/'build/qt/chat_qt'),self.url],env=env)
            wid=self.focus(actor); assert self.geometry(wid)==[460,600], 'Do not enlarge compact login'
            self.x.XMoveWindow(self.d,wid,offset,0); self.x.XFlush(self.d)
            wait(lambda:self.target(actor,{'name':'服务器设置'}),'actual gear accessible name')
            self.capture('compact-login-'+actor)
            self.click(actor,{'name':'用户名','role':'text'}); self.keys(actor,['ctrl+a']); self.paste(actor,self.manifest['actors'][actor]['username']); self.keys(actor,['Tab'])
            self.keys(actor,list(self.password)); self.keys(actor,['Return']); wait(lambda:self.target(actor,{'name':'会话列表'}),'Qt logged in')
            wait(lambda:self.geometry(wid)==[1180,760],'product-owned chat resize')
            self.original[actor]=identity(self.qt[actor].pid)
        for actor,x in [('B',0),('C',1200)]:
            self.tmux_owned=True
            command='exec '+shlex.quote(str(REPO/'build/chat_tui'))+' '+shlex.quote(self.url)
            pane=self.tmux('-f',str(self.w/'tmux.conf'),'new-session','-d','-P','-F','#{pane_id}','-s','daily'+actor,'-x','120','-y','40',command); self.panes[actor]=pane
            self.tmux_identity=identity(int(self.tmux('display-message','-p','-t',pane,'#{pid}'))); assert self.tmux_identity and self.tmux_identity['sha']==NATIVE['/usr/bin/tmux']
            self.tmux('pipe-pane','-o','-t',pane,'exec tee '+shlex.quote(str(self.w/(actor+'-output.raw')))+' >/dev/null')
            self.spawn('wezterm-'+actor,['/usr/bin/wezterm','--config-file',str(self.w/'wezterm.lua'),'start','--always-new-process','--class','ChatDaily'+actor,'--position',str(x)+',780','--','/usr/bin/tmux','-S',self.socket,'attach-session','-t','daily'+actor])
            wait(lambda:'"ChatDaily'+actor+'"' in self.run(['xwininfo','-root','-tree']).stdout,'actual WezTerm window')
            wait(lambda:'Login / Register' in self.pane(actor),'TUI login'); self.paste(actor,self.manifest['actors'][actor]['username']); self.keys(actor,['Tab']); self.paste(actor,self.password); self.keys(actor,['Enter'])
            wait(lambda:'connected' in self.pane(actor) and ('Chats' in self.pane(actor) or 'Conversations' in self.pane(actor)),'TUI logged in')
            self.original[actor]=identity(int(self.tmux('display-message','-p','-t',pane,'#{pane_pid}')))
        for actor,info in self.original.items():
            path='build/qt/chat_qt' if actor in self.qt else 'build/chat_tui'; assert info and info['exe']==str(REPO/path) and info['sha']==BINS[path]
        self.capture('login')
    def capture(self,label):
        self.check(); assert re.fullmatch('[A-Za-z0-9_-]+',label)
        from PIL import ImageGrab
        stem='%03d-%s'%(len(self.shots),label); image=self.w/(stem+'.png'); ImageGrab.grab(xdisplay=self.display).save(image)
        record={'label':label,'png_sha':sha(image),'original_clients':self.original}
        for actor,pane in self.panes.items():
            for ext,styled in [('txt',False),('ansi',True)]: (self.w/(stem+'-'+actor+'.'+ext)).write_text(self.pane(actor,styled))
            tty=self.tmux('display-message','-p','-t',pane,'#{pane_tty}')
            with open(tty,'rb',buffering=0) as stream: record[actor+'-tty']={'tty':tty,'rows_cols':list(struct.unpack('HHHH',fcntl.ioctl(stream.fileno(),termios.TIOCGWINSZ,b'\0'*8))[:2])}
            raw=self.w/(actor+'-output.raw')
            if raw.exists(): assert self.password.encode() not in raw.read_bytes(); record[actor+'-raw_sha']=sha(raw)
        for actor,p in self.qt.items():
            maps=(Path('/proc')/str(p.pid)/'maps').read_text(); (self.w/(stem+'-'+actor+'.maps')).write_text(maps)
            loaded={str(Path(line.split(maxsplit=5)[-1]).resolve()) for line in maps.splitlines() if len(line.split(maxsplit=5))==6 and line.split(maxsplit=5)[-1].startswith(str(self.prefix))}
            assert self.required_qt<=loaded,'Expected actual Qt SDK/plugin maps absent'
            for path in loaded: assert path in self.runtime and sha(path)==self.runtime[path],'Unpinned actual Qt loaded file: '+path
            self.save(stem+'-'+actor+'.tree.json',[i for _,i in self.nodes(actor)])
        self.shots.append(record); self.save('captures.json',self.shots); return record
    def operation(self,o):
        self.check(); op=o['op']; actor=o.get('actor'); result=None
        if op=='nodes': result=[i for _,i in self.nodes(actor)]
        elif op=='focus': result=self.focus(actor,o.get('title','Chat'))
        elif op=='click': result=self.click(actor,o['selector'],o.get('button',1))
        elif op=='keys': self.keys(actor,o['keys'])
        elif op=='paste': self.paste(actor,o['text'])
        elif op=='field':
            assert o['selector'].get('role')=='text'; self.click(actor,o['selector']); self.keys(actor,['ctrl+a']); self.paste(actor,o['text'])
        elif op=='tui-command':
            assert actor in self.panes; self.keys(actor,[':']); self.paste(actor,o['text']); wait(lambda:'Enter: run · Esc: cancel' in self.pane(actor),'command prompt'); self.keys(actor,['Enter']); wait(lambda:'Enter: run · Esc: cancel' not in self.pane(actor),'command processed')
        elif op=='wait-tui': result=wait(lambda:o['contains'] in self.pane(actor),'actual TUI text')
        elif op=='wait-qt': result=wait(lambda:self.target(actor,o['selector']),'actual Qt node')[1]
        elif op=='qt-text':
            node,info=wait(lambda:self.target(actor,o['selector']),'exact text node'); result=node.queryText().getText(0,-1); assert result==o['equals']
        elif op=='sql': result=wait(lambda:((v,) if (v:=self.sql(o['statement']))==o['equals'] else None),'read-only SQL exact value')[0] if 'equals' in o else self.sql(o['statement'])
        elif op=='files':
            a,b=[Path(p).resolve() for p in o['paths']]; assert a.parent==b.parent==self.w and a.read_bytes()==b.read_bytes(); result={'bytes':a.stat().st_size,'sha':sha(a)}
        elif op=='capture': result=self.capture(o['label'])
        elif op=='phase': assert o['name'] in PHASES; result=self.capture(o['name']); result['verdict']='CHECKPOINT_NOT_PASS'
        elif op=='restart-own-server':
            assert self.server.poll() is None; before=identity(self.server.pid); self.server.terminate(); self.server.wait(timeout=15); self.capture('offline'); self.start_server(); result={'before':before,'after':identity(self.server.pid)}
        elif op=='finish': self.capture('handoff'); self.completed=True; result={'status':'EXECUTOR_HANDOFF_NOT_PRODUCT_PASS'}
        else: raise ValueError('Unknown native input operation')
        self.events.append({'operation':o,'result':result}); self.save('operations.json',self.events); return result
    def close(self):
        if not getattr(self,'made',False): return
        errors=[]; descendants=[]
        try:
            table={int(a):int(b) for a,b in (l.split() for l in self.run(['ps','-e','-o','pid=,ppid=']).stdout.splitlines())}; tree={os.getpid()}
            while more:={p for p,parent in table.items() if parent in tree}-tree: tree.update(more)
            descendants=[i for pid in tree-{os.getpid()} if pid not in PROTECTED and (i:=identity(pid))]
        except Exception as error: errors.append(str(error))
        for actor in self.panes:
            try: self.keys(actor,['C-c'])
            except Exception as error: errors.append(str(error))
        if self.tmux_owned:
            if self.tmux_identity is None:
                try: self.tmux_identity=identity(int(self.tmux('display-message','-p','#{pid}')))
                except Exception: pass
            try: self.tmux('kill-server')
            except Exception as error: errors.append(str(error))
            if self.tmux_identity:
                try: wait(lambda:identity(self.tmux_identity['pid']) is None,'own tmux daemon cleanup')
                except Exception as error: errors.append(str(error))
        for name,p in reversed(self.owned):
            try:
                if p.poll() is None:
                    p.terminate()
                    try: p.wait(timeout=15)
                    except S.TimeoutExpired: p.kill(); p.wait(timeout=15)
            except Exception as error: errors.append(name+': '+str(error))
        for info in descendants:
            if identity(info['pid'])!=info: continue
            try:
                os.kill(info['pid'],signal.SIGTERM)
                try: wait(lambda:identity(info['pid']) is None,'owned descendant cleanup')
                except AssertionError:
                    if identity(info['pid'])==info: os.kill(info['pid'],signal.SIGKILL); wait(lambda:identity(info['pid']) is None,'owned descendant killed')
            except ProcessLookupError: pass
            except Exception as error: errors.append(str(error))
        exists=None
        if self.db_attempt:
            env=self.env.copy(); env.update(PGDATABASE=self.maintenance,PGOPTIONS='-c default_transaction_read_only=on')
            try: exists=self.run(['psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-c',"SELECT count(*) FROM pg_database WHERE datname='"+self.database+"'"],env=env).stdout.strip()=='1'
            except Exception as error: errors.append(str(error))
        self.save('cleanup.json',{'status':'HANDOFF_NOT_PASS' if self.completed else 'INCOMPLETE','owned':[{'name':n,'pid':p.pid,'exit':p.returncode,'gone':identity(p.pid) is None} for n,p in self.owned],
            'activation_descendants':[{'identity':i,'gone':identity(i['pid']) is None} for i in descendants],
            'tmux_server':{'identity':self.tmux_identity,'gone':identity(self.tmux_identity['pid']) is None if self.tmux_identity else None},
            'database_retained':self.database if self.db_attempt and exists is not False else None,'database_present':exists,
            'protected_unchanged':self.protected=={str(pid):identity(pid) for pid in PROTECTED},'errors':errors})
def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--execute',action='store_true'); p.add_argument('--reviewed-sha'); p.add_argument('--scope'); p.add_argument('--port',type=int,default=18961); args=p.parse_args()
    if not args.execute: print(json.dumps({'status':'PREPARED_NOT_EXECUTED','actors':{'A':'Qt','D':'Qt','B':'TUI','C':'TUI'},'phases':PHASES,'head':HEAD,'current69':False})); return
    assert args.scope and args.reviewed_sha; d=Daily.__new__(Daily)
    try:
        d.__init__(args); d.setup(); d.login()
        print(json.dumps({'status':'FOUR_CLIENTS_READY_NOT_DAILY_PASS','scope':str(d.w),'database':d.database,'actors':d.manifest['actors']},ensure_ascii=False),flush=True)
        for line in sys.stdin:
            try:
                result=d.operation(json.loads(line)); print(json.dumps({'ok':True,'result':result},ensure_ascii=False),flush=True)
                if d.completed: break
            except Exception as error:
                print(json.dumps({'ok':False,'status':'INCOMPLETE','error':str(error).replace(d.password,'[REDACTED]')},ensure_ascii=False),flush=True)
                d.capture('operation-failure')
    finally: d.close()
if __name__=='__main__': main()
