#!/usr/bin/env python3
"""Explicit native-client experiment; all workload actions are XTest/tmux input.

SQL is used only for an owned database's foundation and read-only evidence.
No SDK, WebSocket or JSON-RPC client is used. Passwords exist only in memory
and worker stdin, never configuration artifacts, command lines or reports.
"""
import argparse
import csv
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import resource
import platform
import secrets
import shutil
import selectors
import signal
import socket
import subprocess
import sys
import time

sys.dont_write_bytecode = True
from native_server_observer import Server
from tui_scale_observer import process_sample

SIZES = (10, 30, 50, 100, 200)
SECONDS = {10: 600, 30: 600, 50: 600, 100: 600, 200: 900}
# Startup watchdog only. Measured native readiness needs 4.4 s per GUI client on
# one shared X display (10 clients, including two scrolled-up peer reads); TUI
# previously completed 200 clients well inside 5 s each. The margin detects a
# genuinely stuck client without failing a merely slow shard.
READINESS_SECONDS_PER_CLIENT = {'gui': 15, 'tui': 5}


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()


def percentile(values):
    values = sorted(values)
    return {'samples': len(values), **{f'p{p}_ms': round(values[min(len(values)-1,
            math.ceil(len(values)*p/100)-1)], 3) if values else None for p in (50, 95, 99)}}


def event_wall_seconds(record):
    return record['utc_ns']/1e9 if record.get('utc_ns') else record.get('timestamp')


def within_window(record, beginning, end, include_duration=False):
    finished = event_wall_seconds(record)
    if finished is None:
        return False
    started = finished - record.get('elapsed_ms', 0)/1000 if include_duration else finished
    return beginning <= started <= finished <= end


def pss(pid):
    try:
        fields = dict(line.split(':', 1) for line in
                      Path(f'/proc/{pid}/smaps_rollup').read_text().splitlines() if ':' in line)
        return int(fields['Pss'].split()[0])
    except (OSError, KeyError, ValueError):
        return None


def host(work):
    mem = dict(line.split(':', 1) for line in Path('/proc/meminfo').read_text().splitlines())
    result = {key+'_kib': int(mem[key].split()[0]) for key in
              ('MemTotal', 'MemAvailable', 'SwapTotal', 'SwapFree')}
    result['disk_free_bytes'] = os.statvfs(work).f_bavail * os.statvfs(work).f_frsize
    result['pty_in_use'] = int(Path('/proc/sys/kernel/pty/nr').read_text())
    result['pty_max'] = int(Path('/proc/sys/kernel/pty/max').read_text())
    result['loadavg'] = list(map(float, Path('/proc/loadavg').read_text().split()[:3]))
    result['psi'] = {}
    for kind in ('cpu', 'memory', 'io'):
        result['psi'][kind] = {line.split()[0]: {pair.split('=')[0]: float(pair.split('=')[1])
            for pair in line.split()[1:]} for line in Path('/proc/pressure/'+kind).read_text().splitlines()}
    result['pid_max']=int(Path('/proc/sys/kernel/pid_max').read_text())
    result['threads_max']=int(Path('/proc/sys/kernel/threads-max').read_text())
    result['total_tasks']=int(Path('/proc/loadavg').read_text().split()[3].split('/')[1])
    result['oom_kills']=int(next(line.split()[1] for line in Path('/proc/vmstat').read_text().splitlines() if line.startswith('oom_kill ')))
    result['network_bytes'] = {}
    for line in Path('/proc/net/dev').read_text().splitlines()[2:]:
        name, counters = line.split(':'); fields = counters.split()
        result['network_bytes'][name.strip()] = {'received': int(fields[0]), 'sent': int(fields[8]),
                                                'receive_drop': int(fields[3]), 'send_drop': int(fields[11])}
    return result


