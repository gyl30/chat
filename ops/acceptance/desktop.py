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
        self.hold_requests = set()
        self.held_requests = []
        self.transform = None

    async def handle(self, downstream):
        async with connect(self.upstream) as server:
            self.down = downstream
            methods = {}

            async def send():
                async for packet in downstream:
                    request = json.loads(packet)
                    methods[request["id"]] = request["method"]
                    self.messages.append(request)
                    if request["method"] in self.hold_requests:
                        self.held_requests.append((server, request))
                    else:
                        await server.send(packet)

            async def receive():
                async for packet in server:
                    result = json.loads(packet)
                    method = methods.pop(result.get("id"), result.get("method"))
                    if self.transform:
                        self.transform(method, result)
                    if method in self.hold:
                        self.held.append((method, result))
                    else:
                        await downstream.send(json.dumps(result) if self.transform else packet)

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

    async def release_requests(self, method):
        self.hold_requests.discard(method)
        for server, request in self.held_requests.copy():
            if request["method"] == method:
                self.held_requests.remove((server, request))
                await server.send(json.dumps(request))

    async def caught(self, method):
        await until(lambda: any(k == method for k, _ in self.held), method)


class Desktop:
    async def start(
        self,
        binary,
        root,
        url,
        user="a",
        user_id=1,
        password="pass",
        *,
        native=False,
        platform=None,
    ):
        env = dict(
            os.environ,
            XDG_DATA_HOME=str(root / "data"),
            XDG_CONFIG_HOME=str(root / "config"),
        )
        if native:
            if platform:
                env["QT_QPA_PLATFORM"] = platform
            else:
                env.pop("QT_QPA_PLATFORM", None)
            env["CHAT_TEST_NATIVE_DESKTOP"] = "1"
        else:
            env["QT_QPA_PLATFORM"] = "offscreen"
            env.pop("CHAT_TEST_NATIVE_DESKTOP", None)
        self.proc = await asyncio.create_subprocess_exec(
            binary, url, env=env, stdin=-1, stdout=-1
        )
        if platform:
            actual = (await self.call("desktop_environment"))["platform"]
            if actual != platform and not (
                platform == "wayland" and actual.startswith("wayland-")
            ):
                await self.stop()
                raise RuntimeError(
                    f"Qt platform mismatch: expected {platform}, got {actual}"
                )
        await self.call("login", user=user, password=password)
        await self.wait("state", lambda r: r["user"] == user_id)

    async def call(self, op, **params):
        self.proc.stdin.write((json.dumps(dict(op=op, **params)) + "\n").encode())
        await self.proc.stdin.drain()
        response = await asyncio.wait_for(self.proc.stdout.readline(), 8)
        if not response:
            raise RuntimeError(f"Qt probe exited before replying to {op}")
        return json.loads(response)

    async def wait(self, op, check, **params):
        async def test():
            result = await self.call(op, **params)
            return result if check(result) else None

        return await until(test, op)

    async def stop(self):
        if self.proc.returncode is None:
            self.proc.kill()
            await self.proc.wait()


