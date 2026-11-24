"""Qt message selection using a separate Xvfb display, loopback RPC, and disposable Silo."""

import argparse
import base64
from contextlib import AsyncExitStack
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put

PNG = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAMAAAACCAIAAAASFvFNAAAAFUlEQVR4nGOUrHjOwMDAwMDAxAADABneAXybXmOGAAAAAElFTkSuQmCC"
)


async def run(root, probe):
    db = root / "chat.sqlite3"
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, storage) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                peer = await login(stack, url, "b", "b", True)
                await peer.ok("send_friend_request", user=1)
                await a.ok("respond_friend_request", user=2, accept=True)
                target = (await a.ok("open_direct_conversation", user=2))[
                    "conversation"
                ]
                group = (
                    await peer.ok("create_group", title="消息选择验收", members=[1])
                )["conversation"]
                seeded = [
                    (
                        await peer.ok(
                            "send_message",
                            conversation=group,
                            text=f"历史 {n}",
                            client_id=100 + n,
                        )
                    )["id"]
                    for n in range(64)
                ]
                text = "中文 English 👩🏽‍💻\n保留第二行 é"
                mixed = (
                    await peer.ok(
                        "send_message", conversation=group, text=text, client_id=250
                    )
                )["id"]

                async def attach(ident, data, name, media):
                    grant = await begin(peer, group, ident, data, name, media)
                    await peer.ok(
                        "confirm_attachment",
                        upload=ident,
                        number=1,
                        etag=put(grant["url"], data),
                    )
                    return (await peer.ok("finish_attachment", upload=ident))["id"]

                image = await attach(501, PNG, "示例.png", "image/png")
                file = await attach(
                    502, b"data", "example.bin", "application/octet-stream"
                )
                gate = Gate(url)
                proxy = await stack.enter_async_context(
                    serve(gate.handle, "127.0.0.1", 0)
                )
                ui = Desktop()
                await ui.start(
                    probe,
                    root,
                    f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws",
                    native=True,
                    platform="xcb",
                )
                try:
                    await cases(
                        ui,
                        gate,
                        peer,
                        db,
                        group,
                        target,
                        seeded,
                        mixed,
                        image,
                        file,
                        text,
                    )
                finally:
                    await ui.stop()


