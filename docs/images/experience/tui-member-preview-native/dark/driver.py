import os,sys,json,time,subprocess as S,socket,secrets,selectors,hashlib,ctypes as C,re,fcntl,termios,struct,traceback
from pathlib import Path
from PIL import ImageGrab,Image
sys.dont_write_bytecode=True
repo=Path('/home/gyl/example/chat');sys.path.insert(0,str(repo/'tests'))
from tui_scale_observer import process_sample,read_only_sql
w=Path(sys.argv[1]);mode=sys.argv[2];assert w.parent == Path('/tmp/chat-member-preview-native.UivDD0NU') and w.name in ('dark','light') and mode=='mux'
light=w.name=='light'
build=w.parent/'bin';owned=[];app=None;tmux_pid=None;db_created=False
candidate=w.parent/'chat_tui.after'
maintenance=os.environ.get('PGDATABASE','postgres');database='chat_t04_candidate_'+mode+'_'+time.strftime('%m%d%H%M%S')+'_'+str(os.getpid());password=secrets.token_hex(16)
result={'scope':str(w),'mode':mode,'driver_process':process_sample(os.getpid()),'driver_exit':1,'captures':[],'actions':[],'protected_before':{str(p):process_sample(p) for p in (2876288,3225,29935)}}
def save():(w/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2))
def wait(test,desc):
 end=time.monotonic()+15
 while time.monotonic()<end:
  value=test()
  if value:return value
  time.sleep(.08)
 raise AssertionError('15s '+desc)
def spawn(name,command,**kw):
 p=S.Popen(command,stdout=(w/(name+'.log')).open('wb'),stderr=S.STDOUT,**kw);owned.append((name,p));result.setdefault('processes',{})[name]=process_sample(p.pid);save();return p
