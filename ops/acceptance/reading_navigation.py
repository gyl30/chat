"""Reading navigation over real Chat RPC, SDK and an isolated Qt desktop."""

import argparse
import asyncio
import json
from contextlib import AsyncExitStack, ExitStack
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, chat, login


async def run(root, probe, *, native=False, theme="system", ctrl_enter=False):
    db = root / "chat.sqlite3"
    store = Store()
    with ExitStack() as isolated:
        api, storage = isolated.enter_context(store.node(root / "silo"))
        api.create_bucket(Bucket=store.bucket)
        url = isolated.enter_context(chat(db, storage))
        gate = Gate(url)
        async with AsyncExitStack() as stack:
            a = await login(stack, url, "a", "a", True)
            b = await login(stack, url, "b", "b", True)
            await b.ok("send_friend_request", user=1)
            await a.ok("respond_friend_request", user=2, accept=True)
            group = (
                await b.ok(
                    "create_group",
                    title="原生阅读导航测试：这是一条足够长的会话名称，用于验证窄窗口里的标题和阅读按钮"
                    if native
                    else "导航",
                    members=[1],
                )
            )["conversation"]
            other = (await b.ok("create_group", title="只有手工未读", members=[1]))[
                "conversation"
            ]
            counter = 1000

            async def send(who, text, conv=group):
                nonlocal counter
                counter += 1
                return (
                    await who.ok(
                        "send_message", conversation=conv, client_id=counter, text=text
                    )
                )["id"]

            async def history(conv=group, **params):
                return await a.ok("get_messages", conversation=conv, **params)

            old = await send(b, "@a 历史不可见")
            await b.ok("remove_group_member", conversation=group, user=1)
            assert "error" in await a.ask(
                "get_messages", conversation=group, capture_unread=True
            )
            await b.ok("invite_group_members", conversation=group, members=[1])
            empty = await history(capture_unread=True)
            assert empty["visible_after"] == old and empty.get("first_unread") is None
            assert "error" in await a.ask(
                "get_messages", conversation=group, around=old
            )

            await send(a, "只有自己的消息", other)
            recalled = await send(b, "撤回的候选", other)
            await b.ok("delete_message", conversation=other, message=recalled)
            await a.ok("set_unread_mark", conversation=other, marked=True)
            assert (await history(other, capture_unread=True)).get(
                "first_unread"
            ) is None

            anchor = await send(b, "@a 起点 https://example.test")
            with sqlite3.connect(db) as conn:
                conn.execute(
                    "UPDATE messages SET created_at=? WHERE id=?",
                    (1736942400000, anchor),
                )
            subprocess.run(
                [
                    sys.executable,
                    str(Path(__file__).resolve().parents[1] / "chat_admin.py"),
                    "--db",
                    str(db),
                    "archive",
                    "2025-01",
                ],
                check=True,
            )
            ids = [anchor]
            for i in range(104):
                ids.append(await send(b, f"@a 导航 {i}"))
            editable = await send(b, "待编辑的普通消息")
            state = await history(capture_unread=True)
            assert state["first_unread"] == anchor and state["has_more"]
            assert len(state["messages"]) == 50 and state["messages"][0]["id"] > anchor
            assert (await history()).get("first_unread") is None
            assert "error" in await a.ask(
                "get_messages", conversation=group, around=anchor, capture_unread=True
            )

            async def mentions(**filters):
                return await a.ok("search_all_messages", mentions_me=True, **filters)

            first = await mentions()
            assert [r["id"] for r in first["messages"]] == ids[::-1][:50]
            assert first["next_before"] == ids[-50]
            assert [
                r["id"]
                for r in (
                    await mentions(
                        conversation=group,
                        sender=2,
                        category="links",
                        query=" 起点",
                        start=1736942400000,
                        end=1736942400001,
                    )
                )["messages"]
            ] == [anchor]
            assert not (await mentions(query="无结果的文字"))["messages"]
            assert "error" in await a.ask(
                "search_all_messages", query="", mentions_me=True
            )
            assert "error" in await a.ask("search_all_messages", mentions_me=False)
            print(
                "PASS real RPC unread snapshot, joined boundary, withdrawn/self/manual-only, archived anchor, indexed structured mention filters",
                flush=True,
            )

            proxy = await stack.enter_async_context(serve(gate.handle, "127.0.0.1", 0))
            entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            ui = Desktop()
            if native and (theme != "system" or ctrl_enter):
                config = root / "config" / "chat" / "chat_qt.conf"
                config.parent.mkdir(parents=True, exist_ok=True)
                config.write_text(
                    f"[appearance]\nmode={theme}\n"
                    + ("[chat]\nsend_key=ctrl_enter\n" if ctrl_enter else "")
                )
            await ui.start(probe, root, entry, native=native)
            try:
                if native:
                    await native_session(
                        root, db, group, anchor, editable, ui, gate, send, b
                    )
                    return

                async def select(conv):
                    await ui.wait("select", lambda r: r["ok"], conv=conv)
                    await ui.wait("reading", lambda r: r["ready"])

                def requested(method):
                    return [p["params"] for p in gate.messages if p["method"] == method]

                def read_at_least(mid):
                    with sqlite3.connect(db) as conn:
                        return (
                            conn.execute(
                                "SELECT last_read_message_id FROM conversation_members WHERE user_id=1 AND conversation_id=?",
                                (group,),
                            ).fetchone()[0]
                            >= mid
                        )

                async def open_anchor(expected):
                    await ui.call("reading", unread=True)
                    return await ui.wait(
                        "searchui", lambda r: r.get("anchor") == expected
                    )

                await select(group)
                await ui.call("reading", activate=True)
                await until(
                    lambda: read_at_least(editable), "initial latest page marked read"
                )
                initial = await ui.call("reading")
                assert (
                    initial["unread"]
                    and initial["first"] > anchor
                    and initial["last"] == editable
                )
                assert requested("get_messages")[0]["capture_unread"] is True
                mark_count = len(requested("mark_read"))
                await open_anchor(anchor)
                assert len(requested("mark_read")) == mark_count
                await ui.call("searchui", close=True)

                await ui.call("reading", older=True)
                await ui.wait(
                    "reading", lambda r: r["ready"] and r["first"] < initial["first"]
                )
                await ui.call("reading", latest=True, activate=True)
                await ui.wait("reading", lambda r: r["viewingLatest"])
                pending_file = root / "keep-upload.bin"
                pending_file.write_bytes(b"pending upload")
                gate.hold.add("confirm_attachment")
                await ui.call("upload", conv=group, path=str(pending_file))
                await gate.caught("confirm_attachment")
                transfer = await ui.wait(
                    "transfers",
                    lambda r: any(pending_file.name in label for label in r["labels"]),
                )
                label = next(
                    label for label in transfer["labels"] if pending_file.name in label
                )
                assert (await ui.call("transfers", label=label, action="暂停上传"))[
                    "triggered"
                ]
                upload_id = requested("confirm_attachment")[-1]["upload"]
                await gate.release("confirm_attachment")
                gate.hold_requests.add("send_message")
                await ui.call("send", text="排队的消息保持")
                await until(
                    lambda: bool(gate.held_requests),
                    "actual Outbox request held before server",
                )
                text_id = gate.held_requests[-1][1]["params"]["client_id"]
                (journal,) = (root / "data").rglob("tasks.sqlite3")

                def pending():
                    with sqlite3.connect(journal) as conn:
                        return (
                            conn.execute(
                                "SELECT client FROM tasks WHERE client=?", (upload_id,)
                            ).fetchone(),
                            conn.execute(
                                "SELECT client FROM text_outbox WHERE client=?",
                                (text_id,),
                            ).fetchone(),
                        )

                assert pending() == ((upload_id,), (text_id,))
                await ui.call("reply_to", id=editable)
                await ui.call("draft", text="导航不丢草稿")
                up = await ui.call("reading", up=True)
                assert up["latest"] and 0 < up["scroll"] < up["maximum"]
                newest = await send(b, "阅读时到达的新消息")
                await ui.wait("reading", lambda r: r["last"] == newest)
                assert (await ui.call("reading"))["scroll"] == up["scroll"]
                assert not read_at_least(newest)
                history_count = len(requested("get_messages"))
                await ui.call("reading", latest=True, activate=True)
                positioned = await ui.call("reading")
                assert positioned["viewingLatest"], positioned
                await until(
                    lambda: read_at_least(newest), "latest button advanced read cursor"
                )
                assert len(requested("get_messages")) == history_count
                draft = await ui.call("state")
                assert draft["draft"] == "导航不丢草稿" and draft["reply"]

                assert pending() == ((upload_id,), (text_id,))
                await gate.release_requests("send_message")
                await until(
                    lambda: pending()[1] is None,
                    "original Outbox accepted after navigation",
                )
                await ui.call("reading", up=True)
                read_elsewhere = await send(b, "另一设备推进已读")
                await a.ok("mark_read", conversation=group, message=read_elsewhere)
                assert read_at_least(read_elsewhere)
                assert (await ui.call("reading"))["unread"]
                gate.hold.add("get_messages")
                await gate.down.close()
                await gate.caught("get_messages")
                assert requested("get_messages")[-1].get("after") is not None
                assert not requested("get_messages")[-1].get("capture_unread")
                later = await send(b, "恢复期间的新消息")
                await ui.wait("reading", lambda r: r["last"] == later)
                stale = await ui.call("reading", bottom=True)
                assert stale["scroll"] == stale["maximum"] and stale["latest"]
                assert not stale["viewingLatest"] and not read_at_least(later)
                await ui.call("reading", latest=True)
                await gate.release("get_messages")
                await ui.wait("reading", lambda r: r["ready"] and r["viewingLatest"])
                await until(
                    lambda: read_at_least(later),
                    "recovered timeline advanced read cursor",
                )
                await open_anchor(anchor)
                await b.ok("delete_message", conversation=group, message=anchor)
                await ui.wait(
                    "searchui",
                    lambda r: r.get("status") == "消息不可用" and r["count"] == 0,
                )
                assert not (await ui.call("reading"))["unread"]
                await ui.call("searchui", close=True)
                print(
                    "PASS Qt captured old archived unread after auto-read, paging and reconnect; latest preserves reading position, draft, reply, Outbox and paused upload",
                    flush=True,
                )

                await ui.call("searchui", shortcut=True)
                result = await ui.wait(
                    "searchui", lambda r: len(r.get("ids", [])) == 50
                )
                assert result["mentions"] and result["ids"] == ids[::-1][:50]
                selected = (await ui.call("searchui", row=20))["selected"]
                before_edit = await ui.call("searchui")
                await b.ok(
                    "edit_message", conversation=group, message=later, text="无关编辑"
                )
                barrier = await ui.call("searchui")
                assert (
                    barrier["selected"] == selected
                    and barrier["scroll"] == before_edit["scroll"]
                )

                tail = result["ids"][-1]
                await b.ok(
                    "edit_message", conversation=group, message=tail, text="移除提及"
                )
                await ui.wait(
                    "searchui", lambda r: len(r["ids"]) == 49 and tail not in r["ids"]
                )
                await ui.call("searchui", more=True)
                await ui.wait("searchui", lambda r: len(r["ids"]) == 99)
                assert requested("search_all_messages")[-1]["before"] == tail
                await ui.call("searchui", more=True)
                await ui.wait(
                    "searchui", lambda r: len(r["ids"]) == 103 and not r["more"]
                )

                await b.ok(
                    "edit_message",
                    conversation=group,
                    message=editable,
                    text="@a 追加提及",
                )
                await ui.wait("searchui", lambda r: r["ids"][0] == editable)
                await b.ok("delete_message", conversation=group, message=editable)
                await ui.wait("searchui", lambda r: editable not in r["ids"])
                assert all(r["id"] != old for r in (await mentions())["messages"])

                gate.hold.add("search_all_messages")
                await ui.call("searchui", mentions=True, query="导航")
                await gate.caught("search_all_messages")
                await ui.call("searchui", mentions=False, query="完全没有结果")
                await gate.release("search_all_messages")
                await ui.wait(
                    "searchui",
                    lambda r: not r["ids"] and r["status"] == "没有找到相关消息",
                )
                await ui.call("searchui", mentions=True, query="")
                await ui.wait("searchui", lambda r: len(r["ids"]) == 50)
                await a.ok("clear_history", conversation=group, through=later)
                await ui.wait("searchui", lambda r: not r["ids"])
                fresh = await send(b, "@a 清空水位之后")
                await ui.call("searchui", submit=True)
                await ui.wait("searchui", lambda r: r["ids"] == [fresh])
                await b.ok("remove_group_member", conversation=group, user=1)
                await ui.wait(
                    "searchui", lambda r: not r["ids"] and group not in r["scopes"]
                )
                await b.ok("invite_group_members", conversation=group, members=[1])
                await ui.wait("searchui", lambda r: group in r["scopes"])
                assert not (await mentions(conversation=group))["messages"]
                await ui.call("searchui", close=True)

                await select(other)
                assert not (await ui.call("reading"))["unread"]
                stale_anchor = await send(b, "@a 不得迟到污染")
                gate.hold.add("get_messages")
                await ui.call("select", conv=group)
                await gate.caught("get_messages")
                await ui.call("select", conv=other)
                await gate.release("get_messages")
                await ui.wait("reading", lambda r: r["ready"])
                state = await ui.call("state")
                assert (
                    state["conv"] == other and not (await ui.call("reading"))["unread"]
                )
                assert all(r["id"] != stale_anchor for r in state["rows"])
                await select(group)
                await open_anchor(stale_anchor)
                await a.ok("clear_history", conversation=group, through=stale_anchor)
                await ui.wait(
                    "searchui",
                    lambda r: r.get("status") == "消息不可用" and r["count"] == 0,
                )
                assert not (await ui.call("reading"))["unread"]
                await ui.call("searchui", close=True)
                await select(other)
                lost_anchor = await send(b, "@a 失权时关闭上下文")
                await select(group)
                await open_anchor(lost_anchor)
                await b.ok("remove_group_member", conversation=group, user=1)
                await ui.wait(
                    "searchui",
                    lambda r: r.get("status") == "消息不可用" and r["count"] == 0,
                )
                assert not (await ui.call("reading"))["unread"]
                assert "error" in await a.ask(
                    "get_messages", conversation=group, around=lost_anchor
                )
                await ui.call("searchui", close=True)
                print(
                    "PASS real Qt mention shortcut, live add/remove, 50/51 and next-page cursor, stale filters, permissions and late conversation reply",
                    flush=True,
                )
            finally:
                await ui.stop()


