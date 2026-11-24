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
            return await client.ok(
                "set_user_profile", nickname=name, signature=signature
            )

        await edit(b, "猫" * 32, "🌏" * 128)
        assert (await a.ok("get_user_profile", user=2))["signature"] == "🌏" * 128
        assert (await stranger.ok("get_user_profile", user=2))["signature"] == ""
        for name, sig in [("🌏" * 33, ""), ("", "文" * 129)]:
            assert "error" in await b.ask(
                "set_user_profile", nickname=name, signature=sig
            )
        await edit(b, "小猫", "晚安")
        await edit(stranger, "小猫", "无关")
        assert {
            u["username"] for u in (await a.ok("search_users", query="猫"))["users"]
        } == {"b", "c"}
        assert (await other.ok("get_user_profile", user=2))["username"] == "b"
        assert any(e.get("method") == "profile_changed" for e in other.events)
        with tempfile.TemporaryDirectory() as home:
            gui = Desktop()
            await gui.start(
                probe, Path(home), f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws"
            )
            try:
                await gui.wait("filter", lambda r: r["names"] == ["小猫"])
                await gui.call("contacts", open=True)
                await gui.wait("contacts", lambda r: r["names"] == ["小猫"])
                await gui.call("contacts", profile=True)
                await gui.wait("contacts", lambda r: r.get("profile") == "小猫")
                await edit(b, "新昵称", "再见")
                await gui.wait("contacts", lambda r: r.get("profile") == "新昵称")
                await gui.call("contacts", closeProfile=True, close=True)
                await a.ok("set_friend_remark", user=2, remark="私人")
                await gui.wait("filter", lambda r: r["names"] == ["私人"])
                msg = await b.ok(
                    "send_message", conversation=dm, text="测试", client_id=32
                )
                results = (await a.ok("search_all_messages", query="测试"))["messages"]
                assert (
                    results[0]["id"] == msg["id"] and results[0]["username"] == "私人"
                )
                assert (await b.ok("search_all_messages", query="测试"))["messages"][0][
                    "username"
                ] == "新昵称"
                await gui.call("select", conv=dm)
                reply = await a.ok(
                    "send_message",
                    conversation=dm,
                    text="引用",
                    reply_to=msg["id"],
                    client_id=33,
                )
                forward = await a.ok(
                    "send_message", conversation=dm, forward=msg["id"], client_id=34
                )

                def names(state, expected):
                    rows = {m["id"]: m for m in state["rows"]}
                    return (
                        rows.get(msg["id"], {}).get("author") == expected
                        and expected in rows.get(reply["id"], {}).get("quote", "")
                        and rows.get(forward["id"], {}).get("forward") == expected
                    )

                await gui.wait("state", lambda s: names(s, "私人"), authors=True)
                await a.ok("set_friend_remark", user=2, remark="")
                await gui.wait("state", lambda s: names(s, "新昵称"), authors=True)
                await edit(b, "新名字", "仍在线")
                await gui.wait("state", lambda s: names(s, "新名字"), authors=True)
                await gui.call("contacts", selfProfile=True)
                await gui.wait("contacts", lambda s: s.get("canEdit"))
                await gui.call("contacts", nickname="本人", signature="由 Qt 保存")
                await gui.wait("contacts", lambda s: s.get("profile") == "本人")
                assert (await other.ok("get_user_profile", user=1))[
                    "signature"
                ] == "由 Qt 保存"
                await gui.call("contacts", closeProfile=True)
                group = (await a.ok("create_group", title="群提及", members=[2]))[
                    "conversation"
                ]
                await gui.wait("filter", lambda s: "群提及" in s["names"])
                await gui.call("select", conv=group)
                label = "新名字 (@b)"
                await gui.wait("mention", lambda s: label in s["labels"])
                await gui.call("mention", choose=label)
                assert (await gui.call("state"))["draft"] == "@b "
                await gui.call("send")
                from desktop import until

                async def mentioned():
                    rows = (await b.ok("get_messages", conversation=group))["messages"]
                    return any(
                        m["text"] == "@b "
                        and any(v["user"] == 2 for v in m["mentions"])
                        for m in rows
                    )

                await until(mentioned, "structured mention keeps login and user id")
                await gui.call("contacts", open=True)
                await gui.wait("contacts", lambda s: "新名字" in s["names"])
                await gui.call("contacts", profile=True)
                await gui.wait("contacts", lambda s: s.get("signature") == "仍在线")
                await a.ok("remove_contact", user=2)
                assert (await a.ok("get_user_profile", user=2))["signature"] == "仍在线"
                await gui.wait(
                    "contacts",
                    lambda s: s.get("profile") == "新名字"
                    and not s["friend"]
                    and s["signature"] == "仍在线",
                )
                await b.ok("leave_group", conversation=group)
                assert (await a.ok("get_user_profile", user=2))["signature"] == ""
                await gui.wait("contacts", lambda s: s.get("signature") == "")
                await gui.call("contacts", closeProfile=True)
                await edit(b, "", "")
                assert (await other.ok("get_user_profile", user=2)) == dict(
                    id=2, username="b", nickname="", signature=""
                )
            finally:
                await gui.stop()
        print(
            "PASS nickname/signature bounds, privacy, duplicate names, login identity, own-device sync, Qt live profile, private search names"
        )


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1]))