def raw():return (w/'session.raw').read_bytes() if (w/'session.raw').exists() else b''
def wait_raw(token,offset=0):return wait(lambda:token.encode() in raw()[offset:],token)
def rows():return read_only_sql('SELECT id,conversation_id,sender_id,body FROM messages ORDER BY id')
def source_hash():return {str(p.relative_to(repo)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [repo/'tui/cmake/ftxui-7.0.3-grapheme.patch',repo/'tui/tests/render_test.cpp',*sorted((repo/'tui/src').glob('*'))] if p.is_file()}
try:
 result['head']=S.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip();result['tui_source_before']=source_hash()
 result['binary_sha']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (build/'chat_server',build/'chat_tui_scale_fixture')};result['binary_sha']['chat_tui']=hashlib.sha256(candidate.read_bytes()).hexdigest()
 manifest=json.loads((w.parent/'build-manifest.json').read_text())
 assert result['binary_sha']['chat_tui']==manifest['candidate_binary_sha256']
 assert result['tui_source_before']==manifest['tui_source_sha256']
 result['candidate_binary']={'path':str(candidate),'sha256':hashlib.sha256(candidate.read_bytes()).hexdigest()}
 assert result['candidate_binary']['sha256']==manifest['candidate_binary_sha256']
 result['candidate_manifest_sha256']=hashlib.sha256((w.parent/'build-manifest.json').read_bytes()).hexdigest()
 result['wezterm_version']=S.check_output(['wezterm','--version'],text=True).strip();result['tmux_version']=S.check_output(['tmux','-V'],text=True).strip()
 with socket.socket() as port_probe:port_probe.bind(('127.0.0.1',0));port=port_probe.getsockname()[1]
 result.update(database=database,port=port)
 S.run(['createdb','--maintenance-db='+maintenance,'--template=template0',database],check=True,stdout=S.DEVNULL,stderr=(w/'createdb.log').open('wb'));db_created=True;os.environ['PGDATABASE']=database
 with (w/'migration.log').open('wb') as log:
  for sql in sorted((repo/'sql').glob('[0-9][0-9][0-9]_*.sql')):S.run(['psql','-X','-v','ON_ERROR_STOP=1','-f',str(sql)],check=True,stdout=log,stderr=log)
 server=spawn('server',[str(build/'chat_server'),str(port),'256','16'])
 import urllib.request
 def health():
  if server.poll() is not None:raise RuntimeError('Own server exited')
  try:return urllib.request.urlopen('http://127.0.0.1:'+str(port)+'/health',timeout=.5).status==200
  except OSError:return False
 wait(health,'own server')
 helper=S.Popen([str(build/'chat_tui_scale_fixture')],stdin=S.PIPE,stdout=S.PIPE,stderr=(w/'sdk-stderr.log').open('wb'),text=True,bufsize=1);owned.append(('helper',helper));seq=0
 def control(command,**payload):
  global seq
  seq+=1;helper.stdin.write(json.dumps({'id':seq,'command':command,**payload},ensure_ascii=False)+'\n');helper.stdin.flush()
  with selectors.DefaultSelector() as select:select.register(helper.stdout,selectors.EVENT_READ);assert select.select(15),'15s fixture';answer=json.loads(helper.stdout.readline())
  assert answer['ok'],'SDK fixture failed';return answer['result']
 prefix='u14_'+time.strftime('%H%M%S')+'_'+str(os.getpid());url='ws://127.0.0.1:'+str(port)+'/ws'
 control('configure',url=url,prefix=prefix,password=password);manifest=control('seed_navigation');username=manifest['actors']['A']['username'];group=manifest['group']['id'];control('connect',actors=['A','B'])
 title='HDR L👩\u200d💻R ❤️ 1️⃣ 👩🏽 END';answer=control('sdk',actor='A',method='rename_group',conversation=group,title=title);assert answer['ok']
 cases=['LABR END','L中\u0301R END','L👩\u200d💻R END','L1️⃣R END','L❤️R END','L👩🏽R END']
 for body in cases:assert control('sdk',actor='B',method='send_message',conversation=group,text=body)['ok']
 authoritative=control('sdk',actor='A',method='get_members',conversation=group);assert authoritative['ok'];expected_members=authoritative['value'];assert len(expected_members)==4;result['authoritative_sdk_members']=expected_members
 def membership_sql():return read_only_sql('SELECT cm.conversation_id,cm.user_id,u.username,cm.is_admin,c.owner_id FROM conversation_members cm JOIN users u ON u.id=cm.user_id JOIN conversations c ON c.id=cm.conversation_id WHERE cm.conversation_id='+str(int(group))+' ORDER BY cm.user_id')
 control('close');helper.stdin.close();result['helper_exit']=helper.wait(timeout=15);assert result['helper_exit']==0;result['prelogin_seed']={'title':title,'cases':cases,'sql':rows(),'sdk_closed_before_login':True};save()
 result['private_membership_sql_before']=membership_sql();assert len(result['private_membership_sql_before'])==4
 for name in ('runtime','config'):(w/name).mkdir(mode=0o700)
 xvf=S.Popen(['Xvfb','-displayfd','1','-screen','0','1900x900x24','-nolisten','tcp'],stdout=S.PIPE,stderr=(w/'xvfb.log').open('wb'),text=True);owned.append(('xvfb',xvf))
 with selectors.DefaultSelector() as select:select.register(xvf.stdout,selectors.EVENT_READ);assert select.select(15),'15s Xvfb';display=':'+xvf.stdout.readline().strip()
 env=os.environ.copy();env.update(DISPLAY=display,XDG_RUNTIME_DIR=str(w/'runtime'),XDG_CONFIG_HOME=str(w/'config'),GTK_IM_MODULE='none',QT_IM_MODULE='none',XMODIFIERS='@im=none');env.pop('WAYLAND_DISPLAY',None);env.pop('TMUX',None)
 wez=spawn('wezterm',['wezterm','--config-file',str(w/'wezterm.lua'),'start','--always-new-process','--class','ChatU14_'+prefix,'--position','0,0','--cwd',str(w),'--','python3','-B',str(w/'host.py'),str(w),mode,'outer',str(candidate),url],env=env)
 x=C.CDLL('libX11.so.6');xt=C.CDLL('libXtst.so.6');x.XOpenDisplay.argtypes=[C.c_char_p];x.XOpenDisplay.restype=C.c_void_p;d=x.XOpenDisplay(display.encode());assert d
 for name,args,ret in [('XSetInputFocus',[C.c_void_p,C.c_ulong,C.c_int,C.c_ulong],C.c_int),('XFlush',[C.c_void_p],C.c_int),('XStringToKeysym',[C.c_char_p],C.c_ulong),('XKeysymToKeycode',[C.c_void_p,C.c_ulong],C.c_uint),('XMoveResizeWindow',[C.c_void_p,C.c_ulong,C.c_int,C.c_int,C.c_uint,C.c_uint],C.c_int)]:f=getattr(x,name);f.argtypes=args;f.restype=ret
 xt.XTestFakeKeyEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
 xt.XTestFakeMotionEvent.argtypes=[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong];xt.XTestFakeButtonEvent.argtypes=[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]
 def window():
  tree=S.check_output(['xwininfo','-root','-tree'],env=env,text=True);found=re.search(r'(0x[0-9a-f]+)\s+.*\("ChatU14_'+re.escape(prefix)+r'"',tree);return int(found.group(1),16) if found else None
 wid=wait(window,'own Wez window');x.XSetInputFocus(d,wid,2,0);x.XFlush(d)
 def key(name,ctrl=False,shift=False):
  mods=(['Control_L'] if ctrl else [])+(['Shift_L'] if shift else [])
  for mod in mods:xt.XTestFakeKeyEvent(d,x.XKeysymToKeycode(d,x.XStringToKeysym(mod.encode())),1,0)
  code=x.XKeysymToKeycode(d,x.XStringToKeysym(name.encode()));assert code;xt.XTestFakeKeyEvent(d,code,1,0);xt.XTestFakeKeyEvent(d,code,0,0)
  for mod in reversed(mods):xt.XTestFakeKeyEvent(d,x.XKeysymToKeycode(d,x.XStringToKeysym(mod.encode())),0,0)
  x.XFlush(d);time.sleep(.13)
 def paste(text):
  p=S.Popen(['xclip','-selection','clipboard','-quiet'],stdin=S.PIPE,stdout=S.DEVNULL,stderr=(w/'clipboard.log').open('ab'),env=env);owned.append(('clipboard',p));p.stdin.write(text.encode());p.stdin.close();time.sleep(.1);key('v',ctrl=True,shift=True)
 def size():
  if not app:return None
  try:
   with open(app['tty'],'rb',buffering=0) as terminal:return list(struct.unpack('HHHH',fcntl.ioctl(terminal.fileno(),termios.TIOCGWINSZ,b'\0'*8)))[:2]
  except OSError:return None
 def capture(label):
  time.sleep(.25);p=w/(label+'.png');ImageGrab.grab(xdisplay=display).save(p)
  live=app is not None and process_sample(app['pid'])['alive']
  plain=S.check_output(['tmux','-S',str(w/'tmux.sock'),'capture-pane','-p'],text=True) if live else ''
  styled=S.check_output(['tmux','-S',str(w/'tmux.sock'),'capture-pane','-p','-e'],text=True) if live else ''
  (w/(label+'.txt')).write_text(plain);(w/(label+'.ansi')).write_text(styled)
  geometry=S.check_output(['xwininfo','-id',hex(wid)],env=env,text=True);(w/(label+'.window.txt')).write_text(geometry)
  result['captures'].append({'file':p.name,'sha':hashlib.sha256(p.read_bytes()).hexdigest(),'raw_offset':len(raw()),'tty_rows_cols':size(),'plain_sha':hashlib.sha256(plain.encode()).hexdigest(),'styled_sha':hashlib.sha256(styled.encode()).hexdigest(),'full_image_pixels':list(Image.open(p).size),'window_geometry_file':label+'.window.txt'});save()
 def wait_copyable(offset):
  if mode=='direct':return wait_raw('Copyable text',offset)
  # tmux updates individual ASCII cells, so contiguous raw title bytes are not
  # a valid page oracle. Compare the already-captured real product ASCII title.
  return wait(lambda:'Copyable text' in S.check_output(['tmux','-S',str(w/'tmux.sock'),'capture-pane','-p'],text=True),'actual tmux Copyable page')
 wait(lambda:(w/'outer9.ready').exists(),'outer9 ready');capture('00-outer9-oracle');key('Return')
 if mode=='mux':wait(lambda:(w/'mux9-off.ready').exists(),'mux9-off ready');capture('01-mux9-off-oracle');key('Return')
 wait(lambda:(w/'app.json').exists(),'app metadata');app=json.loads((w/'app.json').read_text());tmux_pid=app.get('tmux_server_pid');result['inside']=app;result['outer']=json.loads((w/'outer.json').read_text());wait_raw('Username')
 executable=Path('/proc')/str(app['pid'])/'exe';assert executable.resolve()==candidate and hashlib.sha256(executable.read_bytes()).hexdigest()==result['candidate_binary']['sha256']
 result['actual_live_binary']={'pid':app['pid'],'start_ticks':process_sample(app['pid'])['start_ticks'],'path':str(executable.resolve()),'sha256':hashlib.sha256(executable.read_bytes()).hexdigest()}
 assert size()==[30,80];capture('02-login80')
 paste(username);key('Tab');paste(password);key('Return');wait_raw('connected');capture('03-chats80')
 oracle=Image.open(w/'00-outer9-oracle.png').convert('RGB');occupied=[yy for yy in range(65) if any((max(oracle.getpixel((xx,yy)))<128 if light else min(oracle.getpixel((xx,yy)))>128) for xx in range(9))];starts=[yy for i,yy in enumerate(occupied) if i==0 or yy!=occupied[i-1]+1];row_pitch=starts[2]-starts[1]
 result['row_pitch']=row_pitch;assert row_pitch==19
 def command(value):
  key('semicolon',shift=True);paste(value);key('Return')
 def pane():return S.check_output(['tmux','-S',str(w/'tmux.sock'),'capture-pane','-p'],text=True)
 def resize(cols,rows):
  x.XMoveResizeWindow(d,wid,0,0,cols*10,rows*row_pitch);x.XFlush(d);wait(lambda:size()==[rows,cols],'actual resize '+str((cols,rows)))
 key('Return');wait(lambda:'members' in pane() and 'END' in pane(),'actual selected seeded conversation');capture('04-selected-conversation80')
 result['focused_cases']=[]
 for height in (24,40):
  for width in (60,120,160):
   resize(width,height);prefix_label='preview-'+str(width)+'x'+str(height)
   command('group');wait(lambda:'Preview (3 of 4):' in pane() and 'Members: 4' in pane() and 'All members' in pane(),'actual Group subset label');capture(prefix_label+'-group');group_text=pane()
   evidence={'width':width,'height':height,'actual_rows_cols':size(),'preview_label_visible':'Preview (3 of 4):' in group_text,'total_members4_visible':'Members: 4' in group_text,'owner_role_visible':'Your role: owner' in group_text,'all_members_action_visible':'All members' in group_text,'preview_line':next(line for line in group_text.splitlines() if 'Preview (3 of 4):' in line),'action_method':'Real Return on selected All members, then real Escape to parent Group','product_observation_only':True}
   key('Return');wait(lambda:'Members (4)' in pane() and all(m['username'] in pane() for m in expected_members),'actual full4 Members reached by Return');capture(prefix_label+'-members');members_text=pane()
   evidence['actual_all_members_enter']=True;evidence['full_members_header_visible']='Members (4)' in members_text;evidence['full_members_identities_roles']=[{'id':m['id'],'username':m['username'],'role':m['role'],'same_row_identity_and_role_visible':any(m['username'] in line and m['role'] in line for line in members_text.splitlines())} for m in expected_members];evidence['no_preview_label_on_full_members']='Preview (' not in members_text
   key('Escape');wait(lambda:'Preview (3 of 4):' in pane() and 'Group ·' in pane(),'actual Esc returns Group');capture(prefix_label+'-esc-group');evidence['actual_esc_return_to_group']=True;result['focused_cases'].append(evidence);save()
 result['focused_preview_contract_met']=all(c['preview_label_visible'] and c['total_members4_visible'] and c['owner_role_visible'] and c['all_members_action_visible'] and c['actual_all_members_enter'] and c['full_members_header_visible'] and all(m['same_row_identity_and_role_visible'] for m in c['full_members_identities_roles']) and c['no_preview_label_on_full_members'] and c['actual_esc_return_to_group'] for c in result['focused_cases'])
 result['private_membership_sql_after']=membership_sql();result['private_membership_unchanged']=result['private_membership_sql_before']==result['private_membership_sql_after'];assert result['private_membership_unchanged']
 result['sql_after']=rows();result['raw_contains_password']=password.encode() in raw();assert not result['raw_contains_password'];key('c',ctrl=True)
 wait(lambda:(w/'child-exit.json').exists(),'native child exit');result['native_actual_exit']=json.loads((w/'child-exit.json').read_text());wait(lambda:(w/'outer9-after-test.ready').exists(),'outer9 after test ready');capture('16-after-test9-oracle');key('Return');result['wezterm_exit']=wez.wait(timeout=15);result['driver_exit']=0
