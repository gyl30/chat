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
        room = (await a.ok("create_group", title="研发讨论", members=[2]))["conversation"]
        with tempfile.TemporaryDirectory() as home:
            gui = Desktop()
            await gui.start(probe, Path(home),
                            f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws")
            try:
                await a.ok("create_conversation_folder", name="工作")
                await a.ok("create_conversation_folder", name="朋友")
                folders = (await other.ok("get_conversations"))["folders"]
                work, friends = [f["id"] for f in folders]
                assert [f["name"] for f in folders] == ["工作", "朋友"]
                await a.ok("move_conversation_to_folder", conversation=room, folder=work)
                await a.ok("move_conversation_to_folder", conversation=direct, folder=friends)
                await gui.wait("filter", lambda r: r["names"] == ["工作", "研发讨论", "朋友", "b", "未分组"])
                for method, params in [
                    ("delete_conversation_folder", {"folder": work}),
                    ("move_conversation_to_folder", {"conversation": direct, "folder": work}),
                ]:
                    assert "error" in await b.ask(method, **params)
                assert not (await b.ok("get_conversations"))["folders"]
                await a.ok("reorder_conversation_folder", folder=friends, up=True)
                await a.ok("rename_conversation_folder", folder=work, name="项目")
                await gui.wait("filter", lambda r: r["names"] == ["朋友", "b", "项目", "研发讨论", "未分组"])
                await gui.call("toggle_folder", folder=work)
                await gui.wait("filter", lambda r: "研发讨论" not in r["names"])
                assert (await gui.call("filter", query="研发"))["names"] == ["研发讨论"]
                assert "研发讨论" not in (await gui.call("filter", query=""))["names"]
                await a.ok("hide_conversation", conversation=room)
                assert room not in [c["id"] for c in (await a.ok("get_conversations"))["conversations"]]
                await b.ok("send_message", conversation=room, text="重现", client_id=811)
                visible = (await a.ok("get_conversations"))["conversations"]
                assert next(c for c in visible if c["id"] == room)["folder"] == work
                await a.ok("delete_conversation_folder", folder=work)
                await gui.wait("filter", lambda r: r["names"] == ["朋友", "b", "未分组", "研发讨论"])
                assert any(e.get("method") == "conversation_view" for e in other.events)
            finally:
                await gui.stop()
        print("PASS conversation folder RPC, isolation, reorder, hidden membership, Qt synchronization and collapse/search")


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1]))
