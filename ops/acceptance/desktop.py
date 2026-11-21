"""Real Qt + SDK + server scenarios; hold actual RPC replies at selected boundaries."""

import asyncio
from contextlib import AsyncExitStack
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve

from isolated import Store, begin, chat, login, put


async def until(check, label):
    start = time.monotonic()
    while time.monotonic() - start < 12:
        result = check()
        if asyncio.iscoroutine(result):
            result = await result
        if result:
            return result
        await asyncio.sleep(0.025)
    raise AssertionError(label)


class Gate:
    def __init__(self, upstream):
        self.upstream = upstream
        self.hold = set()
        self.messages = []
        self.held = []

    async def handle(self, downstream):
        async with connect(self.upstream) as server:
            self.down = downstream
            methods = {}

            async def send():
                async for packet in downstream:
                    request = json.loads(packet)
                    methods[request["id"]] = request["method"]
                    self.messages.append(request)
                    await server.send(packet)

            async def receive():
                async for packet in server:
                    result = json.loads(packet)
                    method = methods.pop(result.get("id"), result.get("method"))
                    if method in self.hold:
                        self.held.append((method, result))
                    else:
                        await downstream.send(packet)

            tasks = [asyncio.create_task(fn()) for fn in (send, receive)]
            try:
                await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)

    async def release(self, method, error=False):
        self.hold.discard(method)
        for key, reply in self.held.copy():
            if key == method:
                self.held.remove((key, reply))
                if error:
                    reply.pop("result", None)
                    reply["error"] = {"code": -32000, "message": "QA search failure"}
                await self.down.send(json.dumps(reply))

    async def caught(self, method):
        await until(lambda: any(k == method for k, _ in self.held), method)


class Desktop:
    async def start(self, binary, root, url):
        env = dict(
            os.environ,
            QT_QPA_PLATFORM="offscreen",
            XDG_DATA_HOME=str(root / "data"),
            XDG_CONFIG_HOME=str(root / "config"),
        )
        self.proc = await asyncio.create_subprocess_exec(binary, url, env=env, stdin=-1, stdout=-1)
        await self.call("login", user="a")
        await self.wait("state", lambda r: r["user"] == 1)

    async def call(self, op, **params):
        self.proc.stdin.write((json.dumps(dict(op=op, **params)) + "\n").encode())
        await self.proc.stdin.drain()
        return json.loads(await asyncio.wait_for(self.proc.stdout.readline(), 8))

    async def wait(self, op, check, **params):
        async def test():
            result = await self.call(op, **params)
            return result if check(result) else None

        return await until(test, op)

    async def stop(self):
        if self.proc.returncode is None:
            self.proc.kill()
            await self.proc.wait()


async def search_cases(ui, gate, a, b, g):
    ids = []
    for n in range(102):
        ids.append((await b.ok("send_message", conversation=g, text=f"事务回归 {n}", client_id=100 + n))["id"])

    async def show(query):
        await ui.call("searchui", query=query)

    async def rows(count):
        return await ui.wait("searchui", lambda r: len(r.get("ids", [])) == count)

    await ui.call("searchui", open=True)
    await ui.wait("searchui", lambda r: r.get("open"))
    await show("事务回归")
    await rows(50)
    await ui.call("searchui", more=True)
    await rows(100)
    await b.ok("delete_message", conversation=g, message=ids[2])
    await rows(99)
    await ui.call("searchui", more=True)
    await rows(101)
    assert [
        r["params"]["before"]
        for r in gate.messages
        if r["method"] == "search_all_messages" and "before" in r["params"]
    ][-1] == ids[2]
    for revoke in (False, True):
        await show("事务回归 101" if revoke else "事务回归 100")
        state = await rows(1)
        params = dict(conversation=g, message=state["ids"][0])
        if revoke:
            await b.ok("delete_message", **params)
        else:
            await b.ok("edit_message", **params, text="改后正文")
        state = await rows(0)
        assert state["status"] == "没有找到相关消息", state
    # First page made invisible while a later page is pending. Inject an RPC error
    # and revoke one remaining hit to ensure that errors survive row removal.
    await show("事务回归")
    first = await rows(50)
    assert first["more"]
    gate.hold.add("search_all_messages")
    await ui.call("searchui", more=True)
    await gate.caught("search_all_messages")
    for mid in first["ids"][:-1]:
        await b.ok("delete_message", conversation=g, message=mid)
    state = await rows(1)
    assert "搜索" in state["status"]
    await gate.release("search_all_messages", error=True)
    state = await ui.wait("searchui", lambda r: r["more"])
    error = state["status"]
    assert error and error != "没有找到相关消息"
    await b.ok("delete_message", conversation=g, message=first["ids"][-1])
    state = await rows(0)
    assert state["more"] and state["status"] == error
    await ui.call("searchui", more=True)
    await rows(49)
    # Deliver a real around response only after membership is revoked.
    gate.hold.add("get_messages")
    await ui.call("searchui", row=0, enter=True)
    await gate.caught("get_messages")
    await b.ok("remove_group_member", conversation=g, user=1)
    await ui.wait("searchui", lambda r: not r["ctx"] and not r["ids"])
    await gate.release("get_messages")
    assert not (await ui.call("searchui"))["count"]
    await ui.call("searchui", close=True)
    print("PASS Qt search: empty on edit/retract, pending/error/continuation, three pages, late around", flush=True)