class Experiment:
    def __init__(self, args):
        import psycopg2
        self.pg = psycopg2
        self.args = args
        self.repo = Path(__file__).resolve().parent.parent
        self.work = Path(args.work).resolve()
        self.work.mkdir(parents=True, exist_ok=False)
        self.run = secrets.token_hex(4)
        self.dbname = f'chat_native_{args.mode}_{args.count}_{int(time.time())}_{self.run}'
        self.password = secrets.token_hex(16)
        self.events = (self.work/'events.log').open('w', buffering=1)
        self.metrics = (self.work/'metrics.jsonl').open('w', buffering=1)
        self.errorlogs, self.workers = [], []
        self.server, self.db, self.created = None, None, False
        self.selector = selectors.DefaultSelector()
        self.output_buffers = {}
        self.records, self.pids, self.ready, self.finished = [], {}, set(), set()
        self.steady = {}
        self.steady_ends = {}
        self.resource_pids = {}
        self.driver_pids = {}
        self.lifecycle_ready = set()
        self.prepared, self.steady_finished = set(), set()
        self.closed_workers = set()
        self.private_directories = set()
        self.previous_cpu = {}
        self.previous_time = None
        self.sample_rows = []
        self.baseline = host(self.work)
        self.safety = {'mem_available_min_kib': max(4*1024**2, int(self.baseline['MemTotal_kib']*.20)),
                       'disk_free_min_bytes': 5*1024**3, 'swap_growth_max_kib': 128*1024,
                       'sustained_samples': 6, 'psi_memory_full_avg10_max': 5.0,
                       'psi_io_full_avg10_max': 15.0, 'psi_cpu_some_avg10_max': 90.0,
                       'estimated_client_kib': 64*1024 if args.mode=='gui' else 8*1024,
                       'estimated_shard_overhead_kib': 64*1024 if args.mode=='gui' else 8*1024,
                       'server_reserve_kib':512*1024,
                       'estimate_basis':'Two-client deep GUI pilot observed 42.6 MiB PSS/client peak; allow 64 MiB each. TUI about 4 MiB; allow 8 MiB each. Safety projections are not measured capacity'}
        self.safety_streak = {}
        self.summary = {'mode': args.mode, 'target': args.count, 'result': 'FAIL',
                        'pilot': args.pilot_seconds is not None,
                        'database': self.dbname, 'steady_seconds_requested': args.pilot_seconds or SECONDS[args.count],
                        'counts': {}, 'limitations': [], 'server_online_checks': []}
        self.config = {'git_head': subprocess.check_output(['git', '-C', str(self.repo), 'rev-parse', 'HEAD'], text=True).strip(),
                       'mode': args.mode, 'count': args.count, 'seed': args.seed, 'database': self.dbname,
                       'steady_seconds': args.pilot_seconds or SECONDS[args.count],
                       'worker_seconds': (args.pilot_seconds or SECONDS[args.count])+2,
                       'deep_checks': args.pilot_seconds is None or args.deep_pilot,
                       'lifecycle_pilot': args.lifecycle_pilot,
                       'job_control_pilot': args.job_control_pilot,
                       'daily_pilot': args.daily_pilot,
                       'role_pilot': args.role_pilot,
                       'blackhole_pilot': args.blackhole_pilot,
                       'history_diagnostic': args.mode=='gui' and args.count==2 and args.pilot_seconds is not None and not args.daily_pilot,
                       'test_tool_sha256': {p.name: digest(p) for p in (self.repo/'tests').glob('native_*.py')},
                       'build': str(Path(args.build).resolve()), 'build_kind': args.build_kind,
                       'shard_size': args.shard_size, 'server_workers': 8192, 'database_connections': 16,
                       'safety': self.safety, 'host_initial': self.baseline,
                       'environment': {'kernel':platform.release(), 'machine':platform.machine(), 'cpu_count':os.cpu_count(),
                                       'compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
                                       'ftxui_commit':subprocess.check_output(['git','-C',str(self.repo/'third/ftxui'),'rev-parse','HEAD'],text=True).strip()},
                       'input_method': 'XTest' if args.mode == 'gui' else 'tmux keys/bracketed paste',
                       'latency_measurement': 'Native UI observation includes driver polling; not internal RPC timestamps',
                       'sql_fixture': 'Users, accepted pairs, one nonfriend pending request, empty directs, small/medium group membership; no messages',
                       'gdb': 'Owned server; read-only authenticated session memory at barriers, no inferior function calls'}
        save(self.work/'test-config.json', self.config)

    def event(self, kind, **fields):
        value = {'event': kind, 'timestamp': time.time(), **fields}
        self.events.write(json.dumps(value, ensure_ascii=False)+'\n')
        return value

    def setup_database(self):
        # Creation and deletion are restricted to the exact randomly named owned resource.
        conn = self.pg.connect(dbname='postgres')
        conn.autocommit = True
        try:
            with conn.cursor() as cur:
                from psycopg2 import sql
                cur.execute(sql.SQL('CREATE DATABASE {} TEMPLATE template0 ENCODING \'UTF8\'').format(sql.Identifier(self.dbname)))
        finally:
            conn.close()
        self.created = True
        environment = os.environ.copy(); environment['PGDATABASE'] = self.dbname
        with (self.work/'migrations.log').open('w') as log:
            for path in sorted((self.repo/'sql').glob('[0-9][0-9][0-9]_*.sql')):
                subprocess.run(['psql', '-X', '-v', 'ON_ERROR_STOP=1', '-f', str(path)],
                               stdin=subprocess.DEVNULL, stdout=log, stderr=log, env=environment, check=True)
        import bcrypt
        password_hash = bcrypt.hashpw(self.password.encode(), bcrypt.gensalt(rounds=12)).decode()
        self.db = self.pg.connect(dbname=self.dbname)
        actors = []
        with self.db.cursor() as cur:
            for index in range(self.args.count):
                name = f'n{self.run}_{index:04}'
                cur.execute('INSERT INTO users(username,password_hash) VALUES(%s,%s) RETURNING id', (name,password_hash))
                identity = cur.fetchone()[0]
                actors.append({'id': identity, 'username': name, 'index': index})
            for index in range(0, len(actors), 2):
                pair = actors[index:index+2]
                if len(pair) != 2: raise ValueError('Native experiment requires paired actors')
                a,b=pair
                cur.execute('INSERT INTO contacts(owner_id,contact_id) VALUES(%s,%s),(%s,%s)',(a['id'],b['id'],b['id'],a['id']))
                cur.execute("INSERT INTO conversations(kind,direct_user_low,direct_user_high) VALUES('direct',%s,%s) RETURNING id",(a['id'],b['id']))
                direct=cur.fetchone()[0]
                cur.execute('INSERT INTO conversation_members(conversation_id,user_id) VALUES(%s,%s),(%s,%s)',(direct,a['id'],direct,b['id']))
                a.update(peer_id=b['id'],peer_username=b['username'],direct_id=direct)
                b.update(peer_id=a['id'],peer_username=a['username'],direct_id=direct)
            for offset in range(0,len(actors),10):
                members=actors[offset:offset+10]; title=f'Scale {offset//10+1}'
                cur.execute("INSERT INTO conversations(kind,title,owner_id) VALUES('group',%s,%s) RETURNING id",(title,members[0]['id']))
                group=cur.fetchone()[0]
                for index, actor in enumerate(members):
                    cur.execute('INSERT INTO conversation_members(conversation_id,user_id,is_admin) VALUES(%s,%s,%s)',(group,actor['id'],index==1))
                    actor.update(group_id=group,group_title=title,role='owner' if index==0 else 'admin' if index==1 else 'member', member_count=len(members))
            pending=[]
            if len(actors)>=10:
                requester,recipient=actors[-2],actors[-4]
                cur.execute('INSERT INTO friend_requests(requester_id,recipient_id) VALUES(%s,%s)',(requester['id'],recipient['id']))
                pending.append((requester['id'],recipient['id']))
            medium_groups=[]
            for offset in range(0,len(actors),50):
                members=actors[offset:offset+50]
                if len(members)<20:continue
                title=f'Medium {offset//50+1}'
                cur.execute("INSERT INTO conversations(kind,title,owner_id) VALUES('group',%s,%s) RETURNING id",(title,members[0]['id']))
                group=cur.fetchone()[0]
                for index, actor in enumerate(members):
                    cur.execute('INSERT INTO conversation_members(conversation_id,user_id,is_admin) VALUES(%s,%s,%s)',(group,actor['id'],index in (1,2,3)))
                for actor in members:
                    actor.update(medium_id=group,medium_title=title,medium_member_count=len(members))
                medium_groups.append({'id':group,'title':title,'member_count':len(members)})
        self.fixture_pending=pending
        self.db.commit()
        self.db.set_session(readonly=True, autocommit=True)
        self.actors = actors
        save(self.work/'fixture.json', {'actors':actors, 'messages_seeded':0,'pending_requests':pending,'medium_groups':medium_groups})
        return environment

    def start_server(self, environment, restart=False):
        if restart:port=self.port
        else:
            probe=socket.socket();probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
            self.port=port
        server_binary=Path(self.args.build).resolve()/'chat_server'
        self.server=Server(server_binary,port,8192,16,self.work/('server-generation-2' if restart else 'server'),env=environment)
        identity=self.server.start();self.server_identity=identity
        self.url=f'ws://127.0.0.1:{port}/ws'
        self.config.update(server=identity, server_url=self.url, binary_sha256={'server':digest(server_binary),
             'client':digest(self.client_binary())})
        manifest=Path(self.args.build).resolve()/'native-scale-build-manifest.json'
        if manifest.exists():
            data=json.loads(manifest.read_text())
            client_key='qt/chat_qt' if self.args.mode=='gui' else 'chat_tui'
            if data['binary_sha256']['chat_server']!=self.config['binary_sha256']['server'] or data['binary_sha256'][client_key]!=self.config['binary_sha256']['client']:
                raise AssertionError('Optimized binary changed since its build manifest was recorded')
            self.config['binary_build_manifest']=data
        else:self.config['binary_build_manifest']=None
        save(self.work/'test-config.json',self.config)
        self.config.setdefault('server_generations',[]).append(identity)
        save(self.work/'test-config.json',self.config)
        if not restart:self.check_online(0)

    def client_binary(self):
        return Path(self.args.build).resolve()/('qt/chat_qt' if self.args.mode=='gui' else 'chat_tui')

    def check_online(self, expected):
        value=self.server.snapshot(include_ids=True)
        value['expected']=expected;value['timestamp']=time.time()
        self.summary['server_online_checks'].append(value);self.event('online-proof',**value)
        wanted=sorted(a['id'] for a in self.actors) if expected else []
        if value['online']!=expected or value.get('users')!=wanted:
            raise AssertionError('Authenticated server session IDs differ from intended native clients')

    def launch(self):
        window_manager = None
        if self.args.mode == 'gui':
            window_manager = shutil.which(self.args.openbox)
            if window_manager is None:
                raise FileNotFoundError('Openbox executable unavailable; specify --openbox')
            window_manager = str(Path(window_manager).resolve())
        for offset in range(0,len(self.actors),self.args.shard_size):
            snapshot=host(self.work)
            if snapshot['MemAvailable_kib']<self.safety['mem_available_min_kib'] or snapshot['disk_free_bytes']<self.safety['disk_free_min_bytes']:
                raise RuntimeError('Safety stop before launching the next native shard')
            index=len(self.workers); shard=self.work/f'worker-{index:03}'
            command=[sys.executable,str(self.repo/'tests'/('native_gui_scale.py' if self.args.mode=='gui' else 'native_tui_scale.py'))]
            if self.args.mode=='gui':command=['dbus-run-session','--',*command]
            errorlog=(self.work/f'worker-{index:03}.stderr.log').open('w');self.errorlogs.append(errorlog)
            process=subprocess.Popen(command,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=errorlog,text=True,bufsize=1,start_new_session=True)
            self.workers.append(process);self.selector.register(process.stdout,selectors.EVENT_READ,index)
            config={'binary':str(self.client_binary()),'server_url':self.url,
                    'actors':self.actors[offset:offset+self.args.shard_size],
                    'password':self.password,'work':str(shard),'seed':self.args.seed,
                    'steady_seconds':self.config['worker_seconds'],'deep_checks':self.args.pilot_seconds is None or self.args.deep_pilot,
                    'steady_gate': True, 'finish_gate': True, 'lifecycle_pilot': self.args.lifecycle_pilot,
                    'job_control_pilot': self.args.job_control_pilot,
                    'daily_pilot': self.args.daily_pilot,
                    'role_pilot': self.args.role_pilot,
                    'blackhole_pilot': self.args.blackhole_pilot,
                    'history_diagnostic': self.config['history_diagnostic']}
            if window_manager is not None:
                config.update(openbox=window_manager, openbox_data=str(Path(window_manager).parents[1]/'share'))
            process.stdin.write(json.dumps(config)+'\n');process.stdin.flush()

    def accept_record(self, index, record):
        record['worker']=index
        record['phase']='cleanup' if index in self.finished else 'steady' if index in self.steady and index not in self.steady_finished else 'prewarm' if index in self.ready else 'login'
        self.events.write(json.dumps(record,ensure_ascii=False)+'\n');self.records.append(record)
        event=record.get('event')
        if event=='started':
            self.pids[int(record['actor'])]=int(record['pid'])
            self.resource_pids[int(record['pid'])]={'start_ticks':int(record['starttime']), 'executable':str(self.client_binary())}
        elif event=='resource-started':
            pid=int(record['pid']);original=process_sample(pid)
            if original.get('alive') and str(original.get('start_ticks'))==str(record['starttime']):self.resource_pids[pid]=original
        elif event=='driver-started':
            pid=int(record['pid']);identity=process_sample(pid)
            if not identity.get('alive') or str(identity.get('start_ticks'))!=str(record['starttime']):
                raise AssertionError('Native driver identity mismatch')
            self.driver_pids[index]=identity
        elif event=='private-runtime':
            directory=Path(record['path'])
            if directory.parent!=Path('/tmp') or not directory.name.startswith('chat-native-gui-'):
                raise AssertionError('Unexpected private GUI runtime path')
            self.private_directories.add(directory)
        elif event=='closed':
            if not record.get('clients') or any(c.get('exit_status')!=0 or c.get('native_close') is not True for c in record['clients']):
                raise AssertionError('Native GUI clients did not close normally')
            self.closed_workers.add(index)
        elif event=='preference-observe':
            field=record.get('field');expected=record.get('expected')
            actor=record.get('actor');conversation=record.get('conversation')
            if self.args.mode!='gui' or not getattr(self.args,'daily_pilot',False):
                raise AssertionError('Preference observation outside the GUI daily pilot')
            if field not in ('muted','pinned') or not isinstance(expected,bool) or not any(
                    a['id']==actor and conversation in (a['direct_id'],a['group_id']) for a in self.actors):
                raise AssertionError('Invalid native preference observation context')
            deadline=time.monotonic()+3
            while True:
                with self.db.cursor() as cur:
                    cur.execute('SELECT muted,pinned FROM conversation_members WHERE conversation_id=%s AND user_id=%s',
                                (conversation,actor))
                    row=cur.fetchone()
                if row is None:raise AssertionError('Native preference member disappeared')
                if row[0 if field=='muted' else 1] is expected:break
                if time.monotonic()>=deadline:raise AssertionError('Native preference did not reach its expected database state')
                time.sleep(.02)
            self.event('preference-database-proof',actor=actor,conversation=conversation,field=field,
                       expected=expected,muted=row[0],pinned=row[1],readonly=True)
            self.workers[index].stdin.write('{"command":"preference-observed"}\n')
            self.workers[index].stdin.flush()
        elif event=='lifecycle-ready':self.lifecycle_ready.add(index)
        elif event=='ready':self.ready.add(index)
        elif event=='steady-ready':self.prepared.add(index)
        elif event=='steady-finished':
            self.steady_finished.add(index)
            self.steady_ends[index]=record.get('monotonic',record.get('timestamp'))
        elif event=='steady-start':self.steady[index]=record.get('monotonic',record.get('timestamp'))
        elif event=='completed':
            self.finished.add(index)
            if not record.get('ok',True):raise AssertionError(f'Worker {index} completed with failures')
        elif event=='failure':raise AssertionError(f'Native worker failure: {record.get("error",record.get("kind"))}')
        elif event=='operation' and record.get('ok') is False:raise AssertionError('Native operation failed')
        elif event in ('not-verified','unsupported'):self.summary['limitations'].append(record)

    def read_events(self, timeout=1):
        # os.read avoids TextIO.readline buffering hiding already-read events from select.
        for key,_ in self.selector.select(timeout):
            index=key.data;chunk=os.read(key.fd,65536)
            if not chunk:
                self.selector.unregister(key.fileobj)
                if self.output_buffers.get(index):raise RuntimeError('Incomplete worker event at EOF')
                if index not in self.finished:raise RuntimeError(f'Worker {index} exited without completion')
                continue
            data=self.output_buffers.get(index,b'')+chunk
            lines=data.split(b'\n');self.output_buffers[index]=lines.pop()
            for raw in lines:
                line=raw.decode('utf-8')
                if not line.strip():continue
                try:record=json.loads(line)
                except json.JSONDecodeError:
                    # Accessibility service activation is inherited by dbus-run-session.
                    with (self.work/f'worker-{index:03}.stdout-diagnostic.log').open('a') as log:
                        log.write(line.replace(self.password,'[REDACTED]')+'\n')
                    self.event('driver-diagnostic',worker=index,source='non-JSON service stdout')
                    continue
                if not isinstance(record,dict):raise ValueError('Worker event is not an object')
                self.accept_record(index,record)

    def sample(self, label):
        now=time.monotonic();snapshot=host(self.work)
        client_values=[]
        for actor,pid in self.pids.items():
            value=process_sample(pid);value['actor']=actor;value['pss_kib']=pss(pid)
            client_values.append(value)
        server_value=process_sample(self.server_identity['pid']);server_value['pss_kib']=pss(self.server_identity['pid'])
        elapsed=now-self.previous_time if self.previous_time else None
        for value in [*client_values,server_value]:
            previous=self.previous_cpu.get(value['pid'])
            value['cpu_percent_interval']=(value.get('cpu_seconds',0)-previous)*100/elapsed if previous is not None and elapsed else None
            if value.get('alive'):self.previous_cpu[value['pid']]=value.get('cpu_seconds',0)
        self.previous_time=now
        with self.db.cursor() as cur:
            started=time.monotonic()
            cur.execute("SELECT count(*),count(*) FILTER(WHERE state='active'), count(*) FILTER(WHERE wait_event_type='Lock') FROM pg_stat_activity WHERE datname=current_database()")
            connections,active,lock_waiters=cur.fetchone()
            cur.execute('SELECT pid FROM pg_stat_activity WHERE datname=current_database()');backend_pids=[row[0] for row in cur.fetchall()]
            cur.execute('SELECT pg_database_size(current_database())');dbsize=cur.fetchone()[0]
            cur.execute("SELECT xact_commit,xact_rollback,deadlocks,tup_inserted FROM pg_stat_database WHERE datname=current_database()")
            commits,rollbacks,deadlocks,inserted=cur.fetchone()
            query_ms=(time.monotonic()-started)*1000
        auxiliaries=[]
        auxiliary_pids=(set(self.resource_pids)|{v['pid'] for v in self.driver_pids.values()}|{w.pid for w in self.workers})-set(self.pids.values())
        for pid in sorted(auxiliary_pids):
            item=process_sample(pid);item['pss_kib']=pss(pid);auxiliaries.append(item)
        backends=[]
        for pid in backend_pids:
            item=process_sample(pid);item['pss_kib']=pss(pid);backends.append(item)
        for item in [*auxiliaries,*backends]:
            previous=self.previous_cpu.get(item['pid'])
            item['cpu_percent_interval']=(item.get('cpu_seconds',0)-previous)*100/elapsed if previous is not None and elapsed else None
            if item.get('alive'):self.previous_cpu[item['pid']]=item.get('cpu_seconds',0)
        value={'timestamp':time.time(),'checkpoint':label,'host':snapshot,'server':server_value,'clients':client_values,'load_generators_and_x11':auxiliaries,'postgres_backends':backends,
               'database':{'connections':connections,'active':active,'lock_waiters':lock_waiters,'size_bytes':dbsize,
                           'xact_commit':commits,'xact_rollback':rollbacks,'deadlocks':deadlocks,'tuples_inserted':inserted,'observer_query_ms':query_ms}}
        self.metrics.write(json.dumps(value)+'\n')
        row={'timestamp':value['timestamp'],'checkpoint':label,'clients_alive':sum(bool(v['alive']) for v in client_values),
             'client_rss_kib':sum(v.get('rss_kib',0) for v in client_values),'client_pss_kib':sum(v.get('pss_kib') or 0 for v in client_values),
             'client_cpu_percent':sum(v.get('cpu_percent_interval') or 0 for v in client_values),'client_fds':sum(v.get('fds',0) for v in client_values),
             'client_threads':sum(v.get('threads',0) for v in client_values),'server_rss_kib':server_value.get('rss_kib',0),
             'server_pss_kib':server_value.get('pss_kib'),'server_cpu_percent':server_value.get('cpu_percent_interval'),
             'mem_available_kib':snapshot['MemAvailable_kib'],'swap_free_kib':snapshot['SwapFree_kib'],
             'auxiliary_rss_kib':sum(v.get('rss_kib',0) for v in auxiliaries),'auxiliary_pss_kib':sum(v.get('pss_kib') or 0 for v in auxiliaries),
             'auxiliary_cpu_percent':sum(v.get('cpu_percent_interval') or 0 for v in auxiliaries),
             'postgres_backend_pss_kib':sum(v.get('pss_kib') or 0 for v in backends),
             'postgres_backend_cpu_percent':sum(v.get('cpu_percent_interval') or 0 for v in backends),
             'db_connections':connections,'db_active':active,'db_lock_waiters':lock_waiters,'db_observer_query_ms':query_ms,'db_size_bytes':dbsize}
        self.sample_rows.append(row)
        with (self.work/'metrics.csv').open('w',newline='') as stream:
            writer=csv.DictWriter(stream,fieldnames=list(row));writer.writeheader();writer.writerows(self.sample_rows)
        reasons=[]
        if snapshot['MemAvailable_kib'] < self.safety['mem_available_min_kib']:reasons.append('available-memory')
        if snapshot['disk_free_bytes'] < self.safety['disk_free_min_bytes']:reasons.append('disk-space')
        if self.baseline['SwapFree_kib']-snapshot['SwapFree_kib']>self.safety['swap_growth_max_kib']:reasons.append('swap-growth')
        for kind,axis,threshold in [('memory','full',5),('io','full',15),('cpu','some',90)]:
            if snapshot['psi'][kind].get(axis,{}).get('avg10',0)>threshold:reasons.append(kind+'-pressure')
        hard=set(reasons)&{'available-memory','disk-space','swap-growth'}
        if snapshot['oom_kills']>self.baseline['oom_kills']:hard.add('host-oom')
        if snapshot['total_tasks']>int(snapshot['threads_max']*.8):hard.add('host-task-limit')
        if hard:raise RuntimeError('Safety stop: '+','.join(sorted(hard)))
        for name in set(self.safety_streak)|set(reasons):
            self.safety_streak[name]=self.safety_streak.get(name,0)+1 if name in reasons else 0
            if self.safety_streak[name]<self.safety['sustained_samples']:continue
            if self.args.pilot_seconds is not None:
                # A functional pilot makes no capacity claim, so another workload's IO or CPU
                # pressure must not throw away the native UI evidence it already produced.
                # Record it and restart the streak; capacity measurements still stop.
                record={'kind':'sustained-'+name,'streak':self.safety_streak[name],'phase':label,
                        'psi':snapshot['psi'],'reason':'Host pressure during a functional pilot; no capacity claim is made'}
                self.summary['limitations'].append(record)
                self.event('host-pressure', pressure=record['kind'], streak=record['streak'],
                           phase=label, psi=snapshot['psi'], reason=record['reason'])
                self.safety_streak[name]=0
                continue
            raise RuntimeError('Safety stop: sustained '+name)
        if self.ready and len(self.ready)==len(self.workers) and label=='steady' and not self.finished and any(not v['alive'] for v in client_values):
            raise RuntimeError('Native client unexpectedly exited during steady state')
        return value

    def verify_business(self):
        operations=[r for r in self.records if r.get('event')=='operation']
        markers={r['marker'] for r in self.records if r.get('marker') and r.get('event') in ('send-confirmed','send-start')}
        markers.update(r['marker'] for r in operations if r.get('marker') and r.get('kind') in
                       ('initial-group-send','direct-send','group-send','send-message','group-message','reply-message'))
        facts=[]
        with self.db.cursor() as cur:
            cur.execute('SELECT id,conversation_id,sender_id,body,deleted,edited_at IS NOT NULL,reply_to_id FROM messages ORDER BY id')
            rows=cur.fetchall()
            cur.execute('SELECT id,kind,direct_user_low,direct_user_high,owner_id,announcement,pinned_message_id,title FROM conversations ORDER BY id')
            conversations=cur.fetchall()
        save(self.work/'messages-state.json', {'columns':['id','conversation_id','sender_id','body','deleted','edited','reply_to_id'], 'rows':rows})
        for marker in sorted(markers):
            matching=[row for row in rows if (row[3] or '').split(maxsplit=1)[:1] == [marker]]
            expected=[]
            for record in self.records:
                if record.get('marker')!=marker:continue
                if record.get('event')!='send-start' and not (
                        record.get('event')=='operation' and record.get('kind') in
                        ('initial-group-send','direct-send','group-send','send-message','group-message','reply-message')):continue
                context=record.get('conversation')
                if record.get('conversation_title'):
                    groups=[c for c in conversations if c[1]=='group' and c[7]==record['conversation_title']]
                    if len(groups)!=1:raise AssertionError('Native group title context is missing or ambiguous')
                    context=groups[0][0]
                if context is not None:expected.append((context,record['actor']))
            # Deleted/edit markers are audited independently; their old bodies need not remain.
            deleted=any(r.get('event')=='operation' and r.get('kind')=='delete' and r.get('marker')==marker for r in self.records)
            edited=any(r.get('event')=='operation' and r.get('kind')=='edit' and r.get('marker')==marker for r in self.records)
            if edited:
                edited_tokens={marker+'-edited',marker+'_EDIT'}
                matching=[row for row in rows if (row[3] or '').split(maxsplit=1)[:1] and (row[3] or '').split(maxsplit=1)[0] in edited_tokens]
            fact={'marker':marker,'matching_ids':[r[0] for r in matching], 'ui_deleted':deleted,'ui_edited':edited}
            if not deleted and len(matching)!=1:raise AssertionError('Native send marker missing or duplicated: '+marker+'; matches='+str(len(matching)))
            if edited and any(not row[5] for row in matching):raise AssertionError('Native edited marker lacks the database edited flag')
            if matching and expected and any((row[1],row[2])!=context for row in matching for context in expected):raise AssertionError('Native marker was persisted with the wrong conversation or sender')
            facts.append(fact)
        with self.db.cursor() as cur:
            cur.execute('SELECT owner_id,contact_id FROM contacts ORDER BY owner_id,contact_id');contacts=cur.fetchall()
            cur.execute('SELECT requester_id,recipient_id FROM friend_requests ORDER BY requester_id,recipient_id');pending=cur.fetchall()
            cur.execute('SELECT conversation_id,user_id,is_admin,last_read_message_id FROM conversation_members ORDER BY conversation_id,user_id');members=cur.fetchall()
        save(self.work/'relation-facts.json',{'contacts':contacts,'pending':pending,'conversations':conversations,'members':members})
        if sorted(contacts)!=sorted((a['id'],a['peer_id']) for a in self.actors):raise AssertionError('Native friendship flow did not restore the authoritative accepted pairs')
        for actor in self.actors:
            direct=[c for c in conversations if c[1]=='direct' and {c[2],c[3]}=={actor['id'],actor['peer_id']}]
            if len(direct)!=1 or direct[0][0]!=actor['direct_id']:raise AssertionError('Native reaccept created a different or duplicate direct history')
        if all('group_id' in a for a in self.actors):
            for group in {a['group_id'] for a in self.actors}:
                expected_members=[a for a in self.actors if a['group_id']==group]
                actual_members=[m for m in members if m[0]==group]
                info=next(c for c in conversations if c[0]==group)
                owner=next(a['id'] for a in expected_members if a['role']=='owner')
                if sorted(m[1] for m in actual_members)!=sorted(a['id'] for a in expected_members) or info[4]!=owner:
                    raise AssertionError('Native group management did not restore its membership/owner')
                if sorted(m[1] for m in actual_members if m[2])!=sorted(a['id'] for a in expected_members if a['role']=='admin'):
                    raise AssertionError('Native group management did not restore its admins')
                if info[5] or info[6] is not None:raise AssertionError('Native group clear/unpin did not restore its authoritative state')
        for event in self.records:
            if event.get('event')!='daily-group-created':continue
            groups=[c for c in conversations if c[1]=='group' and c[7]==event['conversation_title']]
            if len(groups)!=1:raise AssertionError('Native created group is missing or ambiguous')
            info=groups[0]
            actual_members=[m for m in members if m[0]==info[0]]
            if info[4]!=event['actor'] or sorted(m[1] for m in actual_members)!=sorted((event['actor'],event['peer_id'])) or info[5] or info[6] is not None:
                raise AssertionError('Native created group final owner/membership/clear state differs')
        if sorted(pending)!=sorted(getattr(self,'fixture_pending',[])):raise AssertionError('Native request decision flow left unexpected pending relationships')
        save(self.work/'message-facts.json',{'messages':len(rows),'marker_facts':facts,
             'database_state_counts': {'deleted':sum(bool(r[4]) for r in rows),'edited':sum(bool(r[5]) for r in rows),'replies':sum(r[6] is not None for r in rows)},
             'limitation': 'Mutation UI evidence is retained; aggregate database flags do not prove exact deleted target IDs'})
        acknowledged={r.get('actor') for r in operations if r.get('ok')}
        self.summary['counts']={'started':len(self.pids),'authenticated':len({r['actor'] for r in self.records if r.get('event')=='authenticated'}),
            'effective_online':self.args.count,'ui_participants':len(acknowledged),'successful_business_operations':sum(r.get('ok') is True for r in operations),
            'failed_business_operations':sum(r.get('ok') is False for r in operations),'persisted_messages':len(rows),
            'send_markers':len(markers),'native_screen_receipts':len([r for r in self.records if r.get('event')=='screen-received'])}
        beginnings=[event_wall_seconds(r) for r in self.records if r.get('event')=='steady-start']
        endings=[event_wall_seconds(r) for r in self.records if r.get('event')=='steady-finished']
        beginning=max(beginnings) if beginnings else float('-inf')
        end=min(endings) if endings else float('inf')
        self.summary['latency_common_window_utc']={'start':beginning if beginnings else None,'end':end if endings else None}
        steady_ops=[r for r in operations if r.get('phase')=='steady' and within_window(r,beginning,end,True)]
        self.summary['latency']={'login':percentile([r['login_ms'] for r in self.records if r.get('event')=='authenticated']),
             'ui_operation_steady':percentile([r['elapsed_ms'] for r in steady_ops if r.get('ok') and 'elapsed_ms' in r]),
             'ui_operation_prewarm':percentile([r['elapsed_ms'] for r in operations if r.get('ok') and r.get('phase')!='steady' and 'elapsed_ms' in r]),
             'native_send_confirmation_steady':percentile([r['elapsed_ms'] for r in steady_ops if r.get('ok') and 'elapsed_ms' in r and r.get('kind') in ('direct-send','group-send','send-message','group-message')])}
        starts={r['marker']:r for r in self.records if r.get('event')=='send-start'}
        end_to_end=[]
        for r in self.records:
            if r.get('event')!='screen-received' or r.get('marker') not in starts:continue
            original=starts[r['marker']]
            if r.get('actor')==original.get('actor') or original.get('phase')!='steady':continue
            if not within_window(original,beginning,end) or not within_window(r,beginning,end):continue
            sent_at=event_wall_seconds(original)
            received_at=event_wall_seconds(r)
            end_to_end.append((received_at-sent_at)*1000)
        self.summary['latency']['end_to_end_native_steady']=percentile(end_to_end)
        self.summary['resource_peaks']={key:max((r[key] or 0 for r in self.sample_rows),default=0) for key in
            ('client_rss_kib','client_pss_kib','client_threads','client_fds','server_rss_kib','server_pss_kib')}
        if len(acknowledged)!=self.args.count:raise AssertionError('Not every counted native client completed an actual UI operation')

    def cleanup(self):
        errors=[]
        # SIGINT is caught by workers, which close only their native children.
        for index,process in enumerate(self.workers):
            reported_failure=any(r.get('worker')==index and (r.get('event')=='failure' or r.get('event')=='operation' and r.get('ok') is False) for r in self.records)
            if process.poll() is None and index not in self.finished and not reported_failure:
                identity=self.driver_pids.get(index)
                if identity:
                    current=process_sample(identity['pid'])
                    if current.get('alive') and current.get('start_ticks')==identity['start_ticks'] and current.get('executable')==identity['executable']:
                        with contextlib.suppress(ProcessLookupError):os.kill(identity['pid'],signal.SIGINT)
                else:process.send_signal(signal.SIGINT)
        deadline=time.monotonic()+90
        while any(p.poll() is None for p in self.workers) and time.monotonic()<deadline:
            try:self.read_events(.2)
            except Exception as error:
                errors.append('Cleanup event: '+str(error))
        for process in self.workers:
            if process.poll() is None:
                errors.append('Native worker did not clean up within deadline')
                # Group was created by this coordinator, no user processes can be members.
                os.killpg(process.pid,signal.SIGTERM)
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:errors.append('Owned process group still running')
        while self.selector.select(0):
            try:self.read_events(0)
            except Exception as error:errors.append('Final cleanup event: '+str(error))
        if self.args.mode=='gui' and self.closed_workers!=set(range(len(self.workers))):
            errors.append('Missing native GUI close evidence')
        # D-Bus activation helpers inherit only this worker's isolated process
        # group. They can outlive dbus-run-session; stop them before removing
        # private XDG runtime paths (document portals may hold a FUSE mount).
        for worker in self.workers:
            helpers=[]
            for entry in Path('/proc').iterdir():
                if not entry.name.isdigit():continue
                pid=int(entry.name)
                try:
                    if os.getpgid(pid)!=worker.pid:continue
                except ProcessLookupError:continue
                identity=process_sample(pid)
                if identity.get('alive'):
                    if identity.get('executable')==str(self.client_binary()):errors.append('Native client survived driver shutdown')
                    helpers.append(identity)
            for identity in helpers:
                current=process_sample(identity['pid'])
                if current.get('alive') and current.get('start_ticks')==identity['start_ticks'] and current.get('executable')==identity['executable']:
                    self.event('owned-service-stop',pid=identity['pid'],starttime=identity['start_ticks'],executable=identity['executable'])
                    with contextlib.suppress(ProcessLookupError):os.kill(identity['pid'],signal.SIGTERM)
            deadline=time.monotonic()+5
            while any(process_sample(v['pid']).get('alive') for v in helpers) and time.monotonic()<deadline:time.sleep(.05)
            if any(process_sample(v['pid']).get('alive') for v in helpers):errors.append('Owned desktop service did not stop')
        for directory in self.private_directories:
            try:
                if directory.exists():
                    if directory.is_symlink() or directory.stat().st_uid!=os.getuid():
                        raise RuntimeError('Private runtime ownership mismatch')
                    shutil.rmtree(directory)
            except Exception as error:errors.append('Private GUI runtime cleanup: '+str(error))
        for index,process in enumerate(self.workers):
            if process.poll() != 0:errors.append(f'Native worker {index} exit status {process.poll()}')
        for pid,original in self.resource_pids.items():
            current=process_sample(pid)
            if current.get('alive') and current.get('start_ticks')==original.get('start_ticks') and current.get('executable')==original.get('executable'):
                errors.append(f'Owned resource {pid} survived native shutdown')
                os.kill(pid,signal.SIGTERM)
                deadline=time.monotonic()+10
                while process_sample(pid).get('alive') and time.monotonic()<deadline:time.sleep(.05)
                current=process_sample(pid)
                if current.get('alive') and current.get('start_ticks')==original.get('start_ticks') and current.get('executable')==original.get('executable'):os.kill(pid,signal.SIGKILL)
        if self.server:
            release_started=time.monotonic()
            deadline=release_started+10
            while True:
                try:
                    self.check_online(0)
                    self.summary['online_release_wait_seconds']=time.monotonic()-release_started
                    break
                except AssertionError as error:
                    if time.monotonic()>=deadline:
                        errors.append(str(error));break
                    time.sleep(.05)
                except Exception as error:errors.append(str(error));break
            self.summary['server_cleanup']=self.server.stop()
            if self.summary['server_cleanup'].get('status')!='PASS':errors.append('Owned server cleanup was not PASS')
        for logfile in self.errorlogs:logfile.close()
        if self.db:self.db.close()
        if self.created:
            try:
                conn=self.pg.connect(dbname='postgres');conn.autocommit=True
                with conn.cursor() as cur:
                    from psycopg2 import sql
                    cur.execute(sql.SQL('DROP DATABASE {}').format(sql.Identifier(self.dbname)))
                conn.close();self.summary['database_cleaned']=True
            except Exception:errors.append('Owned isolated database cleanup failed')
        if errors:self.summary['cleanup_errors']=errors
        return not errors

    def run_stage(self):
        try:
            if self.args.mode=='tui' and self.args.count+self.baseline['pty_in_use']+128>self.baseline['pty_max']:
                self.summary['result']='NOT REACHED';self.summary['reason']='PTY capacity would exceed existing system limit'
                return
            estimated=self.args.count*self.safety['estimated_client_kib']+math.ceil(self.args.count/self.args.shard_size)*self.safety['estimated_shard_overhead_kib']+self.safety['server_reserve_kib']
            self.summary['resource_preflight']={'estimated_required_kib':estimated,'available_for_test_kib':self.baseline['MemAvailable_kib']-self.safety['mem_available_min_kib'],'basis':self.safety['estimate_basis']}
            if estimated>self.baseline['MemAvailable_kib']-self.safety['mem_available_min_kib']:
                self.summary['result']='NOT REACHED';self.summary['reason']='Conservative native-client memory projection exceeds safe host budget; not a measured application limit'
                return
            environment=self.setup_database();self.start_server(environment);self.launch()
            started=time.monotonic();deadline=started+max(240,len(self.actors)*READINESS_SECONDS_PER_CLIENT[self.args.mode])
            sample_at=0
            while len(self.ready)<len(self.workers):
                self.read_events(.1)
                if time.monotonic()>deadline:raise TimeoutError('Native authentication/readiness deadline')
                if time.monotonic()>sample_at:self.sample('login');sample_at=time.monotonic()+10
            self.check_online(self.args.count)
            self.summary['target_online_seconds']=time.monotonic()-started
            for process in self.workers:process.stdin.write('{"command":"start"}\n');process.stdin.flush()
            deadline=time.monotonic()+max(240,self.args.shard_size*20)
            sample_at=0
            restarted=False
            while len(self.prepared)<len(self.workers):
                self.read_events(.1)
                if self.args.lifecycle_pilot and not restarted and len(self.lifecycle_ready)==len(self.workers):
                    stopped=self.server.stop()
                    self.event('lifecycle-server-stop',result=stopped)
                    if stopped.get('status')!='PASS':raise AssertionError('Lifecycle old server did not shut down normally')
                    self.start_server(environment,restart=True)
                    self.event('lifecycle-server-restarted',identity=self.server_identity)
                    for process in self.workers:process.stdin.write('{"command":"recover"}\n');process.stdin.flush()
                    restarted=True
                if time.monotonic()>deadline:raise TimeoutError('Native UI prewarm deadline')
                if time.monotonic()>sample_at:self.sample('prewarm');sample_at=time.monotonic()+10
            self.check_online(self.args.count)
            self.previous_cpu.clear();self.previous_time=None
            self.sample('steady-baseline')
            formal_start=time.monotonic()
            for process in self.workers:process.stdin.write('{"command":"steady"}\n');process.stdin.flush()
            deadline=time.monotonic()+self.config['steady_seconds']+max(120,self.args.shard_size*10)
            while len(self.steady_finished)<len(self.workers):
                self.read_events(.1)
                if time.monotonic()>deadline:raise TimeoutError('Native steady workload deadline')
                if time.monotonic()>sample_at:self.sample('steady');sample_at=time.monotonic()+10
            self.summary['simultaneous_online_seconds']=time.monotonic()-formal_start
            if len(self.steady)!=len(self.workers) or len(self.steady_ends)!=len(self.workers):raise AssertionError('Missing native steady boundary events')
            self.summary['common_workload_seconds']=min(self.steady_ends.values())-max(self.steady.values())
            if self.summary['common_workload_seconds'] < self.config['steady_seconds']:raise AssertionError('Native simultaneous workload duration below target')
            self.check_online(self.args.count)
            self.verify_business()
            for process in self.workers:process.stdin.write('{"command":"finish"}\n');process.stdin.flush()
            deadline=time.monotonic()+90
            while len(self.finished)<len(self.workers):
                self.read_events(.1)
                if time.monotonic()>deadline:raise TimeoutError('Native exit deadline')
            self.summary['result']='PILOT PASS' if self.args.pilot_seconds is not None else 'PARTIAL'
            self.summary['reason']='Native connections/UI/messages verified; dedicated daily scenarios remain unverified' if self.summary['limitations'] else None
        except BaseException as error:
            if isinstance(error,KeyboardInterrupt):text='Interrupted by user'
            else:text=type(error).__name__+': '+str(error)
            self.summary['reason']=text.replace(self.password,'[REDACTED]');self.event('stage-failure',error=self.summary['reason'])
            if self.db is not None and not self.db.closed:
                # Retain read-only facts before the owned database is removed.
                # Password hashes and attachment bytes are never part of evidence.
                try:
                    with self.db.cursor() as cur:
                        cur.execute('SELECT id,conversation_id,sender_id,body,deleted,edited_at IS NOT NULL,reply_to_id FROM messages ORDER BY id')
                        messages=cur.fetchall()
                        cur.execute('SELECT owner_id,contact_id FROM contacts ORDER BY owner_id,contact_id')
                        contacts=cur.fetchall()
                        cur.execute('SELECT requester_id,recipient_id FROM friend_requests ORDER BY requester_id,recipient_id')
                        requests=cur.fetchall()
                        cur.execute('SELECT conversation_id,user_id,is_admin,last_read_message_id FROM conversation_members ORDER BY conversation_id,user_id')
                        members=cur.fetchall()
                    save(self.work/'failure-database.json',{'message_columns':['id','conversation_id','sender_id','body','deleted','edited','reply_to_id'],
                        'messages':messages,'contacts':contacts,'friend_requests':requests,'members':members})
                    self.event('failure-database-saved',path=str(self.work/'failure-database.json'))
                except Exception as diagnostic_error:
                    self.event('failure-database-unavailable',error=(type(diagnostic_error).__name__+': '+str(diagnostic_error)).replace(self.password,'[REDACTED]'))
        finally:
            if not self.cleanup() and self.summary['result']!='NOT REACHED':self.summary['result']='FAIL'
            if not self.summary['counts']:
                operations=[r for r in self.records if r.get('event')=='operation']
                self.summary['counts']={'started':len(self.pids),'authenticated':len({r['actor'] for r in self.records if r.get('event')=='authenticated'}),
                    'ui_participants':len({r['actor'] for r in operations if r.get('ok')}),
                    'successful_business_operations':sum(r.get('ok') is True for r in operations),
                    'failed_business_operations':sum(r.get('ok') is False for r in operations),
                    'effective_online':max((v['online'] for v in self.summary['server_online_checks'] if v['online']==v['expected']),default=0)}
            self.summary['ended_at']=time.time()
            save(self.work/'summary.json',self.summary)
            self.events.close();self.metrics.close();self.selector.close()
            print(json.dumps(self.summary,ensure_ascii=False),flush=True)
        return 0 if self.summary['result'] in ('PASS','PILOT PASS','PARTIAL','NOT REACHED') else 1


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode',choices=('gui','tui'),required=True)
    parser.add_argument('--count',type=int,choices=(*SIZES,2),required=True)
    parser.add_argument('--work',required=True)
    parser.add_argument('--build',required=True)
    parser.add_argument('--build-kind',choices=('Debug-pilot','RelWithDebInfo'),required=True)
    parser.add_argument('--seed',type=int,default=104302)
    parser.add_argument('--shard-size',type=int,default=40)
    parser.add_argument('--pilot-seconds',type=float)
    parser.add_argument('--lifecycle-pilot',action='store_true',help='Separate two-native-client server restart and process pause scenario')
    parser.add_argument('--job-control-pilot',action='store_true',help='Separate two-TUI Ctrl+Z/fg and terminal restoration scenario')
    parser.add_argument('--daily-pilot',action='store_true',help='Separate two-native-client account and create/join group scenario')
    parser.add_argument('--role-pilot',action='store_true',help='Separate TUI group-role scenario: owner, admin and ordinary member')
    parser.add_argument('--blackhole-pilot',action='store_true',help='Separate two-TUI silent link loss, catch-up and recovery scenario')
    parser.add_argument('--deep-pilot',action='store_true',help='Exercise deeper native UI scenarios during a non-capacity pilot')
    parser.add_argument('--openbox', default='openbox', help='Openbox executable for private X11 shards; use an explicit path for a private installation')
    args=parser.parse_args()
    if args.lifecycle_pilot and (args.count!=2 or args.pilot_seconds is None):parser.error('Lifecycle pilot requires exactly 2 clients and --pilot-seconds')
    if args.job_control_pilot and (args.mode!='tui' or args.count!=2 or args.pilot_seconds is None or args.lifecycle_pilot):parser.error('Job control pilot requires TUI, exactly 2 clients, --pilot-seconds, and no --lifecycle-pilot')
    if args.daily_pilot and (args.count!=2 or args.pilot_seconds is None or not args.deep_pilot or args.lifecycle_pilot or args.job_control_pilot):parser.error('Daily pilot requires exactly 2 clients, --pilot-seconds, --deep-pilot, and no lifecycle/job-control pilot')
    if args.role_pilot and (args.mode!='tui' or args.count!=10 or args.pilot_seconds is None or args.lifecycle_pilot or args.job_control_pilot or args.daily_pilot):parser.error('Role pilot requires TUI, ten clients, --pilot-seconds, and no other pilot')
    if args.blackhole_pilot and (args.mode!='tui' or args.count!=2 or args.pilot_seconds is None or args.lifecycle_pilot or args.job_control_pilot or args.daily_pilot or args.role_pilot):parser.error('Blackhole pilot requires TUI, two clients, --pilot-seconds, and no other pilot')
    if args.count==2 and args.pilot_seconds is None:parser.error('2 clients is a pilot only')
    if args.build_kind!='RelWithDebInfo' and args.pilot_seconds is None:parser.error('Formal capacity measurements require the optimization build')
    if not 2<=args.shard_size<=40 or args.shard_size%10:parser.error('Shard size must be 10, 20, 30 or 40 to keep peer/group boundaries intact')
    soft,hard=resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE,(min(16384,hard),hard))
    experiment=Experiment(args)
    return experiment.run_stage()


if __name__=='__main__':
    raise SystemExit(main())
