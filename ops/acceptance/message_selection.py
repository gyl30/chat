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

    await select(mixed, file)
    await multi(forward=target)

    async def forwarded():
        with sqlite3.connect(db) as connection:
            return connection.execute(
                "SELECT forward_source FROM messages WHERE conversation_id=? AND forward_source IS NOT NULL ORDER BY id",
                (target,),
            ).fetchall() == [(mixed,), (file,)]

    await until(forwarded, "batch forwarded in history order")
    assert not (await multi())["mode"]

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
    await select(mixed)
    await peer.ok("remove_group_member", conversation=group, user=1)
    await ui.wait("state", lambda s: s["conv"] != group)
    assert not (await multi())["mode"] and not (await multi())["valid"]

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
