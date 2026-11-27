"""Check live profile categories against isolated Silo and authorized history."""

import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put


async def check(root, probe):
    store = Store()
    with store.node(root / "silo") as (s3, storage):
        s3.create_bucket(Bucket=store.bucket)
        with chat(root / "chat.sqlite", storage) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                b = await login(stack, url, "b", "b", True)
                c = await login(stack, url, "c", "c", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                await b.ok("send_friend_request", user=3)
                await c.ok("respond_friend_request", user=2, accept=True)
                direct = (await a.ok("open_direct_conversation", user=2))[
                    "conversation"
                ]
                group = (await b.ok("create_group", title="Profiles", members=[1]))[
                    "conversation"
                ]

                async def upload(
                    user, room, upload_id, kind
                ):
                    payload = b"isolated-category-" + str(upload_id).encode()
                    name = "image.png" if kind == "image/png" else "document.bin"
                    part = await begin(user, room, upload_id, payload, name, kind)
                    await user.ok(
                        "confirm_attachment",
                        upload=upload_id,
                        number=1,
                        etag=put(part["url"], payload),
                    )
                    return await user.ok("finish_attachment", upload=upload_id)

                gate = Gate(url)
                proxy = await stack.enter_async_context(
                    serve(gate.handle, "127.0.0.1", 0)
                )
                entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
                ui = Desktop()
                await ui.start(probe, root / "a", entry)

                async def media(*values, op="group_profile"):
                    return await ui.wait(op, lambda r: r.get("media") == list(values))

                try:
                    gate.hold.add("get_messages")
                    await ui.call("group", id=group)
                    await media()
                    await until(
                        lambda: sum(
                            request["method"] == "get_messages"
                            and request["params"].get("limit") == 1
                            for request in gate.messages
                        )
                        == 3,
                        "one request per shared category",
                    )
                    plain = (
                        await b.ok(
                            "send_message",
                            conversation=group,
                            client_id=81,
                            text="Only text",
                        )
                    )["id"]
                    img = (await upload(b, group, 82, "image/png"))["id"]
                    await until(
                        lambda: len(gate.held) == 4,
                        "old empty and new image replies held",
                    )
                    gate.held.reverse()
                    await gate.release("get_messages")
                    await media("图片")
                    await b.ok(
                        "edit_message",
                        conversation=group,
                        message=plain,
                        text="Added https://example.com",
                    )
                    await media("图片", "链接")
                    file = (await upload(b, group, 83, "application/octet-stream"))[
                        "id"
                    ]
                    await media("图片", "文件", "链接")

                    extra = (await upload(b, group, 86, "image/png"))["id"]
                    gate.hold.add("get_messages")
                    await b.ok("delete_message", conversation=group, message=img)
                    await gate.caught("get_messages")
                    await gate.release("get_messages", error=True)
                    # The recalled witness is invalid, but a transient failure must not
                    # strand the category while another accessible image still exists.
                    await media("文件", "链接")
                    await media("图片", "文件", "链接")
                    await b.ok("delete_message", conversation=group, message=extra)
                    await media("文件", "链接")
                    await b.ok(
                        "edit_message",
                        conversation=group,
                        message=plain,
                        text="Only text again",
                    )
                    await media("文件")
                    await a.ok("clear_history", conversation=group, through=file)
                    await media()
                    old = await upload(b, group, 85, "image/png")
                    await media("图片")
                    await b.ok("invite_group_members", conversation=group, members=[3])
                    assert not (
                        await c.ok(
                            "get_messages",
                            conversation=group,
                            category="images",
                            limit=1,
                        )
                    )["messages"]
                    assert (
                        await a.ok(
                            "get_messages",
                            conversation=group,
                            category="images",
                            limit=1,
                        )
                    )["messages"][0]["id"] == old["id"]
                    await a.ok("leave_group", conversation=group)
                    await ui.wait("group_profile", lambda r: "media" not in r)
                    await b.ok("invite_group_members", conversation=group, members=[1])
                    await ui.wait("select", lambda r: r["ok"], conv=group)
                    await ui.call("group", id=group)
                    await media()
                    assert not (
                        await a.ok(
                            "get_messages",
                            conversation=group,
                            category="images",
                            limit=1,
                        )
                    )["messages"]
                    await ui.call("group_profile", close=True)

                    await ui.call("contacts", open=True)
                    await ui.wait("contacts", lambda r: r["names"] == ["b"])
                    await ui.call("contacts", profile=True)
                    await media(op="contacts")
                    await b.ok(
                        "send_message",
                        conversation=direct,
                        client_id=91,
                        text="https://example.com/private",
                    )
                    await media("链接", op="contacts")
                    await ui.call("contacts", closeProfile=True, close=True)
                finally:
                    await ui.stop()
    print(
        "PASS shared entries: live per-category heads, errors, withdrawal, edits, clear, new membership and friend scope"
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-profile-media-") as directory:
        asyncio.run(check(Path(directory), sys.argv[1]))
