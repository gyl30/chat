import os,sys,json,subprocess as S,shlex,time
from pathlib import Path
sys.dont_write_bytecode=True
w=Path(sys.argv[1]);mode=sys.argv[2];stage=sys.argv[3];binary=sys.argv[4];url=sys.argv[5]
rows=['0123456789'*8,'LABR END','L👩\u200d💻R END','L1\ufe0f\u20e3R END','L\u2764\ufe0fR END','L👩🏽R END','L👩\u200d💻�R END','L中\u0301R END']
def oracle(name):
 print('\x1b[2J\x1b[H\x1b[?25l',end='')
 virtual=[]
 for row in rows:
  print(row,end='',flush=True)
  if name=='mux9-off':
   time.sleep(.08)
   observed=S.check_output(['tmux','-S',str(w/'tmux.sock'),'display-message','-p','#{cursor_x}|#{cursor_y}'],text=True).strip()
   virtual.append({'raw_text':row,'cursor_x_y':observed})
  print(flush=True)
 if virtual:(w/'tmux-virtual-oracle.json').write_text(json.dumps({'basis':'Actual tmux virtual cursor after raw bytes, not physical expected; no DSR.','rows':virtual},ensure_ascii=False,indent=2))
 print('PRIVATE '+name,flush=True)
 (w/(name+'.ready')).write_text('ready\n')
 input()
if stage=='inner':
 if mode=='mux':oracle('mux9-off')
 meta={'pid':os.getpid(),'tty':os.ttyname(0),'TERM':os.environ.get('TERM'),'TERM_PROGRAM':os.environ.get('TERM_PROGRAM'),'TERM_PROGRAM_VERSION':os.environ.get('TERM_PROGRAM_VERSION'),'inside_tmux':'TMUX' in os.environ}
 if mode=='mux':
  meta['tmux_server_pid']=int(S.check_output(['tmux','-S',str(w/'tmux.sock'),'display-message','-p','#{pid}'],text=True))
  meta['variation_selector_always_wide']=S.check_output(['tmux','-S',str(w/'tmux.sock'),'show-options','-sv','variation-selector-always-wide'],text=True).strip()
  meta['allow_passthrough']=S.check_output(['tmux','-S',str(w/'tmux.sock'),'show-options','-wv','allow-passthrough'],text=True).strip()
  meta['clients']=S.check_output(['tmux','-S',str(w/'tmux.sock'),'list-clients','-F','#{client_termname}|#{client_termtype}|#{client_termfeatures}'],text=True).splitlines()
 (w/'app.json').write_text(json.dumps(meta,ensure_ascii=False))
 os.execv(binary,[binary,url])
label='chat-t04-'+w.name
(w/'outer.json').write_text(json.dumps({'pid':os.getpid(),'tty':os.ttyname(0),'TERM':os.environ.get('TERM'),'TERM_PROGRAM':os.environ.get('TERM_PROGRAM'),'TERM_PROGRAM_VERSION':os.environ.get('TERM_PROGRAM_VERSION')}))
try:
 oracle('outer9')
 command=['python3','-B',str(w/'host.py'),str(w),mode,'inner',binary,url]
 if mode=='mux':command=['tmux','-S',str(w/'tmux.sock'),'-f',str(w/'tmux.conf'),'new-session','-s','t04','-x','80','-y','30',' '.join(shlex.quote(a) for a in command)]
 command=['script','-q','-f','-e','-E','never','-c',' '.join(shlex.quote(a) for a in command),str(w/'session.raw')]
 child=S.Popen(command)
 (w/'child.json').write_text(json.dumps({'script_pid':child.pid}))
 code=child.wait();(w/'child-exit.json').write_text(json.dumps({'actual_exit':code}))
finally:
 pass
oracle('outer9-after-test')
print('\x1b[?25h',end='',flush=True)
sys.exit(code)