async def sidebar_search_cases(ui, gate, reader, sender, conversation):
    buckets = {}
    for size in (4, 5, 6):
        query = f"侧栏{size}条"
        ids = []
        for n in range(size):
            text = f"{query} {n}"
            if size == 6 and n == 0:
                text = "长英文NoBreakToken" * 90 + "\n\n中文 😃\n" * 60 + f"{query} {query}"
            reply = await sender.ok(
                "send_message",
                conversation=conversation,
                text=text,
                client_id=7000 + size * 10 + n,
            )
            ids.append(reply["id"])
        buckets[query] = ids[::-1]

    for query, ids in buckets.items():
        start = len(gate.messages)
        await ui.call("sidebar_search", query=query)
        state = await ui.wait("sidebar_search", lambda r: r["ids"] == ids[:5])
        assert state["more"] == (len(ids) > 5), state
        assert not state["rooms"], state
        assert sum(
            request["method"] == "search_all_messages"
            for request in gate.messages[start:]
        ) == 1

    query = "侧栏6条"
    await ui.call("sidebar_search", open=True)
    preview = await ui.wait("searchui", lambda r: r.get("ids") == buckets[query])
    assert preview["heights"] == [70] * 6, preview["heights"]
    assert preview["snips"][-1].count(query) == 2 and "\n" not in preview["snips"][-1], preview["snips"][-1]
    await ui.call("searchui", close=True)
    state = await ui.call("sidebar_search")
    assert state["query"] == query and state["ids"] == buckets[query][:5], state

    gate.hold.add("search_all_messages")
    try:
        await ui.call("sidebar_search", query="侧栏4条")
        await gate.caught("search_all_messages")
        state = await ui.call("sidebar_search", query="")
        assert state["rooms"] and not state["ids"] and not state["more"], state
    finally:
        await gate.release("search_all_messages")
    state = await ui.call("sidebar_search")
    assert state["rooms"] and not state["ids"] and not state["more"], state
    gate.hold.add("search_all_messages")
    query = "侧栏空状态验收"
    try:
        loading = await ui.call("sidebar_search", query=query)
        assert loading["status"] == "正在搜索…" and not loading["rows"], loading
        await gate.caught("search_all_messages")
        assert (await ui.call("sidebar_search"))["status"] == "正在搜索…"
    finally:
        await gate.release("search_all_messages")
    empty = await ui.wait("sidebar_search", lambda r: r["status"] == "未找到相关结果")
    assert not empty["rows"] and not empty["more"], empty
    await reader.ok("set_friend_remark", user=2, remark=query)
    matched = await ui.wait("sidebar_search", lambda r: r["rows"] == 2)
    assert not matched["status"] and not matched["ids"] and not matched["more"], matched
    await reader.ok("set_friend_remark", user=2, remark="")
    await ui.wait("sidebar_search", lambda r: r["status"] == "未找到相关结果")
    await ui.call("sidebar_search", query="")
    print("PASS sidebar loading, real empty state and live friend refresh remain outside the result model")
    print("PASS sidebar preview limits, newest-first IDs, automatic full search and late-response isolation")


async def search_cases(ui, gate, a, b, g):
    await sidebar_search_cases(ui, gate, a, b, g)
    ids = []
    for n in range(102):
        ids.append((await b.ok("send_message", conversation=g, text=f"事务回归 {n}", client_id=100 + n))["id"])

    async def show(query):
        await ui.call("searchui", query=query)

    async def rows(count):
        return await ui.wait("searchui", lambda r: len(r.get("ids", [])) == count)

    await ui.call("searchui", open=True)
    await ui.wait("searchui", lambda r: r.get("open"))

    unrelated = (await b.ok("create_group", title="跨设备清空", members=[1]))["conversation"]
    marker = (await b.ok("send_message", conversation=unrelated, text="测试清空", client_id=999))["id"]
    await ui.call("searchui", query="必定没有匹配", type_only=True)
    start = len(gate.messages)
    await a.ok("clear_history", conversation=unrelated, through=marker)
    await until(lambda: any(m["method"] == "get_conversations" for m in gate.messages[start:]), "clear event")
    state = await ui.call("searchui")
    assert not state["status"] and not state["ids"], state
    assert not any(m["method"] == "search_all_messages" for m in gate.messages[start:])
    await ui.call("searchui", submit=True)
    await ui.wait("searchui", lambda r: r["status"] == "未找到相关消息" and not r["ids"])
    # Open an old snapshot only after a newer cleared snapshot has already landed.
    source = (await b.ok("send_message", conversation=unrelated, text="引用源", client_id=3100))["id"]
    anchor = (await a.ok("send_message", conversation=unrelated, text="上下文定位", client_id=3101, reply_to=source))["id"]
    peer = (await b.ok("send_message", conversation=unrelated, text="邻近初稿", client_id=3102))["id"]
    await show("上下文定位")
    await rows(1)
    gate.hold.add("get_messages")
    await ui.call("searchui", row=0, enter=True)
    await gate.caught("get_messages")
    gate.hold.discard("get_messages")
    await a.ok("clear_history", conversation=unrelated, through=source)
    current = await ui.wait("searchui", lambda r: r.get("anchor") == anchor, inspect=anchor)
    assert current["contextIds"] == [anchor, peer] and current["outgoing"] and not current["quote"], current
    await gate.release("get_messages")
    await b.ok("edit_message", conversation=unrelated, message=peer, text="邻近定稿")
    await ui.wait("searchui", lambda r: r.get("body") == "邻近定稿", inspect=peer)
    await b.ok("delete_message", conversation=unrelated, message=peer)
    current = await ui.wait("searchui", lambda r: r.get("deleted"), inspect=peer)
    assert current["body"] == "消息已撤回" and current["anchor"] == anchor and current["contextIds"] == [anchor, peer]
    await a.ok("clear_history", conversation=unrelated, through=anchor)
    await ui.wait("searchui", lambda r: not r["ctx"] and not r["ids"] and not r["count"])
    print("PASS context bubbles: stale around after clear, outgoing anchor, quote redaction, adjacent edit and withdrawal", flush=True)
    await show("事务回归")
    await rows(50)
    await ui.call("searchui", more=True)
    await rows(100)
    await ui.call("searchui", row=50)
    position = (await ui.call("searchui"))["scroll"]
    assert position > 0
    await b.ok("delete_message", conversation=g, message=ids[2])
    assert (await rows(99))["scroll"] == position
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
        assert state["status"] == "未找到相关消息", state
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
    assert error and error != "未找到相关消息"
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
    # A real PNG whose message visibility differs for each member.
    import base64

    png = base64.b64decode(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC"
    )
    p = await begin(a, g, 951, png, "inspect.png", "image/png")
    await a.ok("confirm_attachment", upload=951, number=1, etag=put(p["url"], png))
    image = await a.ok("finish_attachment", upload=951)
    await ui.call("preview", conv=g, id=image["id"])
    await ui.wait("dialogstate", lambda r: r.get("image"))
    await ui.call("dialogstate", save=True)
    await ui.wait("dialogstate", lambda r: r.get("chooser"))
    await a.ok("clear_history", conversation=g, through=image["id"])
    await ui.wait("dialogstate", lambda r: not r["shown"])
    assert "url" in await b.ok("get_attachment", conversation=g, message=image["id"])
    denied = await a.ask("get_attachment", conversation=g, message=image["id"])
    assert denied.get("error", {}).get("code") == -32007
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


