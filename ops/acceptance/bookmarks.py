"""Real Chat RPC, SQLite, Silo and optionally Qt on an isolated virtual X display."""

import argparse
import asyncio
import base64
from contextlib import AsyncExitStack
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate
from fixture import Client, connect, fixture
from isolated import Store, begin, chat, login, put
from isolated_x11 import run_x11


def rows(db, query, *args):
    with sqlite3.connect(db) as conn:
        return conn.execute(query, args).fetchall()


async def page(client, **params):
    return await client.ok("list_message_bookmarks", **params)


async def set_bookmark(client, message, marked=True):
    return await client.ok("set_message_bookmark", message=message, bookmarked=marked)


async def attach(client, conversation, ident, name, kind, data):
    upload = await begin(client, conversation, ident, data, name, kind)
    await client.ok(
        "confirm_attachment", upload=ident, number=1, etag=put(upload["url"], data)
    )
    return (await client.ok("finish_attachment", upload=ident))["id"]


async def migration(root, previous_server):
    migrated = root / "migrated.sqlite3"
    async with fixture(previous_server) as (old, _, _, peer, direct, _):
        message = (
            await peer.ok(
                "send_message", conversation=direct, text="迁移前消息", client_id=101
            )
        )["id"]
        assert not rows(
            old, "SELECT name FROM sqlite_schema WHERE name='message_bookmarks'"
        )
        with sqlite3.connect(old) as source, sqlite3.connect(migrated) as destination:
            source.backup(destination)

    storage = dict(
        CHAT_S3_ENDPOINT="127.0.0.1:1",
        CHAT_S3_BUCKET="migration",
        CHAT_S3_ACCESS_KEY="isolated",
        CHAT_S3_SECRET_KEY="isolated",
        CHAT_S3_HTTPS="0",
    )
    with chat(migrated, storage) as url:
        async with AsyncExitStack() as stack:
            a = await login(stack, url, "a", "a")
            assert not (await page(a))["bookmarks"]
            await set_bookmark(a, message)
            assert (await page(a))["bookmarks"][0]["source"]["snippet"] == "迁移前消息"
    with chat(migrated, storage) as url:
        async with AsyncExitStack() as stack:
            a = await login(stack, url, "a", "a")
            assert (await page(a))["bookmarks"][0]["message"] == message
            assert not rows(migrated, "PRAGMA foreign_key_check")
    print(
        "PASS old server database migration, restart persistence, and foreign keys",
        flush=True,
    )


