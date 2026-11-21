"""Measure real loopback RPCs on a disposable WAL database; run one version at a time."""

import argparse
import asyncio
import ctypes
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid

from fixture import fixture


def seed(db, rows):
    with sqlite3.connect(db) as conn:
        conn.execute("pragma foreign_keys=on")
        for group in range(100, 220):
            conn.execute(
                "insert into conversations(id,kind,title,activity,owner_id) values(?,'group','讨论组',0,1)",
                (group,),
            )
            conn.execute(
                "insert into conversation_members(conversation_id,user_id) values(?,1)",
                (group,),
            )
        conn.execute(
            "insert into conversation_members(conversation_id,user_id,joined_message_id) values(100,2,2000)"
        )
        conn.executemany(
            "insert into messages(conversation_id,sender_id,body,created_at) values(?,1,?,?)",
            (
                (
                    100 + i % 120,
                    "产品发布 测试需求 研发进度 " * 3
                    + str(i)
                    + (" 稀有" if i % 3001 == 0 else ""),
                    (
                        1736942400000
                        if i < rows // 20
                        else 1739404800000 if i < rows // 10 else 1767225600000
                    ),
                )
                for i in range(rows)
            ),
        )
        conn.execute("analyze")


def quantiles(values):
    vals = sorted(values)
    return [round(vals[math.ceil(len(vals) * p) - 1], 3) for p in (0.95, 0.99)]


def sqlite_runtime(pid):
    paths = {
        line.split(maxsplit=5)[5]
        for line in Path(f"/proc/{pid}/maps").read_text().splitlines()
        if "/libsqlite3.so." in line and len(line.split(maxsplit=5)) == 6
    }
    (path,) = paths
    lib = ctypes.CDLL(path)
    lib.sqlite3_libversion.restype = ctypes.c_char_p
    return dict(version=lib.sqlite3_libversion().decode(), path=path)


async def pipeline(reader, writer, search, send):
    # On one connection exactly one task consumes frames and demultiplexes by ID.
    stamp = {}
    expected = {}

    def request(client, method, params):
        client.ident += 1
        expected.setdefault(client, set()).add(client.ident)
        return dict(jsonrpc="2.0", id=client.ident, method=method, params=params)

    async def receive(client, ids):
        replies = {}
        while len(replies) < len(ids):
            response = json.loads(await asyncio.wait_for(client.ws.recv(), 60))
            received = time.perf_counter_ns()
            ident = response.get("id")
            if ident in ids:
                assert "error" not in response, response
                stamp[client, ident] = received
                replies[ident] = response["result"]
        return replies

    search_packet = (
        request(reader, "search_all_messages", search) if search is not None else None
    )
    send_packet = request(writer, "send_message", send)
    tasks = {
        client: asyncio.create_task(receive(client, ids))
        for client, ids in expected.items()
    }
    send_id = send_packet["id"]
    try:
        if search_packet:
            search_id = search_packet["id"]
            search_at = time.perf_counter_ns()
            await reader.ws.send(json.dumps(search_packet))
            await asyncio.sleep(0.002)
        send_at = time.perf_counter_ns()
        await writer.ws.send(json.dumps(send_packet))
        await asyncio.gather(*tasks.values())
        send_end = stamp[writer, send_id]
        sample = dict(
            send_rpc_id=send_id,
            send_submit_ns=send_at,
            send_response_ns=send_end,
            send_ack=(send_end - send_at) / 1e6,
        )
        if not search_packet:
            return sample, None
        search_end = stamp[reader, search_id]
        sample.update(
            search_rpc_id=search_id,
            search_submit_ns=search_at,
            search_response_ns=search_end,
            search_rpc=(search_end - search_at) / 1e6,
            submit_gap_ms=(send_at - search_at) / 1e6,
            submitted_before_search_response=send_at < search_end,
        )
        return sample, tasks[reader].result()[search_id]
    finally:
        for task in tasks.values():
            task.cancel()
        await asyncio.gather(*tasks.values(), return_exceptions=True)