async def cases(ui, gate, peer, db, group, target, seeded, mixed, image, file, text):
    async def multi(**params):
        return await ui.call("multi", **params)

    async def select(*ids):
        await multi(key="esc")
        assert (await multi(start=ids[0]))["mode"]
        for mid in ids[1:]:
            await multi(click=mid, modifier="ctrl")

    assert (await ui.call("select", conv=group))["ok"]
    await ui.wait("state", lambda s: file in [row["id"] for row in s["rows"]])
    await ui.call("reply_to", id=seeded[-1])
    await ui.call("draft", text="给回复的草稿\n未发送")
    initial = await multi(cursor=3)
    assert initial["reply"] == seeded[-1] and initial["cursor"] == 3

    await select(file)
    contiguous = await multi(click=mixed, modifier="shift")
    assert contiguous["valid"] == [mixed, image, file], contiguous
    discontinuous = await multi(click=image, modifier="ctrl")
    assert discontinuous["valid"] == [mixed, file], discontinuous
    await select(file, mixed, image)
    rpc_start = len(gate.messages)
    key_result = await multi(key="copy")
    button_result = await multi(button=True)
    assert button_result["buttonFocused"], button_result
    expected = text + "\n图片：示例.png\n0.1 KiB\n文件：example.bin\n0.0 KiB"
    assert key_result["clipboard"] == button_result["clipboard"] == expected, (
        button_result
    )
    assert all(
        button_result[field] == initial[field] for field in ("draft", "cursor", "reply")
    ), button_result
    assert not any(m["method"] == "get_attachment" for m in gate.messages[rpc_start:])
    focused_copy = await multi(seed="焦点在复制按钮", key="copy", focused=True)
    escaped_from_button = await multi(key="esc", focused=True)

    # Start over so the remaining observations also run on the unmodified version.
    await select(seeded[-3], seeded[-1])
    gate.hold.add("get_messages")
    await multi(older=True)
    await gate.caught("get_messages")
    while_loading = await multi()
    assert while_loading["first"] != seeded[0]
    assert while_loading["valid"] == [seeded[-3], seeded[-1]]
    await gate.release("get_messages")
    await ui.wait("multi", lambda result: result["first"] == seeded[0])
    assert (await multi())["valid"] == [seeded[-3], seeded[-1]]
    gate.hold_requests.add("resume_session")
    await gate.down.close()
    await until(
        lambda: any(m["method"] == "resume_session" for _, m in gate.held_requests),
        "held reconnect",
    )
    disconnected = await ui.wait("multi", lambda r: not r["connected"])
    print(
        "offline selection:",
        {k: disconnected[k] for k in ("count", "copy", "forward")},
        flush=True,
    )
    await gate.release_requests("resume_session")
    reconnected = await ui.wait("multi", lambda r: r["connected"])
    assert reconnected["valid"] == [seeded[-3], seeded[-1]]

    await peer.ok(
        "edit_message", conversation=group, message=seeded[-1], text="被更新的选中消息"
    )
    added = (
        await peer.ok(
            "send_message", conversation=group, text="实时插入", client_id=315
        )
    )["id"]
    await ui.wait("state", lambda s: added in [row["id"] for row in s["rows"]])
    assert (await multi(key="copy"))["clipboard"].endswith("被更新的选中消息")

    await peer.ok("delete_message", conversation=group, message=seeded[-3])
    after_first = await ui.wait("multi", lambda r: r["deleted"], inspect=seeded[-3])
    partial_copy = (await multi(key="copy"))["clipboard"]
    await peer.ok("delete_message", conversation=group, message=seeded[-1])
    after_all = await ui.wait("multi", lambda r: r["deleted"], inspect=seeded[-1])
    no_copy = await multi(seed="保留已有剪贴板", key="copy")
    print(
        "withdraw partial:",
        {k: after_first[k] for k in ("count", "marked", "valid", "copy", "forward")},
        flush=True,
    )
    print(
        "withdraw all:",
        {k: after_all[k] for k in ("count", "marked", "valid", "copy", "forward")},
        flush=True,
    )
    print("Esc with copy button focused:", escaped_from_button["mode"], flush=True)
    assert partial_copy == "被更新的选中消息"
    assert no_copy["clipboard"] == "保留已有剪贴板"

    await select(seeded[-5])
    shifted = await multi(click=seeded[-2], modifier="shift")
    deleted_row = await multi(inspect=seeded[-3])
    assert seeded[-3] not in shifted["valid"]

    gate.hold_requests.add("send_message")
    await ui.call("send", text="尚未发出的本地行")
    await until(
        lambda: any(m["method"] == "send_message" for _, m in gate.held_requests),
        "held send",
    )
    await ui.wait("multi", lambda r: r["pending"] in (1, 2))
    pending = await multi(click="pending", modifier="ctrl", inspect="pending")
    await peer.ok("set_group_member_mute", conversation=group, user=1, seconds=-1)
    await gate.release_requests("send_message")
    await ui.wait("multi", lambda r: r["pending"] == 3)
    failed = await multi(click="pending", modifier="ctrl", inspect="pending")
    await peer.ok("set_group_member_mute", conversation=group, user=1, seconds=0)
    print(
        "range/pending/failed:",
        [
            {k: r[k] for k in ("selectable", "marked", "valid")}
            for r in (deleted_row, pending, failed)
        ],
        flush=True,
    )

    await select(seeded[2], mixed)
    # Clear from a second authenticated device of the same user, not a Qt model injection.
    async with AsyncExitStack() as stack:
        a2 = await login(stack, gate.upstream, "a", "d")
        await a2.ok("clear_history", conversation=group, through=seeded[2])
    cleared = await ui.wait("multi", lambda r: r["valid"] == [mixed])
    assert cleared["count"] == "已选 1 条"
    assert (await ui.call("select", conv=target))["ok"]
    await ui.wait("state", lambda r: r["conv"] == target)
    assert not (await multi())["mode"] and not (await multi())["valid"]
    assert (await ui.call("select", conv=group))["ok"]
    await ui.wait("state", lambda s: mixed in [row["id"] for row in s["rows"]])
    menu_message = (
        await peer.ok(
            "send_message", conversation=group, text="菜单原正文", client_id=9901
        )
    )["id"]
    await ui.wait("state", lambda r: any(x["id"] == menu_message for x in r["rows"]))
    await ui.call("message_menu", open=menu_message)
    await ui.wait("message_menu", lambda r: r.get("open"))
    await peer.ok(
        "edit_message", conversation=group, message=menu_message, text="打开菜单后编辑"
    )
    await ui.wait(
        "state", lambda r: any(x["text"] == "打开菜单后编辑" for x in r["rows"])
    )
    await ui.call("message_menu", copy=True)
    copied = await ui.wait("message_menu", lambda r: not r.get("open"))
    assert copied["clipboard"] == "打开菜单后编辑", f"menu copied stale body: {copied}"
    await multi(seed="保留剪贴板")
    await ui.call("message_menu", open=menu_message)
    await ui.wait("message_menu", lambda r: r.get("open"))
    await peer.ok("delete_message", conversation=group, message=menu_message)
    await ui.wait("multi", lambda r: r.get("deleted"), inspect=menu_message)
    await ui.call("message_menu", copy=True)
    copied = await ui.wait("message_menu", lambda r: not r.get("open"))
    assert (
        copied["clipboard"] == "保留剪贴板"
    )  # Withdrawal must leave the clipboard unchanged.
    print(
        "PASS a still-open message menu copies current text and never copies a withdrawn row",
        flush=True,
    )

    async with AsyncExitStack() as stack:
        a2 = await login(stack, gate.upstream, "a", "d")
        await forwarding_cases(
            ui, gate, peer, a2, db, group, target, mixed, image, file, select
        )

    assert after_first["count"] == "已选 1 条" and not after_first["marked"], (
        after_first
    )
    assert (
        after_all["count"] == "已选 0 条"
        and not after_all["copy"]
        and not after_all["forward"]
        and not after_all["marked"]
    ), after_all
    assert all(
        not r["selectable"] and not r["marked"] for r in (deleted_row, pending, failed)
    )
    assert focused_copy["clipboard"] == expected, focused_copy
    assert not escaped_from_button["mode"], escaped_from_button
    assert (
        disconnected["copy"] and not disconnected["forward"] and reconnected["forward"]
    ), disconnected
    print(
        "PASS real Qt selection: range/toggle, remote withdrawal, pending/failed, paging, draft/cursor/reply, clipboard and forwarding",
        flush=True,
    )