async def run(root, probe, previous_server):
    if previous_server:
        await migration(root, previous_server)
    db = root / "chat.sqlite3"
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, storage) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                a2 = await login(stack, url, "a", "c")
                b = await login(stack, url, "b", "b", True)
                unauthorized = Client(await stack.enter_async_context(connect(url)))
                assert (await unauthorized.ask("list_message_bookmarks"))["error"][
                    "code"
                ] == -32001
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                direct = (await a.ok("open_direct_conversation", user=2))[
                    "conversation"
                ]
                group = (await b.ok("create_group", title="收藏群", members=[1]))[
                    "conversation"
                ]
                c = await login(stack, url, "c", "d", True)
                await b.ok("send_friend_request", user=3)
                await c.ok("respond_friend_request", user=2, accept=True)
                other = (await b.ok("create_group", title="不可见", members=[3]))[
                    "conversation"
                ]
                counter = 1000

                async def send(who, conversation, text):
                    nonlocal counter
                    counter += 1
                    return (
                        await who.ok(
                            "send_message",
                            conversation=conversation,
                            client_id=counter,
                            text=text,
                        )
                    )["id"]

                async def status(who, message):
                    return await who.ok("get_message_bookmark", message=message)

                target = await send(b, group, "SOLE 远处锚点")
                recent = await send(b, direct, "保持此聊天与回复")
                private = await send(b, other, "不许暴露")
                assert (
                    await a.ask(
                        "set_message_bookmark", message=private, bookmarked=True
                    )
                )["error"]["code"] == -32007
                assert "error" in await a.ask("list_message_bookmarks", user_id=2)
                assert "error" in await a.ask("list_message_bookmarks", before=0)
                assert "error" in await a.ask("list_message_bookmarks", query="\0")
                assert not (await status(a2, target))["bookmarked"]
                a2.events.clear()
                assert (await set_bookmark(a, target))["bookmarked"]
                assert (await status(a2, target))["bookmarked"]
                first = (await page(a))["bookmarks"][0]
                assert first["source"]["conversation"] == group
                assert first["source"]["title"] == "收藏群"
                assert first["source"]["username"] == "b"
                assert (
                    len(
                        [
                            e
                            for e in a2.events
                            if e.get("method") == "message_bookmark_changed"
                        ]
                    )
                    == 1
                )
                b.events.clear()
                await set_bookmark(a, target)
                assert (await page(a))["bookmarks"] == [first]
                await page(a2)
                assert (
                    len(
                        [
                            e
                            for e in a2.events
                            if e.get("method") == "message_bookmark_changed"
                        ]
                    )
                    == 1
                )
                assert not (await status(b, target))["bookmarked"]
                assert not any(
                    e.get("method") == "message_bookmark_changed" for e in b.events
                )
                await set_bookmark(b, target)
                await set_bookmark(b, target, False)
                await set_bookmark(b, target, False)
                assert (await status(a, target))["bookmarked"] and not (await page(b))[
                    "bookmarks"
                ]
                await set_bookmark(a2, target, False)
                assert not (await status(a, target))["bookmarked"]
                await set_bookmark(a, target)
                assert (await page(a))["bookmarks"][0]["id"] > first["id"]

                ids = [await send(b, group, f"Paginate %_ 中文 {i}") for i in range(51)]
                for mid in reversed(ids):
                    await set_bookmark(a, mid)
                found = await page(a, query="pAGInate %_ 中文")
                tail = await page(
                    a, query="pAGInate %_ 中文", before=found["next_before"]
                )
                assert [
                    m["message"] for m in found["bookmarks"] + tail["bookmarks"]
                ] == ids
                assert len(found["bookmarks"]) == 50 and tail["next_before"] is None
                assert not (await page(a, query="不存在"))["bookmarks"]

                image = await attach(
                    b,
                    direct,
                    501,
                    "picture.png",
                    "image/png",
                    base64.b64decode(
                        "iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAIAAAASFvFNAAAAFUlEQVR4nGOUrHjOwMDAwMDAxAADABneAXybXmOGAAAAAElFTkSuQmCC"
                    ),
                )
                file = await attach(
                    a,
                    group,
                    502,
                    "file.txt",
                    "application/octet-stream",
                    b"isolated file",
                )
                saved_objects = sorted(
                    x["Key"]
                    for x in api.list_objects_v2(Bucket=store.bucket)["Contents"]
                )

                def attachments():
                    return (
                        rows(
                            db,
                            "SELECT message_id,object_key FROM message_attachments ORDER BY message_id",
                        ),
                        rows(db, "SELECT object_key FROM files ORDER BY object_key"),
                        rows(
                            db,
                            "SELECT object_key FROM bundle_items WHERE object_key IS NOT NULL ORDER BY object_key",
                        ),
                        rows(
                            db,
                            "SELECT object_key FROM attachment_uploads ORDER BY object_key",
                        ),
                    )

                references = attachments()
                await set_bookmark(a, image)
                await set_bookmark(a, file)
                await set_bookmark(a, file, False)
                await set_bookmark(a, file)
                assert references == attachments()
                assert saved_objects == sorted(
                    x["Key"]
                    for x in api.list_objects_v2(Bucket=store.bucket)["Contents"]
                )
                result = {r["message"]: r for r in (await page(a))["bookmarks"]}
                assert result[image]["attachment_type"] == "image/png"
                assert result[file]["attachment_type"] == "application/octet-stream"

                if probe:
                    await qt_cases(
                        root,
                        probe,
                        url,
                        db,
                        a,
                        a2,
                        b,
                        direct,
                        group,
                        target,
                        recent,
                        image,
                    )

                await b.ok(
                    "edit_message",
                    conversation=group,
                    message=target,
                    text="UPDATED editable anchor",
                )
                assert not (await page(a, query="SOLE"))["bookmarks"]
                fresh = (await page(a, query="updated"))["bookmarks"]
                assert (
                    fresh[0]["message"] == target
                    and "UPDATED" in fresh[0]["source"]["snippet"]
                )
                with sqlite3.connect(db) as conn:
                    conn.execute(
                        "UPDATE messages SET created_at=1736942400000 WHERE id=?",
                        (target,),
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
                assert (await page(a, query="updated"))["bookmarks"][0][
                    "message"
                ] == target

                await b.ok("delete_message", conversation=direct, message=image)
                assert "error" in await a.ask(
                    "get_attachment", conversation=direct, message=image
                )
                assert not rows(
                    db, "SELECT * FROM message_attachments WHERE message_id=?", image
                )
                await a.ok("clear_history", conversation=group, through=file)
                assert "error" in await a.ask(
                    "get_messages", conversation=group, around=target
                )
                assert not (await page(a, query="UPDATED"))["bookmarks"]
                new = await send(b, group, "权限摘除")
                await set_bookmark(a, new)
                await b.ok("remove_group_member", conversation=group, user=1)
                await b.ok("invite_group_members", conversation=group, members=[1])
                assert (
                    await a.ask("set_message_bookmark", message=new, bookmarked=True)
                )["error"]["code"] == -32007
                assert not (await page(a, query="权限摘除"))["bookmarks"]
                placeholders = (await page(a))["bookmarks"]
                assert all(
                    not r["source"] and not r["attachment_type"] for r in placeholders
                )
                assert image in {r["message"] for r in placeholders}
                assert (await status(a, new))["bookmarked"]
                assert not (await set_bookmark(a, new, False))["bookmarked"]

                # Physical removal cascades; logical removal/visibility loss keeps a removable placeholder.
                with sqlite3.connect(db) as conn:
                    conn.execute("PRAGMA foreign_keys=ON")
                    conn.execute("DELETE FROM messages WHERE id=?", (image,))
                    assert not list(conn.execute("PRAGMA foreign_key_check"))
                assert not (await status(a, image))["bookmarked"]
                print(
                    "PASS real RPC privacy, idempotency, 50/51 ordering, edit/archive, retraction/clear/rejoin, object references and cascade",
                    flush=True,
                )


async def qt_cases(
    root, probe, url, db, a, a2, b, direct, group, target, recent, image
):
    ui = Desktop()
    gate = Gate(url)
    async with serve(gate.handle, "127.0.0.1", 0) as proxy:
        entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
        await ui.start(probe, root, entry, native=True, platform="xcb")
        try:
            assert (await ui.call("desktop_environment"))["platform"] == "xcb"
            assert (await ui.call("select", conv=direct))["ok"]
            await ui.wait("state", lambda s: recent in [m["id"] for m in s["rows"]])
            assert (await ui.call("bookmark_menu", message=recent, action="收藏"))[
                "action"
            ] == "收藏"
            await ui.wait("state", lambda r: r["notice"] == "已收藏")
            assert (await ui.call("bookmark_menu", message=recent, action="取消收藏"))[
                "action"
            ] == "取消收藏"
            await ui.wait("state", lambda r: r["notice"] == "已取消收藏")
            await ui.call("reply_to", id=recent)
            await ui.call("draft", text="未发送的草稿")
            assert (await ui.call("state"))["reply"]

            await ui.call("bookmarks", open=True)
            await ui.wait(
                "bookmarks", lambda r: len(r.get("rows", [])) == 50 and r["more"]
            )
            await ui.call("bookmarks", more=True)
            await ui.wait(
                "bookmarks", lambda r: len(r.get("rows", [])) == 54 and not r["more"]
            )

            await ui.call("bookmarks", query="Paginate")
            await ui.wait(
                "bookmarks", lambda r: len(r.get("rows", [])) == 50 and r["more"]
            )
            await ui.call("bookmarks", more=True)
            before = await ui.wait("bookmarks", lambda r: len(r.get("rows", [])) == 51)
            selected = before["rows"][-1]["id"]
            await ui.call("bookmarks", select=selected)
            await b.ok(
                "edit_message", conversation=direct, message=recent, text="未收藏的编辑"
            )
            await ui.wait(
                "state", lambda r: any(x["text"] == "未收藏的编辑" for x in r["rows"])
            )
            observed = await ui.call("bookmarks")
            assert (
                observed["rows"] == before["rows"] and observed["selected"] == selected
            ), (
                f"unbookmarked edit lost filtered page or selection: {len(observed['rows'])}, {observed['selected']}"
            )
            await b.ok(
                "edit_message",
                conversation=group,
                message=target,
                text="Paginate edited into results",
            )
            # This bookmark was not in either loaded page, but should now match the query.
            matched = await ui.wait(
                "bookmarks",
                lambda r: r.get("more")
                or any(x["id"] == target for x in r.get("rows", [])),
            )
            if matched["more"]:
                await ui.call("bookmarks", more=True)
            await ui.wait(
                "bookmarks", lambda r: any(x["id"] == target for x in r.get("rows", []))
            )
            await b.ok(
                "edit_message", conversation=group, message=target, text="SOLE 远处锚点"
            )
            print(
                "PASS Qt unrelated edit preserves filtered pages and selection; an edited bookmark can newly match",
                flush=True,
            )

            gate.hold.add("list_message_bookmarks")
            await ui.call("bookmarks", query="Paginate")
            await gate.caught("list_message_bookmarks")
            gate.hold.discard("list_message_bookmarks")
            await ui.call("bookmarks", query="不存在")
            await ui.wait(
                "bookmarks", lambda r: r.get("status") == "没有匹配的收藏消息"
            )
            await gate.release("list_message_bookmarks")
            barrier = (
                await b.ok(
                    "send_message",
                    conversation=direct,
                    text="迟到回包后的屏障",
                    client_id=9980,
                )
            )["id"]
            await ui.wait("state", lambda r: barrier in [x["id"] for x in r["rows"]])
            stale = await ui.call("bookmarks")
            assert stale["status"] == "没有匹配的收藏消息" and not stale["rows"]
            await ui.call("bookmarks", query="sole")
            await ui.wait(
                "bookmarks", lambda r: [x["id"] for x in r.get("rows", [])] == [target]
            )
            read = rows(
                db,
                "SELECT last_read_message_id FROM conversation_members WHERE conversation_id=? AND user_id=1",
                group,
            )
            start = len(gate.messages)
            await ui.call("bookmarks", select=target, locate=True)
            await ui.wait("searchui", lambda r: r.get("anchor") == target)
            assert (
                rows(
                    db,
                    "SELECT last_read_message_id FROM conversation_members WHERE conversation_id=? AND user_id=1",
                    group,
                )
                == read
            )
            assert not any(
                m["method"] == "mark_read" and m["params"]["conversation"] == group
                for m in gate.messages[start:]
            )
            await ui.call("searchui", close=True)

            await b.ok(
                "edit_message",
                conversation=group,
                message=target,
                text="SOLE 远处锚点 edited while open",
            )
            await ui.wait(
                "bookmarks",
                lambda r: any(
                    "edited while open" in x["summary"] for x in r.get("rows", [])
                ),
            )
            await set_bookmark(a2, target, False)
            await ui.wait(
                "bookmarks", lambda r: r.get("status") == "没有匹配的收藏消息"
            )
            await set_bookmark(a, target)
            await ui.wait(
                "bookmarks", lambda r: [x["id"] for x in r.get("rows", [])] == [target]
            )
            await ui.call("bookmarks", query="")
            await ui.wait(
                "bookmarks", lambda r: image in [x["id"] for x in r.get("rows", [])]
            )
            hidden = await b.ok("create_group", title="失权占位", members=[1])
            gm = hidden["conversation"]
            msg = (
                await b.ok(
                    "send_message", conversation=gm, text="不应继续显示", client_id=9981
                )
            )["id"]
            await set_bookmark(a2, msg)
            await ui.wait(
                "bookmarks",
                lambda r: any(
                    x["id"] == msg and x["conversation"] == gm
                    for x in r.get("rows", [])
                ),
            )
            await b.ok("remove_group_member", conversation=gm, user=1)
            await ui.wait(
                "bookmarks",
                lambda r: any(
                    x["id"] == msg and not x["conversation"] for x in r.get("rows", [])
                ),
            )
            assert not (await ui.call("bookmarks", select=msg))["locate"]
            await ui.call("bookmarks", remove=True)
            await ui.wait(
                "bookmarks",
                lambda r: len(r.get("rows", [])) == 50
                and msg not in [x["id"] for x in r["rows"]],
            )
            await ui.call("bookmarks", close=True)
            state = await ui.call("state")
            assert (
                state["conv"] == direct
                and state["draft"] == "未发送的草稿"
                and state["reply"]
            )
            print(
                "PASS Qt xcb menu, scrollable pages, bookmark search/empty, own device sync, unavailable removal, around without mark_read, draft/reply",
                flush=True,
            )
        finally:
            await ui.stop()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--probe", help="run Qt cases with a freshly linked probe in an isolated X11 session"
    )
    parser.add_argument(
        "--previous-server",
        help="optional pre-bookmark chat_server to verify migration",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chat-bookmarks-") as tmp:
        if args.probe:
            script = (
                "import asyncio; from pathlib import Path; from bookmarks import run; "
                "asyncio.run(run(Path(sys.argv[1]), sys.argv[2], sys.argv[3] or None))"
            )
            sys.exit(run_x11(Path(tmp), script, tmp, args.probe, args.previous_server or ""))
        asyncio.run(run(Path(tmp), None, args.previous_server))
