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
            journal, = (root / "data").rglob("tasks.sqlite3")

            def rows(table):
                with sqlite3.connect(journal) as conn:
                    return conn.execute(f"SELECT body,markdown FROM {table}").fetchall()

            try:
                await ui.call("draft", text=source, markdown=True)
                await until(lambda: rows("compose_drafts") == [(source, 1)], "durable Markdown draft")
                await ui.stop()
                await start()
                await ui.wait("state", lambda s: s["draft"] == source and s["markdown"])
                gate.hold_requests.add("send_message")
                await ui.call("send")
                await until(lambda: rows("text_outbox") == [(source, 1)], "durable Markdown Outbox")
                ui.proc.kill()
                await ui.proc.wait()
                gate.hold_requests.clear()
                await start()
                await until(lambda: not rows("text_outbox"), "recovered Markdown acknowledged")
                page = await peer.ok("get_messages", conversation=conv)
                original, = page["messages"]
                assert original["text"] == source and original["markdown"]
                mid = original["id"]
                forward = await a.ok("send_message", conversation=conv, forward=mid, client_id=81)
                assert forward["markdown"] and forward["text"] == source
                await a.ok("edit_message", conversation=conv, message=mid, text="# 编辑")
                found = await peer.ok("search_messages", conversation=conv, query="# 编辑")
                assert found["messages"][0]["markdown"]
                await peer.ok("set_message_bookmark", message=mid, bookmarked=True)
                plain = await a.ok("send_message", conversation=conv,text="**普通**",client_id=82)
                assert not plain["markdown"]
            finally:
                await ui.stop()
    print("PASS Qt/SDK/server Markdown draft restart, Outbox process kill, mixed history, original-text search, edit, bookmark and forward")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-content-") as scratch:
        asyncio.run(run(Path(scratch), sys.argv[1]))