async def native_session(root, db, group, anchor, last, ui, gate, send, peer):
    """Let real desktop input drive reading; stdin only observes or drives the peer/server."""
    print(
        json.dumps(
            dict(
                ready=True,
                group=group,
                first_unread=anchor,
                last=last,
                root=str(root),
                desktop=await ui.call("desktop_environment", conversation=group),
            )
        ),
        flush=True,
    )
    while line := await asyncio.to_thread(sys.stdin.readline):
        cmd = json.loads(line)
        action = cmd.pop("op")
        if action == "quit":
            return
        if action == "peer":
            result = {"id": await send(peer, cmd["text"])}
        elif action == "network":
            if cmd.get("hold"):
                gate.hold.add("get_messages")
            if cmd.get("disconnect"):
                await gate.down.close()
            if cmd.get("release"):
                await gate.release("get_messages")
            result = {"held": len(gate.held)}
        elif action == "observe":
            with sqlite3.connect(db) as conn:
                (read,) = conn.execute(
                    "SELECT last_read_message_id FROM conversation_members WHERE user_id=1 AND conversation_id=?",
                    (group,),
                ).fetchone()
            result = dict(
                reading=await ui.call("reading"),
                state=await ui.call("state"),
                desktop=await ui.call("desktop_environment", conversation=group),
                context=await ui.call("searchui"),
                read=read,
                held=len(gate.held),
                requests=[
                    message["params"]
                    for message in gate.messages
                    if message["method"] == "mark_read"
                ],
                sent=[
                    entry["params"]
                    for entry in gate.messages
                    if entry["method"] == "send_message"
                ],
                history=sum(
                    message["method"] == "get_messages" for message in gate.messages
                ),
            )
        else:
            result = {"error": "expected peer, network, observe or quit"}
        print(json.dumps(result), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    parser.add_argument(
        "--native",
        action="store_true",
        help="use the logged-in desktop, drive input externally",
    )
    parser.add_argument(
        "--theme",
        choices=("system", "light", "dark"),
        default="system",
        help="isolated native probe theme",
    )
    parser.add_argument(
        "--ctrl-enter",
        action="store_true",
        help="use Ctrl+Enter sending in the isolated native profile",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chat-reading-navigation-") as tmp:
        asyncio.run(
            run(
                Path(tmp),
                args.probe,
                native=args.native,
                theme=args.theme,
                ctrl_enter=args.ctrl_enter,
            )
        )