async def outbox_queue(ui, gate, direct, group, root, db, binary, entry):
    await ui.stop()
    (journal,) = (root / "data").rglob("tasks.sqlite3")
    first = list(range(8101, 8113))
    second = list(range(8201, 8207))
    rejected = 8100
    with sqlite3.connect(journal) as conn:
        conn.executemany(
            "INSERT INTO text_outbox(user,client,conversation,body,created,rejection) VALUES(?,?,?,?,unixepoch(),?)",
            [(1, rejected, direct, "已拒绝", -32015)]
            + [(1, client, direct, f"队列 {client}", 0) for client in first]
            + [(1, client, group, f"队列 {client}", 0) for client in second],
        )
    start = len(gate.messages)
    method = "send_message"
    gate.hold.add(method)
    await ui.start(binary, root, entry)

    def sent(conversation):
        return [
            r["params"]["client_id"]
            for r in gate.messages[start:]
            if r["method"] == method and r["params"]["conversation"] == conversation
        ]

    await until(lambda: len(gate.held) == 2, "only the two independent Outbox heads")
    assert sent(direct) == first[:1] and sent(group) == second[:1]
    await gate.release(method)

    def remaining():
        with sqlite3.connect(journal) as conn:
            return conn.execute("SELECT client FROM text_outbox WHERE user=1").fetchall()

    await until(lambda: remaining() == [(rejected,)], "queued messages acknowledged, rejection retained")
    assert sent(direct) == first and sent(group) == second
    with sqlite3.connect(db) as conn:
        for conversation, expected in ((direct, first), (group, second)):
            assert [
                client for (client,) in conn.execute(
                    "SELECT client_id FROM messages WHERE conversation_id=? AND sender_id=1 "
                    "AND client_id BETWEEN 8100 AND 8206 ORDER BY id", (conversation,)
                )
            ] == expected
    print("PASS Outbox dispatches independent heads, preserves 18 row order and skips definitive rejection", flush=True)


