"""Real Qt and RPC regression for sparse categorized history and direct location."""

import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put


async def main(root, probe, baseline):
    store = Store()
    db = root / "fixture.sqlite3"
    with store.node(root / "silo") as (s3, storage):
        s3.create_bucket(Bucket=store.bucket)
        with chat(db, storage) as url:
            async with AsyncExitStack() as users:
                a = await login(users, url, "a", "a", True)
                b = await login(users, url, "b", "b", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                conv = (await a.ok("open_direct_conversation", user=2))["conversation"]
                image = b"test image category"
                part = await begin(a, conv, 42, image, "old.png", "image/png")
                await a.ok(
                    "confirm_attachment",
                    upload=42,
                    number=1,
                    etag=put(part["url"], image),
                )
                anchor = (await a.ok("finish_attachment", upload=42))["id"]
                if not baseline:
                    archived = (
                        await b.ok(
                            "send_message",
                            conversation=conv,
                            text='中  文 https://archive.test %_"',
                            client_id=61,
                        )
                    )["id"]
                    file = b"test file category"
                    part = await begin(
                        a, conv, 43, file, "old.bin", "application/octet-stream"
                    )
                    await a.ok(
                        "confirm_attachment",
                        upload=43,
                        number=1,
                        etag=put(part["url"], file),
                    )
                    file_id = (await a.ok("finish_attachment", upload=43))["id"]
                    with sqlite3.connect(db) as conn:
                        conn.execute(
                            "UPDATE messages SET created_at=? WHERE id=?",
                            (1735689600000, archived),
                        )
                    subprocess.run(
                        [
                            sys.executable,
                            str(Path(__file__).parents[1] / "chat_admin.py"),
                            "--db",
                            str(db),
                            "archive",
                            "2025-01",
                        ],
                        check=True,
                    )
                with sqlite3.connect(db) as conn:
                    conn.executemany(
                        "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(?,2,?,?)",
                        ((conv, f"普通文字 {i}", 1767225600000) for i in range(3100)),
                    )

                if not baseline:
                    # Keep the image outside the main timeline, so activating it exercises around.
                    tail = (await a.ok("get_messages", conversation=conv))["messages"][
                        -1
                    ]["id"]
                    await a.ok("mark_read", conversation=conv, message=tail)
                    older = dict(conversation=conv, category="links")
                    reverse = await a.ok("get_messages", **older)
                    assert [m["id"] for m in reverse["messages"]] == [archived]
                    assert (
                        await a.ok("get_messages", conversation=conv, around=archived)
                    )["messages"]
                    assert [
                        m["id"]
                        for m in (
                            await a.ok(
                                "get_messages", conversation=conv, category="files"
                            )
                        )["messages"]
                    ] == [file_id]

                gate = Gate(url)
                async with serve(gate.handle, "127.0.0.1", 0) as proxy:
                    entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
                    ui = Desktop()
                    await ui.start(probe, root, entry)
                    try:
                        await ui.wait("select", lambda r: r["ok"], conv=conv)
                        await ui.call("history", open=True, conv=conv)
                        await ui.wait("history", lambda r: r.get("count", 0) > 0)
                        gate.messages.clear()
                        await ui.call("history", tab=1)
                        if baseline:

                            def walked():
                                return (
                                    len(
                                        [
                                            p
                                            for p in gate.messages
                                            if p["method"] == "get_messages"
                                            and "before" in p["params"]
                                        ]
                                    )
                                    >= 20
                                )

                            await until(walked, "Qt exhausted its automatic scan")
                            result = await ui.wait(
                                "history", lambda r: r.get("status") == "暂无图片"
                            )
                            assert result["count"] == 0 and not result["more"]
                            print(
                                "RED: 3100 newer texts; Qt reported no images after 20 pages and hid More",
                                flush=True,
                            )
                            return
                        await ui.wait("history", lambda r: r.get("count") == 1)
                        gate.hold.add("get_messages")
                        await ui.call("history", tab=3)
                        await gate.caught("get_messages")
                        await ui.call("history", tab=2)
                        await until(
                            lambda: len(gate.held) == 2, "two category responses held"
                        )
                        gate.held.reverse()
                        await gate.release("get_messages")
                        await ui.wait("history", lambda r: r.get("count") == 1)
                        await ui.call("history", tab=1)
                        await ui.wait("history", lambda r: r.get("count") == 1)
                        gate.messages.clear()
                        await ui.call("history", activate=True)
                        await until(
                            lambda: any(
                                p["method"] == "get_messages"
                                and p["params"].get("around") == anchor
                                for p in gate.messages
                            ),
                            "single anchored read",
                        )
                        await ui.wait(
                            "searchui", lambda r: r.get("ctx") and r.get("count", 0) > 0
                        )
                        requests = [
                            p["params"]
                            for p in gate.messages
                            if p["method"] == "get_messages"
                        ]
                        assert requests == [{"conversation": conv, "around": anchor}], (
                            requests
                        )
                        assert not any(
                            p["method"] == "mark_read" for p in gate.messages
                        )
                        await ui.call("searchui", close=True)
                        await ui.call("searchui", open=True)
                        await ui.wait("searchui", lambda r: r.get("open"))
                        gate.messages.clear()
                        await ui.wait(
                            "searchui", lambda r: r.get("scope"), conversation=conv
                        )
                        await ui.call("searchui", category=3)
                        state = await ui.call("searchui")
                        assert not state["status"] and not state["ids"]
                        assert not any(
                            p["method"] == "search_all_messages" for p in gate.messages
                        )
                        await ui.call("searchui", submit=True)
                        await ui.wait("searchui", lambda r: r.get("ids") == [archived])
                        print(
                            "PASS sparse history, archived categories, reversed pages, and one around; Qt filter-only submit",
                            flush=True,
                        )
                    finally:
                        await ui.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-history-") as path:
        asyncio.run(main(Path(path), sys.argv[1], "--baseline" in sys.argv))
