"""Verify native and Python SQLite during real writes, checkpointing and paged search."""

import asyncio
import sqlite3
import threading

from fixture import fixture
from sqlite_runtime import python_sqlite, server_sqlite


async def run():
    python = python_sqlite()
    async with fixture() as (db, login, a, b, direct, pid):
        server = server_sqlite(pid)
        stop = threading.Event()
        started = threading.Event()

        def checkpoint():
            count = 0
            with sqlite3.connect(db) as conn:
                while not stop.is_set():
                    busy, frames, written = conn.execute(
                        "pragma wal_checkpoint(passive)"
                    ).fetchone()
                    assert busy == 0 and 0 <= written <= frames
                    count += 1
                    started.set()
                    stop.wait(0.001)
            return count

        c = await login("c", "c", True)
        await b.ok("send_friend_request", user=3)
        await c.ok("respond_friend_request", user=2, accept=True)
        group = (await b.ok("create_group", title="权限验证", members=[3]))[
            "conversation"
        ]
        await b.ok(
            "send_message", conversation=group, text="独立连接关键词 MiXeD", client_id=1
        )
        work = asyncio.create_task(asyncio.to_thread(checkpoint))
        try:
            assert await asyncio.to_thread(started.wait, 5)
            ids = []
            for n in range(56):
                payload = dict(
                    conversation=direct, text="独立连接关键词 MiXeD", client_id=n + 1
                )
                row = await a.ok("send_message", **payload)
                assert (await a.ok("send_message", **payload))["id"] == row["id"]
                ids.append(row["id"])
            assert await a.ok("clear_history", conversation=direct, through=ids[0]) == {
                "through": ids[0]
            }
            first = await a.ok("search_all_messages", query="独立连接关键词 mixed")
            second = await a.ok(
                "search_all_messages",
                query="独立连接关键词 mixed",
                before=first["next_before"],
            )
            assert [r["id"] for r in first["messages"]] == ids[::-1][:50]
            assert [r["id"] for r in second["messages"]] == ids[::-1][50:-1]
        finally:
            stop.set()
            count = await work
        assert count > 1
        with sqlite3.connect(db) as conn:
            assert conn.execute("pragma journal_mode").fetchone() == ("wal",)
            assert conn.execute("pragma integrity_check").fetchall() == [("ok",)]
            assert not conn.execute("pragma foreign_key_check").fetchall()
        print(
            f"PASS SQLite {server['version']} ({server['source_id']}), "
            f"Python {python['path']}, {count} checkpoints during RPC workload"
        )


if __name__ == "__main__":
    asyncio.run(run())
