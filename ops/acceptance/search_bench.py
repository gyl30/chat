"""Measure real loopback RPCs on a disposable WAL database; run one version at a time."""

import argparse
import asyncio
import json
import math
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

from fixture import fixture


def seed(db, rows):
    with sqlite3.connect(db) as conn:
        conn.execute("pragma foreign_keys=on")
        for group in range(100, 220):
            conn.execute(
                "insert into conversations(id,kind,title,activity,owner_id) values(?,'group','讨论组',0,1)", (group,)
            )
            conn.execute("insert into conversation_members(conversation_id,user_id) values(?,1)", (group,))
        conn.execute("insert into conversation_members(conversation_id,user_id,joined_message_id) values(100,2,2000)")
        conn.executemany(
            "insert into messages(conversation_id,sender_id,body,created_at) values(?,1,?,?)",
            (
                (
                    100 + i % 120,
                    "产品发布 测试需求 研发进度 " * 3 + str(i) + (" 稀有" if i % 3001 == 0 else ""),
                    1736942400000 if i < rows // 20 else 1739404800000 if i < rows // 10 else 1767225600000,
                )
                for i in range(rows)
            ),
        )
        conn.execute("analyze")


def quantiles(values):
    vals = sorted(values)
    return [round(vals[math.ceil(len(vals) * p) - 1], 3) for p in (0.95, 0.99)]


async def run(args):
    with tempfile.TemporaryFile("w+") as log:
        # Read through a separate fd so the reader cannot move the writer's file offset.
        async with fixture(args.binary, log) as (db, login, a, b, direct, pid):
            seed(db, args.rows)
            writer = await login("c", "c", True)
            peer = await login("d", "d", True)
            await writer.ok("send_friend_request", user=4)
            await peer.ok("respond_friend_request", user=3, accept=True)
            direct = (await writer.ok("open_direct_conversation", user=4))["conversation"]
            await peer.ws.close()
            client_id = 0
            metrics = open(f"/proc/self/fd/{log.fileno()}")
            for layout in ("hot", "single", "multi"):
                if layout != "hot":
                    month = "2025-01" if layout == "single" else "2025-02"
                    subprocess.run(
                        [
                            sys.executable,
                            str(Path(__file__).resolve().parents[1] / "chat_admin.py"),
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
                    if args.case and kind != args.case:
                        continue
                    samples = {
                        k: []
                        for k in (
                            "search_queue",
                            "search_sql",
                            "search_rpc",
                            "send_queue",
                            "send_ack",
                            "opens",
                            "reads",
                            "open_ms",
                        )
                    }

                    def ticks():
                        return sum(map(int, Path(f"/proc/{pid}/stat").read_text().split()[13:15]))

                    cpu = ticks()
                    for n in range(args.samples + 2):

                        async def search():
                            start = time.perf_counter()
                            result = await client.ok(
                                "search_all_messages", query=query, **({"before": before} if before else {})
                            )
                            elapsed = (time.perf_counter() - start) * 1000
                            if kind in ("popular", "deep"):
                                assert len(result["messages"]) == 50
                            elif kind == "rare":
                                assert len(result["messages"]) == min(50, (args.rows + 3000) // 3001)
                            if kind in ("nohit", "sparse"):
                                assert not result["messages"]
                            return elapsed

                        if client:
                            pending = asyncio.create_task(search())
                            await asyncio.sleep(0.002)
                        client_id += 1
                        start = time.perf_counter()
                        await writer.ok("send_message", conversation=direct, text="并发验证", client_id=client_id)
                        ack = (time.perf_counter() - start) * 1000
                        rpc = await pending if client else None
                        rows = {
                            v[1]: list(map(float, v[2:]))
                            for v in (line.split() for line in metrics.readlines())
                            if v and v[0] == "METRIC"
                        }
                        assert "send" in rows and (not client or "search" in rows)
                        if n >= 2:
                            samples["send_ack"].append(ack)
                            samples["send_queue"].append(rows["send"][0])
                            if client:
                                samples["search_rpc"].append(rpc)
                                for name, val in zip(
                                    ("search_queue", "search_sql", "opens", "reads", "open_ms"), rows["search"]
                                ):
                                    samples[name].append(val)
                    cpu = (ticks() - cpu) / os.sysconf("SC_CLK_TCK")
                    with sqlite3.connect(db) as conn:
                        wal = Path(str(db) + "-wal").stat().st_size
                        checkpoint = conn.execute("pragma wal_checkpoint(passive)").fetchone()
                    rss = next(
                        line.split()[1]
                        for line in Path(f"/proc/{pid}/status").read_text().splitlines()
                        if line.startswith("VmHWM:")
                    )
                    print(
                        json.dumps(
                            dict(
                                layout=layout,
                                case=kind,
                                samples=args.samples,
                                cpu_seconds=cpu,
                                peak_kib=int(rss),
                                wal_bytes=wal,
                                checkpoint=checkpoint,
                                **{k: quantiles(v) for k, v in samples.items() if v},
                            )
                        ),
                        flush=True,
                    )
            metrics.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--case", choices=("popular", "rare", "nohit", "sparse", "deep", "idle"))
    parser.add_argument("--rows", type=int, default=120000)
    parser.add_argument("--samples", type=int, default=100)
    asyncio.run(run(parser.parse_args()))
