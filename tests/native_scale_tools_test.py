#!/usr/bin/env python3
"""Manual reliability checks: python3 -B tests/native_scale_tools_test.py.

No server, database, SDK, or native client is started. This is deliberately not
registered with CTest; the native experiments remain separate evidence.
"""
import io
import contextlib
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import threading
import time
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import native_client_scale as scale
import native_gui_scale as gui
import native_tui_scale as tui
import native_server_observer as observer
import native_network


class BlackholeRelayTest(unittest.TestCase):
    """Loopback only: proves the relay can silence one client path and bring it back."""

    def setUp(self):
        self.echo = socket.socket()
        self.echo.bind(('127.0.0.1', 0))
        self.echo.listen(4)
        self.sockets = []
        threading.Thread(target=self._serve, daemon=True).start()
        self.link = native_network.Blackhole('127.0.0.1', self.echo.getsockname()[1])

    def tearDown(self):
        self.link.close()
        for sock in self.sockets:
            with contextlib.suppress(OSError):
                sock.close()
        self.echo.close()

    def _serve(self):
        while True:
            try:
                peer, _ = self.echo.accept()
            except OSError:
                return
            threading.Thread(target=self._echo, args=(peer,), daemon=True).start()

    @staticmethod
    def _echo(peer):
        while True:
            try:
                data = peer.recv(4096)
                if not data:
                    return
                peer.sendall(data)
            except OSError:
                return

    def _client(self):
        client = socket.create_connection(('127.0.0.1', self.link.port))
        client.settimeout(2)
        self.sockets.append(client)
        return client

    def test_pause_silences_the_path_and_resume_delivers_again(self):
        client = self._client()
        client.sendall(b'up')
        self.assertEqual(client.recv(16), b'up')
        self.link.pause()
        time.sleep(.3)
        client.sendall(b'gone')
        with self.assertRaises(TimeoutError):
            client.recv(16)
        self.link.resume()
        self.assertEqual(client.recv(16), b'gone')
        client.sendall(b'again')
        self.assertEqual(client.recv(16), b'again')
        self.assertGreater(self.link.relayed, 0)

    def test_sever_closes_the_path_and_the_same_address_accepts_a_reconnect(self):
        client = self._client()
        client.sendall(b'first')
        self.assertEqual(client.recv(16), b'first')
        self.link.sever()
        self.assertEqual(client.recv(16), b'')
        again = self._client()
        again.sendall(b'reconnected')
        self.assertEqual(again.recv(16), b'reconnected')



class Rows:
    """Read-only result provider; never opens a database."""
    def __init__(self, rows):
        self.rows = rows
        self.current = rows
        self.relations = {
            "contacts": [(7, 8), (8, 7)],
            "friend_requests": [],
            "conversations": [(22, "direct", 7, 8, None, None, None, None)],
            "conversation_members": [(22, 7, False, 0), (22, 8, False, 0)],
        }

    def cursor(self):
        return self

    def __enter__(self):
        return self

    def __exit__(self, *args):
        pass

    def execute(self, statement):
        if not statement.lstrip().startswith('SELECT '):
            raise AssertionError('Reliability test attempted a mutation')
        table = statement.split(' FROM ', 1)[1].split()[0]
        self.current = self.rows if table == 'messages' else self.relations[table]

    def fetchall(self):
        return self.current


class FinishedWorker:
    def __init__(self, returncode):
        self.returncode = returncode
        # Above the kernel PID range: the cleanup scan cannot target a process.
        self.pid = int(Path('/proc/sys/kernel/pid_max').read_text()) + 1

    def poll(self):
        return self.returncode

    def send_signal(self, signum):
        raise AssertionError('Finished fake worker must not receive a signal')


class NativeToolsTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='chat-native-tools-test-')
        self.experiments = []
        self.streams = []
        self.write_fds = []

    def tearDown(self):
        for fd in self.write_fds:
            os.close(fd)
        for stream in self.streams:
            stream.close()
        for experiment in self.experiments:
            experiment.selector.close()
            experiment.events.close()
        self.directory.cleanup()

    def experiment(self):
        experiment = scale.Experiment.__new__(scale.Experiment)
        experiment.work = Path(self.directory.name)
        experiment.repo = Path(scale.__file__).resolve().parent.parent
        experiment.password = 'private-test-input'
        experiment.args = SimpleNamespace(count=2, mode="gui")
        experiment.events = io.StringIO()
        experiment.selector = selectors.DefaultSelector()
        experiment.output_buffers = {}
        experiment.records, experiment.pids, experiment.resource_pids = [], {}, {}
        experiment.ready, experiment.prepared, experiment.finished = set(), set(), set()
        experiment.steady, experiment.steady_ends, experiment.steady_finished = {}, {}, set()
        experiment.workers, experiment.errorlogs = [], []
        experiment.server, experiment.db, experiment.created = None, None, False
        experiment.summary = {'limitations': []}
        experiment.sample_rows = []
        experiment.closed_workers = set()
        experiment.private_directories = set()
        self.experiments.append(experiment)
        return experiment

    def pipe(self, experiment, worker=0):
        read_fd, write_fd = os.pipe()
        stream = os.fdopen(read_fd, 'rb', buffering=0)
        experiment.selector.register(stream, selectors.EVENT_READ, worker)
        self.streams.append(stream)
        self.write_fds.append(write_fd)
        return write_fd

    def close_writer(self, fd):
        os.close(fd)
        self.write_fds.remove(fd)

    def test_gui_emit_keeps_numeric_fields_when_invite_code_collides(self):
        private = set(gui._private_values)
        try:
            gui._private_values.clear()
            gui._private_values.add('12345678')
            output = io.StringIO()
            with contextlib.redirect_stdout(output), mock.patch.object(gui.time, 'time', return_value=12345678.5), mock.patch.object(gui.time, 'time_ns', return_value=123456789012345678):
                gui.emit('privacy-check', code='12345678', counter=12345678, ok=True)
            record = json.loads(output.getvalue())
            self.assertEqual(record['code'], '[private]')
            self.assertEqual(record['timestamp'], 12345678.5)
            self.assertEqual(record['utc_ns'], 123456789012345678)
            self.assertEqual(record['counter'], 12345678)
            self.assertIs(record['ok'], True)
        finally:
            gui._private_values.clear()
            gui._private_values.update(private)

    def test_gui_redaction_reaches_nested_strings_without_mutating_input(self):
        private = set(gui._private_values)
        try:
            gui._private_values.clear()
            gui._private_values.update({'ABCD-EFGH', 'ABCDEFGH', 'fixture-password'})
            original = {'error': 'code ABCD-EFGH', 'nested': [
                {'text': 'normalized ABCDEFGH'}, ('fixture-password', 19, False, None)]}
            redacted = gui.redact_fields(original)
            self.assertEqual(redacted, {'error': 'code [private]', 'nested': [
                {'text': 'normalized [private]'}, ['[private]', 19, False, None]]})
            self.assertEqual(original['error'], 'code ABCD-EFGH')
            self.assertEqual(original['nested'][1][0], 'fixture-password')
            self.assertEqual(json.loads(json.dumps(redacted)), redacted)
        finally:
            gui._private_values.clear()
            gui._private_values.update(private)

    def test_gui_exception_text_redacts_registered_secret_forms(self):
        private = set(gui._private_values)
        try:
            gui._private_values.clear()
            gui._private_values.update({'ABCD-EFGH', 'ABCDEFGH', 'fixture-password'})
            self.assertEqual(gui.redact_text('failure ABCD-EFGH / ABCDEFGH / fixture-password'),
                             'failure [private] / [private] / [private]')
        finally:
            gui._private_values.clear()
            gui._private_values.update(private)

    def preference_experiment(self):
        experiment = self.experiment()
        experiment.args.daily_pilot = True
        experiment.actors = [{'id': 7, 'direct_id': 23, 'group_id': 22},
                             {'id': 8, 'direct_id': 23, 'group_id': 22}]
        output = io.StringIO()
        experiment.workers = [SimpleNamespace(stdin=output)]
        experiment.db = mock.MagicMock()
        cursor = experiment.db.cursor.return_value.__enter__.return_value
        return experiment, cursor, output

    def test_preference_barrier_acknowledges_only_after_readonly_state_matches(self):
        experiment, cursor, output = self.preference_experiment()
        rows = iter([(False, False), (False, True)])
        def next_row():
            self.assertEqual(output.getvalue(), '', 'No ACK before the expected database fact')
            return next(rows)
        cursor.fetchone.side_effect = next_row
        with mock.patch.object(scale.time, 'sleep'):
            experiment.accept_record(0, {'event': 'preference-observe', 'actor': 7,
                                        'conversation': 22, 'field': 'pinned', 'expected': True})
        self.assertEqual(cursor.execute.call_count, 2)
        for call in cursor.execute.call_args_list:
            statement, parameters = call.args
            normalized = ' '.join(statement.split())
            self.assertEqual(normalized.split('FROM', 1)[0].replace(' ', ''), 'SELECTmuted,pinned')
            self.assertIn('FROM conversation_members', normalized)
            self.assertEqual(statement.count('%s'), 2)
            self.assertEqual(parameters, (22, 7))
        records = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]['command'], 'preference-observed')

    def test_preference_barrier_rejects_invalid_context_without_ack_or_sql(self):
        invalid = [{'field': 'not-a-setting'}, {'expected': 1}, {'expected': 'true'},
                   {'actor': 9}, {'conversation': 99}]
        for changed in invalid:
            with self.subTest(changed=changed):
                experiment, cursor, output = self.preference_experiment()
                record = {'event': 'preference-observe', 'actor': 7, 'conversation': 22,
                          'field': 'pinned', 'expected': True}
                record.update(changed)
                with self.assertRaises(AssertionError):
                    experiment.accept_record(0, record)
                cursor.execute.assert_not_called()
                self.assertEqual(output.getvalue(), '')

    def test_preference_barrier_requires_gui_daily_scope(self):
        for mode, daily in [('gui', False), ('tui', True)]:
            with self.subTest(mode=mode, daily=daily):
                experiment, cursor, output = self.preference_experiment()
                experiment.args.mode, experiment.args.daily_pilot = mode, daily
                with self.assertRaises(AssertionError):
                    experiment.accept_record(0, {'event': 'preference-observe', 'actor': 7,
                                                'conversation': 22, 'field': 'pinned', 'expected': True})
                cursor.execute.assert_not_called()
                self.assertEqual(output.getvalue(), '')

    def test_one_pipe_write_delivers_all_json_events(self):
        experiment = self.experiment()
        fd = self.pipe(experiment)
        records = [{'event': 'ready'}, {'event': 'steady-ready'},
                   {'event': 'steady-start', 'monotonic': 100.0},
                   {'event': 'steady-finished', 'monotonic': 702.0},
                   {'event': 'completed', 'ok': True}]
        os.write(fd, ''.join(json.dumps(row) + '\n' for row in records).encode())
        experiment.read_events(0)
        self.assertEqual([r['event'] for r in experiment.records],
                         ['ready', 'steady-ready', 'steady-start', 'steady-finished', 'completed'])
        self.assertEqual(experiment.prepared, {0})
        self.assertEqual(experiment.finished, {0})
        self.assertFalse(experiment.selector.select(0), 'All events must be processed even after the OS pipe is drained')

    def test_both_shards_reach_barriers_from_bursts(self):
        experiment = self.experiment()
        for worker in (0, 1):
            fd = self.pipe(experiment, worker)
            os.write(fd, b'{"event":"ready"}\n{"event":"steady-ready"}\n')
        experiment.read_events(0)
        self.assertEqual(experiment.ready, {0, 1})
        self.assertEqual(experiment.prepared, {0, 1})

    def test_external_diagnostic_does_not_forge_readiness(self):
        experiment = self.experiment()
        fd = self.pipe(experiment)
        os.write(fd, b'dbus service says ready private-test-input\n{"event":"steady-ready"}\n')
        experiment.read_events(0)
        self.assertEqual(experiment.ready, set())
        self.assertEqual(experiment.prepared, {0})
        self.assertEqual([r['event'] for r in experiment.records], ['steady-ready'])
        diagnostic = (experiment.work / 'worker-000.stdout-diagnostic.log').read_text()
        self.assertIn('dbus service says ready', diagnostic)
        self.assertNotIn(experiment.password, diagnostic)
        self.assertIn('[REDACTED]', diagnostic)

    def test_incomplete_json_at_eof_is_failure(self):
        experiment = self.experiment()
        fd = self.pipe(experiment)
        os.write(fd, b'{"event":"ready"}')
        self.close_writer(fd)
        experiment.read_events(0)
        with self.assertRaisesRegex(RuntimeError, 'Incomplete worker event at EOF'):
            experiment.read_events(0)
        self.assertEqual(experiment.ready, set())

    def test_percentiles_use_nearest_rank_and_empty_is_unknown(self):
        self.assertEqual(scale.percentile(list(range(100, 0, -1))),
                         {'samples': 100, 'p50_ms': 50, 'p95_ms': 95, 'p99_ms': 99})
        self.assertEqual(scale.percentile([]),
                         {'samples': 0, 'p50_ms': None, 'p95_ms': None, 'p99_ms': None})
        self.assertEqual(scale.percentile([0.3334]),
                         {'samples': 1, 'p50_ms': 0.333, 'p95_ms': 0.333, 'p99_ms': 0.333})

    def business_experiment(self):
        experiment = self.experiment()
        # Prefix collisions previously counted one send twice. A reply's response
        # is independently persisted, not a second copy of the original marker.
        bodies = [(101, 7, 'marker1 hello'), (110, 8, 'marker10 中文'),
                  (201, 7, 'reply hello'), (202, 8, 'reply-response response')]
        experiment.db = Rows([(identity, 22, sender, body, False, False,
                               201 if identity == 202 else None)
                              for identity, sender, body in bodies])
        experiment.records = [
            {'event': 'operation', 'kind': 'send-message', 'actor': sender,
             'marker': body.split()[0], 'conversation': 22, 'ok': True,
             'elapsed_ms': 5, 'phase': 'steady'}
            for _, sender, body in bodies]
        experiment.pids = {7: 700, 8: 800}
        experiment.actors = [{'id': 7, 'peer_id': 8, 'direct_id': 22},
                             {'id': 8, 'peer_id': 7, 'direct_id': 22}]
        return experiment

    def test_marker_prefixes_and_reply_response_have_exact_facts(self):
        experiment = self.business_experiment()
        experiment.verify_business()
        facts = json.loads((experiment.work / 'message-facts.json').read_text())
        self.assertEqual({row['marker']: row['matching_ids'] for row in facts['marker_facts']},
                         {'marker1': [101], 'marker10': [110], 'reply': [201], 'reply-response': [202]})
        self.assertEqual(experiment.summary['counts']['send_markers'], 4)
        self.assertEqual(facts['database_state_counts']['replies'], 1)

    def test_marker_inside_other_body_is_not_send_proof(self):
        experiment = self.business_experiment()
        experiment.db.rows[0] = (101, 22, 7, 'unrelated quoted marker1', False, False, None)
        with self.assertRaisesRegex(AssertionError, 'Native send marker missing or duplicated'):
            experiment.verify_business()

    def test_send_requires_correct_sender(self):
        experiment = self.business_experiment()
        experiment.db.rows[0] = (101, 22, 8, 'marker1 hello', False, False, None)
        with self.assertRaisesRegex(AssertionError, 'wrong conversation or sender'):
            experiment.verify_business()

    def observed_send_experiment(self):
        experiment = self.business_experiment()
        experiment.records[0]['marker'] = 'markerX'
        experiment.db.rows[0] = (101, 22, 7, 'markerX hello', False, False, None)
        experiment.records.append({'event': 'send-start', 'actor': 7,
                                   'marker': 'markerX', 'conversation': 22})
        for kind in ('direct-unread-visible', 'direct-read-visible'):
            experiment.records.append({'event': 'operation', 'kind': kind, 'actor': 8,
                                       'marker': 'markerX', 'conversation': 22,
                                       'ok': True, 'elapsed_ms': 5, 'phase': 'prewarm'})
        return experiment

    def test_receiving_observations_do_not_replace_the_message_sender(self):
        experiment = self.observed_send_experiment()
        experiment.verify_business()
        facts = json.loads((experiment.work / 'message-facts.json').read_text())
        fact = next(row for row in facts['marker_facts'] if row['marker'] == 'markerX')
        self.assertEqual(fact['matching_ids'], [101])
        self.assertEqual(experiment.summary['counts']['send_markers'], 4)
        self.assertEqual(experiment.summary['counts']['successful_business_operations'], 6)

    def test_receiving_observations_do_not_allow_a_wrong_message_sender(self):
        experiment = self.observed_send_experiment()
        experiment.db.rows[0] = (101, 22, 8, 'markerX hello', False, False, None)
        with self.assertRaisesRegex(AssertionError, 'wrong conversation or sender'):
            experiment.verify_business()

    def test_receiving_observations_do_not_allow_a_wrong_conversation(self):
        experiment = self.observed_send_experiment()
        experiment.db.rows[0] = (101, 33, 7, 'markerX hello', False, False, None)
        with self.assertRaisesRegex(AssertionError, 'wrong conversation or sender'):
            experiment.verify_business()

    def title_experiment(self):
        experiment = self.business_experiment()
        # A real newly-created group has no numeric ID available to the UI.
        # The remaining markers still target their original numeric direct ID.
        experiment.db.relations['conversations'].extend([
            (33, 'group', None, None, 7, None, None, 'Daily final 中文'),
            (44, 'group', None, None, 7, None, None, 'Different group'),
        ])
        experiment.db.relations['conversation_members'].extend([
            (33, 7, False, 0), (33, 8, False, 0),
        ])
        experiment.db.rows[0] = (101, 33, 7, 'marker1 hello', False, False, None)
        operation = experiment.records[0]
        operation.pop('conversation')
        operation['conversation_title'] = 'Daily final 中文'
        return experiment

    def test_unique_group_title_preserves_numeric_message_validation(self):
        experiment = self.title_experiment()
        experiment.verify_business()
        facts = json.loads((experiment.work / 'message-facts.json').read_text())
        self.assertEqual({row['marker']: row['matching_ids'] for row in facts['marker_facts']},
                         {'marker1': [101], 'marker10': [110], 'reply': [201], 'reply-response': [202]})

    def test_group_title_must_exist_exactly(self):
        experiment = self.title_experiment()
        experiment.records[0]['conversation_title'] = 'Daily final'
        with self.assertRaises(AssertionError):
            experiment.verify_business()

    def test_group_title_rejects_message_in_other_conversation(self):
        experiment = self.title_experiment()
        experiment.db.rows[0] = (101, 44, 7, 'marker1 hello', False, False, None)
        with self.assertRaises(AssertionError):
            experiment.verify_business()

    def test_group_title_requires_expected_sender(self):
        experiment = self.title_experiment()
        experiment.db.rows[0] = (101, 33, 8, 'marker1 hello', False, False, None)
        with self.assertRaises(AssertionError):
            experiment.verify_business()

    def test_group_title_ambiguity_cannot_prove_destination(self):
        experiment = self.title_experiment()
        experiment.db.relations['conversations'].append(
            (45, 'group', None, None, 7, None, None, 'Daily final 中文'))
        with self.assertRaises(AssertionError):
            experiment.verify_business()

    def edited_experiment(self, suffix):
        experiment = self.business_experiment()
        experiment.db.rows[0] = (101, 22, 7, 'marker1' + suffix + ' revised', False, True, None)
        experiment.records.append({'event': 'operation', 'kind': 'edit',
                                   'actor': 7, 'marker': 'marker1', 'conversation': 22,
                                   'ok': True, 'elapsed_ms': 5, 'phase': 'steady'})
        return experiment

    def test_exact_edited_marker_with_flag_is_valid(self):
        for suffix in ('_EDIT', '-edited'):
            with self.subTest(suffix=suffix):
                experiment = self.edited_experiment(suffix)
                experiment.verify_business()
                facts = json.loads((experiment.work / 'message-facts.json').read_text())
                fact = next(row for row in facts['marker_facts'] if row['marker'] == 'marker1')
                self.assertTrue(fact['ui_edited'])
                self.assertEqual(facts['database_state_counts']['edited'], 1)

    def test_duplicate_edited_marker_is_rejected(self):
        for suffix in ('_EDIT', '-edited'):
            with self.subTest(suffix=suffix):
                experiment = self.edited_experiment(suffix)
                experiment.db.rows.append((102, 22, 7, 'marker1' + suffix + ' duplicate',
                                           False, True, None))
                with self.assertRaises(AssertionError):
                    experiment.verify_business()

    def test_edited_marker_requires_original_sender(self):
        for suffix in ('_EDIT', '-edited'):
            with self.subTest(suffix=suffix):
                experiment = self.edited_experiment(suffix)
                experiment.db.rows[0] = (101, 22, 8, 'marker1' + suffix + ' revised', False, True, None)
                with self.assertRaises(AssertionError):
                    experiment.verify_business()

    def test_edited_marker_requires_database_flag(self):
        experiment = self.edited_experiment('_EDIT')
        experiment.db.rows[0] = (101, 22, 7, 'marker1_EDIT revised', False, False, None)
        with self.assertRaises(AssertionError):
            experiment.verify_business()

    def common_window_experiment(self):
        experiment = self.business_experiment()
        # Worker 0 starts first and worker 1 finishes last; their common
        # wall-clock workload interval is [102, 198]. Mixed clock fields are
        # deliberate: utc_ns wins over a conflicting legacy timestamp.
        experiment.records.extend([
            {'event': 'steady-start', 'worker': 0, 'utc_ns': 100_000_000_000, 'timestamp': 999},
            {'event': 'steady-start', 'worker': 1, 'timestamp': 102.0},
            {'event': 'steady-finished', 'worker': 0, 'timestamp': 198.0},
            {'event': 'steady-finished', 'worker': 1, 'utc_ns': 200_000_000_000, 'timestamp': 1},
        ])
        return experiment

    def test_operation_latency_requires_full_common_window_interval(self):
        experiment = self.common_window_experiment()
        # End in the window alone is insufficient: the third operation began
        # before the last shard was ready. The fourth ends after the first ended.
        for record, end, duration in zip(experiment.records[:4],
                                         (103.0, 198.0, 102.5, 198.001),
                                         (1000.0, 500.0, 1000.0, 1.0)):
            record.update(utc_ns=int(end * 1_000_000_000), timestamp=999,
                          elapsed_ms=duration)
        experiment.verify_business()
        expected = {'samples': 2, 'p50_ms': 500.0, 'p95_ms': 1000.0, 'p99_ms': 1000.0}
        self.assertEqual(experiment.summary['latency']['ui_operation_steady'], expected)
        self.assertEqual(experiment.summary['latency']['native_send_confirmation_steady'], expected)

    def test_cross_client_latency_requires_both_events_in_common_window(self):
        experiment = self.common_window_experiment()
        for record in experiment.records[:4]:
            record.update(timestamp=150.0)
        for marker, sender, begin, end, use_ns in [
            ('marker1', 7, 102.0, 103.0, True),
            ('marker10', 8, 101.999, 103.0, False),
            ('reply', 7, 197.0, 198.001, True),
            ('reply-response', 8, 197.0, 198.0, False),
        ]:
            sent = {'event': 'send-start', 'actor': sender, 'marker': marker,
                    'conversation': 22, 'phase': 'steady', 'timestamp': begin}
            received = {'event': 'screen-received', 'actor': 15 - sender,
                        'marker': marker, 'phase': 'steady', 'timestamp': end}
            if use_ns:
                sent.update(utc_ns=int(begin * 1_000_000_000), timestamp=999)
                received.update(utc_ns=int(end * 1_000_000_000), timestamp=999)
            experiment.records.extend([sent, received])
        experiment.verify_business()
        self.assertEqual(experiment.summary['latency']['end_to_end_native_steady'],
                         {'samples': 2, 'p50_ms': 1000.0, 'p95_ms': 1000.0, 'p99_ms': 1000.0})

    def message_matches(self, name, marker):
        # Exercise the native driver's legacy substring predicate until its
        # pure matcher exists; this gives semantic RED, not a missing-symbol error.
        matcher = getattr(gui, 'message_name_matches', None)
        return matcher(name, marker) if matcher else marker in name

    def test_gui_body_requires_exact_first_token(self):
        marker = 'native-104302-1-wave-1'
        self.assertTrue(self.message_matches('你；2026-10-11 12:00；' + marker + ' 中文 😀\nsecond line；已读', marker))
        self.assertFalse(self.message_matches('你；2026-10-11 12:00；unrelated ' + marker + ' quoted text', marker))

    def test_gui_wave_and_actor_prefixes_do_not_match(self):
        marker = 'native-104302-1-wave-1'
        for other in ('native-104302-1-wave-10', 'native-104302-10-wave-1',
                      'native-104302-1-wave-1-extra'):
            with self.subTest(other=other):
                self.assertFalse(self.message_matches('alice；2026-10-11 12:00；' + other + ' hello', marker))

    def test_gui_reply_quote_is_not_original_body(self):
        marker = 'native-104302-1-deep-reply'
        name = '你；2026-10-11 12:00；回复 alice：' + marker + ' hello；' + marker + '-response 回复正文；已读 1 人'
        self.assertFalse(self.message_matches(name, marker))
        self.assertTrue(self.message_matches(name, marker + '-response'))

    def test_gui_metadata_is_not_body(self):
        self.assertFalse(self.message_matches('marker1；2026-10-11 12:00；unrelated body；已读', 'marker1'))
        self.assertFalse(self.message_matches('你；2026-10-11 12:00；hello body；反应 👍 1 人', '👍'))

    def test_gui_attachment_requires_complete_unicode_filename(self):
        filename = '附件 中文 空格 😀.bin'
        for kind in ('文件', '图片'):
            with self.subTest(kind=kind):
                name = '你；2026-10-11 12:00；' + kind + '：' + filename + '\n1.0 KiB；已读'
                self.assertTrue(self.message_matches(name, filename))
                self.assertFalse(self.message_matches(name, '附件 中文'))
                self.assertFalse(self.message_matches(name, filename + '.extra'))

    def test_gui_attachment_filename_in_quote_is_not_attachment_body(self):
        filename = '图片 中文 文件 😀.png'
        name = 'alice；2026-10-11 12:00；回复 bob：图片：' + filename + '\n2.0 KiB；response hello；已读 1 人'
        self.assertFalse(self.message_matches(name, filename))
        self.assertTrue(self.message_matches(name, 'response'))

    def test_tui_launch_does_not_require_window_manager(self):
        experiment = self.experiment()
        experiment.actors = [{'id': 7}, {'id': 8}]
        experiment.args = SimpleNamespace(mode='tui', openbox='openbox', shard_size=40,
            seed=1, pilot_seconds=30, deep_pilot=False, lifecycle_pilot=False,
            job_control_pilot=False, daily_pilot=False,
            role_pilot=False, blackhole_pilot=False, read_pilot=False, heartbeat_pilot=False)
        experiment.config = {'worker_seconds': 32, 'history_diagnostic': False}
        experiment.safety = {'mem_available_min_kib': 1, 'disk_free_min_bytes': 1}
        experiment.url = 'ws://127.0.0.1:12345'
        experiment.client_binary = lambda: Path('/owned/test-client')
        process = SimpleNamespace(stdin=io.StringIO(), stdout=io.StringIO())
        self.streams.extend((process.stdin, process.stdout))
        try:
            with mock.patch.object(scale, 'host', return_value={
                    'MemAvailable_kib': 1024, 'disk_free_bytes': 1024}), \
                 mock.patch.object(scale.subprocess, 'Popen', return_value=process) as start, \
                 mock.patch.object(scale.shutil, 'which',
                                   side_effect=AssertionError('TUI must not require Openbox')), \
                 mock.patch.object(experiment.selector, 'register'):
                experiment.launch()
            start.assert_called_once()
            config = json.loads(process.stdin.getvalue())
            self.assertNotIn('openbox', config)
            self.assertNotIn('openbox_data', config)
        finally:
            for logfile in experiment.errorlogs:
                logfile.close()

    def test_gui_launch_resolves_openbox_path_before_start(self):
        for requested in ('openbox', '/private/bin/openbox'):
            with self.subTest(requested=requested):
                experiment = self.experiment()
                experiment.actors = [{'id': 7}, {'id': 8}]
                experiment.args = SimpleNamespace(mode='gui', openbox=requested, shard_size=40,
                    seed=1, pilot_seconds=30, deep_pilot=False, lifecycle_pilot=False,
                    job_control_pilot=False, daily_pilot=False,
            role_pilot=False, blackhole_pilot=False, read_pilot=False, heartbeat_pilot=False)
                experiment.config = {'worker_seconds': 32, 'history_diagnostic': False}
                experiment.safety = {'mem_available_min_kib': 1, 'disk_free_min_bytes': 1}
                experiment.url = 'ws://127.0.0.1:12345'
                experiment.client_binary = lambda: Path('/owned/test-client')
                process = SimpleNamespace(stdin=io.StringIO(), stdout=io.StringIO())
                self.streams.extend((process.stdin, process.stdout))
                try:
                    with mock.patch.object(scale, 'host', return_value={
                            'MemAvailable_kib': 1024, 'disk_free_bytes': 1024}), \
                         mock.patch.object(scale.subprocess, 'Popen', return_value=process), \
                         mock.patch.object(scale.shutil, 'which',
                                           return_value='/private/bin/openbox') as resolve, \
                         mock.patch.object(experiment.selector, 'register'):
                        experiment.launch()
                    resolve.assert_called_once_with(requested)
                    config = json.loads(process.stdin.getvalue())
                    self.assertEqual(config['openbox'], '/private/bin/openbox')
                    self.assertEqual(config['openbox_data'], '/private/share')
                finally:
                    for logfile in experiment.errorlogs:
                        logfile.close()

    def test_missing_gui_openbox_rejected_before_any_native_process(self):
        experiment = self.experiment()
        experiment.actors = [{'id': 7}, {'id': 8}]
        experiment.args = SimpleNamespace(mode='gui', openbox='missing-openbox', shard_size=40,
            seed=1, pilot_seconds=30, deep_pilot=False, lifecycle_pilot=False,
            job_control_pilot=False, daily_pilot=False,
            role_pilot=False, blackhole_pilot=False, read_pilot=False, heartbeat_pilot=False)
        experiment.config = {'worker_seconds': 32, 'history_diagnostic': False}
        experiment.safety = {'mem_available_min_kib': 1, 'disk_free_min_bytes': 1}
        experiment.url = 'ws://127.0.0.1:12345'
        experiment.client_binary = lambda: Path('/owned/test-client')
        process = SimpleNamespace(stdin=io.StringIO(), stdout=io.StringIO())
        self.streams.extend((process.stdin, process.stdout))
        try:
            with mock.patch.object(scale, 'host', return_value={
                    'MemAvailable_kib': 1024, 'disk_free_bytes': 1024}), \
                 mock.patch.object(scale.subprocess, 'Popen', return_value=process) as start, \
                 mock.patch.object(scale.shutil, 'which', return_value=None), \
                 mock.patch.object(experiment.selector, 'register'):
                with self.assertRaises(FileNotFoundError):
                    experiment.launch()
                start.assert_not_called()
        finally:
            for logfile in experiment.errorlogs:
                logfile.close()

    def test_gui_popup_geometry_error_only_matches_destroyed_target(self):
        # daily02 recorded BadDrawable for XGetGeometry(14) of the popup
        # that had just closed. Other errors must remain driver failures.
        window = 0x8000D4
        for code, request, resource, expected in (
                (3, 14, window, True), (9, 14, window, True),
                (9, 40, window, False), (9, 20, window, False),
                (2, 14, window, False), (3, 14, window + 1, False),
                (9, 14, window + 1, False), (0, 14, window, False)):
            with self.subTest(code=code, request=request, resource=resource):
                error = {'error_code': code, 'request_code': request,
                         'resource_id': resource, 'serial': 1234}
                self.assertIs(gui.is_transient_popup_geometry_error(error, window),
                              expected)

    def test_gui_popup_translate_error_only_matches_destroyed_target(self):
        # daily04: the same owned popup vanished after GetGeometry and
        # before TranslateCoordinates, producing BadWindow/request40.
        window = 8388816
        for code, resource, expected in ((3, window, True),
                                         (9, window, False),
                                         (3, window + 1, False)):
            with self.subTest(code=code, resource=resource):
                self.assertIs(gui.is_transient_popup_geometry_error(
                    {'error_code': code, 'request_code': 40, 'resource_id': resource},
                    window), expected)

    def test_tui_sidebar_tracks_target_reordered_above_selection(self):
        # actor37's actual failed frame had Medium 1, Scale 4, then the
        # selected direct. Activity can move Medium 1 above the current ID.
        frames = (
            '│  Medium 1              13:33 │ history │\n'
            '│> Scale 4               13:32 │ history │\n'
            '│  nf51bd85a_0037 · 在线       │ history │',
            '│  Medium 1              13:33 │ history │\n'
            '│  Scale 4               13:32 │ history │\n'
            '│> nf51bd85a_0037 · 在线       │ history │',
        )
        for frame in frames:
            with self.subTest(frame=frame):
                self.assertEqual(tui.sidebar_step(frame, 'Medium 1'), 'Up')

    def test_tui_sidebar_target_below_selection_keeps_down(self):
        frame = ('│> Scale 4               13:32 │ history │\n'
                 '│  Medium 1              13:33 │ history │\n'
                 '│  nf51bd85a_0037 · 在线       │ history │')
        self.assertEqual(tui.sidebar_step(frame, 'Medium 1'), 'Down')
        self.assertEqual(tui.sidebar_step(frame.replace('Medium 1', 'Medium …'),
                                          'Medium 1'), 'Down')

    def test_tui_sidebar_title_requires_full_name_boundary(self):
        cases = (
            ('> Scale 1               13:33', 'Scale 1', True),
            ('> Scale 10              13:33', 'Scale 1', False),
            ('> Scale 1-user10        13:33', 'Scale 1', False),
            ('> 张 三 · 在线            13:33', '张 三', True),
            ('> Alice Bob             免打扰 13:33', 'Alice', False),
            ('> Alice Bob             免打扰 13:33', 'Alice Bob', True),
        )
        for label, title, expected in cases:
            with self.subTest(label=label, title=title):
                self.assertEqual(tui.sidebar_title_matches(label, title), expected)

    def test_tui_receipt_metadata_accepts_native_edge_glyphs(self):
        for edge, state in [('┃│', '已读'), ('╻│', '已发送'), ('╹│', '已读'), ('│', '已发送')]:
            with self.subTest(edge=edge, state=state):
                line = '                               \x1b[2m' + state + '                                                   \x1b[0m' + edge
                self.assertTrue(tui.receipt_metadata_matches(line, state))
        self.assertTrue(tui.receipt_metadata_matches('  \x1b[0;2m已读\x1b[0m┃│', '已读'))

    def test_tui_receipt_metadata_requires_exact_state(self):
        for line in ['  \x1b[2m已发送\x1b[0m┃│',
                     '  \x1b[2m已读 多余正文\x1b[0m┃│',
                     '  \x1b[2mmarker 已读\x1b[0m┃│',
                     '  \x1b[2m已读回复\x1b[0m┃│']:
            with self.subTest(line=line):
                self.assertFalse(tui.receipt_metadata_matches(line, '已读'))

    def test_tui_receipt_metadata_rejects_body_and_truecolor_digit_two(self):
        for line in ['  已读 │', '  已读 ┃│',
                     '  \x1b[38;2;100;200;150m已读\x1b[0m┃│',
                     '  \x1b[1m已读\x1b[0m│']:
            with self.subTest(line=line):
                self.assertFalse(tui.receipt_metadata_matches(line, '已读'))

    def test_closed_event_requires_zero_exit_status(self):
        experiment = self.experiment()
        with self.assertRaises(AssertionError):
            experiment.accept_record(0, {'event': 'closed', 'clients': [
                {'actor': 7, 'exit_status': -15, 'native_close': False}]})

    def test_closed_event_requires_native_close(self):
        experiment = self.experiment()
        with self.assertRaises(AssertionError):
            experiment.accept_record(0, {'event': 'closed', 'clients': [
                {'actor': 7, 'exit_status': 0, 'native_close': False}]})

    def test_completed_worker_without_closed_event_is_failure(self):
        experiment = self.experiment()
        experiment.workers = [FinishedWorker(0)]
        experiment.finished = {0}
        self.assertFalse(experiment.cleanup(),
                         'Completed work does not prove owned native clients closed')

    def test_normal_native_close_allows_cleanup(self):
        experiment = self.experiment()
        experiment.workers = [FinishedWorker(0)]
        experiment.finished = {0}
        experiment.accept_record(0, {'event': 'closed', 'clients': [
            {'actor': 7, 'exit_status': 0, 'native_close': True}]})
        self.assertTrue(experiment.cleanup())

    def test_owned_private_runtime_is_removed_after_worker_exit(self):
        experiment = self.experiment()
        with tempfile.TemporaryDirectory(prefix='chat-native-gui-') as directory:
            private = Path(directory)
            (private / 'owned-file').write_text('fixture')
            experiment.accept_record(0, {'event': 'private-runtime', 'path': directory})
            self.assertTrue(experiment.cleanup())
            self.assertFalse(private.exists())

    def test_observer_already_exited_requires_proven_zero_exit(self):
        for code in ('1', '01', None):
            with self.subTest(code=code):
                server = observer.Server('owned-test-server', 12345, 128, 8,
                                         self.directory.name)
                server._exit_code = code
                with mock.patch.object(server, '_owned', return_value=False), \
                     mock.patch('native_server_observer.os.kill',
                                side_effect=AssertionError('No owned live process')):
                    result = server.stop()
                self.assertEqual(result['status'], 'FAIL')
                self.assertTrue(result['owned_inferior_exited'])
                self.assertFalse(result['forced_cleanup'])

    def test_observer_already_exited_zero_mi_codes_are_normal(self):
        for code in ('0', '00'):
            with self.subTest(code=code):
                server = observer.Server('owned-test-server', 12345, 128, 8,
                                         self.directory.name)
                server._exit_code = code
                with mock.patch.object(server, '_owned', return_value=False), \
                     mock.patch('native_server_observer.os.kill',
                                side_effect=AssertionError('No owned live process')):
                    result = server.stop()
                self.assertEqual(result['status'], 'PASS')
                self.assertEqual(result['inferior_exit_code'], code)

    def test_gui_rejected_login_does_not_count_as_authenticated(self):
        driver = gui.NativeGui.__new__(gui.NativeGui)
        process = SimpleNamespace(pid=12345)
        driver.clients = {7: process}
        driver.Atspi = SimpleNamespace(StateType=SimpleNamespace(SENSITIVE='sensitive'))
        node = mock.Mock()
        node.get_text_iface.return_value.get_character_count.return_value = 2
        node.get_state_set.return_value.contains.return_value = True
        driver.field = mock.Mock(return_value=node)
        driver.find = mock.Mock(return_value=None)
        driver.paste, driver.click, driver.key = mock.Mock(), mock.Mock(), mock.Mock()
        driver.wait = lambda predicate, description: self.assertTrue(predicate(), description)
        actor = {'id': 7, 'username': 'fixture-user'}
        with mock.patch('native_gui_scale.emit') as emitted:
            driver.authenticate_existing(actor, 'ab', expected_error='用户名或密码错误')
            self.assertEqual([call.args[0] for call in emitted.call_args_list], ['login-rejected'])
            self.assertEqual(emitted.call_args.kwargs['pid'], process.pid)
            self.assertEqual(emitted.call_args.kwargs['reason'], '用户名或密码错误')
            driver.field.assert_any_call(7, '用户名或密码错误')
            driver.find.assert_called_with(7, '主菜单')
            driver.authenticate_existing(actor, 'ab')
            self.assertEqual([call.args[0] for call in emitted.call_args_list],
                             ['login-rejected', 'authenticated'])
            self.assertEqual(emitted.call_args.kwargs['pid'], process.pid)
        self.assertIs(driver.clients[7], process)

    def test_gui_property_error_only_matches_destroyed_target(self):
        window = 0x8000d4
        cases = [
            ({'error_code': 3, 'request_code': 20, 'resource_id': window}, True),
            ({'error_code': 9, 'request_code': 20, 'resource_id': window}, False),
            ({'error_code': 3, 'request_code': 14, 'resource_id': window}, False),
            ({'error_code': 3, 'request_code': 20, 'resource_id': window + 1}, False),
            ({'error_code': 2, 'request_code': 20, 'resource_id': window}, False),
        ]
        for error, expected in cases:
            with self.subTest(error=error):
                self.assertIs(gui.is_transient_window_property_error(error, window), expected)

    def test_gui_navigation_clicks_owned_main_menu_then_destination(self):
        for index, label in enumerate(['我的资料', '聊天', '联系人', '新的朋友']):
            with self.subTest(label=label):
                driver = gui.NativeGui.__new__(gui.NativeGui)
                driver.native = object()
                driver.Atspi = SimpleNamespace(Role=SimpleNamespace(MENU_ITEM='menu-item'))
                button = object()
                driver.field = mock.Mock(return_value=button)
                driver.click = mock.Mock()
                rectangle = {'window': 12, 'x': 30, 'y': 60, 'width': 180, 'height': 174}
                driver.popup = mock.Mock(side_effect=[rectangle, None])
                class RemovedAccessible(Exception):
                    pass
                driver.GLib = SimpleNamespace(GError=RemovedAccessible)
                removed = mock.Mock()
                removed.get_role.side_effect = RemovedAccessible('Qt transient destroyed')
                driver.nodes = mock.Mock(return_value=[removed])
                driver.wait = lambda predicate, description: self.assertTrue(predicate(), description) or rectangle
                driver.xt, driver.lib = mock.Mock(), mock.Mock()
                with mock.patch('native_gui_scale.emit'):
                    driver.navigate(7, label)
                driver.field.assert_called_once_with(7, '主菜单')
                driver.click.assert_called_once_with(7, button)
                driver.xt.XTestFakeMotionEvent.assert_called_once_with(
                    driver.native, -1, 120, round(67 + (index + .5) * 40), 0)
                self.assertEqual(driver.xt.XTestFakeButtonEvent.call_args_list,
                    [mock.call(driver.native, 1, True, 0), mock.call(driver.native, 1, False, 0)])
                driver.lib.XFlush.assert_called_once_with(driver.native)

    def test_gui_navigation_rejects_unrelated_action_before_any_input(self):
        driver = gui.NativeGui.__new__(gui.NativeGui)
        driver.click = mock.Mock()
        with self.assertRaises(ValueError):
            driver.navigate(7, '聊天信息')
        driver.click.assert_not_called()

    def test_gui_latest_initializes_native_navigation_before_end(self):
        for diagnostics in (False, True):
            with self.subTest(diagnostics=diagnostics):
                driver = gui.NativeGui.__new__(gui.NativeGui)
                driver.cfg = {'history_diagnostic': diagnostics}
                view = object()
                driver.field = mock.Mock(return_value=view)
                driver.click = mock.Mock()
                driver.key = mock.Mock()
                driver.history_observation = mock.Mock()
                driver.wait = mock.Mock(side_effect=AssertionError('Latest must not add a retry or wait'))
                driver.latest(7)
                driver.field.assert_called_once_with(7, '消息记录')
                driver.click.assert_called_once_with(7, view, x_margin=2)
                self.assertEqual(driver.key.call_args_list, [mock.call('Home'), mock.call('End')])
                self.assertEqual(driver.history_observation.call_args_list,
                    [mock.call(7, 'before-end'), mock.call(7, 'after-end')] if diagnostics else [])

    def test_server_cleanup_fail_cannot_be_success(self):
        experiment = self.experiment()
        experiment.server = SimpleNamespace(stop=lambda: {'status': 'FAIL', 'forced_cleanup': True})
        experiment.check_online = lambda expected: None
        self.assertFalse(experiment.cleanup())
        self.assertIn('Owned server cleanup was not PASS', experiment.summary['cleanup_errors'])

    def test_late_nonzero_worker_exit_cannot_be_success(self):
        experiment = self.experiment()
        experiment.workers = [FinishedWorker(1)]
        experiment.finished = {0}  # Completion was announced before shutdown failed.
        self.assertFalse(experiment.cleanup())
        self.assertIn('Native worker 0 exit status 1', experiment.summary['cleanup_errors'])

    def test_late_worker_failure_during_cleanup_is_retained(self):
        experiment = self.experiment()
        worker = FinishedWorker(None)
        experiment.workers, experiment.finished = [worker], {0}
        def late_failure(timeout):
            worker.returncode = 1
            raise AssertionError('late native shutdown failure')
        experiment.read_events = late_failure
        self.assertFalse(experiment.cleanup())
        self.assertTrue(any('late native shutdown failure' in error
                            for error in experiment.summary['cleanup_errors']))

    def driver_source(self, name):
        source = Path(gui.__file__).read_text()
        start = source.index(f'    def {name}(self, seed):')
        return source[start:source.index('\n    def ', start + 10)]

    def test_heartbeat_pilot_arguments_reject_other_modes_and_pilots(self):
        entry = str(Path(scale.__file__).resolve())
        base = ['--work', '/tmp/native-tools-arguments', '--build', '/tmp/native-tools-build',
                '--build-kind', 'Debug-pilot']
        cases = [
            ['--mode', 'tui', '--count', '2', '--pilot-seconds', '10', '--heartbeat-pilot'],
            ['--mode', 'gui', '--count', '10', '--pilot-seconds', '10', '--heartbeat-pilot'],
            ['--mode', 'gui', '--count', '2', '--heartbeat-pilot'],
            ['--mode', 'gui', '--count', '2', '--pilot-seconds', '10', '--heartbeat-pilot', '--blackhole-pilot'],
            ['--mode', 'gui', '--count', '2', '--pilot-seconds', '10', '--heartbeat-pilot', '--read-pilot'],
        ]
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = subprocess.run([sys.executable, '-B', entry, *base, *arguments],
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn('pilot', result.stderr.lower())

    def test_heartbeat_pilot_never_restarts_or_reconnects_the_client_it_observes(self):
        # The scenario is about the client recovering on its own, so the driver may not help it:
        # no restart, no re-login, and no sever() pretending to be a heartbeat timeout.
        source = self.driver_source('heartbeat_pilot')
        for forbidden in ('self.login(', 'authenticate_existing', 'terminate()', '.kill(', 'sever()',
                          'self.clients.pop'):
            self.assertNotIn(forbidden, source, f'The heartbeat pilot must not {forbidden}')
        self.assertIn('relay.pause', source)
        self.assertIn('relay.resume', source)

    def test_heartbeat_pilot_requires_both_deactivation_signals_before_recovering(self):
        source = self.driver_source('heartbeat_pilot')
        self.assertIn("['online'] == 1", source)
        self.assertIn('EXTRA_DOWN_SECONDS', source)
        self.assertIn('occurrences(missed)', source)
        self.assertIn("self.proof('online'", source)

    def test_proof_request_answers_online_from_the_server_observer(self):
        experiment = self.experiment()
        output = io.StringIO()
        experiment.workers = [SimpleNamespace(stdin=output)]
        experiment.server = mock.MagicMock()
        experiment.server.snapshot.return_value = {'online': 1, 'users': [8]}
        experiment.accept_record(0, {'event': 'proof-request', 'kind': 'online',
                                     'stage': 'heartbeat-release'})
        self.assertEqual(json.loads(output.getvalue()),
                         {'command': 'proof', 'kind': 'online', 'online': 1, 'users': [8]})
        proof = [json.loads(line) for line in experiment.events.getvalue().splitlines()]
        proof = [record for record in proof if record['event'] == 'online-proof']
        self.assertEqual([record['stage'] for record in proof], ['heartbeat-release'])

    def test_proof_request_answers_read_position_with_a_readonly_query(self):
        experiment, cursor, output = self.preference_experiment()
        cursor.fetchone.return_value = (12, 3)
        experiment.accept_record(0, {'event': 'proof-request', 'kind': 'read-position',
                                     'conversation': 23, 'member': 7})
        statement, parameters = cursor.execute.call_args.args
        normalized = ' '.join(statement.split()).upper()
        self.assertTrue(normalized.startswith('SELECT'))
        self.assertNotIn(';', normalized)
        self.assertIn('CONVERSATION_MEMBERS', normalized)
        self.assertEqual(parameters, (23, 7))
        answer = json.loads(output.getvalue())
        self.assertEqual((answer['read_position'], answer['unread']), (12, 3))

    def test_proof_request_rejects_unknown_kind_without_reply(self):
        experiment, cursor, output = self.preference_experiment()
        with self.assertRaises(AssertionError):
            experiment.accept_record(0, {'event': 'proof-request', 'kind': 'guess'})
        self.assertEqual(output.getvalue(), '')
        cursor.execute.assert_not_called()

    def test_unread_badge_reads_presence_from_real_pixels_and_never_the_digit(self):
        from PIL import Image, ImageDraw
        driver = gui.NativeGui.__new__(gui.NativeGui)
        driver.work = Path(self.directory.name) / 'badge'
        driver.work.mkdir()
        driver.display = None
        driver.badge_rgb = (122, 60, 170)
        driver.Atspi = SimpleNamespace(CoordType=SimpleNamespace(SCREEN=0))
        driver.focus = lambda actor: None
        row = mock.MagicMock()
        row.get_name.return_value = 'peer'
        extents = row.get_component_iface().get_extents.return_value
        extents.x, extents.y, extents.width = 0, 0, 200
        driver.rows = lambda actor, view: [row]

        def picture(digit):
            image = Image.new('RGB', (200, 62), (248, 248, 246))
            # With no badge the preview text reaches the right edge, and it may contain a
            # colourful emoji; a coloured blob must not read as a badge.
            ImageDraw.Draw(image).text((10, 12), 'peer', fill=(37, 51, 45))
            ImageDraw.Draw(image).ellipse((148, 36, 168, 54), fill=(255, 200, 40))
            if digit is not None:
                ImageDraw.Draw(image).rounded_rectangle((172, 34, 191, 53), 9, fill=driver.badge_rgb)
                ImageDraw.Draw(image).text((179, 38), digit, fill=(255, 255, 255))
            return image

        for digit, expected in ((None, False), ('1', True), ('3', True)):
            with self.subTest(digit=digit):
                driver.ImageGrab = SimpleNamespace(grab=lambda xdisplay=None: picture(digit))
                with contextlib.redirect_stdout(io.StringIO()) as captured:
                    self.assertEqual(driver.unread_badge(7, 'peer', 'badge-under-test'), expected)
                record = json.loads(captured.getvalue().strip().splitlines()[-1])
                self.assertEqual(record['event'], 'unread-badge-capture')
                self.assertEqual(record['badge_visible'], expected)
                self.assertTrue(Path(record['path']).exists())
                self.assertTrue(Path(record['crop']).exists())
        # A badge showing 1 and a badge showing 3 are indistinguishable here, which is exactly why
        # the driver reports a visual review instead of a numeric pass.

    def test_unread_color_comes_from_each_installed_theme_json(self):
        root = Path(self.directory.name) / 'theme-product'
        themes = root / 'themes'
        themes.mkdir(parents=True)
        binary = root / 'chat_qt'
        for name, rgb in (('daylight', (53, 104, 190)), ('midnight', (184, 117, 220))):
            token = '#' + ''.join(f'{channel:02x}' for channel in rgb)
            (themes / (name + '.json')).write_text(json.dumps({'colors': {'highlight': token}}))
            self.assertEqual(gui.theme_highlight_rgb(binary, name), rgb)
        (themes / 'daylight.json').write_text(json.dumps({'colors': {'highlight': 'broken'}}))
        with self.assertRaises(ValueError):
            gui.theme_highlight_rgb(binary)

    def test_accent_pixels_separates_a_painted_pill_from_header_decoration(self):
        from PIL import Image, ImageDraw
        band = (0, 0, 300, 52)

        def header(colour):
            image = Image.new('RGB', (300, 52), (248, 248, 246))
            ImageDraw.Draw(image).ellipse((8, 8, 44, 44), fill=colour)
            ImageDraw.Draw(image).text((60, 16), 'peer 中文', fill=(37, 51, 45))
            return image

        # A reddish avatar already contributes accent pixels, so only the change caused by a banner
        # can be the signal; and a banner drawn in another colour must not be counted at all.
        palette = (200, 70, 80)
        warm = gui.accent_pixels(header((200, 60, 60)), band, palette)
        cool = gui.accent_pixels(header((60, 120, 200)), band, palette)
        self.assertEqual(cool, 0)
        decorated = header((200, 60, 60))
        ImageDraw.Draw(decorated).rounded_rectangle((120, 16, 232, 36), 9, fill=palette)
        ImageDraw.Draw(decorated).text((130, 18), '正在连接…', fill=(255, 255, 255))
        self.assertGreaterEqual(gui.accent_pixels(decorated, band, palette) - warm, 120)

    def test_heartbeat_pilot_reads_the_notice_from_the_clients_own_tree(self):
        # The notice must come from the same accessibility walk every other check uses, and a failure
        # has to record what the window really showed instead of only saying nothing was found.
        source = Path(gui.__file__).read_text()
        start = source.index('    def heartbeat_pilot(self, seed):')
        body = source[start:source.index('\n    def ', start + 10)]
        self.assertIn('for node in self.nodes(sender_id)', body)
        self.assertIn('BREAK_STATES', body)
        self.assertIn("observed['window'] = visible_words()", body)

    def test_read_pilot_reports_badge_digits_as_visual_review_not_as_pass(self):
        source = self.driver_source('read_pilot')
        self.assertIn("digits='visual-review-required'", source)
        self.assertIn('unread-badge-digit-recognition', Path(gui.__file__).read_text())

    def test_unread_evidence_payloads_are_redacted_like_every_other_event(self):
        private = set(gui._private_values)
        try:
            gui._private_values.clear()
            gui._private_values.update({'S3cr3t-Marker'})
            with contextlib.redirect_stdout(io.StringIO()) as captured:
                gui.emit('unread-evidence', actor=7, conversation=23, label='unread-1',
                         screenshot='/tmp/S3cr3t-Marker.png', read_position=12)
            self.assertNotIn('S3cr3t-Marker', captured.getvalue())
        finally:
            gui._private_values.clear()
            gui._private_values.update(private)


if __name__ == '__main__':
    unittest.main()
