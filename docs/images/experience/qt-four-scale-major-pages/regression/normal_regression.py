import hashlib,json,os,secrets,subprocess,time
from pathlib import Path

root=Path('/home/gyl/example/chat')
out=Path('/tmp/chat-qt-matrix-driver.2y82nyss')
env=os.environ.copy()
for key in ('LD_LIBRARY_PATH','LD_PRELOAD'):
    env.pop(key,None)
maintenance=env.get('PGDATABASE','postgres')
database='chat_matrix_regression_'+secrets.token_hex(8)
protected={2876288:'521400965',3225:'17583',29935:'248258'}
def ticks(pid):
    return (Path('/proc')/str(pid)/'stat').read_text().rsplit(')',1)[1].split()[19]
def run(args,e=env):
    return subprocess.run(args,env=e,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True,timeout=15).stdout.strip()
assert all(ticks(pid)==value for pid,value in protected.items())
assert run(['git','-C',str(root),'status','--porcelain'])==''
proof={'database':database,'fresh_database':True,'fresh_build_configuration':False,
       'normal_build_exit':0,'previous_missing_pg_environment_ctest_exit':8,
       'sanitizers_run':False,'timeouts_changed':False}
made=False
try:
    run(['createdb','--maintenance-db='+maintenance,'--template=template0','--encoding=UTF8',database])
    made=True
    env['PGDATABASE']=database
    migrations=sorted((root/'sql').glob('[0-9][0-9][0-9]_*.sql'))
    assert len(migrations)==26
    for migration in migrations:
        run(['psql','-X','-v','ON_ERROR_STOP=1','-f',str(migration)])
    started=time.monotonic()
    with (out/'ctest-correct-env.log').open('wb') as log:
        result=subprocess.run(['ctest','--test-dir',str(root/'build'),'--parallel','1','--output-on-failure'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=900)
    proof.update(ctest_actual_exit=result.returncode,seconds=time.monotonic()-started,
                 ctest_log_sha256=hashlib.sha256((out/'ctest-correct-env.log').read_bytes()).hexdigest())
finally:
    checkenv=env.copy()
    checkenv['PGDATABASE']=maintenance
    checkenv['PGOPTIONS']='-c default_transaction_read_only=on'
    if made:
        count=run(['psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-c',"SELECT count(*) FROM pg_stat_activity WHERE datname='"+database+"'"],checkenv)
        proof['connections_before_drop']=int(count)
        assert count=='0'
        dropenv=checkenv.copy()
        dropenv.pop('PGOPTIONS')
        run(['dropdb','--maintenance-db='+maintenance,database],dropenv)
        proof['dropdb_actual_exit']=0
        proof['database_remaining']=int(run(['psql','-X','-A','-t','-v','ON_ERROR_STOP=1','-c',"SELECT count(*) FROM pg_database WHERE datname='"+database+"'"],checkenv))
    proof['protected_ticks_unchanged']=all(ticks(pid)==value for pid,value in protected.items())
    (out/'normal-regression-proof.json').write_text(json.dumps(proof,indent=2))
print(json.dumps(proof),flush=True)
raise SystemExit(proof['ctest_actual_exit'])
