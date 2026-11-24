"""Public nicknames, restricted signatures and viewer-specific remarks, including a live Qt profile."""
import asyncio
from pathlib import Path
import sys
import tempfile
from desktop import Desktop
from fixture import fixture


async def main(probe):
    async with fixture() as (_, login, a, b, dm, _pid):
        stranger = await login("c", "c", True)
        other = await login("b", "d")
        async def edit(client, name, signature):
            return await client.ok("set_user_profile", nickname=name, signature=signature)
        await edit(b, "猫"*32, "🌏"*128)
        assert (await a.ok("get_user_profile",user=2))["signature"] == "🌏"*128
        assert (await stranger.ok("get_user_profile",user=2))["signature"] == ""
        for name,sig in [("🌏"*33,""),("","文"*129)]:
            assert "error" in await b.ask("set_user_profile",nickname=name,signature=sig)
        await edit(b,"小猫","晚安")
        await edit(stranger,"小猫","无关")
        assert {u["username"] for u in (await a.ok("search_users",query="猫"))["users"]} == {"b","c"}
        assert (await other.ok("get_user_profile", user=2))["username"]=="b"
        assert any(e.get("method")=="profile_changed" for e in other.events)
        with tempfile.TemporaryDirectory() as home:
            gui=Desktop()
            await gui.start(probe,Path(home),f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws")
            try:
                await gui.wait("filter",lambda r: r["names"]==["小猫"])
                await gui.call("contacts",open=True)
                await gui.wait("contacts", lambda r:r["names"]==["小猫"])
                await gui.call("contacts",profile=True)
                await gui.wait("contacts",lambda r: r.get("profile")=="小猫")
                await edit(b,"新昵称","再见")
                await gui.wait("contacts",lambda r:r.get("profile")=="新昵称")
                await gui.call("contacts",closeProfile=True,close=True)
                await a.ok("set_friend_remark",user=2,remark="私人")
                await gui.wait("filter",lambda r:r["names"]==["私人"])
                msg=await b.ok("send_message",conversation=dm,text="测试",client_id=32)
                results=(await a.ok("search_all_messages",query="测试"))["messages"]
                assert results[0]["id"]==msg["id"] and results[0]["username"]=="私人"
                assert (await b.ok("search_all_messages",query="测试"))["messages"][0]["username"]=="新昵称"
                await edit(b,"","")
                assert (await other.ok("get_user_profile",user=2))==dict(id=2,username="b",nickname="",signature="")
            finally:
                await gui.stop()
        print("PASS nickname/signature bounds, privacy, duplicate names, login identity, own-device sync, Qt live profile, private search names")


if __name__=="__main__":
    asyncio.run(main(sys.argv[1]))
