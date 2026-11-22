"""Real Qt draft reply beyond the visible timeline, with controlled old RPC replies."""

import asyncio
from pathlib import Path
import sqlite3
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from fixture import fixture


async def run(root, probe, baseline):
    async with fixture() as (db, _, account, peer, _, _):
        host, port = peer.ws.remote_address[:2]
        gate = Gate(f"ws://{host}:{port}/ws")
        conv = (await peer.ok("create_group", title="深层回复", members=[1]))[
            "conversation"
        ]
        anchor = (
            await peer.ok(
                "send_message", conversation=conv, text="旧正文", client_id=1003
            )
        )["id"]
        async with serve(gate.handle, "127.0.0.1", 0) as proxy:
            entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            ui = Desktop()

            async def start():
                await ui.start(probe, root, entry)
                await ui.wait("select", lambda r: r["ok"], conv=conv)

            async def graceful():
                await ui.call("quit")
                assert await asyncio.wait_for(ui.proc.wait(), 8) == 0

            await start()
            try:
                await ui.wait(
                    "state", lambda s: any(r["id"] == anchor for r in s["rows"])
                )
                assert (await ui.call("reply_to", id=anchor))["reply"]
                await ui.call("draft", text="我的深层草稿")
                await graceful()
                with sqlite3.connect(db) as conn:
                    conn.executemany(
                        "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(?,2,?,?)",
                        ((conv, f"更新的文字 {i}", 1767225600000) for i in range(70)),
                    )
                await start()
                restored = await ui.wait(
                    "state", lambda s: s["reply"] and "旧正文" in s["preview"]
                )
                assert restored["draft"] == "我的深层草稿"
                assert anchor not in [r["id"] for r in restored["rows"]]
                gate.messages.clear()
                await peer.ok(
                    "edit_message", conversation=conv, message=anchor, text="第一次编辑"
                )
                await until(
                    lambda: any(
                        p["method"] == "get_conversations" for p in gate.messages
                    ),
                    "Qt consumed edit notification",
                )
                state = await ui.wait(
                    "state",
                    lambda s: s["preview"] == "回复 ："
                    if baseline
                    else "第一次编辑" in s["preview"],
                )
                if baseline:
                    assert state["reply"] and state["preview"] == "回复 ：", state
                    assert state["draft"] == "我的深层草稿"
                    print(
                        "RED: actual Qt restored a deep reply, then editing the original made its preview empty",
                        flush=True,
                    )
                    return
                assert state["reply"] and "第一次编辑" in state["preview"], state
                assert anchor not in [r["id"] for r in state["rows"]]
                # Both timeline and anchor may arrive as get_messages. Only the anchor has the old body.
                await graceful()
                gate.hold.add("get_messages")
                await start()
                await until(
                    lambda: any(
                        any(
                            m["id"] == anchor
                            for m in r.get("result", {}).get("messages", [])
                        )
                        for _, r in gate.held
                    ),
                    "held genuine around snapshot",
                )
                gate.messages.clear()
                await peer.ok(
                    "edit_message", conversation=conv, message=anchor, text="在途编辑"
                )
                await ui.wait(
                    "state", lambda s: s["reply"] and "在途编辑" in s["preview"]
                )
                await gate.release("get_messages")
                await ui.wait("state", lambda s: len(s["rows"]) == 50)
                assert "在途编辑" in (await ui.call("state"))["preview"]
                await ui.call("send", text="仍要回复")
                await until(
                    lambda: any(p["method"] == "send_message" for p in gate.messages),
                    "actual reply sent",
                )
                sent = next(
                    p["params"] for p in gate.messages if p["method"] == "send_message"
                )
                assert sent["reply_to"] == anchor, sent
                await ui.wait("state", lambda s: not s["draft"])
                # Reset a persisted deep draft between invalidation scenarios, without restoring any old body.
                (journal,) = (root / "data").rglob("tasks.sqlite3")
                invalidations = [("recall", anchor), ("clear", None), ("remove", None)]
                for revision, (action, reset_id) in enumerate(invalidations, 9991):
                    await graceful()
                    if reset_id is None:
                        with sqlite3.connect(db) as conn:
                            reset_id = conn.execute(
                                "SELECT id FROM messages WHERE conversation_id=? AND deleted=0 AND body LIKE '更新的文字%' ORDER BY id LIMIT 1 OFFSET ?",
                                (conv, 0 if action == "clear" else 1),
                            ).fetchone()[0]
                    with sqlite3.connect(journal) as conn:
                        conn.execute(
                            "INSERT OR REPLACE INTO compose_drafts(user,conversation,revision,reply,body,updated) VALUES(1,?,?,?,?,CAST(strftime('%s','now') AS INTEGER))",
                            (conv, revision, reset_id, "引用失效仍留文字"),
                        )
                    if action == "clear":
                        gate.hold.add("get_messages")
                    await start()
                    if action == "clear":
                        await until(
                            lambda: any(
                                any(
                                    m["id"] == reset_id
                                    for m in r.get("result", {}).get("messages", [])
                                )
                                for _, r in gate.held
                            ),
                            "held around before cross-device clear",
                        )
                        assert not (await ui.call("state"))["reply"]
                    else:
                        await ui.wait(
                            "state",
                            lambda s: s["reply"] and s["draft"] == "引用失效仍留文字",
                        )
                    if action == "recall":
                        await peer.ok(
                            "delete_message", conversation=conv, message=reset_id
                        )
                    elif action == "clear":
                        await account.ok(
                            "clear_history", conversation=conv, through=reset_id
                        )
                    else:
                        await peer.ok("remove_group_member", conversation=conv, user=1)
                    if action != "remove":
                        await ui.wait(
                            "state",
                            lambda s: not s["reply"]
                            and s["draft"] == "引用失效仍留文字",
                        )
                    else:
                        await ui.wait(
                            "state", lambda s: not s["reply"] and s["conv"] != conv
                        )

                    def saved():
                        with sqlite3.connect(journal) as conn:
                            return conn.execute(
                                "SELECT body,reply FROM compose_drafts WHERE user=1 AND conversation=?",
                                (conv,),
                            ).fetchone()

                    await until(
                        lambda: saved() == ("引用失效仍留文字", 0),
                        "preserved draft without invalid reply",
                    )
                    if action == "clear":
                        await gate.release("get_messages")
                        await ui.wait("state", lambda s: len(s["rows"]) == 50)
                        state = await ui.call("state")
                        assert (
                            not state["reply"] and state["draft"] == "引用失效仍留文字"
                        )
                print(
                    "PASS Qt deep reply restart, actual edit and stale validation, send reply ID, recall, clear and loss of membership",
                    flush=True,
                )
            finally:
                await ui.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-deep-reply-") as path:
        asyncio.run(run(Path(path), sys.argv[1], "--baseline" in sys.argv))
