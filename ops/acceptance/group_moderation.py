"""Isolated real Chat RPC, SQLite, Qt and Silo group-management acceptance."""

import asyncio
from contextlib import AsyncExitStack, contextmanager
import hashlib
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
from threading import Event, Thread
from urllib.parse import parse_qs, urlsplit
import time

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put

MUTED = -32015


@contextmanager
def hold_silo_completion(storage):
    """Forward signed requests to real Silo and hold the successful Complete response."""
    origin = storage["CHAT_S3_ENDPOINT"]
    armed, completed, release = Event(), Event(), Event()

    class S3Request(BaseHTTPRequestHandler):
        def proxy(self):
            target = HTTPConnection(origin, timeout=10)
            headers = {
                k: v for k, v in self.headers.items() if k.lower() != "connection"
            }
            body = self.rfile.read(int(headers.get("Content-Length", 0)))
            try:
                target.request(self.command, self.path, body=body, headers=headers)
                response = target.getresponse()
                data = response.read()
                if (
                    armed.is_set()
                    and self.command == "POST"
                    and response.status == 200
                    and "uploadId" in parse_qs(urlsplit(self.path).query)
                ):
                    armed.clear()
                    completed.set()
                    assert release.wait(10), "test did not release Silo Complete"
                self.send_response(response.status)
                for key, value in response.getheaders():
                    if key.lower() not in {
                        "connection",
                        "transfer-encoding",
                        "server",
                        "date",
                    }:
                        self.send_header(key, value)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(data)
            finally:
                target.close()

        do_GET = do_HEAD = do_POST = do_PUT = do_DELETE = proxy

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), S3Request) as server:
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield (
                dict(storage, CHAT_S3_ENDPOINT=f"127.0.0.1:{server.server_port}"),
                armed,
                completed,
                release,
            )
        finally:
            release.set()
            server.shutdown()
            thread.join()


async def publication_race(root, storage, api, bucket):
    with hold_silo_completion(storage) as (endpoint, armed, completed, release):
        db = root / "race.sqlite3"
        with chat(db, endpoint) as url:
            async with AsyncExitStack() as stack:
                owner = await login(stack, url, "a", "a", True)
                member = await login(stack, url, "b", "b", True)
                await contact(owner, 1, member, 2)
                conv = (
                    await owner.ok("create_group", title="Silo 完成屏障", members=[2])
                )["conversation"]
                payload = b"real Silo completed before Chat publication"
                for cid, method in ((901, "finish_attachment"), (902, "complete_file")):
                    part = await begin(member, conv, cid, payload)
                    await member.ok(
                        "confirm_attachment",
                        upload=cid,
                        number=1,
                        etag=await asyncio.to_thread(put, part["url"], payload),
                    )
                    [(key,)] = read(
                        db,
                        "SELECT object_key FROM attachment_uploads WHERE client_id=?",
                        cid,
                    )
                    armed.set()
                    completed.clear()
                    release.clear()
                    pending = asyncio.create_task(member.ask(method, upload=cid))
                    try:
                        await until(
                            completed.is_set,
                            "Silo succeeded; Chat has not received completion",
                        )
                        assert api.head_object(Bucket=bucket, Key=key)[
                            "ContentLength"
                        ] == len(payload)
                        await muted(owner, conv, 2, -1)
                    finally:
                        release.set()
                    result = await pending
                    assert result["error"]["code"] == MUTED, result
                    assert not read(
                        db, "SELECT id FROM messages WHERE client_id=?", cid
                    )
                    assert not read(db, "SELECT 1 FROM files WHERE object_key=?", key)
                    assert read(
                        db,
                        "SELECT object_key FROM attachment_uploads WHERE client_id=?",
                        cid,
                    ) == [(key,)]
                    await muted(owner, conv, 2, 0)
                    resumed = await member.ok("resume_attachment", upload=cid)
                    assert resumed["number"] == 0
                    published = await member.ok(method, upload=cid)
                    if method == "complete_file":
                        assert published["object_key"] == key
                        await member.ok(
                            "add_conversation_file",
                            conversation_id=conv,
                            object_key=key,
                        )
                    else:
                        assert published["attachment"]["object_key"] == key
                print(
                    "PASS actual Silo Complete was held after success; both final Chat write transactions rejected a concurrent mute and resumed safely",
                    flush=True,
                )


async def error(rpc, method, code, **params):
    result = await rpc.ask(method, **params)
    assert result.get("error", {}).get("code") == code, (method, result)


async def contact(a, aid, b, bid):
    await a.ok("send_friend_request", user=bid)
    await b.ok("respond_friend_request", user=aid, accept=True)


async def send(user, conv, cid, text=""):
    return await user.ok(
        "send_message", conversation=conv, client_id=cid, text=text or f"message {cid}"
    )


async def muted(owner, conv, member, seconds):
    return await owner.ok(
        "set_group_member_mute", conversation=conv, user=member, seconds=seconds
    )


