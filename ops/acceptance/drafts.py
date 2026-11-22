"""Qt restart, draft scope, reply invalidation and durable Outbox handoff."""

import asyncio
from pathlib import Path
import sqlite3
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from fixture import fixture


async def run(root, probe):
    async with fixture() as (_, login, a, b, direct, _):
        url = a.ws.uri if hasattr(a.ws, "uri") else None
        # Fixture clients already use an isolated loopback socket.
        host, port = a.ws.remote_address[:2]
        gate = Gate(url or f"ws://{host}:{port}/ws")
        group = (await b.ok("create_group", title="草稿", members=[1]))["conversation"]
        anchor = (
            await b.ok(
                "send_message", conversation=direct, text="旧回复正文", client_id=548
            )
        )["id"]
        async with serve(gate.handle, "127.0.0.1", 0) as proxy:
            entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            ui = Desktop()
            await ui.start(probe, root, entry)
            (journal,) = (root / "data").rglob("tasks.sqlite3")

            def saved(conv):
                with sqlite3.connect(journal) as conn:
                    return conn.execute(
                        "SELECT body,reply FROM compose_drafts WHERE user=1 AND conversation=?",
                        (conv,),
                    ).fetchone()

            async def select(conv):
                await ui.wait("select", lambda r: r["ok"], conv=conv)

            try:
                await select(direct)
                await ui.call("draft", text="第一个草稿")
                await select(group)
                await ui.call("draft", text="第二个草稿")
                await until(
                    lambda: saved(direct) == ("第一个草稿", 0)
                    and saved(group) == ("第二个草稿", 0),
                    "both drafts committed",
                )
                await ui.stop()
                with sqlite3.connect(journal) as conn:
                    conn.execute(
                        "UPDATE compose_drafts SET reply=? WHERE user=1 AND conversation=?",
                        (anchor, direct),
                    )
                await ui.start(probe, root, entry)
                await select(direct)
                await ui.wait(
                    "state", lambda r: r["draft"] == "第一个草稿" and r["reply"]
                )
                await b.ok("delete_message", conversation=direct, message=anchor)
                await ui.wait(
                    "state", lambda r: not r["reply"] and r["draft"] == "第一个草稿"
                )
                await until(
                    lambda: saved(direct) == ("第一个草稿", 0), "recalled reply removed"
                )
                await select(group)
                await ui.wait("state", lambda r: r["draft"] == "第二个草稿")
                # Hold the actual SDK completion before it reaches chat_widget.
                # The independent ACK hold below only keeps the durable row inspectable.
                await ui.call("text_save_barrier", hold=True)
                gate.hold.add("send_message")
                await ui.call("send", text="提交前草稿")
                done = await ui.wait("text_save_barrier", lambda r: r["pending"])
                assert done["error"] == ""
                assert not (await ui.call("state"))["sendEnabled"]
                await gate.caught("send_message")
                assert not saved(group)
                with sqlite3.connect(journal) as conn:
                    assert conn.execute(
                        "SELECT body FROM text_outbox WHERE user=1 AND conversation=?",
                        (group,),
                    ).fetchone() == ("提交前草稿",)
                await ui.call("draft", text="新的草稿")
                assert (await ui.call("state"))["draft"] == "新的草稿"
                assert (await ui.call("text_save_barrier"))["pending"]
                await ui.call("text_save_barrier", release=True)
                state = await ui.call("state")
                assert state["draft"] == "新的草稿" and state["sendEnabled"]
                await until(
                    lambda: saved(group) == ("新的草稿", 0),
                    "new draft stored after releasing local completion",
                )
                await ui.call("quit")
                assert await asyncio.wait_for(ui.proc.wait(), 8) == 0
                gate.hold.clear()
                gate.held.clear()
                await ui.start(probe, root, entry)
                await select(group)
                await ui.wait("state", lambda r: r["draft"] == "新的草稿")
                page = await a.ok("get_messages", conversation=group)
                assert sum(m["text"] == "提交前草稿" for m in page["messages"]) == 1
                failed_anchor = (
                    await b.ok(
                        "send_message",
                        conversation=group,
                        client_id=812,
                        text="失败时保留的回复目标",
                    )
                )["id"]
                await ui.wait(
                    "state", lambda r: any(m["id"] == failed_anchor for m in r["rows"])
                )
                assert (await ui.call("reply_to", id=failed_anchor))["reply"]
                with sqlite3.connect(journal) as conn:
                    conn.execute("""CREATE TRIGGER reject_test_outbox BEFORE INSERT ON text_outbox
                                 WHEN new.body='本地入队失败' BEGIN SELECT RAISE(ABORT, 'QA'); END""")
                await ui.call("text_save_barrier", hold=True)
                await ui.call("send", text="本地入队失败")
                done = await ui.wait("text_save_barrier", lambda r: r["pending"])
                assert "无法保存待发送消息" in done["error"]
                await ui.call("text_save_barrier", release=True)
                state = await ui.call("state")
                assert (
                    state["draft"] == "本地入队失败"
                    and state["reply"]
                    and state["sendEnabled"]
                )
                assert "失败时保留的回复目标" in state["preview"]
                assert saved(group) == ("本地入队失败", failed_anchor)
                with sqlite3.connect(journal) as conn:
                    assert (
                        conn.execute(
                            "SELECT 1 FROM text_outbox WHERE body='本地入队失败'"
                        ).fetchone()
                        is None
                    )
                    conn.execute("DROP TRIGGER reject_test_outbox")
                await ui.call("draft", text="退出时保存")
                await ui.call("quit")
                assert await asyncio.wait_for(ui.proc.wait(), 8) == 0
                assert saved(group) == ("退出时保存", failed_anchor)
                await ui.start(probe, root, entry)
                await select(group)
                await ui.wait("state", lambda r: r["draft"] == "退出时保存")
                await ui.stop()
                await ui.start(probe, root, entry, user="b", user_id=2)
                await select(direct)
                assert (await ui.call("state"))["draft"] == ""
                await ui.stop()
                async with serve(gate.handle, "127.0.0.1", 0) as other:
                    alternate = f"ws://127.0.0.1:{other.sockets[0].getsockname()[1]}/ws"
                    await ui.start(probe, root, alternate)
                    await select(direct)
                    assert (await ui.call("state"))["draft"] == ""
                print(
                    "PASS Qt draft switching/restart, account/endpoint isolation, reply reauthorization and recall, local completion barrier, newer draft and forced local transaction failure",
                    flush=True,
                )
            finally:
                await ui.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-drafts-") as path:
        asyncio.run(run(Path(path), sys.argv[1]))
