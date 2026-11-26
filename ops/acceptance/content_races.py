"""Hold real WebSocket responses until the Qt event under test has been processed."""

import asyncio
from pathlib import Path
import sys
import sqlite3
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from fixture import fixture


async def scenario(probe, mode):
    with tempfile.TemporaryDirectory(prefix="chat-content-race-") as tmp:
        async with fixture() as (db, _, a, b, direct, _):
            g = (await a.ok("create_group", title="竞态", members=[2]))["conversation"]

            async def create(cid):
                return (
                    await b.ok(
                        "send_message",
                        conversation=g,
                        text="问题",
                        client_id=cid,
                        poll_options=["A", "B"],
                    )
                )["id"]

            mid = await create(1)
            args = dict(conversation=g, message=mid)
            other = (
                await b.ok("send_message", conversation=g, text="来源", client_id=2)
            )["id"]
            if mode == "older":
                for n in range(55):
                    await a.ok(
                        "send_message",
                        conversation=g,
                        text=f"历史 {n}",
                        client_id=100 + n,
                    )
            if mode == "bundle":
                second = (
                    await b.ok(
                        "send_message", conversation=g, text="第二条", client_id=3
                    )
                )["id"]
                bundle = (
                    await a.ok(
                        "send_message",
                        conversation=g,
                        bundle=[other, second],
                        client_id=4,
                    )
                )["id"]
            if mode == "paging":
                # The 50 fixture voters precede the UI login. Exercise all subsequent changes
                # against the running server, including cancel and both actual page queries.
                with sqlite3.connect(db) as conn:
                    conn.execute("PRAGMA foreign_keys=ON")
                    for n in range(50):
                        uid = conn.execute(
                            "INSERT INTO users(username,password_hash,last_seen_at) SELECT ?,password_hash,0 FROM users WHERE id=1",
                            (f"v{n}",),
                        ).lastrowid
                        conn.execute(
                            "INSERT INTO conversation_members(conversation_id,user_id) VALUES(?,?)",
                            (g, uid),
                        )
                        conn.execute(
                            "INSERT INTO poll_votes(message,user,choice) VALUES(?,?,1)",
                            (mid, uid),
                        )
                await b.ok("vote_poll", choice=1, **args)
            gate = Gate(f"ws://{a.ws.remote_address[0]}:{a.ws.remote_address[1]}/ws")
            ui = Desktop()
            async with serve(gate.handle, "127.0.0.1", 0) as proxy:
                await ui.start(
                    probe,
                    Path(tmp),
                    f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws",
                )
                try:
                    await ui.wait("select", lambda r: r["ok"], conv=direct)
                    await ui.wait("reading", lambda r: r["ready"])
                    history = mode in ("first", "withdraw", "clear", "switch", "leave")
                    if history:
                        gate.hold.add("get_messages")
                    await ui.wait("select", lambda r: r["ok"], conv=g)
                    if history:
                        await gate.caught("get_messages")
                    else:
                        await ui.wait("reading", lambda r: r["ready"])

                    async def count(key):
                        return (await ui.call("deliveries")).get(key, 0)

                    async def seen(key, n):
                        await ui.wait("deliveries", lambda r: r.get(key, 0) > n)

                    if mode == "older":
                        gate.hold.add("get_messages")
                        await ui.call("reading", older=True)
                        await gate.caught("get_messages")
                    if mode == "loaded":
                        gate.hold.add("get_messages")
                        await a.ok(
                            "set_group_admin", conversation=g, user=2, admin=True
                        )
                        await gate.caught("get_messages")
                    if mode in ("first", "older", "loaded", "new"):
                        if mode == "new":
                            gate.hold.add("message")
                            mid = await create(22)
                            args["message"] = mid
                            await gate.caught("message")
                        n = await count("update")
                        await b.ok("vote_poll", choice=1, **args)
                        await a.ok("vote_poll", choice=1, **args)
                        await b.ok("close_poll", **args)
                        await seen("update", n + 2)
                        history_count = await count("history")
                        await gate.release(
                            "message" if mode == "new" else "get_messages"
                        )

                        if mode != "new":
                            await seen("history", history_count)

                        def correct(r):
                            return any(
                                x["id"] == mid
                                and {"label": "A", "votes": 2} in x["poll"]
                                and x["pollClosed"]
                                for x in r["rows"]
                            )

                        await ui.wait("state", correct, polls=True)
                    elif mode == "newwithdraw":
                        gate.hold.add("message")
                        mid = await create(22)
                        await gate.caught("message")
                        n = await count("edit")
                        await b.ok("delete_message", conversation=g, message=mid)
                        await seen("edit", n)
                        gate.hold.add("get_messages")
                        n = await count("new")
                        await gate.release("message")
                        await seen("new", n)
                        row = next(
                            x
                            for x in (await ui.call("state", polls=True))["rows"]
                            if x["id"] == mid
                        )
                        assert (
                            row["text"] == "消息已撤回" and not row["poll"]
                        ), "late creation redisplayed a withdrawn poll"
                    elif mode == "withdraw":
                        n = await count("edit")
                        await b.ok("delete_message", **args)
                        await seen("edit", n)
                        await gate.release("get_messages")
                        await ui.wait(
                            "state",
                            lambda r: any(
                                x["id"] == mid
                                and x["text"] == "消息已撤回"
                                and not x["poll"]
                                for x in r["rows"]
                            ),
                            polls=True,
                        )
                    elif mode in ("clear", "switch", "leave"):
                        if mode == "clear":
                            await a.ok("clear_history", conversation=g, through=other)
                        elif mode == "leave":
                            await a.ok("dissolve_group", conversation=g)
                        else:
                            await ui.call("select", conv=direct)
                        if mode == "clear":
                            await ui.wait("state", lambda r: r.get("floor", 0) >= other)
                        else:
                            await ui.wait("state", lambda r: r["conv"] != g)
                        await gate.release("get_messages")
                        # A later real event is a barrier even when the old reply was discarded.
                        n = await count("preference")
                        await a.ok(
                            "set_notification_level",
                            conversation=direct,
                            level=1,
                            until=0,
                        )
                        await seen("preference", n)
                        state = await ui.call("state")
                        assert not any(x["id"] == mid for x in state["rows"])
                    elif mode == "voters":
                        gate.hold.update(("get_poll_voters", "poll_updated"))
                        await ui.call("poll_ui", open=mid)
                        await gate.caught("get_poll_voters")
                        gate.hold.discard(
                            "get_poll_voters"
                        )  # Retain the captured empty option A page.
                        await ui.call("poll_ui", view=2)
                        await b.ok("vote_poll", choice=1, **args)
                        await ui.call("poll_ui", view=1)
                        await ui.wait("poll_ui", lambda r: r.get("members") == ["b"])
                        n = await count("voters")
                        await gate.release("get_poll_voters")
                        await seen("voters", n)
                        assert (await ui.call("poll_ui"))["members"] == [
                            "b"
                        ], "obsolete option A overwrote current voters"
                    elif mode in ("authority", "poll_error"):
                        gate.hold.add("get_poll")
                        await ui.call("poll_ui", open=mid)
                        await gate.caught("get_poll")
                        gate.hold.clear()
                        await ui.call("poll_ui", close=True)
                        await ui.call("poll_ui", open=mid)
                        await ui.wait("poll_ui", lambda r: len(r.get("rows", [])) == 2)
                        if mode == "authority":
                            # Same revision; only the authority to end this poll changes.
                            # The Qt user is the owner, so transfer ownership and then remove admin rights.
                            await a.ok(
                                "set_group_admin", conversation=g, user=2, admin=True
                            )
                            await a.ok("transfer_group_owner", conversation=g, user=2)
                            await b.ok(
                                "set_group_admin", conversation=g, user=1, admin=False
                            )
                            await ui.wait(
                                "poll_ui", lambda r: r.get("canClose") == False
                            )
                        n = await count("poll")
                        await gate.release("get_poll", error=mode == "poll_error")
                        await seen("poll", n)
                        state = await ui.call("poll_ui")
                        if mode == "authority":
                            assert not state["canClose"], "old role restored"
                        else:
                            assert (
                                "QA search failure" not in state["labels"]
                                and state["canClose"]
                            ), state
                    elif mode == "write":
                        await ui.call("poll_ui", open=mid)
                        await ui.wait("poll_ui", lambda r: len(r.get("rows", [])) == 2)
                        gate.hold_requests.add("vote_poll")
                        gate.hold.add("poll_updated")
                        await ui.call("poll_ui", action="提交投票")
                        await until(
                            lambda: bool(gate.held_requests), "vote awaiting dispatch"
                        )
                        await ui.call("poll_ui", close=True)
                        await ui.call("poll_ui", open=mid)
                        await ui.wait(
                            "poll_ui", lambda r: "共 0 票" in r.get("labels", "")
                        )
                        await gate.release_requests("vote_poll")
                        await ui.wait(
                            "poll_ui",
                            lambda r: "共 1 票" in r.get("labels", "")
                            and r.get("members") == ["a"],
                        )
                        assert (await a.ok("get_poll", **args))["selection"] == 1
                    elif mode == "closed":
                        gate.hold.add("get_poll")
                        await ui.call("poll_ui", open=mid)
                        await gate.caught("get_poll")
                        gate.hold.discard("get_poll")
                        await b.ok("close_poll", **args)
                        await ui.wait(
                            "poll_ui", lambda r: "投票已结束" in r.get("labels", "")
                        )
                        n = await count("poll")
                        await gate.release("get_poll")
                        await seen("poll", n)
                        detail = await ui.call("poll_ui")
                        assert (
                            "投票已结束" in detail["labels"] and not detail["canClose"]
                        )
                    elif mode == "paging":
                        await ui.call("poll_ui", open=mid)
                        await ui.wait(
                            "poll_ui",
                            lambda r: len(r.get("members", [])) == 50 and r.get("more"),
                        )
                        gate.hold.add("get_poll_voters")
                        await ui.call("poll_ui", action="更多投票成员")
                        await gate.caught("get_poll_voters")
                        gate.hold.clear()
                        await b.ok("vote_poll", choice=0, **args)
                        state = await ui.wait(
                            "poll_ui",
                            lambda r: len(r.get("members", [])) == 50
                            and not r.get("more")
                            and "b" not in r["members"],
                        )
                        n = await count("voters")
                        await gate.release("get_poll_voters")
                        await seen("voters", n)
                        assert (await ui.call("poll_ui"))["members"] == state["members"]
                    elif mode == "context":
                        await ui.call("searchui", open=True)
                        await ui.wait("searchui", lambda r: r.get("open"))
                        await ui.call("searchui", query="问题")
                        await ui.wait("searchui", lambda r: r.get("ids") == [mid])
                        gate.hold.add("get_messages")
                        await ui.call("searchui", row=0, enter=True)
                        await gate.caught("get_messages")
                        n = await count("update")
                        await b.ok("vote_poll", choice=2, **args)
                        await seen("update", n)
                        await gate.release("get_messages")
                        await ui.wait(
                            "searchui",
                            lambda r: r.get("ctx") and mid in r.get("contextIds", []) and r.get("pollRevision") == 2,
                            inspect=mid,
                        )
                        gate.hold.add("get_messages")
                        await ui.call("searchui", back=True)
                        await ui.call("searchui", row=0, enter=True)
                        await gate.caught("get_messages")
                        n = await count("edit")
                        await b.ok("delete_message", **args)
                        await seen("edit", n)
                        await gate.release("get_messages")
                        assert mid not in (await ui.call("searchui")).get(
                            "contextIds", []
                        )
                    elif mode == "bundle":
                        gate.hold.add("get_forwarded_messages")
                        await ui.call("bundle_view", open=bundle)
                        await gate.caught("get_forwarded_messages")
                        gate.hold.clear()
                        await ui.call("bundle_view", close=True)
                        await ui.call("bundle_view", open=bundle)
                        await ui.wait("bundle_view", lambda r: r.get("count") == 2)
                        n = await count("bundle")
                        await gate.release("get_forwarded_messages")
                        await seen("bundle", n)
                        assert (await ui.call("bundle_view"))[
                            "count"
                        ] == 2, "reopened bundle appended stale page"
                    print("PASS Qt Gate", mode, flush=True)
                finally:
                    await ui.stop()


async def run():
    for mode in sys.argv[2:] or (
        "first",
        "older",
        "loaded",
        "new",
        "newwithdraw",
        "withdraw",
        "switch",
        "clear",
        "leave",
        "voters",
        "paging",
        "poll_error",
        "closed",
        "write",
        "authority",
        "context",
        "bundle",
    ):
        await scenario(sys.argv[1], mode)


if __name__ == "__main__":
    asyncio.run(run())
