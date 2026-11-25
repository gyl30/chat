"""Drive merged forwarding with Qt, archived source text and real Silo file references."""

import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import sqlite3
import sys
import tempfile
import urllib.request

from desktop import Desktop, Gate, until
from websockets.asyncio.server import serve
from isolated import Store, begin, chat, login, put
from message_selection import PNG
from recovery import admin


async def run(root, probe):
    store = Store()
    db = root / "bundle.sqlite"
    with store.node(root / "silo") as (api, env):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                a, b, c = [await login(stack, url, name, name, True) for name in "abc"]
                for peer, uid in [(b, 2), (c, 3)]:
                    await a.ok("send_friend_request", user=uid)
                    await peer.ok("respond_friend_request", user=1, accept=True)
                source = (await a.ok("open_direct_conversation", user=2))[
                    "conversation"
                ]
                target = (await a.ok("create_group", title="合并目标", members=[3]))[
                    "conversation"
                ]

                async def text(cid, body, markdown=False):
                    return (
                        await b.ok(
                            "send_message",
                            conversation=source,
                            client_id=cid,
                            text=body,
                            markdown=markdown,
                        )
                    )["id"]

                first = await text(10, "**原始正文**", True)

                async def attach(cid, payload, name, media):
                    part = await begin(b, source, cid, payload, name, media)
                    await b.ok(
                        "confirm_attachment",
                        upload=cid,
                        number=1,
                        etag=put(part["url"], payload),
                    )
                    return await b.ok("finish_attachment", upload=cid)

                image = await attach(20, PNG, "图.png", "image/png")
                file = await attach(
                    21, b"snapshot", "文件.bin", "application/octet-stream"
                )
                ids = [first, image["id"], file["id"]]
                with sqlite3.connect(db) as con:
                    con.execute(
                        "update messages set created_at=1736942400000 where id=?",
                        (first,),
                    )
                admin("--db", db, "archive", "2025-01")
                ui = Desktop()
                gate = Gate(url)
                proxy = await stack.enter_async_context(
                    serve(gate.handle, "127.0.0.1", 0)
                )
                await ui.start(
                    probe,
                    root,
                    f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws",
                )
                stack.push_async_callback(ui.stop)
                await ui.wait("select", lambda r: r["ok"], conv=source)
                await ui.wait(
                    "state", lambda r: file["id"] in [x["id"] for x in r["rows"]]
                )
                await ui.call("multi", start=first)
                for mid in ids[1:]:
                    await ui.call("multi", click=mid, modifier="ctrl")
                await ui.call("forward_picker", open=True)
                await ui.wait("forward_picker", lambda r: r["open"])
                assert (await ui.call("forward_picker", target=target, bundle=True))[
                    "confirmed"
                ]

                def placed():
                    with sqlite3.connect(db) as con:
                        return con.execute(
                            "select id,client_id from messages where content='bundle'"
                        ).fetchone()

                mid, cid = await until(placed, "one merged message from Qt")
                args = dict(conversation=target, message=mid)
                page = await c.ok("get_forwarded_messages", **args)
                assert [x["position"] for x in page["items"]] == [1, 2, 3] and page[
                    "items"
                ][0]["markdown"]
                assert (
                    await a.ok(
                        "send_message", conversation=target, client_id=cid, bundle=ids
                    )
                )["id"] == mid
                assert "error" in await c.ask(
                    "get_messages", conversation=source, around=first
                )
                assert "error" in await c.ask(
                    "send_message", conversation=target, client_id=9, bundle=ids
                )
                assert "error" in await a.ask(
                    "send_message", conversation=target, client_id=19, forward=mid
                )
                assert "error" in await a.ask(
                    "send_message",
                    conversation=target,
                    client_id=19,
                    bundle=[mid, first],
                )
                await ui.wait("select", lambda r: r["ok"], conv=target)
                await ui.call("bundle_view", open=mid)
                await ui.wait(
                    "bundle_view",
                    lambda r: r.get("count") == 3 and r["body"] == "原始正文",
                )
                await ui.call("bundle_view", close=True)
                long_ids = [await text(101 + i, "a" * 58000) for i in range(12)]
                long = await a.ok(
                    "send_message", conversation=target, bundle=long_ids, client_id=71
                )
                after = 0
                positions = []
                while True:
                    chunk = await c.ok(
                        "get_forwarded_messages",
                        conversation=target,
                        message=long["id"],
                        after=after,
                    )
                    assert all(i["body"] == "a" * 58000 for i in chunk["items"])
                    positions.extend(i["position"] for i in chunk["items"])
                    after = chunk["next"]
                    if not after:
                        break
                assert positions == list(range(1, 13))
                await ui.call("bundle_view", open=long["id"])
                first_page = await ui.wait("bundle_view", lambda r: r.get("more") and r.get("limit", 0) > 120)
                await ui.call("bundle_view", scroll=112, more=True)
                next_page = await ui.wait("bundle_view", lambda r: r.get("count", 0) > first_page["count"])
                assert next_page["scroll"] == 112 and next_page["body"] == "a" * 58000
                await ui.call("bundle_view", close=True)
                elsewhere = (await a.ok("create_group", title="独立记录", members=[2]))[
                    "conversation"
                ]
                other = await a.ok(
                    "send_message", conversation=elsewhere, bundle=ids, client_id=72
                )
                await a.ok("dissolve_group", conversation=elsewhere)
                assert "error" in await b.ask(
                    "get_forwarded_messages",
                    conversation=elsewhere,
                    message=other["id"],
                )
                await b.ok(
                    "edit_message", conversation=source, message=first, text="编辑后"
                )
                for src in ids:
                    await b.ok("delete_message", conversation=source, message=src)
                assert (await c.ok("get_forwarded_messages", **args)) == page
                for i, payload in [(2, PNG), (3, b"snapshot")]:
                    download = await c.ok("get_attachment", **args, item=i)
                    assert urllib.request.urlopen(download["url"]).read() == payload
                await ui.call("bundle_view", open=mid)
                await ui.wait("bundle_view", lambda r: r.get("count") == 3)
                gate.hold.add("get_attachment")
                await ui.call("bundle_view", attachment_row=1)
                await gate.caught("get_attachment")
                await ui.call("dialogstate", close=True)
                await ui.wait("bundle_view", lambda r: r.get("count") == 3)
                gate.hold.clear()
                await ui.call("bundle_view", attachment_row=1)
                await ui.wait("dialogstate", lambda r: r.get("image"))
                n = (await ui.call("deliveries")).get("image", 0)
                await gate.release("get_attachment", error=True)
                await ui.wait("deliveries", lambda r: r.get("image", 0) > n)
                assert (
                    "QA search failure" not in (await ui.call("dialogstate"))["status"]
                ), "previous preview error leaked into reopened image"
                await ui.call("dialogstate", close=True)
                await ui.wait("bundle_view", lambda r: r.get("count") == 3)
                saved = []
                for row, payload in [(1, PNG), (2, b"snapshot")]:
                    await ui.call("bundle_view", attachment_row=row)
                    await ui.wait(
                        "dialogstate",
                        lambda r: r["shown"] and (row != 1 or r.get("image")),
                    )
                    gate.hold.add("get_attachment")
                    destination = root / f"saved-{row}"
                    await ui.call(
                        "dialogstate", save=True, destination=str(destination)
                    )
                    await until(
                        lambda: len(gate.held) == row,
                        "independent bundle download grants",
                    )
                    saved.append((destination, payload))
                    await ui.call("dialogstate", close=True)
                    await ui.wait("bundle_view", lambda r: r.get("count") == 3)
                await ui.call("bundle_view", close=True)
                gate.held.reverse()
                await gate.release("get_attachment")
                for destination, payload in saved:
                    await until(destination.exists, "both Qt downloads completed")
                    assert destination.read_bytes() == payload
                # The archive metadata, children and file references must survive an actual restore.
                with sqlite3.connect(db) as con:
                    con.execute(
                        "update messages set created_at=1739404800000 where id=?",
                        (mid,),
                    )
                admin("--db", db, "archive", "2025-02")
                admin("--db", db, "backup", root / "backup")
                await c.ok("clear_history", conversation=target, through=mid)
                assert "error" in await c.ask("get_attachment", **args, item=2)
                assert "error" in await c.ask("get_forwarded_messages", **args)
                await a.ok("invite_group_members", conversation=target, members=[2])
                assert "error" in await b.ask("get_forwarded_messages", **args)
                await ui.call("bundle_view", open=mid)
                await ui.wait("bundle_view", lambda r: r.get("count") == 3)
                await ui.call("bundle_view", attachment_row=1)
                await ui.wait("dialogstate", lambda r: r.get("image"))
                await ui.call("dialogstate", save=True)
                await ui.wait("dialogstate", lambda r: r.get("chooser"))
                await a.ok("delete_message", **args)
                await ui.wait(
                    "dialogstate", lambda r: not r["shown"] and not r.get("chooser")
                )
                assert not (await ui.call("bundle_view"))["open"]
                gate.hold.add("get_forwarded_messages")
                await ui.call("bundle_view", open=long["id"])
                await gate.caught("get_forwarded_messages")
                await a.ok("delete_message", conversation=target, message=long["id"])
                await ui.wait("bundle_view", lambda r: not r["open"])
                count = (await ui.call("deliveries")).get("bundle", 0)
                await gate.release("get_forwarded_messages")
                await ui.wait("deliveries", lambda r: r.get("bundle", 0) > count)
                assert not (await ui.call("bundle_view"))["open"]
                assert "error" in await a.ask("get_attachment", **args, item=2)
                with sqlite3.connect(db) as con:
                    assert con.execute(
                        "select count(*) from bundle_items where parent=?", (mid,)
                    ).fetchone() == (0,)
        # Startup drains outstanding object cleanup in the same disposable bucket.
        with chat(db, env):
            await until(
                lambda: not api.list_objects_v2(Bucket=store.bucket).get("Contents"),
                "last bundle files reclaimed",
            )
    admin("restore", root / "backup", root / "restored")
    with store.node(root / "restored-silo") as (api, env):
        api.create_bucket(Bucket=store.bucket)
        for saved, data in [(image, PNG), (file, b"snapshot")]:
            api.put_object(
                Bucket=store.bucket, Key=saved["attachment"]["object_key"], Body=data
            )
        with chat(root / "restored/chat.sqlite3", env) as url:
            async with AsyncExitStack() as stack:
                reader = await login(stack, url, "c", "c")
                assert await reader.ok("get_forwarded_messages", **args) == page
                grant = await reader.ok("get_attachment", **args, item=2)
                assert urllib.request.urlopen(grant["url"]).read() == PNG
    print(
        "PASS Qt/SDK/server merged selection, isolated Silo shared references and last-reference GC, archived sources, snapshots, keysets and permission floors"
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-bundle-") as tmp:
        asyncio.run(run(Path(tmp), sys.argv[1]))
