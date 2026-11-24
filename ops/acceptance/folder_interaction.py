"""Native Qt drag, read menus and restart. The entry point always creates a private Xvfb."""

import asyncio
import ctypes
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from desktop import Desktop, until
from fixture import fixture


async def drag(start, end):
    x11 = ctypes.CDLL("libX11.so.6")
    xtst = ctypes.CDLL("libXtst.so.6")
    x11.XOpenDisplay.restype = ctypes.c_void_p
    display = ctypes.c_void_p(x11.XOpenDisplay(None))
    assert display
    try:

        async def motion(x, y):
            xtst.XTestFakeMotionEvent(display, -1, x, y, 0)
            x11.XSync(display, 0)
            await asyncio.sleep(0.1)

        await motion(*start)
        xtst.XTestFakeButtonEvent(display, 1, 1, 0)
        await motion(start[0] + 18, start[1])
        await motion(start[0] + 24, start[1])
        await motion(*end)
        await motion(end[0] + 2, end[1])
        xtst.XTestFakeButtonEvent(display, 1, 0, 0)
        x11.XSync(display, 0)
    finally:
        x11.XCloseDisplay(display)


async def run(probe):
    async with fixture() as (_, login, a, b, dm, _pid):
        await a.ok("create_conversation_folder", name="工作")
        f = (await a.ok("get_conversations"))["folders"][0]["id"]
        g = (await a.ok("create_group", title="研发", members=[2]))["conversation"]

        async def room_state(room):
            return next(
                c
                for c in (await a.ok("get_conversations"))["conversations"]
                if c["id"] == room
            )

        with tempfile.TemporaryDirectory() as home:
            url = f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws"
            gui = Desktop()

            async def start():
                await gui.start(probe, Path(home), url, native=True, platform="xcb")
                await gui.wait("filter", lambda r: "工作" in r["names"])

            anchor = (
                await a.ok(
                    "send_message", conversation=dm, text="引用目标", client_id=401
                )
            )["id"]
            await start()
            try:
                await gui.call("select", conv=dm)
                await gui.wait(
                    "state", lambda r: any(m["id"] == anchor for m in r["rows"])
                )
                assert (await gui.call("reply_to", id=anchor))["reply"]
                await gui.call("draft", text="保留草稿")
                frm = (await gui.call("sidebar_action", conversation=g))["point"]
                dest = (await gui.call("sidebar_action", folder=f))["point"]
                await drag(frm, dest)

                async def moved():
                    return (await room_state(g))["folder"] == f

                await until(moved, "native drag must update server folder")
                state = await gui.call("state")
                assert state["conv"] == dm and state["draft"] == "保留草稿", state
                for cid, scope, action in [
                    (1, {"conversation": g}, "标为已读"),
                    (2, {"folder": f}, "全部标为已读"),
                    (3, {}, "所有会话标为已读"),
                ]:
                    await b.ok(
                        "send_message", conversation=g, text="待标读", client_id=cid
                    )
                    assert (await room_state(g))["unread"] > 0
                    await gui.call("sidebar_action", action=action, **scope)

                    async def read():
                        return (await room_state(g))["unread"] == 0

                    await until(read, action)
                await gui.call(
                    "sidebar_action",
                    conversation=g,
                    action="移动到分组",
                    destination="未分组",
                )

                async def ungrouped():
                    return (await room_state(g))["folder"] == 0

                await until(ungrouped, "context menu move back to default")
                await gui.wait(
                    "sidebar_action", lambda r: r.get("membership") == 0, conversation=g
                )
                await gui.call(
                    "sidebar_action",
                    conversation=g,
                    action="移动到分组",
                    destination="工作",
                )
                await until(moved, "context menu move back to work")
                await gui.wait(
                    "sidebar_action", lambda r: r.get("membership") == f, conversation=g
                )
                state = await gui.call("state")
                assert (
                    state["draft"] == "保留草稿"
                    and state["reply"]
                    and state["conv"] == dm
                ), state
                await gui.call("toggle_folder", folder=f)
                await gui.stop()
                await start()
                assert "研发" not in (await gui.call("filter"))["names"]
                assert (await gui.call("filter", query="研发"))["names"] == ["研发"]
            finally:
                await gui.stop()
        print(
            "PASS native Xvfb drag and context-menu moves, retained chat/draft/reply, three read menus, restart/collapse"
        )


if __name__ == "__main__":
    env = os.environ.copy()
    for key in (
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "SESSION_MANAGER",
        "DBUS_SESSION_BUS_ADDRESS",
    ):
        env.pop(key, None)
    env.update(
        XDG_SESSION_TYPE="x11",
        QT_QPA_PLATFORMTHEME="generic",
        NO_AT_BRIDGE="1",
        QT_IM_MODULE="compose",
        GTK_USE_PORTAL="0",
    )
    script = "import asyncio,sys;sys.path.insert(0,sys.argv[1]);from folder_interaction import run;asyncio.run(run(sys.argv[2]))"
    with tempfile.TemporaryDirectory() as private:
        env["XDG_RUNTIME_DIR"] = private
        bus = Path(private) / "session.conf"
        bus.write_text(
            '<busconfig><type>session</type><listen>unix:tmpdir=/tmp</listen><policy context="default"><allow send_destination="*"/><allow receive_sender="*"/><allow own="*"/></policy></busconfig>'
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
                    sys.argv[1],
                ],
                env=env,
            )
        )
