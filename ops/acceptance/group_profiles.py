"""Exercise group profile permissions, isolated avatar namespaces and live Qt member filtering."""

import asyncio
import base64
import struct
import sys
import tempfile
import zlib
from pathlib import Path

from desktop import Desktop
from fixture import fixture


def png(rgb):
    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">2I5B", 4, 4, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress((b"\0" + bytes(rgb) * 4) * 4))
        + chunk(b"IEND", b"")
    )


async def prepare_avatar(client, data, **scope):
    key = (await client.ok("begin_avatar_upload", size=len(data), **scope))["upload"]
    await client.ok(
        "upload_avatar_chunk",
        upload=key,
        offset=0,
        data=base64.b64encode(data).decode(),
    )
    return key


async def main(probe):
    async with fixture() as (_, login, a, b, dm, _pid):
        outsider = await login("stranger", "c", True)
        g = (await a.ok("create_group", title="研发", members=[2]))["conversation"]
        assert g == 2  # Exercise equal user and conversation IDs.
        red, blue = png((255, 0, 0)), png((0, 0, 255))
        await b.ok("finish_avatar_upload", upload=await prepare_avatar(b, red))
        await b.ok("set_user_profile", nickname="设计师", signature="")
        await a.ok("set_friend_remark", user=2, remark="采购")
        await a.ok("set_group_announcement", conversation=g, text="独立公告")
        for client in [b, outsider]:
            for method, args in [
                ("set_group_description", dict(text="拒绝")),
                ("clear_avatar", {}),
                ("begin_avatar_upload", dict(size=len(red))),
            ]:
                assert "error" in await client.ask(method, conversation=g, **args)
        assert "error" in await outsider.ask(
            "get_avatar", conversation=g, revision=0, offset=0
        )
        assert "error" in await a.ask(
            "set_group_description", conversation=g, text="🌏" * 257
        )
        await a.ok("set_group_description", conversation=g, text="🌏" * 256)
        with tempfile.TemporaryDirectory() as home:
            root = Path(home)
            path = root / "group.png"
            path.write_bytes(blue)
            gui = Desktop()
            await gui.start(probe, root, f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws")
            try:
                await gui.wait("filter", lambda r: "研发" in r["names"])
                await gui.call("group", id=g)
                await gui.wait(
                    "group_profile",
                    lambda r: r.get("editable") and "采购" in r["members"],
                )
                await gui.call("group_profile", image=str(path))
                await gui.wait(
                    "group_profile",
                    lambda r: r.get("groupImage") == "#0000ff"
                    and r.get("userImage") == "#ff0000",
                    group=g,
                )
                await gui.call("group_profile", description="第一行\n第二行")
                await gui.wait(
                    "group_profile", lambda r: r.get("description") == "第一行\n第二行"
                )
                for query in ["设计", "采购", "b"]:
                    assert (await gui.call("group_profile", query=query))[
                        "members"
                    ] == ["采购"]
                await gui.call("group_profile", query="项目")
                await b.ok("set_user_profile", nickname="项目经理", signature="")
                await gui.wait("group_profile", lambda r: r["members"] == ["采购"])
                await a.ok("set_friend_remark", user=2, remark="")
                await gui.wait("group_profile", lambda r: r["members"] == ["项目经理"])
                rec = await b.ok("get_avatar", conversation=g, revision=1, offset=0)
                assert base64.b64decode(rec["data"]) == blue
                await a.ok("set_group_admin", conversation=g, user=2, admin=True)
                await b.ok("set_group_description", conversation=g, text="")
                await b.ok(
                    "finish_avatar_upload",
                    upload=await prepare_avatar(b, red, conversation=g),
                )
                await gui.wait(
                    "group_profile", lambda r: r.get("groupImage") == "#ff0000", group=g
                )
                # Restart a separate member device. Role and profile come from fresh server snapshots.
                peer = Desktop()
                try:
                    await peer.start(
                        probe,
                        root / "peer",
                        f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws",
                        user="b",
                        user_id=2,
                    )
                    await peer.wait("filter", lambda s: "研发" in s["names"])
                    await peer.call("group", id=g)
                    await peer.wait(
                        "group_profile",
                        lambda s: s.get("editable")
                        and s.get("groupImage") == "#ff0000",
                        group=g,
                    )
                    await peer.stop()
                    await a.ok(
                        "set_group_description", conversation=g, text="离线后更新"
                    )
                    await a.ok("set_group_admin", conversation=g, user=2, admin=False)
                    await peer.start(
                        probe,
                        root / "peer",
                        f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws",
                        user="b",
                        user_id=2,
                    )
                    await peer.wait("filter", lambda s: "研发" in s["names"])
                    await peer.call("group", id=g)
                    await peer.wait(
                        "group_profile",
                        lambda s: s.get("description") == "离线后更新"
                        and not s["editable"]
                        and s.get("groupImage") == "#ff0000",
                        group=g,
                    )
                    await a.ok("set_group_admin", conversation=g, user=2, admin=True)
                    await peer.wait("group_profile", lambda s: s.get("editable"))
                finally:
                    await peer.stop()
                key = await prepare_avatar(b, blue, conversation=g)
                await a.ok("set_group_admin", conversation=g, user=2, admin=False)
                assert "error" in await b.ask("finish_avatar_upload", upload=key)
                await a.ok("clear_avatar", conversation=g)
                await gui.wait(
                    "group_profile",
                    lambda r: "groupImage" not in r and r.get("userImage") == "#ff0000",
                    group=g,
                )
                assert (await gui.call("pin"))["announcement"] == "独立公告"
                await b.ok("leave_group", conversation=g)
                assert "error" in await b.ask(
                    "get_avatar", conversation=g, revision=1, offset=0
                )
                await gui.wait("group_profile", lambda r: r["members"] == [])
            finally:
                await gui.stop()
        print(
            "PASS real Qt image chooser/SDK upload, two cache namespaces, roles/demotion, description and announcement, search, live membership"
        )


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1]))