async def pin_cases(ui, gate, a, b, g):
    await b.ok("set_group_announcement", conversation=g, text="独立公告")
    old = (await b.ok("send_message", conversation=g, text="旧置顶正文", client_id=11))["id"]
    await b.ok("pin_group_message", conversation=g, message=old)
    await ui.call("group", id=g)
    await ui.wait("pin", lambda r: "旧置顶正文" in r.get("pin", ""))
    gate.hold.add("get_conversations")
    await b.ok("send_message", conversation=g, text="更新列表", client_id=12)
    await gate.caught("get_conversations")
    await a.ok("clear_history", conversation=g, through=old)
    state = await ui.wait("pin", lambda r: r.get("pin") == "暂无置顶消息")
    assert state["announcement"] == "独立公告"
    await gate.release("get_conversations")
    assert (await ui.call("pin"))["pin"] == "暂无置顶消息"
    newer = (await b.ok("send_message", conversation=g, text="新置顶正文", client_id=13))["id"]
    await b.ok("pin_group_message", conversation=g, message=newer)
    await ui.wait("pin", lambda r: "新置顶正文" in r.get("pin", ""))
    gate.hold.add("get_conversations")
    await a.ok("clear_history", conversation=g, through=newer)
    await ui.wait("pin", lambda r: r.get("pin") == "暂无置顶消息")
    await gate.caught("get_conversations")
    await gate.release("get_conversations", error=True)
    await gate.down.close()
    state = await ui.wait("pin", lambda r: "断开" in r.get("error", ""))
    assert state["pin"] == "暂无置顶消息" and state["announcement"] == "独立公告"
    print("PASS Qt open group overview: equal boundary, delayed/failed list, later pin, disconnect", flush=True)


