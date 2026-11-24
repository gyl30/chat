"""Exercise group polls through Qt, concurrent WS clients and restored archive data."""

import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import sqlite3
import sys
import tempfile

from desktop import Desktop, Gate, until
from websockets.asyncio.server import serve
from isolated import chat, login
from recovery import admin


async def run(root, probe):
    env = dict(
        CHAT_S3_ENDPOINT="127.0.0.1:1",
        CHAT_S3_BUCKET="isolated",
        CHAT_S3_ACCESS_KEY="test",
        CHAT_S3_SECRET_KEY="isolated",
        CHAT_S3_HTTPS="0",
    )
    db = root / "source.sqlite"
    with chat(db, env) as url:
        async with AsyncExitStack() as stack:
            a, b, c = [await login(stack, url, k, k, True) for k in "abc"]
            for uid, peer in [(2, b), (3, c)]:
                await a.ok("send_friend_request", user=uid)
                await peer.ok("respond_friend_request", user=1, accept=True)
            d = (await a.ok("open_direct_conversation", user=2))["conversation"]
            g = (await a.ok("create_group", title="投票群", members=[2, 3]))[
                "conversation"
            ]
            for room, question, options in [
                (d, "?", ["A", "B"]),
                (g, " ", ["A", "B"]),
                (g, "?", ["A"]),
                (g, "?", ["", "B"]),
                (g, "?", ["A", "A"]),
                (g, "?", ["猫" * 81, "B"]),
            ]:
                assert "error" in await a.ask(
                    "send_message",
                    conversation=room,
                    text=question,
                    poll_options=options,
                    client_id=1,
                )
            ui = Desktop()
            gate = Gate(url)
            proxy = await stack.enter_async_context(serve(gate.handle, "127.0.0.1", 0))
            await ui.start(
                probe, root, f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            )
            stack.push_async_callback(ui.stop)
            await ui.wait("select", lambda r: r["ok"], conv=g)
            await ui.call("poll_ui", create=True)
            await ui.wait("poll_ui", lambda r: r.get("title") == "发起群投票")
            await ui.call("poll_ui", question="开发会议", options="周一\n周三\n周五")

            def placed():
                with sqlite3.connect(db) as conn:
                    return conn.execute(
                        "SELECT id,client_id FROM messages WHERE content='poll'"
                    ).fetchone()

            mid, cid = await until(placed, "poll created by Qt")
            args = dict(conversation=g, message=mid)
            replay = await a.ok(
                "send_message",
                conversation=g,
                client_id=cid,
                text="开发会议",
                poll_options=["周一", "周三", "周五"],
            )
            assert replay["id"] == mid and replay["poll"]["revision"] == 1
            assert "error" in await a.ask(
                "send_message", conversation=d, forward=mid, client_id=77
            )
            assert "error" in await a.ask("edit_message", text="新问题", **args)
            separate = await c.ok(
                "send_message",
                conversation=g,
                text="成员投票",
                poll_options=["A", "B"],
                client_id=100,
            )
            assert (await c.ok("close_poll", conversation=g, message=separate["id"]))[
                "summary"
            ]["closed"]
            assert "error" in await b.ask("close_poll", **args)
            assert "error" in await b.ask("vote_poll", choice=4, **args)
            await ui.call("poll_ui", open=mid)
            await ui.wait("poll_ui", lambda r: len(r.get("rows", [])) == 3)
            await ui.call("poll_ui", choose=2, action="提交投票")
            await ui.wait(
                "poll_ui",
                lambda r: r.get("rows", ["", ""])[1].startswith("●")
                and r["members"] == ["a"],
            )
            await a.ok("set_group_member_mute", conversation=g, user=2, seconds=-1)
            assert "error" in await b.ask(
                "send_message",
                conversation=g,
                text="?",
                poll_options=["A", "B"],
                client_id=321,
            )
            await asyncio.gather(
                b.ok("vote_poll", choice=1, **args), c.ok("vote_poll", choice=3, **args)
            )
            state = await b.ok("get_poll", **args)
            assert [o["votes"] for o in state["summary"]["options"]] == [
                1,
                1,
                1,
            ] and state["selection"] == 1
            await ui.wait(
                "poll_ui",
                lambda r: all("1 票" in row for row in r.get("rows", []))
                and len(r.get("rows", [])) == 3,
            )
            same = await b.ok("vote_poll", choice=1, **args)
            assert same == state
            await a.ok("vote_poll", choice=1, **args)
            await ui.call("poll_ui", choose=1)
            await ui.wait(
                "poll_ui",
                lambda r: r.get("rows", [""])[0].startswith("●")
                and set(r["members"]) == {"a", "b"},
            )
            await ui.call("poll_ui", action="取消我的投票")
            await ui.wait("poll_ui", lambda r: r.get("members") == ["b"])
            await ui.call("poll_ui", close=True)
            # Miss a vote entirely, then release the real authentication on reconnection.
            gate.hold_requests.add("resume_session")
            await gate.down.close()
            await until(
                lambda: any(
                    req["method"] == "resume_session" for _, req in gate.held_requests
                ),
                "reconnecting",
            )
            await c.ok("vote_poll", choice=1, **args)
            await gate.release_requests("resume_session")
            await ui.wait(
                "state",
                lambda r: any(
                    x["id"] == mid and "周一  ·  2 票" in x.get("poll", "")
                    for x in r["rows"]
                ),
                polls=True,
            )
            await b.ok("leave_group", conversation=g)
            assert "error" in await b.ask("get_poll", **args)
            await a.ok("invite_group_members", conversation=g, members=[2])
            assert "error" in await b.ask("vote_poll", choice=2, **args)
            voters = await a.ok("get_poll_voters", choice=1, after=0, **args)
            assert [v["username"] for v in voters["members"]] == ["b", "c"]
            await a.ok("set_group_admin", conversation=g, user=3, admin=True)
            bck, closed = await asyncio.gather(
                a.ask("vote_poll", choice=3, **args), c.ok("close_poll", **args)
            )
            assert closed["summary"]["closed"]
            assert "error" in await a.ask("vote_poll", choice=0, **args)
            assert (await c.ok("close_poll", **args))["summary"] == closed["summary"]
            assert mid in [
                r["id"]
                for r in (await c.ok("search_all_messages", query="开发会议"))[
                    "messages"
                ]
            ]
            await c.ok("set_message_bookmark", message=mid, bookmarked=True)
            await c.ok("clear_history", conversation=g, through=mid)
            assert "error" in await c.ask("get_poll", **args)
            with sqlite3.connect(db) as conn:
                final = conn.execute(
                    "SELECT count(*) FROM poll_votes WHERE message=?", (mid,)
                ).fetchone()[0]
                assert final == sum(o["votes"] for o in closed["summary"]["options"])
                conn.execute(
                    "UPDATE messages SET created_at=1736942400000 WHERE id=?", (mid,)
                )
            admin("--db", db, "archive", "2025-01")
    admin("--db", db, "backup", root / "backup")
    admin("restore", root / "backup", root / "restored")
    restored = root / "restored/chat.sqlite3"
    with chat(restored, env) as url:
        async with AsyncExitStack() as stack:
            a, c = [await login(stack, url, k, k) for k in "ac"]
            assert (await a.ok("get_poll", **args))["summary"] == closed["summary"]
            assert (await a.ok("get_messages", conversation=g, around=mid))["messages"][
                0
            ]["poll"] == closed["summary"]
            assert "error" in await c.ask("get_poll", **args)
            await a.ok("delete_message", **args)
            assert "error" in await a.ask("get_poll", **args)
            with sqlite3.connect(restored) as conn:
                assert not conn.execute("SELECT user FROM poll_votes").fetchall()
    print(
        "PASS Qt poll composer, vote, cancel and live member counts; real SDK/RPC roles, idempotency, close race, watermarks, archive and restored restart"
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-polls-") as tmp:
        asyncio.run(run(Path(tmp), sys.argv[1]))