async def file_search(ui, gate, b):
    group = (await b.ok("create_group", title="共享文件搜索", members=[1]))["conversation"]
    names = ["Résumé%_文件.TXT", "Réglage.zip"]

    async def publish(client, filename):
        data = filename.encode()
        part = await begin(b, group, client, data, filename)
        await b.ok("confirm_attachment", upload=client, number=1, etag=put(part["url"], data))
        return (await b.ok("finish_attachment", upload=client))["id"]

    first_id, second_id = [await publish(client, name) for client, name in enumerate(names, 9001)]
    await ui.wait("select", lambda r: r["ok"], conv=group)
    await ui.call("history", open=True, conv=group)
    await ui.wait("history", lambda r: r.get("count") == 2)
    await ui.call("history", tab=2)
    await ui.wait("history", lambda r: r.get("count") == 2)
    method = "get_messages"
    gate.hold.add(method)
    await ui.call("history", query="RÉ")
    await gate.caught(method)
    await ui.call("history", query="RÉSUMÉ%_")
    await until(lambda: len(gate.held) == 2, "two file search replies")
    gate.held.reverse()
    await gate.release(method)
    await ui.wait("history", lambda r: [v["id"] for v in r["rows"]] == [first_id])

    await ui.call("history", query="不存在的文件")
    await ui.wait(
        "history",
        lambda r: r.get("widgetRows") == 0 and r.get("status") == "" and not r["statusVisible"] and not r["more"],
    )
    await ui.call("history", tab=1)
    await ui.wait("history", lambda r: r.get("widgetRows") == 0 and r.get("status") == "" and not r["more"])
    await ui.call("history", tab=2)

    pages = [await publish(client, f"分页{index}.txt") for index, client in enumerate(range(9100, 9151))]
    await ui.call("history", query="分页")
    first = (await ui.wait("history", lambda r: r.get("count") == 50 and r["more"]))["rows"]
    assert [v["id"] for v in first] == pages[:0:-1]
    await ui.call("history", more=True)
    rows = (await ui.wait("history", lambda r: r.get("count") == 51 and not r["more"]))["rows"]
    assert [v["id"] for v in rows] == pages[::-1]
    await ui.call("history", query="RÉ")
    await ui.wait("history", lambda r: [v["id"] for v in r["rows"]] == [second_id, first_id])
    assert (await b.ok("remove_group_member", conversation=group, user=1))["changed"]
    await ui.wait("state", lambda r: r["conv"] != group)
    await ui.wait("history", lambda r: not r["open"])
    print("PASS shared files: Unicode literal names, delayed replies, empty tabs, 50/51 pages and membership loss", flush=True)


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
        part_size = 16 * 1024 * 1024
        sources = {}
        for target, size in ((d, part_size + 1), (second, part_size - 1), (g, part_size)):
            folder = root / str(target)
            folder.mkdir()
            source = folder / "同名.bin"
            source.write_bytes(b"x" * size)
            sources[target] = source
        path = sources[d]
        gate.hold.add("confirm_attachment")
        for target, source in sources.items():
            await ui.call("upload", conv=target, path=str(source))

        def tasks():
            with sqlite3.connect(db) as conn:
                return dict(conn.execute("select conversation,client_id from attachment_uploads"))

        await until(lambda: len(tasks()) == 3, "three distinct uploads")
        ids = tasks()
        assert len(set(ids.values())) == 3

        def first_parts():
            with sqlite3.connect(db) as conn:
                return set(conn.execute(
                    "SELECT u.client_id,p.number FROM attachment_uploads u JOIN attachment_parts p ON p.upload=u.id WHERE p.etag<>''"
                ))

        await until(lambda: first_parts() == {(cid, 1) for cid in ids.values()}, "each transfer has exactly one confirmed part")
        painted = await ui.call("transfers")
        assert painted["confirmed"] == [0, 0, 0], painted

        async def label(prefix, *, ready=False):
            field = "resumable" if ready else "labels"
            state = await ui.wait("transfers", lambda r: any(x.startswith(prefix + " · ") for x in r[field]))
            return next(x for x in state[field] if x.startswith(prefix + " · "))

        async def action(prefix, name):
            row = await label(prefix, ready=name == "继续上传")
            assert path.name in row
            assert (await ui.call("transfers", label=row, action=name)).get("triggered")

        def paused():
            with sqlite3.connect(db) as conn:
                return {c for (c,) in conn.execute("select conversation from attachment_uploads where state='paused'")}

        expected = set()
        for target, prefix in ((d, "备注乙"), (second, "c"), (g, "资料")):
            await action(prefix, "暂停上传")
            expected.add(target)
            await until(lambda: paused() == expected, "only selected task paused")
        assert paused() == set(ids)
        await a.ok("hide_conversation", conversation=second)
        await a.ok("hide_conversation", conversation=g)
        await label(f"会话 #{second}")
        await ui.call("quit")
        assert await asyncio.wait_for(ui.proc.wait(), 8) == 0
        gate.hold.clear()
        gate.held.clear()
        await ui.start(binary, root, entry)
        await action(f"会话 #{second}", "继续上传")
        await until(lambda: tasks() == {d: ids[d], g: ids[g]}, "resume only hidden direct")
        await action(f"会话 #{g}", "继续上传")
        await until(lambda: tasks() == {d: ids[d]}, "resume only hidden group attachment")
        await action("备注乙", "继续上传")
        await until(lambda: not tasks(), "finish visible direct")
        resumed = [rpc["params"]["upload"] for rpc in gate.messages if rpc["method"] == "resume_attachment"]
        assert resumed == [ids[second], ids[g], ids[d]]
        with sqlite3.connect(db) as conn:
            assert conn.execute(
                "SELECT conversation_id,count(*) FROM messages WHERE sender_id=1 AND client_id=? GROUP BY conversation_id",
                (ids[second],),
            ).fetchall() == [(second, 1)]
        files = (await a.ok("get_messages", conversation=g, category="files"))["messages"]
        assert [f["client_id"] for f in files if f["attachment"]["filename"] == path.name] == [ids[g]]
        with sqlite3.connect(db) as conn:
            (message,) = conn.execute(
                "SELECT m.id FROM messages m JOIN message_attachments a ON a.message_id=m.id "
                "WHERE m.conversation_id=? AND m.client_id=?", (d, ids[d])
            ).fetchone()
        target = root / "downloaded.bin"
        await ui.call("preview", conv=d, id=message, filename=path.name, inline=False)
        await ui.wait("dialogstate", lambda r: r["shown"])
        await ui.call("dialogstate", save=True, destination=str(target))
        await until(target.exists, "SDK downloaded the attachment")
        assert target.read_bytes() == path.read_bytes()
        await ui.call("dialogstate", close=True)
        assert path.exists()
        print(
            "PASS same-name group and direct attachments at 16 MiB +/- 1 byte, hidden destinations, same-device restart and SDK download",
            flush=True,
        )