async def other_views(ui, gate, a, b, g, d):
    # A real PNG, shared between an ordinary message and a group-file reference.
    import base64

    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"
    )
    p = await begin(a, g, 951, png, "inspect.png", "image/png")
    await a.ok("confirm_attachment", upload=951, number=1, etag=put(p["url"], png))
    image = await a.ok("finish_attachment", upload=951)
    key = image["attachment"]["object_key"]
    await a.ok("add_conversation_file", conversation_id=g, object_key=key)
    await ui.call("preview", conv=g, id=image["id"])
    await ui.wait("dialogstate", lambda r: r.get("image"))
    await ui.call("dialogstate", save=True)
    await ui.wait("dialogstate", lambda r: r.get("chooser"))
    await a.ok("clear_history", conversation=g, through=image["id"])
    await ui.wait("dialogstate", lambda r: not r["shown"])
    assert "url" in await b.ok("get_attachment", conversation=g, message=image["id"])
    assert "url" in await a.ok("get_conversation_file", conversation_id=g, object_key=key)
    late = await begin(a, g, 952, png, "late.png", "image/png")
    await a.ok("confirm_attachment", upload=952, number=1, etag=put(late["url"], png))
    late = (await a.ok("finish_attachment", upload=952))["id"]
    gate.hold.add("get_attachment")
    await ui.call("preview", conv=g, id=late)
    await gate.caught("get_attachment")
    await a.ok("clear_history", conversation=g, through=late)
    await ui.wait("dialogstate", lambda r: not r["shown"])
    await gate.release("get_attachment")
    assert not (await ui.call("dialogstate"))["shown"]
    await ui.wait("select", lambda r: r["ok"], conv=d)
    await ui.call("draft", text="保留草稿")
    old = (await b.ok("send_message", conversation=d, text="截止", client_id=210))["id"]
    await ui.wait("state", lambda r: any(m["id"] == old for m in r["rows"]))
    gate.hold.update({"clear_history", "conversation_view"})
    await ui.call("clear", conv=d)
    await ui.wait("confirm", lambda r: r["open"])
    await gate.caught("clear_history")
    newer = (await b.ok("send_message", conversation=d, text="保留", client_id=211))["id"]
    gate.held.clear()
    gate.hold.clear()
    await ui.call("clear", conv=d)
    await until(lambda: len([r for r in gate.messages if r["method"] == "clear_history"]) == 2, "clear retry")
    assert [r["params"]["through"] for r in gate.messages if r["method"] == "clear_history"] == [old, old]
    st = await ui.wait("state", lambda r: any(m["id"] == newer for m in r["rows"]))
    assert st["draft"] == "保留草稿" and all(m["id"] > old for m in st["rows"])
    print("PASS Qt image/save invalidation, independent references and draft after fixed-H retry", flush=True)


async def outbox(ui, gate, a, b, d, root, db, binary, url):
    await ui.wait("select", lambda r: r["ok"], conv=d)
    await ui.wait("state", lambda r: r["enabled"])
    gate.hold.add("send_message")
    text = "确认丢失"
    await ui.call("send", text=text)
    await gate.caught("send_message")
    (journal,) = list((root / "data").rglob("tasks.sqlite3"))

    def out():
        with sqlite3.connect(journal) as c:
            return c.execute("select client from text_outbox where body=?", (text,)).fetchall()

    ((client_id,),) = out()
    await ui.stop()
    with sqlite3.connect(db) as c:
        (mid,) = c.execute("select id from messages where sender_id=1 and client_id=?", (client_id,)).fetchone()
    await a.ok("clear_history", conversation=d, through=mid)
    gate.held.clear()
    gate.hold.clear()
    await ui.start(binary, root, url)
    await until(lambda: not out(), "hidden ACK retires outbox")
    await ui.wait("select", lambda r: r["ok"], conv=d)
    assert all(m["text"] != text for m in (await ui.call("state"))["rows"])
    assert [m["id"] for m in (await b.ok("search_all_messages", query=text))["messages"]] == [mid]
    print("PASS real Qt process death, hidden receipt reconciliation with original client_id", flush=True)


async def transfers(ui, gate, a, b, g, d, root, db, binary, entry, server):
    async with AsyncExitStack() as stack:
        c = await login(stack, server, "c", "c", True)
        await a.ok("send_friend_request", user=3)
        await c.ok("respond_friend_request", user=1, accept=True)
        second = (await a.ok("open_direct_conversation", user=3))["conversation"]
        await c.ok("send_message", conversation=second, text="新会话", client_id=333)
        await a.ok("set_friend_remark", user=2, remark="备注乙")
        await ui.wait("select", lambda r: r["ok"], conv=second)
        await ui.wait("select", lambda r: r["ok"], conv=d)
        path = root / "同名.bin"
        path.write_bytes(b"menu destination verification")
        gate.hold.add("confirm_attachment")
        for target in (d, second, g):
            await ui.call("upload", conv=target, path=str(path), group=target == g)

        def tasks():
            with sqlite3.connect(db) as conn:
                return dict(conn.execute("select conversation,client_id from attachment_uploads"))

        await until(lambda: len(tasks()) == 3, "three distinct uploads")
        ids = tasks()
        assert len(set(ids.values())) == 3

        async def label(prefix):
            state = await ui.wait("transfers", lambda r: any(x.startswith(prefix + " · ") for x in r["labels"]))
            return next(x for x in state["labels"] if x.startswith(prefix + " · "))

        async def action(prefix, name):
            row = await label(prefix)
            assert path.name in row
            assert (await ui.call("transfers", label=row, action=name)).get("triggered")

        def paused():
            with sqlite3.connect(db) as conn:
                return {c for (c,) in conn.execute("select conversation from attachment_uploads where state='paused'")}

        expected = set()
        for target, prefix in ((d, "备注乙"), (second, "c"), (g, "资料 · 群文件")):
            await action(prefix, "暂停上传")
            expected.add(target)
            await until(lambda: paused() == expected, "only selected task paused")
        assert paused() == set(ids)
        await a.ok("hide_conversation", conversation=second)
        await label(f"会话 #{second}")
        await ui.stop()
        gate.hold.clear()
        gate.held.clear()
        await ui.start(binary, root, entry)
        await action(f"会话 #{second}", "取消上传")
        await until(lambda: set(tasks()) == {d, g}, "cancel only hidden task")
        await action("备注乙", "继续上传")
        await until(lambda: set(tasks()) == {g}, "resume only direct")
        await action("资料 · 群文件", "继续上传")
        await until(lambda: not tasks(), "finish group file")
        files = (await a.ok("list_conversation_files", conversation_id=g))["files"]
        assert any(f["filename"] == path.name for f in files)
        assert path.exists()
        print(
            "PASS same-name independent transfers, group file, hidden destination, restart, pause/resume/cancel",
            flush=True,
        )


