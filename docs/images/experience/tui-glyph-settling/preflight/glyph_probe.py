#!/usr/bin/env python3
"""Single TUI settling observations using the archived Daily native framework.
No Qt client, role mutation, font change, forced redraw or sanitizer execution.
"""
import argparse
import importlib.util
import json
import os
import re
import secrets
import selectors
import shlex
import signal
import socket
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
BASE_SHA = 'b020b77cdae3e88a1124c8f473ba427041f9a3ace85a4b1149a614e639796ddf'
SOURCE_SHA = '65e5443755a5444399fc4e5489066d80e9fb82670ce173e342e92bf5073c8f85'
import hashlib
sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
assert sha(HERE / 'daily-base.py') == BASE_SHA
spec = importlib.util.spec_from_file_location('archived_daily', HERE / 'daily-base.py')
daily = importlib.util.module_from_spec(spec)
spec.loader.exec_module(daily)  # Only definitions; archived __main__ is not executed.
daily.HEAD = '7375815f70e3d9d6f456c3d9a0d173411800f294'
daily.BINS['build/qt/chat_qt'] = '370a68ae9448916c86e3085f1a459e9b1ea0f2d292cf18113c842de96a1b24a4'


class Glyph(daily.Daily):
    def __init__(self, args):
        # Same framework state, with current inputs and no historical gate claim.
        self.args=args; self.made=False; self.owned=[]; self.qt={}; self.panes={}; self.original={}
        self.server=None; self.helper=None; self.db_attempt=False; self.db_created=False
        self.tmux_owned=False; self.tmux_identity=None; self.events=[]; self.shots=[]
        self.completed=False; self.protected={}; self.buffers={}
        self.w=Path(args.scope).resolve(); self.socket=str(self.w/'tmux.sock')
        assert self.w.parent==Path('/tmp') and self.w.name.startswith('chat-tui-glyph-run-') and not self.w.exists()
        assert args.reviewed_sha==sha(__file__), 'Review exact current driver before executing'
        assert sha(HERE/'source206.json')==SOURCE_SHA
        self.sources=json.loads((HERE/'source206.json').read_text())
        assert self.sources['head']==daily.HEAD and self.sources['count']==len(self.sources['inputs'])==206
        self.env=os.environ.copy()
        for key in ('LD_LIBRARY_PATH','LD_PRELOAD','QT_PLUGIN_PATH','QT_QPA_PLATFORM_PLUGIN_PATH'):
            assert not self.env.get(key), 'Clean system loader required: '+key
            self.env.pop(key,None)
        for key in ('DISPLAY','WAYLAND_DISPLAY','TMUX','DBUS_SESSION_BUS_ADDRESS','DBUS_STARTER_ADDRESS',
                    'DBUS_STARTER_BUS_TYPE','AT_SPI_BUS_ADDRESS','IBUS_ADDRESS','IBUS_ADDRESS_FILE',
                    'QT_IM_MODULE','QT_SCALE_FACTOR','I3SOCK','XAUTHORITY'):
            self.env.pop(key,None); os.environ.pop(key,None)
        self.env.update(GTK_IM_MODULE='none',XMODIFIERS='@im=none')
        os.environ.update(GTK_IM_MODULE='none',XMODIFIERS='@im=none')
        assert all(self.env.get(key) for key in ('PGHOST','PGPORT','PGUSER')), 'Explicit PG environment required'
        self.maintenance=self.env.get('PGDATABASE','postgres'); self.database='chat_daily_'+secrets.token_hex(16)
        self.env['PGDATABASE']=self.database; self.password=secrets.token_hex(16)
        self.url='ws://127.0.0.1:'+str(args.port)+'/ws'
        assert sha(daily.GATE)==daily.GATE_SHA, 'Historical runtime-pin archive changed'
        gate=json.loads(daily.GATE.read_text())
        self.runtime=daily.pins(gate['private_qt_runtime_pins']); self.runtime.update(daily.IMAGE_FORMATS)
        historic=json.loads(daily.gzip.decompress(daily.QT_PROOF.read_bytes()))
        assert sha(daily.QT_PROOF)==daily.QT_PROOF_SHA
        for value in historic['loaded_qt']:
            path=str(Path(value['path']).resolve())
            assert path not in self.runtime or self.runtime[path]==value['sha256']
            self.runtime[path]=value['sha256']
        self.prefix=Path(gate['private_qt_prefix']); self.required_qt=set()
        self.check()
        self.protected={str(pid):daily.identity(pid) for pid in daily.PROTECTED}
        assert all(self.protected[str(pid)] and self.protected[str(pid)]['ticks']==tick for pid,tick in daily.PROTECTED.items())
        self.w.mkdir(mode=0o700); self.made=True
        self.save('freeze.json', {'head':daily.HEAD, 'source206':self.sources, 'source_sha256':SOURCE_SHA,
                  'driver_sha256':sha(__file__), 'adapted_base_sha256':BASE_SHA,
                  'archived_base_before_adaptation_sha256':'a3830c73a5441b72bf1702b3611415f843466f55b56abd2c21554591bde0cc48',
                  'bins':daily.BINS, 'native':daily.NATIVE, 'runtime_pins':self.runtime,
                  'protected':self.protected, 'no_current_gate_claim':True,
                  'scope':'One formal TUI B; archived seed/setup/paste/capture/process cleanup framework.'})

    def check(self):
        assert self.run(['git','-C',str(daily.REPO),'rev-parse','HEAD']).stdout.strip()==daily.HEAD
        assert not self.run(['git','-C',str(daily.REPO),'status','--porcelain']).stdout.strip(), 'Repository changed'
        assert sha(HERE/'source206.json')==SOURCE_SHA and sha(HERE/'daily-base.py')==BASE_SHA
        for value in self.sources['inputs']:
            path=Path(value['path']); assert sha(path if path.is_absolute() else daily.REPO/path)==value['sha256'],value['path']
        for path,digest in {**{str(daily.REPO/path):digest for path,digest in daily.BINS.items()},
                            **daily.NATIVE, **self.runtime}.items():
            assert sha(path)==digest,path
        if self.protected:
            assert self.protected=={str(pid):daily.identity(pid) for pid in daily.PROTECTED}
        for actor,original in self.original.items():
            assert daily.identity(original['pid'])==original,actor+' actual PID changed'

    def run(self, command, **kwargs):
        result=super().run(command,**kwargs)
        if command[0]=='createdb': self.db_created=True
        return result

    def line(self, process, label):
        # The sole adapted setup primitive: bounded raw fd line, no buffered readline.
        fd=process.stdout.fileno(); data=self.buffers.pop(fd,b''); end=time.monotonic()+15
        with selectors.DefaultSelector() as selector:
            selector.register(fd,selectors.EVENT_READ)
            while b'\n' not in data:
                remaining=end-time.monotonic()
                assert remaining>0 and selector.select(remaining),label+' timeout'
                chunk=os.read(fd,65536); assert chunk,label+' EOF'; data+=chunk
        first,rest=data.split(b'\n',1); self.buffers[fd]=rest
        return first.decode().strip()

    def login(self):
        assert self.helper.poll()==0 and not self.qt and not self.panes
        actor='B'; self.tmux_owned=True
        command='exec '+shlex.quote(str(daily.REPO/'build/chat_tui'))+' '+shlex.quote(self.url)
        pane=self.tmux('-f',str(self.w/'tmux.conf'),'new-session','-d','-P','-F','#{pane_id}',
                       '-s','glyphB','-x','120','-y','40',command); self.panes[actor]=pane
        self.tmux_identity=daily.identity(int(self.tmux('display-message','-p','-t',pane,'#{pid}')))
        assert self.tmux_identity and self.tmux_identity['sha']==daily.NATIVE['/usr/bin/tmux']
        self.tmux('pipe-pane','-o','-t',pane,'exec tee '+shlex.quote(str(self.w/'B-output.raw'))+' >/dev/null')
        self.wez=self.spawn('wezterm-B',['/usr/bin/wezterm','--config-file',str(self.w/'wezterm.lua'),
            'start','--always-new-process','--class','ChatGlyphB','--position','0,0','--',
            '/usr/bin/tmux','-S',self.socket,'attach-session','-t','glyphB'])
        daily.wait(lambda:'"ChatGlyphB"' in self.run(['xwininfo','-root','-tree']).stdout,'owned native WezTerm window')
        daily.wait(lambda:'Login / Register' in self.pane(actor),'actual TUI login')
        self.paste(actor,self.manifest['actors'][actor]['username']); self.keys(actor,['Tab'])
        self.paste(actor,self.password); self.keys(actor,['Enter'])
        daily.wait(lambda:'connected' in self.pane(actor) and 'Chats' in self.pane(actor),'actual TUI authenticated')
        self.ready=time.monotonic()
        self.original[actor]=daily.identity(int(self.tmux('display-message','-p','-t',pane,'#{pane_pid}')))
        assert self.original[actor] and self.original[actor]['exe']==str(daily.REPO/'build/chat_tui')
        assert self.original[actor]['sha']==daily.BINS['build/chat_tui']
        self.save('native-identities.json', {'tui':self.original[actor], 'tmux':self.tmux_identity,
                  'wezterm':daily.identity(self.wez.pid), 'sdk_pid':self.helper.pid,'sdk_exit':self.helper.returncode,
                  'server':daily.identity(self.server.pid),'authenticated_monotonic':self.ready})

    def sample(self,label,nominal=None):
        before=time.monotonic()
        result=super().capture(label)
        result.update(monotonic_before_capture=before,monotonic_after_capture=time.monotonic(),
                      seconds_after_authenticated=before-self.ready,nominal_settling_seconds=nominal,
                      note='Capture and terminal inspection only; no TUI input, resize, font change or forced redraw.')
        assert result['B-tty']['rows_cols']==[40,120]
        self.save('captures.json',self.shots)

    def close(self):
        if not self.made:return
        errors=[]
        if getattr(self,'d',None):
            self.x.XCloseDisplay.argtypes=[daily.C.c_void_p]
            self.x.XCloseDisplay.restype=daily.C.c_int
            self.x.XCloseDisplay(self.d);self.d=None
        try:
            super().close()  # Original owned tmux/process/activation cleanup; database initially retained.
        except Exception as error:
            errors.append('base cleanup exception: '+str(error))
        absent=not self.db_created
        if self.db_created:
            assert re.fullmatch(r'chat_daily_[0-9a-f]{32}',self.database)
            try:
                self.run(['dropdb','--maintenance-db='+self.maintenance,self.database])
                env=self.env.copy(); env.update(PGDATABASE=self.maintenance,PGOPTIONS='-c default_transaction_read_only=on')
                absent=self.run(['psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-c',
                    "SELECT count(*) FROM pg_database WHERE datname='"+self.database+"'"],env=env).stdout.strip()=='0'
            except Exception as error:errors.append('database: '+str(error))
        with socket.socket() as probe:port_closed=probe.connect_ex(('127.0.0.1',self.args.port))!=0
        gone=all(daily.identity(proc.pid) is None for name,proc in self.owned)
        clients_gone=all(daily.identity(value['pid']) is None for value in self.original.values())
        tmux_gone=self.tmux_identity is None or daily.identity(self.tmux_identity['pid']) is None
        protected_same=self.protected=={str(pid):daily.identity(pid) for pid in daily.PROTECTED}
        source_same=True
        try:
            assert self.run(['git','-C',str(daily.REPO),'rev-parse','HEAD']).stdout.strip()==daily.HEAD
            for value in self.sources['inputs']:
                path=Path(value['path']);assert sha(path if path.is_absolute() else daily.REPO/path)==value['sha256']
        except Exception as error:source_same=False;errors.append('source: '+str(error))
        base_path=self.w/'cleanup.json'; base=json.loads(base_path.read_text()) if base_path.exists() else {}
        descendants_gone=base_path.exists() and all(value['gone'] for value in base.get('activation_descendants',[]))
        self.cleanup_ok=all([absent,port_closed,gone,clients_gone,tmux_gone,protected_same,source_same,descendants_gone]) and not errors
        self.save('final-cleanup.json', {'database':self.database,'database_created':self.db_created,
                  'database_absent':absent,'private_port_closed':port_closed,'all_owned_gone':gone,
                  'tui_gone':clients_gone,'tmux_gone':tmux_gone,'activation_descendants_gone':descendants_gone,
                  'protected_unchanged':protected_same,'source_unchanged':source_same,
                  'errors':errors,'base_cleanup_errors_preserved':base.get('errors',[]),'all_checks':self.cleanup_ok})
        self.save('artifact-sha256.json', {path.name:sha(path) for path in sorted(self.w.iterdir())
                  if path.is_file() and path.name!='artifact-sha256.json'})


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--execute',action='store_true');parser.add_argument('--reviewed-sha')
    parser.add_argument('--scope');parser.add_argument('--port',type=int,required=True)
    args=parser.parse_args()
    if not args.execute:
        print(json.dumps({'status':'PREPARED_NOT_EXECUTED','head':daily.HEAD,'driver_sha256':sha(__file__)}));return
    assert args.scope and args.reviewed_sha
    def stop(signum,frame):
        signal.signal(signal.SIGTERM,signal.SIG_IGN);signal.signal(signal.SIGINT,signal.SIG_IGN)
        raise SystemExit(128+signum)
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    probe=Glyph.__new__(Glyph);status=1
    try:
        probe.__init__(args);probe.setup();probe.login()
        for delay in [0,.25,1,3,5]:
            remaining=probe.ready+delay-time.monotonic()
            if remaining>0:time.sleep(remaining)
            probe.sample('unchanged-'+str(delay).replace('.','p'),delay)
        probe.keys('B',['c']);daily.wait(lambda:'Contacts' in probe.pane('B'),'ordinary Contacts navigation')
        probe.sample('normal-c')
        probe.keys('B',['h']);daily.wait(lambda:'Chats' in probe.pane('B'),'ordinary Chats return')
        probe.sample('normal-h')
        probe.save('actual-observation.json',{'execution_completed':True,'product_pass':False,
            'scope':'5 untouched-page settling samples, then normal c/h; no font change/forced redraw.',
            'font_match':probe.run(['fc-match','Lily Han Sans HW SC']).stdout.strip(),
            'needs_human_original_png_review':True})
        probe.completed=True;status=0
    except BaseException as error:
        if getattr(probe,'made',False):
            probe.save('failure.json',{'type':type(error).__name__,'message':str(error).replace(probe.password,'[REDACTED]')})
        else:print('Preflight failed: '+type(error).__name__+': '+str(error).replace(getattr(probe,'password','__no_password__'),'[REDACTED]'),file=sys.stderr)
    finally:
        if getattr(probe,'made',False):probe.close()
    if getattr(probe,'made',False):
        if not getattr(probe,'cleanup_ok',False):status=1
        (probe.w/'driver.exit').write_text(str(status)+'\n')
    print(json.dumps({'status':'OBSERVATIONS_COLLECTED_NOT_PRODUCT_PASS' if status==0 else 'INCOMPLETE',
          'scope':args.scope,'exit':status}),flush=True)
    raise SystemExit(status)

if __name__=='__main__':main()