async def notifications(ui, gate, a, b, group, root):
    def lines():
        return (root / "notifications").read_text().splitlines() if (root / "notifications").exists() else []

    await ui.wait("filter", lambda s: "资料" in s["names"])
    gate.hold.add("get_conversations")

    async def level(n, expires=0):
        count = len(gate.held)
        await a.ok("set_notification_level", conversation=group, level=n, until=expires)
        await until(lambda: len(gate.held) > count, "preference processed")

    expected = []
    for lvl in (2, 1, 0):
        await level(lvl)
        for kind in ("普通", "@a", "全员"):
            text = f"{kind} 通知探针 {lvl}"
            await b.ok("send_message", conversation=group, text=text, client_id=501+lvl*3+("普通","@a","全员").index(kind), mention_all=kind=="全员")
            if lvl==0 or (lvl==1 and kind!="普通"):
                expected.append(text)
    gate.held.reverse()
    await gate.release("get_conversations")
    await until(lambda: expected[-1] in lines(), "notification matrix")
    assert [x for x in lines() if "通知探针" in x] == expected
    gate.hold.add("get_conversations")
    expires = int(time.time() * 1000) + 2000
    await level(2, expires)
    count = (await ui.call("deliveries")).get("new", 0)
    await b.ok("send_message", conversation=group, text="临时免打扰内", client_id=701, mention_all=True)
    await ui.wait("deliveries", lambda r: r.get("new", 0) > count)
    assert int(time.time() * 1000) < expires
    await until(lambda: int(time.time() * 1000) >= expires, "mute deadline")
    await b.ok("send_message", conversation=group, text="临时免打扰到期", client_id=702, mention_all=True)
    await until(lambda: "临时免打扰到期" in lines(), "expired mute allows new notification")
    assert "临时免打扰内" not in lines()
    await level(0)
    await gate.release("get_conversations")
    print("PASS Qt notification events with reversed, delayed list snapshots", flush=True)