async def notifications(ui, gate, a, b, group, root):
    def lines():
        return (root / "notifications").read_text().splitlines() if (root / "notifications").exists() else []

    await ui.wait("filter", lambda s: "资料" in s["names"])
    gate.hold.add("get_conversations")

    async def level(n):
        count = len(gate.held)
        await a.ok("set_notification_level", conversation=group, level=n, until=0)
        await until(lambda: len(gate.held) > count, "preference processed")

    async def send(text, i):
        await b.ok("send_message", conversation=group, text=text, client_id=i)

    await level(2)
    await send("静音通知探针", 501)
    await level(1)
    await send("普通通知探针", 502)
    await send("@a 通知探针", 503)
    await until(lambda: "@a 通知探针" in lines(), "mention notification")
    await level(0)
    gate.held.reverse()
    await gate.release("get_conversations")
    await send("全量通知探针", 504)
    await until(lambda: "全量通知探针" in lines(), "unmuted notification")
    assert [x for x in lines() if "通知探针" in x] == ["@a 通知探针", "全量通知探针"]
    print("PASS Qt notification events with reversed, delayed list snapshots", flush=True)


async def main(root, binary):
    store = Store()
    db = root / "app.sqlite"
    with store.node(root / "silo") as (api, env):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                b = await login(stack, url, "b", "b", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                d = (await a.ok("open_direct_conversation", user=2))["conversation"]

                async def group(name):
                    return (await b.ok("create_group", title=name, members=[1]))["conversation"]

                g, other = await group("资料"), await group("搜索")
                gate = Gate(url)
                async with serve(gate.handle, "127.0.0.1", 0) as proxy:
                    entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
                    ui = Desktop()
                    await ui.start(binary, root, entry)
                    try:
                        if sys.argv[2:] == ["pin"]:
                            await pin_cases(ui, gate, a, b, g)
                            return
                        await notifications(ui, gate, a, b, g, root)
                        await search_cases(ui, gate, a, b, other)
                        await other_views(ui, gate, a, b, g, d)
                        await outbox(ui, gate, a, b, d, root, db, binary, entry)
                        await transfers(ui, gate, a, b, g, d, root, db, binary, entry, url)
                        await pin_cases(ui, gate, a, b, g)
                    finally:
                        await ui.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        # Start a private bus even on a workstation with its own notification daemon.
        if "--private-bus" not in sys.argv:
            raise SystemExit(
                subprocess.call(["dbus-run-session", "--", sys.executable, __file__, *sys.argv[1:], "--private-bus"])
            )
        sys.argv.remove("--private-bus")

        async def run():
            root = Path(tmp)
            sink = await asyncio.create_subprocess_exec(
                sys.executable, str(Path(__file__).with_name("notification_sink.py")), tmp
            )
            try:
                await until(lambda: (root / "bus-ready").exists(), "private notification bus")
                await main(root, sys.argv[1])
            finally:
                sink.terminate()
                await sink.wait()

        asyncio.run(run())