async def forwarding_cases(
    ui, gate, peer, a2, db, group, target, mixed, image, file, select
):
    replies = {}
    start = 0

    def record(method, response):
        if method in ("send_message", "set_typing") and "id" in response:
            replies[response["id"]] = response

    gate.transform = record

    async def send(text, client_id):
        return (
            await peer.ok(
                "send_message", conversation=group, text=text, client_id=client_id
            )
        )["id"]

    def forwarded_to(destination):
        with sqlite3.connect(db) as connection:
            return [
                source
                for (source,) in connection.execute(
                    "SELECT forward_source FROM messages "
                    "WHERE conversation_id=? AND forward_source IS NOT NULL ORDER BY id",
                    (destination,),
                )
            ]

    async def open_picker(ids, *, single=False):
        nonlocal start
        start = len(gate.messages)
        if single:
            await ui.call("multi", key="esc")
            await ui.call("message_menu", open=ids[0])
            await ui.wait("message_menu", lambda r: r.get("open"))
            await ui.call("message_menu", forward=True)
        else:
            await select(*ids)
            assert (await ui.call("multi"))["valid"] == sorted(ids)
            await ui.call("forward_picker", open=True)
        await ui.wait("forward_picker", lambda r: r.get("open"))

    async def finish(label, destination, expected, *, cancel=False, preserve=True):
        before = await ui.call("multi")
        prior = forwarded_to(destination)

        def requests():
            return [
                rpc
                for rpc in gate.messages[start:]
                if rpc["method"] == "send_message" and "forward" in rpc["params"]
            ]

        if cancel:
            await ui.call("forward_picker", cancel=True)
        elif (await ui.call("forward_picker")).get("open"):
            confirmed = await ui.call("forward_picker", target=destination)
            assert confirmed.get("confirmed"), confirmed
        await ui.wait("forward_picker", lambda r: not r.get("open"))
        if expected:
            await until(
                lambda: any("error" in replies.get(r["id"], {}) for r in requests())
                or (
                    len(requests()) >= len(expected)
                    and all(r["id"] in replies for r in requests())
                ),
                label,
            )

        # Order a harmless request after closing the modal, on the same SDK connection.
        # Its response gives a barrier for cases that must send no forwarding RPC.
        fence_start = len(gate.messages)
        await ui.call("forward_picker", fence=group)
        await until(
            lambda: any(
                rpc["method"] == "set_typing"
                and rpc["params"]["conversation"] == group
                and rpc["id"] in replies
                for rpc in gate.messages[fence_start:]
            ),
            f"{label}: WebSocket fence",
        )
        actual = [
            (r["params"]["conversation"], r["params"]["forward"]) for r in requests()
        ]
        errors = [
            replies[r["id"]]["error"]["code"]
            for r in requests()
            if "error" in replies.get(r["id"], {})
        ]
        stored = forwarded_to(destination)[len(prior) :]
        print(f"{label}: RPC={actual} errors={errors} SQLite={stored}", flush=True)
        assert (actual, errors, stored) == (
            [(destination, mid) for mid in expected],
            [],
            expected,
        )
        if preserve:
            after = await ui.call("multi")
            assert all(
                after[key] == before[key] for key in ("draft", "cursor", "reply")
            )
            assert before["draft"] == "转发不影响我的草稿" and before["cursor"] == 4

    anchor = await send("转发用的独立回复目标", 9950)
    await ui.wait("state", lambda r: any(row["id"] == anchor for row in r["rows"]))
    await ui.call("reply_to", id=anchor)
    await ui.call("draft", text="转发不影响我的草稿")
    assert (await ui.call("multi", cursor=4))["reply"] == anchor

    await open_picker([file, mixed, image])
    await finish("normal batch (text/image/file)", target, [mixed, image, file])
    assert not (await ui.call("multi"))["mode"]

    for index, (case, withdrawn) in enumerate(
        (
            ("first withdrawn", (0,)),
            ("middle withdrawn", (1,)),
            ("last withdrawn", (2,)),
            ("all withdrawn", (0, 1, 2)),
            ("single withdrawn", (0,)),
        )
    ):
        single = case == "single withdrawn"
        ids = [
            await send(f"{case} {i}", 0x3000 + i + 10 * index)
            for i in range(1 if single else 3)
        ]
        await ui.wait(
            "state",
            lambda r: all(mid in [row["id"] for row in r["rows"]] for mid in ids),
        )
        await open_picker(ids, single=single)
        for pos in withdrawn:
            await peer.ok("delete_message", conversation=group, message=ids[pos])
            await ui.wait("multi", lambda r: r["deleted"], inspect=ids[pos])
        assert (await ui.call("forward_picker")).get("open")
        await finish(
            case, target, [mid for pos, mid in enumerate(ids) if pos not in withdrawn]
        )

    await open_picker([mixed])
    await finish("cancel picker", target, [], cancel=True)

    unavailable = (await peer.ok("create_group", title="转发目标失权", members=[1]))[
        "conversation"
    ]
    await ui.wait("forward_picker", lambda r: r["available"], available=unavailable)
    await open_picker([mixed])
    await peer.ok("dissolve_group", conversation=unavailable)
    await ui.wait("forward_picker", lambda r: not r["available"], available=unavailable)
    await finish("target dissolved", unavailable, [])

    for count in (1, 2):
        ids = [
            await send(f"清空边界 {count}/{i}", 0x4000 + 10 * count + i)
            for i in range(2)
        ]
        await ui.wait("state", lambda r: any(row["id"] == ids[-1] for row in r["rows"]))
        # The reply anchor must be newer than this clear, not one of the forwarded messages.
        anchor = await send(f"清空后保留回复 {count}", 0x4100 + count)
        await ui.wait("state", lambda r: any(row["id"] == anchor for row in r["rows"]))
        await ui.call("multi", key="esc")
        await ui.call("reply_to", id=anchor)
        assert (await ui.call("multi", cursor=4))["reply"] == anchor
        await open_picker(ids)
        await a2.ok("clear_history", conversation=group, through=ids[count - 1])
        await ui.wait("multi", lambda r: r["valid"] == ids[count:])
        await finish(f"clear {count}/2", target, ids[count:])

    await open_picker([anchor])
    await peer.ok("remove_group_member", conversation=group, user=1)
    await ui.wait("state", lambda s: s["conv"] != group)
    await finish("source membership revoked", target, [], preserve=False)
    assert not (await ui.call("multi"))["mode"]

    destination = (
        await peer.ok("create_group", title="切换账号后仍可访问", members=[1])
    )["conversation"]
    await ui.wait("forward_picker", lambda r: r["available"], available=destination)
    assert (await ui.call("select", conv=target))["ok"]
    direct = await ui.wait("state", lambda r: r["conv"] == target and r["rows"])
    await open_picker([direct["rows"][-1]["id"]])
    await ui.call("logout")
    await ui.wait("login_state", lambda r: r["user"] == 0)
    replies.clear()
    await ui.call("login", user="b")
    await ui.wait("state", lambda r: r["user"] == 2)
    await ui.wait("forward_picker", lambda r: r["available"], available=destination)
    assert (await ui.call("select", conv=target))["ok"]
    await ui.wait("state", lambda r: r["conv"] == target and r["rows"])
    await finish("account switched", destination, [], preserve=False)
    print(
        "PASS forward picker: real Qt, WebSocket responses and SQLite in history order",
        flush=True,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="chat-message-selection-") as tmp:
        root = Path(tmp)
        # This entry always allocates a separate X server and bus before creating Qt.
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        bus = root / "session.conf"
        bus.write_text(
            "<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen>"
            '<policy context="default"><allow send_destination="*"/>'
            '<allow receive_sender="*"/><allow own="*"/></policy></busconfig>'
        )
        env = os.environ.copy()
        for key in (
            "DISPLAY",
            "WAYLAND_DISPLAY",
            "SESSION_MANAGER",
            "DBUS_SESSION_BUS_ADDRESS",
            "QT_IM_MODULE",
        ):
            env.pop(key, None)
        env.update(
            XDG_RUNTIME_DIR=str(runtime),
            XDG_SESSION_TYPE="x11",
            QT_QPA_PLATFORMTHEME="generic",
            GTK_USE_PORTAL="0",
            NO_AT_BRIDGE="1",
        )
        script = (
            "import asyncio, sys; from pathlib import Path; "
            "sys.path.insert(0, sys.argv[1]); from message_selection import run; "
            "asyncio.run(run(Path(sys.argv[2]), sys.argv[3]))"
        )
        sys.exit(
            subprocess.call(
                [
                    "xvfb-run",
                    "-a",
                    "-s",
                    "-screen 0 1280x900x24",
                    "dbus-run-session",
                    f"--config-file={bus}",
                    "--",
                    sys.executable,
                    "-c",
                    script,
                    str(Path(__file__).resolve().parent),
                    str(root),
                    args.probe,
                ],
                env=env,
            )
        )