except BaseException as error:
 result['error_type']=type(error).__name__;result['error']=str(error).replace(password,'[REDACTED]');(w/'driver-trace.txt').write_text(traceback.format_exc().replace(password,'[REDACTED]'))
finally:
 if app and process_sample(app['pid']).get('alive') and process_sample(app['pid']).get('start_ticks')==result.get('actual_live_binary',{}).get('start_ticks'):
  try:os.kill(app['pid'],15);wait(lambda:not process_sample(app['pid'])['alive'],'own app cleanup')
  except Exception:result['app_cleanup_error']=True
 if mode=='mux':S.run(['tmux','-S',str(w/'tmux.sock'),'kill-server'],stdout=S.DEVNULL,stderr=S.DEVNULL)
 exits={}
 for name,p in reversed(owned):
  if p.poll() is None:
   p.terminate()
   try:p.wait(timeout=15)
   except S.TimeoutExpired:p.kill();p.wait(timeout=15)
  exits.setdefault(name,[]).append({'pid':p.pid,'actual_exit':p.returncode,'gone':not process_sample(p.pid)['alive']})
 result['cleanup_exits']=exits
 if app:result['app_gone']=not process_sample(app['pid'])['alive']
 if tmux_pid:result['tmux_gone']=not process_sample(tmux_pid)['alive']
 if db_created:
  os.environ['PGDATABASE']=maintenance;p=S.run(['dropdb','--maintenance-db='+maintenance,database],stdout=S.DEVNULL,stderr=(w/'dropdb.log').open('wb'));result['dropdb_exit']=p.returncode;result['database_absent']=read_only_sql("SELECT count(*) FROM pg_database WHERE datname='"+database+"'")==[['0']]
 result['protected_after']={str(p):process_sample(p) for p in (2876288,3225,29935)};result['protected_unchanged']=all(result['protected_before'][str(p)].get('start_ticks')==result['protected_after'][str(p)].get('start_ticks') and result['protected_after'][str(p)]['alive'] for p in (2876288,3225,29935));result['tui_source_after']=source_hash();result['tui_source_unchanged']=result['tui_source_before']==result['tui_source_after'];save()
 if 'port' in result:
  with socket.socket() as probe:probe.settimeout(1);result['own_port_closed']=probe.connect_ex(('127.0.0.1',result['port']))!=0
 save()
print(json.dumps({key:result.get(key) for key in ['scope','mode','driver_exit','error','database_absent','protected_unchanged','tui_source_unchanged']},ensure_ascii=False));sys.exit(result['driver_exit'])