async def create_file(user, conv, cid, data):
    grant = await begin(user, conv, cid, data)
    confirmed = await user.ok(
        "confirm_attachment", upload=cid, number=1, etag=put(grant["url"], data)
    )
    assert not confirmed["number"]
    return await user.ok("finish_attachment", upload=cid)


def read(db, sql, *params):
    with sqlite3.connect(db) as conn:
        return conn.execute(sql, params).fetchall()


async def restrictions(a, b, c, conv, direct, db):
    own = (await send(c, conv, 801, "成员正文"))["id"]
    peer = (await send(a, conv, 802, "普通成员可以阅读"))["id"]
    await c.ok("set_message_reaction", conversation=conv, message=peer, emoji="👍")
    file = await create_file(c, conv, 803, b"shared attachment")
    key = file["attachment"]["object_key"]
    await c.ok("add_conversation_file", conversation_id=conv, object_key=key)
    await error(c, "set_group_mute", -32009, conversation=conv, muted=True)
    await error(
        b, "set_group_member_mute", -32009, conversation=conv, user=1, seconds=-1
    )
    await error(
        a, "set_group_member_mute", -32009, conversation=conv, user=2, seconds=-1
    )
    await muted(b, conv, 3, -1)
    await b.ok("set_group_mute", conversation=conv, muted=True)
    # An accepted original is still reconciled before applying speech permission.
    replay = await send(c, conv, 801, "not a new message")
    assert replay["id"] == own
    assert read(
        db, "SELECT count(*) FROM messages WHERE sender_id=3 AND client_id=801"
    ) == [(1,)]
    await error(
        c, "send_message", MUTED, conversation=conv, client_id=804, text="blocked"
    )
    await error(
        c,
        "send_message",
        MUTED,
        conversation=conv,
        client_id=804,
        text="blocked reply",
        reply_to=peer,
    )
    await error(
        c, "send_message", MUTED, conversation=conv, client_id=804, forward=peer
    )
    await error(
        c, "edit_message", MUTED, conversation=conv, message=own, text="blocked"
    )
    await error(
        c, "set_message_reaction", MUTED, conversation=conv, message=peer, emoji="❤️"
    )
    await error(c, "set_typing", -32006, conversation=conv, typing=True)
    await error(
        c,
        "begin_attachment",
        MUTED,
        conversation=conv,
        client_id=805,
        filename="x.bin",
        media_type="application/octet-stream",
        size=1,
        checksum=hashlib.sha256(b"x").hexdigest(),
    )
    await error(c, "add_conversation_file", MUTED, conversation_id=conv, object_key=key)
    await c.ok("set_message_reaction", conversation=conv, message=peer, emoji="")
    await c.ok("delete_message", conversation=conv, message=own)
    await c.ok("remove_conversation_file", conversation_id=conv, object_key=key)
    assert (await c.ok("get_messages", conversation=conv, around=peer))["messages"]
    assert "url" in await c.ok("get_attachment", conversation=conv, message=file["id"])
    assert (await c.ok("search_all_messages", query="普通成员"))["messages"]
    await send(c, direct, 806, "other conversation stays available")
    await send(b, conv, 807, "admin exemption")
    await b.ok("set_group_mute", conversation=conv, muted=False)
    await error(
        c,
        "send_message",
        MUTED,
        conversation=conv,
        client_id=808,
        text="individual survives all-mute off",
    )
    token = (await a.ok("create_group_invite", conversation=conv))["token"]
    await c.ok("leave_group", conversation=conv)
    await c.ok("join_group", token=token)
    await error(
        c,
        "send_message",
        MUTED,
        conversation=conv,
        client_id=808,
        text="still muted after rejoin",
    )
    assert not (await c.ok("get_messages", conversation=conv))["messages"]
    await a.ok("set_group_admin", conversation=conv, user=3, admin=True)
    await send(c, conv, 808, "promoted exemption")
    await a.ok("set_group_admin", conversation=conv, user=3, admin=False)
    await error(
        c,
        "send_message",
        MUTED,
        conversation=conv,
        client_id=809,
        text="demoted restriction",
    )
    await muted(b, conv, 3, 1)
    await error(
        c,
        "send_message",
        MUTED,
        conversation=conv,
        client_id=809,
        text="timed restriction",
    )

    async def expired():
        result = await c.ask(
            "send_message",
            conversation=conv,
            client_id=809,
            text="expires without restart",
        )
        return "result" in result

    await until(expired, "server-clock expiry")
    print(
        "PASS real RPC role matrix, independent mute, rejoin waterline, expiry, idempotency and allow-listed actions",
        flush=True,
    )
    return token


