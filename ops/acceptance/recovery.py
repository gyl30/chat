"""Quiescent, paired Chat/Silo backup, cold restore and business-level verification."""

import asyncio
from contextlib import AsyncExitStack
from datetime import datetime, timezone
import os
from pathlib import Path
import shutil
import stat
import sqlite3
import subprocess
import sys
import tempfile
import time
import urllib.request

from websockets.asyncio.server import serve
from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put


def admin(*args, env=None):
    subprocess.run(
        [sys.executable, str(Path(__file__).resolve().parents[1] / "chat_admin.py"), *map(str, args)],
        check=True,
        env=env,
    )


async def drill(root, gate, entry, probe):
    ui = Desktop()
    store = Store()
    db = root / "source.sqlite"
    payload = b"shared object"
    # Cross a real part boundary. The server determines its length.
    large = b"R" * (17 * 1024 * 1024)
    with store.node(root / "source-silo") as (s3, env):
        s3.create_bucket(Bucket=store.bucket)
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                b = await login(stack, url, "b", "b", True)
                c = await login(stack, url, "c", "c", True)
                for user, client in [(2, b), (3, c)]:
                    await a.ok("send_friend_request", user=user)
                    await client.ok("respond_friend_request", user=1, accept=True)
                g = (await a.ok("create_group", title="恢复验证", members=[2]))["conversation"]
                ids = []
                for n in range(3):
                    ids.append(
                        (await a.ok("send_message", conversation=g, text=f"恢复月份{n}", client_id=30 + n))["id"]
                    )
                    if n == 0:
                        await a.ok("invite_group_members", conversation=g, members=[3])
                await b.ok("clear_history", conversation=g, through=ids[1])
                with sqlite3.connect(db) as conn:
                    conn.executemany(
                        "update messages set created_at=? where id=?", zip([1736942400000, 1739404800000], ids)
                    )
                for month in ("2025-01", "2025-02"):
                    admin("--db", db, "archive", month)
                part = await begin(a, g, 100, payload)
                await a.ok("confirm_attachment", upload=100, number=1, etag=put(part["url"], payload))
                f = await a.ok("finish_attachment", upload=100)
                key = f["attachment"]["object_key"]
                await a.ok("add_conversation_file", conversation_id=g, object_key=key)
                gate.upstream = url
                gate.hold.add("confirm_attachment")
                await ui.start(probe, root, entry)
                stack.push_async_callback(ui.stop)
                await ui.wait("select", lambda r: r["ok"], conv=g)
                source_path = root / "原设备续传.bin"
                source_path.write_bytes(large)
                await ui.call("upload", conv=g, path=str(source_path))
                await gate.caught("confirm_attachment")
                row = (await ui.call("transfers"))["labels"][0]
                assert source_path.name in row
                assert (await ui.call("transfers", label=row, action="暂停上传"))["triggered"]

                def paused():
                    with sqlite3.connect(db) as c:
                        return c.execute(
                            "select client_id,part_size,object_key,multipart from attachment_uploads where state='paused'"
                        ).fetchone()

                client_id, length, object_key, upload = await until(paused, "original Qt task paused")
                assert 0 < length < len(large)
                (journal,) = (root / "data").rglob("tasks.sqlite3")
                with sqlite3.connect(journal) as conn:
                    assert conn.execute("select path from tasks where client=?", (client_id,)).fetchone() == (
                        str(source_path),
                    )
                await ui.stop()
                gate.held.clear()
                gate.hold.clear()
                with sqlite3.connect(db) as conn:
                    conn.execute(
                        "update attachment_uploads set touched=touched-14*86400000 where client_id=?", (client_id,)
                    )

                def parts(client):
                    return [
                        (p["PartNumber"], p["Size"])
                        for p in client.list_parts(Bucket=store.bucket, Key=object_key, UploadId=upload)["Parts"]
                    ]

                assert parts(s3) == [(1, length)]
    point = datetime.now(timezone.utc).isoformat()
    admin("--db", db, "backup", root / "bundle")
    assert stat.S_IMODE((root / "bundle").stat().st_mode) == 0o700
    shutil.copytree(root / "source-silo", root / "silo-backup")
    start = time.monotonic()
    admin("verify-backup", root / "bundle")
    admin("restore", root / "bundle", root / "restored")
    assert stat.S_IMODE((root / "restored").stat().st_mode) == 0o700
    db = root / "restored/chat.sqlite3"
    admin("--db", db, "check")
    admin("--db", db, "archive-check")
    shutil.copytree(root / "silo-backup", root / "restored-silo")
    with store.node(root / "restored-silo") as (s3, env):
        assert parts(s3) == [(1, length)]
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                a, b, c = [await login(stack, url, k, k) for k in "abc"]
                print(
                    f"UTC recovery point={point} RTO to authenticated service={time.monotonic() - start:.3f}s",
                    flush=True,
                )
                for rpc, visible in [(a, ids), (b, ids[2:]), (c, ids[1:])]:
                    result = await rpc.ok("search_all_messages", query="恢复月份")
                    assert [m["id"] for m in result["messages"]] == visible[::-1]
                    for anchor in ids:
                        response = await rpc.ask("get_messages", conversation=g, around=anchor)
                        assert ("result" in response) == (anchor in visible)
                await a.ok("remove_group_member", conversation=g, user=3)
                assert not (await c.ok("search_all_messages", query="恢复月份"))["messages"]
                assert "error" in await c.ask("get_messages", conversation=g, around=ids[-1])
                await a.ok("invite_group_members", conversation=g, members=[3])
                assert not (await c.ok("search_all_messages", query="恢复月份"))["messages"]
                foreign = await login(stack, url, "a", "d")
                assert "error" in await foreign.ask("resume_attachment", upload=client_id)
                gate.upstream = url
                gate.hold.add("confirm_attachment")
                gate.messages.clear()
                await ui.start(probe, root, entry)
                stack.push_async_callback(ui.stop)
                row = (await ui.wait("transfers", lambda r: any(source_path.name in v for v in r["labels"])))[
                    "labels"
                ][0]
                assert (await ui.call("transfers", label=row, action="继续上传"))["triggered"]
                await gate.caught("confirm_attachment")
                confirms = [v["params"] for v in gate.messages if v["method"] == "confirm_attachment"]
                assert [(p["upload"], p["number"]) for p in confirms] == [(client_id, 2)]
                with sqlite3.connect(db) as conn:
                    touched = conn.execute(
                        "select touched from attachment_uploads where client_id=?", (client_id,)
                    ).fetchone()[0]
                    assert abs(time.time() * 1000 - touched) < 60000
                await gate.release("confirm_attachment")

                def published():
                    with sqlite3.connect(db) as conn:
                        return conn.execute(
                            "select m.id from messages m join message_attachments ma on ma.message_id=m.id where client_id=? and object_key=?",
                            (client_id, object_key),
                        ).fetchone()

                (mid,) = await until(published, "original Qt task published")

                def retired():
                    with sqlite3.connect(journal) as c:
                        return not c.execute("select 1 from tasks where client=?", (client_id,)).fetchone()

                await until(retired, "original SDK journal retired")
                await ui.stop()

                def download(auth):
                    with urllib.request.urlopen(auth["url"]) as response:
                        return response.read()

                assert download(await a.ok("get_attachment", conversation=g, message=mid)) == large
                assert download(await a.ok("get_attachment", conversation=g, message=f["id"])) == payload
                await a.ok("delete_message", conversation=g, message=f["id"])
                assert download(await c.ok("get_conversation_file", conversation_id=g, object_key=key)) == payload
                await a.ok("remove_conversation_file", conversation_id=g, object_key=key)
        with chat(db, env):
            keys = {o["Key"] for o in s3.list_objects_v2(Bucket=store.bucket).get("Contents", [])}
            assert key not in keys and object_key in keys
            mcenv = dict(os.environ, MC_CONFIG_DIR=str(root / "mc"))
            subprocess.run(
                ["mc", "alias", "set", "drill", "http://" + env["CHAT_S3_ENDPOINT"], "chat-accept", store.secret],
                env=mcenv,
                check=True,
                stdout=subprocess.DEVNULL,
            )
            admin("--db", db, "objects", "drill/" + store.bucket, "--checksum", env=mcenv)
        print(
            f"PASS original device resume, cross-month permissions, shared references and GC; verified in {time.monotonic() - start:.3f}s"
        )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as directory:

        async def run():
            gate = Gate(None)
            async with serve(gate.handle, "127.0.0.1", 0) as proxy:
                entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
                await drill(Path(directory), gate, entry, sys.argv[1])

        asyncio.run(run())
