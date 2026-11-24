"""Message content round trips through the Qt composer, SDK journal and actual RPC."""

import asyncio
from pathlib import Path
import sqlite3
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from fixture import fixture


async def run(root, probe):
    async with fixture() as (db, _, a, peer, conv, _):
        host, port = a.ws.remote_address[:2]
        gate = Gate(f"ws://{host}:{port}/ws")
        source = "**中文** 😀\n\n```cpp\nreturn 7;\n```"
        async with serve(gate.handle, "127.0.0.1", 0) as proxy:
            entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            ui = Desktop()

            async def start():
                await ui.start(probe, root, entry)
                await ui.wait("select", lambda row: row["ok"], conv=conv)

            await start()
            (journal,) = (root / "data").rglob("tasks.sqlite3")

            def rows(table):
                with sqlite3.connect(journal) as conn:
                    return conn.execute(f"SELECT body,markdown FROM {table}").fetchall()

            try:
                await ui.call("draft", text=source, markdown=True)
                await until(
                    lambda: rows("compose_drafts") == [(source, 1)],
                    "durable Markdown draft",
                )
                await ui.stop()
                await start()
                await ui.wait("state", lambda s: s["draft"] == source and s["markdown"])
                gate.hold_requests.add("send_message")
                await ui.call("send")
                await until(
                    lambda: rows("text_outbox") == [(source, 1)],
                    "durable Markdown Outbox",
                )
                ui.proc.kill()
                await ui.proc.wait()
                gate.hold_requests.clear()
                await start()
                await until(
                    lambda: not rows("text_outbox"), "recovered Markdown acknowledged"
                )
                page = await peer.ok("get_messages", conversation=conv)
                (original,) = page["messages"]
                assert original["text"] == source and original["markdown"]
                mid = original["id"]
                forward = await a.ok(
                    "send_message", conversation=conv, forward=mid, client_id=81
                )
                assert forward["markdown"] and forward["text"] == source
                await a.ok(
                    "edit_message", conversation=conv, message=mid, text="# 编辑"
                )
                found = await peer.ok(
                    "search_messages", conversation=conv, query="# 编辑"
                )
                assert found["messages"][0]["markdown"]
                await peer.ok("set_message_bookmark", message=mid, bookmarked=True)
                plain = await a.ok(
                    "send_message", conversation=conv, text="**普通**", client_id=82
                )
                assert not plain["markdown"]

                group = (
                    await peer.ok("create_group", title="全员提及草稿", members=[1])
                )["conversation"]
                await peer.ok("set_group_admin", conversation=group, user=1, admin=True)
                await ui.wait("select", lambda r: r["ok"], conv=group)
                await ui.wait("mention_tag", lambda r: r["checked"], add=True)
                assert not (await ui.call("mention_tag", remove=True))["checked"]
                await ui.wait("mention_tag", lambda r: r["checked"], add=True)
                await ui.call("draft", text="角色变更", markdown=True)

                def saved(table):
                    with sqlite3.connect(journal) as conn:
                        return conn.execute(
                            f"SELECT markdown,mention_all FROM {table} WHERE body='角色变更'"
                        ).fetchall()

                await until(
                    lambda: saved("compose_drafts") == [(1, 1)], "durable mention draft"
                )
                await ui.stop()
                await start()
                await ui.wait("select", lambda r: r["ok"], conv=group)
                await ui.wait("mention_tag", lambda r: r["checked"])
                gate.hold_requests.add("send_message")
                await ui.call("send")
                await until(
                    lambda: saved("text_outbox") == [(1, 1)], "durable mention send"
                )
                ui.proc.kill()
                await ui.proc.wait()
                await peer.ok(
                    "set_group_admin", conversation=group, user=1, admin=False
                )
                gate.hold_requests.clear()
                await start()

                def rejected():
                    with sqlite3.connect(journal) as conn:
                        return conn.execute(
                            "SELECT client FROM text_outbox WHERE body='角色变更' AND rejection<>0"
                        ).fetchone()

                (ident,) = await until(
                    rejected, "demoted original sender denied on real Outbox retry"
                )
                await peer.ok("set_group_admin", conversation=group, user=1, admin=True)
                await ui.call("outbox_retry", id=ident)
                await until(
                    lambda: not saved("text_outbox"), "promoted sender acknowledged"
                )
                (sent,) = (await peer.ok("get_messages", conversation=group))[
                    "messages"
                ]
                assert (
                    sent["mention_all"]
                    and sent["markdown"]
                    and sent["text"] == "角色变更"
                )
                with sqlite3.connect(db) as conn:
                    assert conn.execute(
                        "SELECT count(*) FROM messages WHERE client_id=? AND sender_id=1",
                        (ident,),
                    ).fetchone() == (1,)
            finally:
                await ui.stop()
    print(
        "PASS Qt/SDK/server Markdown draft restart, Outbox process kill, mixed history, original-text search, edit, bookmark, forward and permission changes on durable @all retry"
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-content-") as scratch:
        asyncio.run(run(Path(scratch), sys.argv[1]))