async def contacts_cases(ui, stack, url, a):
    await ui.call("contacts", open=True)
    await ui.wait("contacts", lambda r: r["names"] == ["b"])
    assert not (await ui.call("contacts"))["requestsEntry"], "empty requests entry"
    await a.ok("create_friend_group", name="同事")
    await a.ok("move_friend", user=2, group="同事")
    await a.ok("set_friend_remark", user=2, remark="小北")
    await ui.wait("contacts", lambda r: r["names"] == ["小北"])
    await ui.wait("contacts", lambda r: r["groups"] == {"我的好友": 0, "同事": 1})
    assert not (await ui.call("contacts", toggle="同事"))["names"]
    await ui.call("contacts", close=True)
    assert not (await ui.call("contacts", open=True))["names"]
    matches = await ui.call("contacts", filter="小北")
    assert matches["names"] == ["小北"] and matches["groups"] == {"同事": 1}
    assert (await ui.call("contacts", filter="b"))["names"] == ["小北"]
    assert not (await ui.call("contacts", filter="无匹配"))["names"]
    restored = await ui.call("contacts", filter="")
    assert not restored["names"] and restored["groups"] == {"同事": 1, "我的好友": 0}
    await a.ok("rename_friend_group", group="同事", name="工作")
    await ui.wait("contacts", lambda r: r["groups"] == {"我的好友": 0, "工作": 1} and r["names"] == ["小北"])
    await ui.call("contacts", profile=True)
    await ui.wait("contacts", lambda r: r.get("profile") == "b" and r["friend"])
    await ui.call("contacts", closeProfile=True)
    await a.ok("delete_friend_group", group="工作")
    await ui.wait("contacts", lambda r: r["groups"] == {"我的好友": 1} and r["names"] == ["小北"])

    c = await login(stack, url, "c", "c", True)
    await c.ok("send_friend_request", user=1)
    await ui.wait("contacts", lambda r: r["requests"] == 1 and r["requestsEntry"])
    await ui.call("contacts", requests=True, reject=True)
    await ui.wait("contacts", lambda r: r["requests"] == 0 and not r["requestsEntry"])
    assert (await ui.call("contacts"))["names"] == ["小北"]
    await a.ok("send_friend_request", user=3)
    await ui.wait("contacts", lambda r: not r["requests"] and r["requestsEntry"])
    await c.ok("respond_friend_request", user=1, accept=True)
    await ui.wait("contacts", lambda r: "c" in r["names"])
    await c.ok("remove_contact", user=1)
    await ui.wait("contacts", lambda r: r["names"] == ["小北"])
    await ui.call("contacts", close=True)
    await a.ok("set_friend_remark", user=2, remark="")
    print(
        "PASS real contact sync: request and refusal, acceptance/removal, alias search, profile, move/rename/delete group",
        flush=True,
    )


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
                        if sys.argv[2:] == ["contacts"]:
                            await contacts_cases(ui, stack, url, a)
                            return
                        if sys.argv[2:] == ["transfers"]:
                            await transfers(ui, gate, a, b, g, d, root, db, binary, entry, url)
                            return
                        if sys.argv[2:] == ["pin"]:
                            await pin_cases(ui, gate, a, b, g)
                            return
                        if sys.argv[2:] == ["search"]:
                            await search_cases(ui, gate, a, b, other)
                            return
                        if sys.argv[2:] == ["outbox"]:
                            await outbox(ui, gate, a, b, d, root, db, binary, entry)
                            await outbox_queue(ui, gate, d, g, root, db, binary, entry)
                            return
                        if sys.argv[2:] == ["file-search"]:
                            await file_search(ui, gate, b)
                            return
                        await notifications(ui, gate, a, b, g, root)
                        await search_cases(ui, gate, a, b, other)
                        await other_views(ui, gate, a, b, g, d)
                        await outbox(ui, gate, a, b, d, root, db, binary, entry)
                        await outbox_queue(ui, gate, d, g, root, db, binary, entry)
                        await file_search(ui, gate, b)
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
