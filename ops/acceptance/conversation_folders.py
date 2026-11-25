"""Personal folder ownership, ordering and synchronized Qt sidebar on an isolated server."""

import asyncio
from pathlib import Path
import sys
import tempfile

from desktop import Desktop
from fixture import fixture


async def main(probe):
    async with fixture() as (db, login, a, b, direct, pid):
        other = await login("a", "c")
        room = (await a.ok("create_group", title="研发讨论", members=[2]))[
            "conversation"
        ]
        with tempfile.TemporaryDirectory() as home:
            gui = Desktop()
            await gui.start(
                probe, Path(home), f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws"
            )
            try:
                await a.ok("create_conversation_folder", name="工作")
                await a.ok("create_conversation_folder", name="朋友")
                folders = (await other.ok("get_conversations"))["folders"]
                work, friends = [f["id"] for f in folders]
                assert [f["name"] for f in folders] == ["工作", "朋友"]
                await a.ok(
                    "move_conversation_to_folder", conversation=room, folder=work
                )
                await a.ok(
                    "move_conversation_to_folder", conversation=direct, folder=friends
                )
                await gui.wait(
                    "filter",
                    lambda r: r["names"] == ["工作", "研发讨论", "朋友", "b", "未分组"],
                )
                for method, params in [
                    ("delete_conversation_folder", {"folder": work}),
                    (
                        "move_conversation_to_folder",
                        {"conversation": direct, "folder": work},
                    ),
                ]:
                    assert "error" in await b.ask(method, **params)
                assert not (await b.ok("get_conversations"))["folders"]
                await a.ok("reorder_conversation_folder", folder=friends, up=True)
                await a.ok("rename_conversation_folder", folder=work, name="项目")
                await gui.wait(
                    "filter",
                    lambda r: r["names"] == ["朋友", "b", "项目", "研发讨论", "未分组"],
                )
                await gui.call("toggle_folder", folder=work)
                await gui.wait("filter", lambda r: "研发讨论" not in r["names"])
                assert (await gui.call("filter", query="研发"))["names"] == ["研发讨论"]
                assert "研发讨论" not in (await gui.call("filter", query=""))["names"]
                await a.ok("hide_conversation", conversation=room)
                assert room not in [
                    c["id"] for c in (await a.ok("get_conversations"))["conversations"]
                ]
                directory = (await a.ok("get_conversations", include_hidden=True))["conversations"]
                hidden = next(c for c in directory if c["id"] == room)
                assert hidden["hidden"] and hidden["folder"] == work and hidden["speech_role"] == 2
                await b.ok(
                    "send_message", conversation=room, text="重现", client_id=811
                )
                visible = (await a.ok("get_conversations"))["conversations"]
                assert next(c for c in visible if c["id"] == room)["folder"] == work
                await a.ok("delete_conversation_folder", folder=work)
                await gui.wait(
                    "filter",
                    lambda r: r["names"] == ["朋友", "b", "未分组", "研发讨论"],
                )
                another = (await b.ok("create_group", title="成员变更", members=[1]))[
                    "conversation"
                ]
                await a.ok(
                    "move_conversation_to_folder", conversation=another, folder=friends
                )
                await a.ok("set_conversation_pinned", conversation=direct, pinned=True)
                await b.ok(
                    "send_message", conversation=another, text="最新活动", client_id=812
                )
                await gui.wait(
                    "filter",
                    lambda s: (
                        s["names"].index("b") < s["names"].index("成员变更")
                        if "成员变更" in s["names"]
                        else False
                    ),
                )
                for method in ("leave_group", "remove_group_member"):
                    if method == "leave_group":
                        await a.ok(method, conversation=another)
                    else:
                        await b.ok(method, conversation=another, user=1)
                    assert "error" in await a.ask(
                        "move_conversation_to_folder",
                        conversation=another,
                        folder=friends,
                    )
                    await b.ok(
                        "invite_group_members", conversation=another, members=[1]
                    )
                    snapshot = (await a.ok("get_conversations"))["conversations"]
                    assert (
                        next(c for c in snapshot if c["id"] == another)["folder"] == 0
                    )
                    await a.ok(
                        "move_conversation_to_folder",
                        conversation=another,
                        folder=friends,
                    )
                await b.ok("dissolve_group", conversation=another)
                assert another not in [
                    c["id"] for c in (await a.ok("get_conversations"))["conversations"]
                ]
                assert any(e.get("method") == "conversation_view" for e in other.events)
                # Test the SDK's multi-page snapshot after restart and live ordering inside the same folder.
                await gui.stop()
                for i in range(51):
                    await b.ok("create_group", title=f"分页{i}", members=[1])
                assert (await a.ok("get_conversations"))["next"]
                await gui.start(
                    probe, Path(home), f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws"
                )
                await gui.wait(
                    "filter",
                    lambda r: len(r["names"]) == 55
                    and r["names"][:3] == ["朋友", "b", "未分组"],
                )
                await b.ok(
                    "send_message", conversation=room, text="跨页更新", client_id=813
                )
                await gui.wait("filter", lambda r: r["names"][3:4] == ["研发讨论"])
                messages = (await a.ok("get_messages", conversation=room))["messages"]
                assert [m["text"] for m in messages] == ["重现", "跨页更新"]
            finally:
                await gui.stop()
        print(
            "PASS conversation folder RPC, isolation, reorder, hidden membership, Qt synchronization and collapse/search"
        )


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1]))