async def run(args):
    with tempfile.TemporaryFile("w+") as log:
        async with fixture(args.binary, log) as (db, login, a, b, direct, pid):
            seed(db, args.rows)
            writers = {}
            if args.mode == "cross":
                writers[a] = await login("a", "c")
                writers[b] = await login("b", "d")
            client_id = 0
            metadata = dict(
                run_id=uuid.uuid4().hex,
                pid=pid,
                sha=args.sha,
                build_type="Release",
                sqlite_runtime=sqlite_runtime(pid),
                ops_python_sqlite=sqlite3.sqlite_version,
                mode=args.mode,
                rows=args.rows,
                group_count=120,
                warmups=2,
                samples_per_case=args.samples,
                latency_unit="ms",
                timestamp_unit="monotonic_ns",
            )
            groups = []
            with open(f"/proc/self/fd/{log.fileno()}") as metrics:
                for layout in ("hot", "single", "multi"):
                    if layout != "hot":
                        if args.layout == "hot":
                            break
                        month = "2025-01" if layout == "single" else "2025-02"
                        subprocess.run(
                            [
                                sys.executable,
                                str(
                                    Path(__file__).resolve().parents[1]
                                    / "chat_admin.py"
                                ),
                                "--db",
                                str(db),
                                "archive",
                                month,
                            ],
                            check=True,
                        )
                    for kind, client, query, before in (
                        ("popular", a, "发布", None),
                        ("rare", a, "稀有", None),
                        ("nohit", a, "没有匹配", None),
                        ("sparse", b, "没有匹配", None),
                        ("deep", a, "发布", args.rows // 10),
                        ("idle", None, "", None),
                    ):
                        if args.case and kind not in args.case:
                            continue
                        samples = []

                        def ticks():
                            return sum(
                                map(
                                    int,
                                    Path(f"/proc/{pid}/stat")
                                    .read_text()
                                    .split()[13:15],
                                )
                            )

                        cpu = ticks()
                        for n in range(args.samples + 2):
                            client_id += 1
                            search = (
                                dict(
                                    query=query,
                                    **({"before": before} if before else {}),
                                )
                                if client
                                else None
                            )
                            writer = writers.get(client or a, client or a)
                            sample, result = await pipeline(
                                client,
                                writer,
                                search,
                                dict(
                                    conversation=direct,
                                    text="并发验证",
                                    client_id=client_id,
                                ),
                            )
                            if kind in ("popular", "deep"):
                                assert len(result["messages"]) == 50
                            elif kind == "rare":
                                assert len(result["messages"]) == min(
                                    50, (args.rows + 3000) // 3001
                                )
                            elif kind in ("nohit", "sparse"):
                                assert not result["messages"]
                            rows = {
                                v[1]: list(map(float, v[2:]))
                                for v in (line.split() for line in metrics.readlines())
                                if v and v[0] == "METRIC"
                            }
                            assert "send" in rows and (not client or "search" in rows)
                            sample.update(
                                send_queue=rows["send"][0], send_db=rows["send"][1]
                            )
                            if client:
                                sample.update(
                                    zip(
                                        (
                                            "search_queue",
                                            "search_sql",
                                            "opens",
                                            "reads",
                                            "open_ms",
                                        ),
                                        rows["search"],
                                    )
                                )
                            if n >= 2:
                                samples.append(sample)
                        cpu = (ticks() - cpu) / os.sysconf("SC_CLK_TCK")
                        with sqlite3.connect(db) as conn:
                            wal = Path(str(db) + "-wal").stat().st_size
                            checkpoint = conn.execute(
                                "pragma wal_checkpoint(passive)"
                            ).fetchone()
                        rss = next(
                            line.split()[1]
                            for line in Path(f"/proc/{pid}/status")
                            .read_text()
                            .splitlines()
                            if line.startswith("VmHWM:")
                        )
                        summary = dict(
                            layout=layout,
                            case=kind,
                            samples=len(samples),
                            cpu_seconds=cpu,
                            peak_kib=int(rss),
                            wal_bytes=wal,
                            checkpoint=checkpoint,
                        )
                        if client:
                            summary["overlap_samples"] = sum(
                                s["submitted_before_search_response"] for s in samples
                            )
                        for k in (
                            "search_queue",
                            "search_sql",
                            "search_rpc",
                            "send_queue",
                            "send_db",
                            "send_ack",
                            "submit_gap_ms",
                            "opens",
                            "reads",
                            "open_ms",
                        ):
                            if k in samples[0]:
                                summary[k] = quantiles([s[k] for s in samples])
                        groups.append(dict(summary=summary, samples=samples))
                        print(json.dumps(summary), flush=True)
            if args.raw:
                with args.raw.open("x") as f:
                    json.dump(
                        dict(metadata=metadata, groups=groups), f, separators=(",", ":")
                    )
                    f.write("\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument(
        "--case",
        action="append",
        choices=("popular", "rare", "nohit", "sparse", "deep", "idle"),
    )
    parser.add_argument("--layout", choices=("hot", "all"), default="all")
    parser.add_argument("--mode", choices=("same", "cross"), default="cross")
    parser.add_argument("--rows", type=int, default=120000)
    parser.add_argument("--samples", type=int, default=100)
    parser.add_argument("--sha", help="verified SHA of the instrumented Release build")
    parser.add_argument(
        "--raw", type=Path, help="new file for sample data and metadata"
    )
    args = parser.parse_args()
    if args.samples < 1 or args.rows < 1 or (args.raw and not args.sha):
        parser.error("positive --rows/--samples required; --raw also requires --sha")
    asyncio.run(run(args))