async def dissolve(a, b, c, b2, conv, direct, db, api, bucket, token):
    mid = (await send(c, conv, 810, "dissolution archived only"))["id"]
    keeper = (
        await send(c, direct, 815, "same immutable month but another conversation")
    )["id"]
    with sqlite3.connect(db) as conn:
        conn.execute(
            "UPDATE messages SET created_at=1735689600000 WHERE id IN (?,?)",
            (mid, keeper),
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
    assert (await c.ok("get_messages", conversation=conv, around=mid))["messages"]
    uploaded = await create_file(
        c, conv, 811, b"retained by a forward in another conversation"
    )
    key = uploaded["attachment"]["object_key"]
    await c.ok("add_conversation_file", conversation_id=conv, object_key=key)
    other_group = (await a.ok("create_group", title="独立群文件引用", members=[3]))[
        "conversation"
    ]
    await c.ok("add_conversation_file", conversation_id=other_group, object_key=key)
    forward = await c.ok(
        "send_message", conversation=direct, client_id=812, forward=uploaded["id"]
    )
    part = await begin(c, conv, 813, b"an actual abandoned multipart part")
    put(part["url"], b"an actual abandoned multipart part")
    await c.ok("pause_attachment", upload=813)
    [(pending_key, multipart)] = read(
        db, "SELECT object_key,multipart FROM attachment_uploads WHERE client_id=813"
    )
    assert api.list_parts(Bucket=bucket, Key=pending_key, UploadId=multipart)["Parts"]
    await error(b, "dissolve_group", -32009, conversation=conv)
    assert await a.ok("dissolve_group", conversation=conv) == {"changed": True}
    assert await a.ok("dissolve_group", conversation=conv) == {"changed": False}
    await b.ok("get_conversations")
    await b2.ok("get_conversations")
    for session in (b, b2):
        events = [
            e
            for e in session.events
            if e.get("params")
            == {"conversation": conv, "removed": True, "dissolved": True}
        ]
        assert len(events) == 1, events
    assert read(
        db,
        "SELECT state,object_key,multipart FROM attachment_uploads WHERE client_id=813",
    ) == [("discarding", pending_key, multipart)]
    for params in ({}, {"around": mid}, {"category": "all"}):
        await error(c, "get_messages", -32006, conversation=conv, **params)
    await error(c, "get_members", -32006, conversation=conv)
    await error(c, "list_conversation_files", -32006, conversation_id=conv)
    await error(c, "get_attachment", -32007, conversation=conv, message=uploaded["id"])
    await error(
        c, "get_conversation_file", -32007, conversation_id=conv, object_key=key
    )
    await error(
        c,
        "send_message",
        -32006,
        conversation=conv,
        client_id=810,
        text="cannot regain receipt",
    )
    await error(c, "join_group", -32014, token=token)
    await error(a, "invite_group_members", -32006, conversation=conv, members=[2])
    assert "url" in await c.ok(
        "get_conversation_file", conversation_id=other_group, object_key=key
    )
    scopes = await c.ok("message_search_scopes")
    assert conv not in {g["id"] for g in scopes}
    assert not (await c.ok("search_all_messages", query="dissolution archived only"))[
        "messages"
    ]
    assert "url" in await c.ok(
        "get_attachment", conversation=direct, message=forward["id"]
    )
    assert api.head_object(Bucket=bucket, Key=key)
    preserved = await c.ok("get_messages", conversation=direct, around=keeper)
    assert any(
        m["id"] == keeper
        and m["text"] == "same immutable month but another conversation"
        for m in preserved["messages"]
    )
    subprocess.run(
        [
            sys.executable,
            str(Path(__file__).parents[1] / "chat_admin.py"),
            "--db",
            str(db),
            "archive-check",
        ],
        check=True,
    )
    await send(c, direct, 814, "another conversation after dissolve")
    print(
        "PASS real dissolution invalidation, original devices, idempotent terminal reply, retained MPU metadata and shared forward",
        flush=True,
    )
    return pending_key, multipart, key, forward["id"], other_group


async def qt_search_rejoin(root, probe, url, owner, member):
    def matches(reply):
        return [row["id"] for row in reply["messages"]]

    group = (await owner.ok("create_group", title="重新加入", members=[3]))[
        "conversation"
    ]
    other = (await owner.ok("create_group", title="仍然失权", members=[3]))[
        "conversation"
    ]
    visible = (await owner.ok("create_group", title="已显示命中", members=[3]))[
        "conversation"
    ]
    token = (await owner.ok("create_group_invite", conversation=group))["token"]
    old = (await send(owner, group, 985, "重入筛选 移除前"))["id"]
    old_other = (await send(owner, other, 986, "重入筛选 其他群"))["id"]
    displayed = (await send(owner, visible, 988, "重入筛选 已显示"))["id"]
    gate = Gate(url)
    async with serve(gate.handle, "127.0.0.1", 0) as proxy:
        address = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
        ui = Desktop()
        await ui.start(probe, root / "rejoining", address, user="c", user_id=3)
        try:
            await ui.call("searchui", open=True)
            await ui.wait(
                "searchui", lambda r: {group, other} <= set(r.get("scopes", []))
            )
            await ui.call("searchui", query="重入筛选")
            await ui.wait(
                "searchui", lambda r: set(r["ids"]) == {old, old_other, displayed}
            )
            await owner.ok("remove_group_member", conversation=visible, user=3)
            await ui.wait(
                "searchui",
                lambda r: set(r["ids"]) == {old, old_other}
                and visible not in r["scopes"],
            )
            gate.hold.add("search_all_messages")
            await ui.call("searchui", submit=True)
            await gate.caught("search_all_messages")
            held = next(r for k, r in gate.held if k == "search_all_messages")
            assert set(matches(held["result"])) == {old, old_other}
            await owner.ok("remove_group_member", conversation=group, user=3)
            await owner.ok("remove_group_member", conversation=other, user=3)
            await ui.wait(
                "searchui",
                lambda r: group not in r["scopes"] and other not in r["scopes"],
            )
            await member.ok("join_group", token=token)
            new = (await send(owner, group, 987, "重入筛选 新消息"))["id"]
            assert matches(
                await member.ok("search_all_messages", query="重入筛选")
            ) == [new]
            await ui.wait("searchui", lambda r: r["scopes"].count(group) == 1)
            # Release the response while its request ID is still current. No newer search can mask the guard.
            await gate.release("search_all_messages")
            await ui.wait("searchui", lambda r: r["status"] != "正在搜索…")
            assert not (await ui.call("searchui"))["ids"]
            await ui.call("searchui", submit=True)
            await ui.wait(
                "searchui", lambda r: r["ids"] == [new] and other not in r["scopes"]
            )
            await ui.call("searchui", conversation=group, submit=True)
            assert (await ui.wait("searchui", lambda r: r["ids"] == [new]))[
                "selected_scope"
            ] == group
            spare = (await owner.ok("create_group", title="独立范围", members=[3]))[
                "conversation"
            ]
            state = await ui.wait("searchui", lambda r: spare in r["scopes"])
            assert (
                state["selected_scope"] == group and state["scopes"].count(group) == 1
            )

            # Also exercise the inverse order: a newer successful query retired the old request ID.
            await ui.call("searchui", conversation=0)
            gate.hold.add("search_all_messages")
            await ui.call("searchui", submit=True)
            await gate.caught("search_all_messages")
            assert matches(gate.held[-1][1]["result"]) == [new]
            await owner.ok("remove_group_member", conversation=group, user=3)
            await ui.wait("searchui", lambda r: group not in r["scopes"])
            await member.ok("join_group", token=token)
            newest = (await send(owner, group, 989, "重入筛选 再加入"))["id"]
            await ui.wait("searchui", lambda r: r["scopes"].count(group) == 1)
            gate.hold.discard("search_all_messages")
            await ui.call("searchui", submit=True)
            await ui.wait("searchui", lambda r: r["ids"] == [newest])
            await gate.release("search_all_messages")
            # Use a subsequent real notification/Qt update as the barrier after delivery of the old reply.
            await owner.ok("remove_group_member", conversation=spare, user=3)
            state = await ui.wait("searchui", lambda r: spare not in r["scopes"])
            assert state["ids"] == [newest] and other not in state["scopes"]
            print(
                "PASS Qt prunes displayed lost hits, restores only the rejoined scope, and rejects old replies before and after a newer search",
                flush=True,
            )
        finally:
            await ui.stop()


async def qt_scenarios(root, probe, url, owner, member, db):
    conv = (await owner.ok("create_group", title="Qt 群", members=[2, 3]))[
        "conversation"
    ]
    direct = (await member.ok("open_direct_conversation", user=1))["conversation"]
    anchor = (await send(owner, conv, 950, "可保留回复"))["id"]
    gate, management, search_gate = Gate(url), Gate(url), Gate(url)
    async with AsyncExitStack() as stack:
        proxies = [
            await stack.enter_async_context(serve(g.handle, "127.0.0.1", 0))
            for g in (gate, management, search_gate)
        ]
        address, manager_address, search_address = (
            f"ws://127.0.0.1:{p.sockets[0].getsockname()[1]}/ws" for p in proxies
        )
        ui, admin, searching = Desktop(), Desktop(), Desktop()
        local, manager_home = root / "member", root / "owner"
        await ui.start(probe, local, address, user="c", user_id=3)
        await admin.start(probe, manager_home, manager_address)
        await searching.start(
            probe, root / "searcher", search_address, user="c", user_id=3
        )

        async def choose(which=conv):
            await ui.wait("select", lambda r: r["ok"], conv=which)

        async def notice(label):
            return await ui.wait(
                "state", lambda r: label in r["speech"] if label else not r["speech"]
            )

        def rows():
            return read(
                journal,
                "SELECT body,client,rejection,reply FROM text_outbox WHERE conversation=?",
                conv,
            )

        try:
            await choose()
            await admin.wait("select", lambda r: r["ok"], conv=conv)
            await admin.call("group", id=conv)
            await admin.wait("moderate", lambda r: r.get("owner") and r["manager"])
            assert (await admin.call("moderate", member=3, duration=4))[
                "member_enabled"
            ]
            await notice("永久禁言")
            await admin.wait("moderate", lambda r: r.get("manager") and not r["muted"])
            await admin.call("moderate", toggle=True)
            await notice("全员禁言")
            await admin.wait("moderate", lambda r: r.get("manager") and r["muted"])
            await admin.call("moderate", toggle=True)
            await notice("永久禁言")
            await admin.wait("moderate", lambda r: r.get("manager") and not r["muted"])
            assert (await admin.call("moderate", member=3, duration=0))[
                "member_enabled"
            ]
            await notice("")
            await ui.call("group", id=conv)
            await ui.wait("moderate", lambda r: r.get("publish"), members=True)
            await muted(owner, conv, 2, -1)

            def has_label(state, user, label):
                return all(
                    any(
                        row["id"] == user
                        and all(
                            label in row[role]
                            for role in ("status", "accessible", "tooltip")
                        )
                        for row in state.get(view, [])
                    )
                    for view in ("list", "preview")
                )

            await admin.wait(
                "moderate", lambda r: has_label(r, 2, "永久禁言"), members=True
            )
            await muted(owner, conv, 3, 3)
            await notice("禁言至")
            await ui.wait("moderate", lambda r: r.get("publish") is False, members=True)
            await admin.call("moderate", member=3)
            before = await admin.wait(
                "moderate",
                lambda r: r["scroll"] > 0 and has_label(r, 3, "禁言至"),
                members=True,
                showList=True,
            )
            rpc_count = len(management.messages)
            await ui.call("draft", text="限时到期后继续发")
            assert not (await ui.call("state"))["sendEnabled"]
            await ui.wait("state", lambda r: r["sendEnabled"] and not r["speech"])
            await ui.wait("moderate", lambda r: r.get("publish"), members=True)
            after = await admin.wait(
                "moderate",
                lambda r: all(
                    any(
                        row["id"] == 3
                        and all(
                            "禁言" not in row[role]
                            for role in ("status", "accessible", "tooltip")
                        )
                        for row in r.get(view, [])
                    )
                    for view in ("list", "preview")
                ),
                members=True,
            )
            assert has_label(after, 2, "永久禁言")
            assert after["selected"] == before["selected"] == 3
            assert after["scroll"] == before["scroll"]
            assert [row["id"] for row in after["list"]] == [
                row["id"] for row in before["list"]
            ]
            assert not management.messages[rpc_count:]
            print(
                "PASS Qt natural mute expiry updates member list, preview, accessibility and own publishing without an RPC",
                flush=True,
            )
            await ui.call("moderate", close=True)
            await admin.call("moderate", close=True)

            (journal,) = (local / "data").rglob("tasks.sqlite3")
            gate.hold.add("send_message")
            assert (await ui.call("send", text="成功提交但 ACK 丢失"))["enabled"]
            await gate.caught("send_message")
            original = next(
                p
                for p in gate.messages
                if p["method"] == "send_message"
                and p["params"]["text"] == "成功提交但 ACK 丢失"
            )
            saved_id = original["params"]["client_id"]
            assert read(
                db, "SELECT count(*) FROM messages WHERE client_id=?", saved_id
            ) == [(1,)]
            await muted(owner, conv, 3, -1)
            await notice("永久禁言")
            gate.hold.clear()
            gate.held.clear()
            await gate.down.close()
            await until(
                lambda: any(
                    p["method"] == "send_message"
                    and p["params"]["client_id"] == saved_id
                    for p in gate.messages[gate.messages.index(original) + 1 :]
                ),
                "reconnected replay uses unchanged client_id",
            )
            await until(
                lambda: not rows(), "successful receipt cleans Outbox despite mute"
            )
            assert read(
                db, "SELECT count(*) FROM messages WHERE client_id=?", saved_id
            ) == [(1,)]

            await muted(owner, conv, 3, 0)
            await notice("")
            await ui.wait("state", lambda r: any(m["id"] == anchor for m in r["rows"]))
            assert (await ui.call("reply_to", id=anchor))["reply"]
            gate.hold_requests.add("send_message")
            await ui.call("send", text="未提交的排队正文")
            await until(
                lambda: bool(gate.held_requests), "actual SDK send held before server"
            )
            cid = gate.held_requests[-1][1]["params"]["client_id"]
            await ui.call("draft", text="继续写的草稿")
            await muted(owner, conv, 3, -1)
            await notice("永久禁言")
            await gate.release_requests("send_message")
            await until(
                lambda: rows() == [("未提交的排队正文", cid, MUTED, anchor)],
                "muted outbox retained",
            )
            assert (await ui.call("state"))["draft"] == "继续写的草稿"
            assert not read(db, "SELECT id FROM messages WHERE client_id=?", cid)
            requests = len(
                [
                    p
                    for p in gate.messages
                    if p["method"] == "send_message" and p["params"]["client_id"] == cid
                ]
            )
            auth = sum(p["method"] == "resume_session" for p in gate.messages)
            await gate.down.close()
            await until(
                lambda: sum(p["method"] == "resume_session" for p in gate.messages)
                > auth,
                "second real reconnect",
            )
            await notice("永久禁言")
            await choose(direct)
            await ui.call("send", text="另一个会话能发送")
            await until(
                lambda: bool(
                    read(db, "SELECT id FROM messages WHERE body='另一个会话能发送'")
                ),
                "other conversation no head-of-line block",
            )
            assert (
                len(
                    [
                        p
                        for p in gate.messages
                        if p["method"] == "send_message"
                        and p["params"]["client_id"] == cid
                    ]
                )
                == requests
            )
            await member.ok("hide_conversation", conversation=conv)
            row = "未提交的排队正文"
            await ui.wait("recover_local", lambda r: row in r["rows"])
            gate.hold.add("send_message")
            fault = await ui.call("recover_local", row=row, action="重新发送")
            assert fault.get("triggered"), (
                "hidden muted task has a manual retry (server decides)"
            )
            await gate.caught("send_message")
            denial = next(r for k, r in gate.held if k == "send_message")
            assert denial["error"]["code"] == MUTED
            retry = next(p for p in gate.messages if p["id"] == denial["id"])
            assert retry["params"]["client_id"] == cid
            await gate.release("send_message")
            await until(
                lambda: rows() == [(row, cid, MUTED, anchor)],
                "real denial preserves body, client_id and reply",
            )
            await muted(owner, conv, 3, 0)
            assert not read(db, "SELECT id FROM messages WHERE client_id=?", cid)
            assert (await ui.call("recover_local", row=row, action="重新发送")).get(
                "triggered"
            )
            await until(
                lambda: not rows(), "manual retry in hidden group delivered body"
            )
            assert read(
                db, "SELECT body,reply_to_id FROM messages WHERE client_id=?", cid
            ) == [(row, anchor)]
            await choose()
            await notice("")

            path = root / "interrupted.bin"
            path.write_bytes(b"Qt upload while muted")
            gate.hold.add("confirm_attachment")
            await ui.call("upload", conv=conv, path=str(path))
            await gate.caught("confirm_attachment")
            [(task,)] = read(
                db,
                "SELECT client_id FROM attachment_uploads WHERE conversation=?",
                conv,
            )
            await muted(owner, conv, 3, -1)
            await notice("永久禁言")
            await until(
                lambda: read(
                    db, "SELECT state FROM attachment_uploads WHERE client_id=?", task
                )
                == [("paused",)],
                "upload paused by multi-device restriction",
            )
            await gate.release("confirm_attachment")
            assert not read(db, "SELECT id FROM messages WHERE client_id=?", task)
            other_file = root / "independent.bin"
            other_file.write_bytes(b"a different conversation is not muted")
            await ui.call("upload", conv=direct, path=str(other_file))
            await until(
                lambda: bool(
                    read(
                        db,
                        "SELECT id FROM messages WHERE conversation_id=? AND body=?",
                        direct,
                        other_file.name,
                    )
                ),
                "other conversation upload continues while moderated group is paused",
            )
            await member.ok("hide_conversation", conversation=conv)

            async def transfer_label(filename, prefix=""):
                result = await ui.wait(
                    "transfers",
                    lambda r: any(
                        filename in x and x.startswith(prefix) for x in r["labels"]
                    ),
                )
                return next(
                    x
                    for x in result["labels"]
                    if filename in x and x.startswith(prefix)
                )

            label = await transfer_label(path.name + " · 群聊禁言", f"会话 #{conv}")
            assert not (await ui.call("transfers", label=label, action="继续上传")).get("triggered")
            # The directory now includes hidden memberships. Hold its refresh after
            # unmuting so the server can revoke speech before this device retries.
            await muted(owner, conv, 3, 0)
            available = await ui.wait("transfers", lambda r: any(
                path.name in v for v in r["resumable"]))
            label = next(v for v in available["resumable"] if path.name in v)
            gate.hold.add("get_conversations")
            await muted(owner, conv, 3, -1)
            await gate.caught("get_conversations")
            gate.hold.add("resume_attachment")
            assert (await ui.call("transfers", label=label, action="继续上传"))[
                "triggered"
            ]
            await gate.caught("resume_attachment")
            denial = next(r for k, r in gate.held if k == "resume_attachment")
            retry = next(p for p in gate.messages if p["id"] == denial["id"])
            assert (
                denial["error"]["code"] == MUTED and retry["params"]["upload"] == task
            )
            assert not read(db, "SELECT id FROM messages WHERE client_id=?", task)
            await gate.release("resume_attachment")
            await gate.release("get_conversations")
            label = await transfer_label(path.name + " · 群聊禁言", f"会话 #{conv}")

            await muted(owner, conv, 3, 0)
            available = await ui.wait("transfers", lambda r: any(path.name in v for v in r["resumable"]))
            label = next(v for v in available["resumable"] if path.name in v)
            assert (await ui.call("transfers", label=label, action="继续上传"))[
                "triggered"
            ]
            await until(
                lambda: bool(
                    read(db, "SELECT id FROM messages WHERE client_id=?", task)
                ),
                "original Qt device resumes hidden upload and publishes",
            )
            await choose()
            await notice("")

            for mode in ("leave_group", "remove_group_member", "dissolve_group"):
                destination = (await owner.ok("create_group", title=mode, members=[3]))[
                    "conversation"
                ]
                await choose(destination)
                lost = root / f"{mode}.bin"
                lost.write_bytes(b"paused before membership loss")
                gate.hold.add("confirm_attachment")
                await ui.call(
                    "upload",
                    conv=destination,
                    path=str(lost),
                    group=mode == "remove_group_member",
                )
                await gate.caught("confirm_attachment")
                [(pending,)] = read(
                    db,
                    "SELECT client_id FROM attachment_uploads WHERE conversation=?",
                    destination,
                )
                label = await transfer_label(lost.name)
                assert (await ui.call("transfers", label=label, action="暂停上传"))[
                    "triggered"
                ]
                await until(
                    lambda: read(
                        db,
                        "SELECT state FROM attachment_uploads WHERE client_id=?",
                        pending,
                    )
                    == [("paused",)],
                    "task paused before membership loss",
                )
                await gate.release("confirm_attachment")
                if mode == "leave_group":
                    await member.ok(mode, conversation=destination)
                elif mode == "remove_group_member":
                    await owner.ok(mode, conversation=destination, user=3)
                else:
                    await owner.ok(mode, conversation=destination)
                label = await transfer_label(lost.name + " · 会话不可用")
                assert not (
                    await ui.call("transfers", label=label, action="继续上传")
                ).get("triggered")
                assert not read(
                    db, "SELECT id FROM messages WHERE client_id=?", pending
                )
                assert not read(
                    db,
                    "SELECT object_key FROM conversation_files WHERE conversation_id=?",
                    destination,
                )
            await choose()
            await ui.wait("state", lambda r: any(m["id"] == anchor for m in r["rows"]))
            print(
                "PASS Qt hidden muted attempts denied by server; original IDs resumed after unmute; retired memberships cannot resume from menu",
                flush=True,
            )

            assert (await ui.call("reply_to", id=anchor))["reply"]
            await ui.call("draft", text="解散后可复制的正文")
            # Deliberately allow a late genuine history page after the dissolution event.
            gate.hold.add("get_messages")
            await ui.call("history", open=True, conv=conv)
            await gate.caught("get_messages")
            await searching.call("searchui", open=True)
            await searching.wait(
                "searchui", lambda r: r.get("scope"), conversation=conv
            )
            search_gate.hold.add("search_all_messages")
            await searching.call("searchui", query="可保留回复")
            await search_gate.caught("search_all_messages")
            assert search_gate.held[-1][1]["result"]["messages"]
            await admin.call("group", id=conv)
            await admin.wait("moderate", lambda r: r.get("owner"))
            management.hold.add("dissolve_group")
            await admin.call("moderate", dissolve=True)
            await management.caught("dissolve_group")
            await ui.wait("state", lambda r: r["conv"] != conv)
            assert "count" not in await ui.call("history")
            await gate.release("get_messages")
            await searching.wait(
                "searchui", lambda r: r.get("status") == "原会话不可用"
            )
            await search_gate.release("search_all_messages")
            state = await searching.call("searchui", conversation=conv)
            assert not state.get("scope") and state["selected_scope"] == 0
            assert not (await searching.call("searchui"))["ids"]
            assert not (await ui.call("select", conv=conv))["ok"]
            key = "草稿 · 解散后可复制的正文"
            await ui.wait("recover_local", lambda r: key in r["rows"])
            assert (await ui.call("recover_local", row=key, action="复制"))[
                "clipboard"
            ] == "解散后可复制的正文"
            assert read(
                journal,
                "SELECT reply,body FROM compose_drafts WHERE conversation=?",
                conv,
            ) == [(0, "解散后可复制的正文")]
            await ui.call("recover_local", row=key, action="删除草稿")
            await ui.wait("confirm", lambda r: r["open"], cancel=True)
            assert read(
                journal, "SELECT 1 FROM compose_drafts WHERE conversation=?", conv
            )
            await ui.call("recover_local", row=key, action="删除草稿")
            await ui.wait("confirm", lambda r: r["open"])
            await until(
                lambda: not read(
                    journal, "SELECT 1 FROM compose_drafts WHERE conversation=?", conv
                ),
                "explicit local discard",
            )
            assert await owner.ok("dissolve_group", conversation=conv) == {
                "changed": False
            }
            # Miss the removal notification entirely on a stopped device.
            await searching.call("searchui", close=True)
            offline = (await owner.ok("create_group", title="离线解散", members=[3]))[
                "conversation"
            ]
            target = (await send(owner, offline, 951, "离线回复锚点"))["id"]
            await searching.wait("select", lambda r: r["ok"], conv=offline)
            await searching.wait(
                "state", lambda r: any(m["id"] == target for m in r["rows"])
            )
            assert (await searching.call("reply_to", id=target))["reply"]
            search_gate.hold_requests.add("send_message")
            await searching.call("send", text="离线解散待发送")
            await until(
                lambda: bool(search_gate.held_requests), "held offline scenario send"
            )
            await searching.call("draft", text="离线解散草稿")
            await muted(owner, offline, 3, -1)
            await searching.wait("state", lambda r: "永久禁言" in r["speech"])
            await search_gate.release_requests("send_message")
            (offline_journal,) = (root / "searcher" / "data").rglob("tasks.sqlite3")
            await until(
                lambda: read(
                    offline_journal,
                    "SELECT rejection,reply FROM text_outbox WHERE conversation=?",
                    offline,
                )
                == [(MUTED, target)],
                "rejected record persisted before going offline",
            )
            await searching.call("quit")
            assert await asyncio.wait_for(searching.proc.wait(), 8) == 0
            await owner.ok("dissolve_group", conversation=offline)
            await searching.start(
                probe, root / "searcher", search_address, user="c", user_id=3
            )
            await until(
                lambda: read(
                    offline_journal,
                    "SELECT rejection,reply FROM text_outbox WHERE conversation=?",
                    offline,
                )
                == [(-32006, None)]
                and read(
                    offline_journal,
                    "SELECT reply,body FROM compose_drafts WHERE conversation=?",
                    offline,
                )
                == [(0, "离线解散草稿")],
                "reconnect without event clears both invalid reply references",
            )
            state = await searching.wait(
                "recover_local", lambda r: "离线解散待发送" in r["rows"]
            )
            assert "草稿 · 离线解散草稿" in state["rows"]
            copy = await searching.call(
                "recover_local", row="离线解散待发送", action="复制"
            )
            assert copy["clipboard"] == "离线解散待发送"
            assert not (
                await searching.call(
                    "recover_local", row="离线解散待发送", action="重新发送"
                )
            ).get("triggered")
            await searching.call(
                "recover_local", row="离线解散待发送", action="删除待发消息"
            )
            await until(
                lambda: not read(
                    offline_journal,
                    "SELECT 1 FROM text_outbox WHERE conversation=?",
                    offline,
                ),
                "explicit local Outbox deletion",
            )
            print(
                "PASS actual Qt moderation controls and expiry, ACK-loss replay, rejected Outbox retry, original-device resume, late history/search, dissolve ACK loss and online/offline recoverable drafts and Outbox",
                flush=True,
            )
        finally:
            await ui.stop()
            await admin.stop()
            await searching.stop()


async def run(root, probe):
    db = root / "hot.sqlite3"
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        await publication_race(root, storage, api, store.bucket)
        with chat(db, storage) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                b = await login(stack, url, "b", "b", True)
                c = await login(stack, url, "c", "c", True)
                b2 = await login(stack, url, "b", "d")
                await contact(a, 1, b, 2)
                await contact(a, 1, c, 3)
                direct = (await c.ok("open_direct_conversation", user=1))[
                    "conversation"
                ]
                conv = (await a.ok("create_group", title="管理验收", members=[2, 3]))[
                    "conversation"
                ]
                await a.ok("set_group_admin", conversation=conv, user=2, admin=True)
                token = await restrictions(a, b, c, conv, direct, db)
                pending_key, multipart, key, forward, other_group = await dissolve(
                    a, b, c, b2, conv, direct, db, api, store.bucket, token
                )
                if probe and "--rejoin-only" in sys.argv:
                    return await qt_search_rejoin(root, probe, url, a, c)
                if probe:
                    await qt_scenarios(root, probe, url, a, c, db)
                    await qt_search_rejoin(root, probe, url, a, c)
        assert read(db, "SELECT state FROM attachment_uploads WHERE client_id=813") == [
            ("discarding",)
        ]
        # Age only the disposal record after shutdown to demonstrate the settling
        # boundary without an actual minute or a production data change.
        with sqlite3.connect(db) as conn:
            conn.execute(
                "UPDATE attachment_uploads SET touched=? WHERE client_id=813",
                (int(time.time() * 1000) - 120_000,),
            )
        with chat(db, storage) as url:
            await until(
                lambda: not read(
                    db, "SELECT id FROM attachment_uploads WHERE client_id=813"
                ),
                "durable abort recovered",
            )
            assert not read(db, "SELECT id FROM conversations WHERE id=?", conv)
            observed = api.list_multipart_uploads(Bucket=store.bucket).get(
                "Uploads", []
            )
            assert all(
                u["UploadId"] != multipart or u["Key"] != pending_key for u in observed
            )
            assert api.head_object(Bucket=store.bucket, Key=key)
            async with AsyncExitStack() as stack:
                c = await login(stack, url, "c", "c")
                await c.ok("delete_message", conversation=direct, message=forward)
                assert api.head_object(Bucket=store.bucket, Key=key)
                await c.ok(
                    "remove_conversation_file",
                    conversation_id=other_group,
                    object_key=key,
                )
        assert not read(db, "SELECT object_key FROM files WHERE object_key=?", key)
        assert all(
            x["Key"] != key
            for x in api.list_objects_v2(Bucket=store.bucket).get("Contents", [])
        )
        assert not read(db, "PRAGMA foreign_key_check")
        print(
            "PASS restart aborts durable multipart, removes shell, and last forward reference gates object reclamation",
            flush=True,
        )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-group-accept-") as directory:
        asyncio.run(run(Path(directory), sys.argv[1] if len(sys.argv) > 1 else None))
