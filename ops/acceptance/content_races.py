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
                        poll_settings=dict(
                            anonymous=False, multiple=False, correct=0, explanation=""
                        ),
                    )
                )["id"]

            if mode == "page_position":
                for i in range(8):
                    await b.ok(
                        "send_message",
                        conversation=g,
                        client_id=800 + i,
                        text="历史消息与长行换行 " * 45,
                    )
            mid = await create(1)
            args = dict(conversation=g, message=mid)
            other = (
                await b.ok("send_message", conversation=g, text="来源", client_id=2)
            )["id"]
            if mode == "page_position":
                await a.ok("vote_poll", choices=1, **args)
                for i in range(8):
                    await b.ok(
                        "send_message",
                        conversation=g,
                        client_id=900 + i,
                        text="更新消息 " * 50,
                    )
            if mode in ("unloaded_edit", "unloaded_reaction"):
                for n in range(12):
                    await b.ok(
                        "send_message",
                        conversation=g,
                        text="在途历史记录 " * 60,
                        client_id=730 + n,
                    )
            if mode == "timeline":
                for n in range(120):
                    tail = (
                        await b.ok(
                            "send_message",
                            conversation=g,
                            text=f"{n}: " + "历史行 " * 60,
                            client_id=300 + n,
                        )
                    )["id"]
                await a.ok("mark_read", conversation=g, message=tail)
            if mode == "older":
                await a.ok("mark_read", conversation=g, message=other)
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
            if mode.startswith("detail_") or mode in (
                "authority",
                "poll_error",
                "closed",
                "voters",
                "paging",
            ):
                await a.ok("vote_poll", choices=2, **args)
            if mode in ("authority", "poll_error", "closed"):
                await b.ok("vote_poll", choices=1, **args)
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
                            "INSERT INTO poll_votes(message,user,choices,voted_revision) VALUES(?,?,1,1)",
                            (mid, uid),
                        )
                await b.ok("vote_poll", choices=1, **args)
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
                    history = mode in (
                        "first",
                        "unloaded_edit",
                        "unloaded_reaction",
                        "withdraw",
                        "clear",
                        "switch",
                        "leave",
                    )
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
                        await b.ok("vote_poll", choices=1, **args)
                        await a.ok("vote_poll", choices=1, **args)
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
                    elif mode in ("unloaded_edit", "unloaded_reaction"):
                        requests = sum(
                            p["method"] == "get_messages" for p in gate.messages
                        )
                        edits = await count("edit")
                        await b.ok(
                            "edit_message",
                            conversation=g,
                            message=other,
                            text="请求途中更新的正文",
                        )
                        await seen("edit", edits)
                        if mode == "unloaded_reaction":
                            changes = await count("reaction")
                            await a.ok(
                                "set_message_reaction",
                                conversation=g,
                                message=other,
                                emoji="👍",
                            )
                            await seen("reaction", changes)
                        await gate.release("get_messages")
                        await ui.wait("reading", lambda r: r["ready"])
                        row = next(
                            r
                            for r in (await ui.call("state", reactions=True))["rows"]
                            if r["id"] == other
                        )
                        assert row["text"] == "请求途中更新的正文", row
                        assert row["reactionGroups"] == int(
                            mode == "unloaded_reaction"
                        ), row
                        assert (
                            sum(p["method"] == "get_messages" for p in gate.messages)
                            == requests
                        ), "unexpected replacement history request"
                    elif mode == "timeline":
                        await ui.call("reading", latest=True, activate=True)
                        gate.hold_requests.add("send_message")
                        await ui.call("send", text="等待确认的文本")
                        await until(lambda: gate.held_requests, "queued text")
                        await ui.call("reading", jumpReply=other)
                        historical = await ui.wait(
                            "reading", lambda r: r["ready"] and r["last"] < tail
                        )
                        assert not any(
                            not row["id"] for row in (await ui.call("state"))["rows"]
                        )
                        await ui.call("reading", jumpReply=mid)
                        await ui.call("reading", unread=True)
                        await ui.wait(
                            "reading",
                            lambda r: r["topId"] == historical["topId"]
                            and r["offset"] == historical["offset"],
                        )
                        await gate.release_requests("send_message")
                        await ui.wait(
                            "reading",
                            lambda r: r["topId"] == historical["topId"]
                            and r["offset"] == historical["offset"],
                        )
                        await ui.call("reading", unread=True)
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and r["viewingLatest"]
                            and r["last"] > tail,
                        )
                        gate.hold_requests.add("resume_session")
                        await gate.down.close()
                        await until(
                            lambda: any(
                                m["method"] == "resume_session"
                                for _, m in gate.held_requests
                            ),
                            "reconnect",
                        )
                        for n in range(58):
                            newest = (
                                await b.ok(
                                    "send_message",
                                    conversation=g,
                                    text="离线期间",
                                    client_id=700 + n,
                                )
                            )["id"]
                        await gate.release_requests("resume_session")
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and r["viewingLatest"]
                            and r["last"] == newest,
                        )
                        await ui.call("reading", jumpReply=other)
                        await ui.wait(
                            "reading", lambda r: r["ready"] and r["last"] < tail
                        )
                        await ui.call("send", text="主动发送应回到末尾")
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and r["viewingLatest"]
                            and r["last"] > newest,
                        )
                    elif mode == "page_position":
                        await ui.call("poll_ui", width=1280)
                        start = await ui.call("reading", inspect=mid, atY=-2)
                        expected = (start["topId"], start["offset"])
                        assert expected == (mid, -2)
                        before = await count("edit")
                        await b.ok(
                            "edit_message",
                            conversation=g,
                            message=mid - 1,
                            text="内容缩短",
                        )
                        await seen("edit", before)
                        after_edit = await ui.call("reading")
                        assert (
                            after_edit["topId"],
                            after_edit["offset"],
                        ) == expected, (
                            "edit",
                            after_edit["topId"],
                            after_edit["offset"],
                        )
                        await ui.call("poll_ui", width=980)
                        await ui.call("poll_ui", width=1280)
                        resized = await ui.call("reading")
                        assert (resized["topId"], resized["offset"]) == expected, (
                            "resize",
                            resized["topId"],
                            resized["offset"],
                        )
                        await ui.call("draft", text="焦点保留", focus=True)
                        await ui.call("poll_ui", open=mid)
                        await ui.wait("poll_ui", lambda r: r.get("members") == ["a"])
                        state = await ui.call("reading")
                        assert (state["topId"], state["offset"]) == expected, (
                            state["topId"],
                            state["offset"],
                            expected,
                        )
                        await ui.call("poll_ui", close=True)
                        state = await ui.call("reading")
                        assert (state["topId"], state["offset"]) == expected
                        assert (await ui.call("desktop_environment"))[
                            "focus"
                        ] == "messageEdit", "poll back lost editor focus"
                        assert (await ui.call("state"))["draft"] == "焦点保留"
                        await ui.call("select", conv=direct)
                        await ui.wait("reading", lambda r: r["ready"])
                        await ui.call("select", conv=g)
                        reopened = await ui.wait("reading", lambda r: r["ready"])
                        assert (reopened["topId"], reopened["offset"]) == expected
                        # Keep a visible poll selected while its result and neighbouring bodies change.
                        assert (await ui.call("multi", start=mid))["valid"] == [mid]
                        position = await ui.call("reading")
                        selected_anchor = (position["topId"], position["offset"])
                        n = await count("update")
                        await b.ok("vote_poll", choices=2, **args)
                        await seen("update", n)
                        await b.ok(
                            "edit_message",
                            conversation=g,
                            message=other,
                            text="选择期间编辑",
                        )
                        await b.ok("delete_message", conversation=g, message=other)
                        await ui.wait("multi", lambda r: r["deleted"], inspect=other)
                        assert (await ui.call("multi"))["valid"] == [mid]
                        position = await ui.call("reading")
                        assert (
                            position["topId"],
                            position["offset"],
                        ) == selected_anchor
                        await ui.call("multi", key="esc")
                        position = await ui.call("reading")
                        assert (
                            position["topId"],
                            position["offset"],
                        ) == selected_anchor

                    elif mode.startswith("detail_"):
                        gate.hold.add("get_poll_voters")
                        await ui.call("poll_ui", open=mid)
                        await gate.caught("get_poll_voters")
                        page = await ui.call("poll_ui", choice=2)
                        assert page["title"] == "投票结果" and page["rows"]
                        if mode == "detail_recall":
                            await b.ok("delete_message", **args)
                        elif mode == "detail_clear":
                            await a.ok("clear_history", conversation=g, through=other)
                        elif mode == "detail_loss":
                            await a.ok("dissolve_group", conversation=g)
                        else:
                            await gate.down.close()
                        await ui.wait(
                            "poll_ui", lambda r: "title" not in r, message=mid
                        )
                        if mode != "detail_disconnect":
                            n = await count("voters")
                            await gate.release("get_poll_voters")
                            await seen("voters", n)
                            assert "rows" not in await ui.call("poll_ui")
                    elif mode == "voters":
                        await b.ok("vote_poll", choices=1, **args)
                        gate.hold.add("get_poll_voters")
                        await ui.call("poll_ui", open=mid)
                        await gate.caught("get_poll_voters")
                        gate.hold.clear()
                        await b.ok("vote_poll", choices=2, **args)
                        await ui.wait(
                            "poll_ui",
                            lambda r: r.get("members") == ["a", "b"],
                            choice=2,
                        )
                        n = await count("voters")
                        await gate.release("get_poll_voters")
                        await seen("voters", n)
                        assert not (await ui.call("poll_ui", choice=1))["members"]
                        assert (await ui.call("poll_ui", choice=2))["members"] == [
                            "a",
                            "b",
                        ]
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
                        n = await count("poll")
                        await gate.release("get_poll", error=mode == "poll_error")
                        await seen("poll", n)
                        state = await ui.call("poll_ui")
                        assert "QA search failure" not in state["labels"]
                        await ui.call("poll_ui", close=True)
                        await ui.call("message_menu", open=mid)
                        menu = await ui.wait(
                            "message_menu",
                            lambda r: r.get("open")
                            and (("结束投票" in r["actions"]) == (mode != "authority")),
                        )
                        assert ("结束投票" in menu["actions"]) == (mode != "authority")
                    elif mode == "write":
                        gate.hold_requests.add("vote_poll")
                        await ui.call("poll_ui", message=mid, vote=1)
                        await until(
                            lambda: bool(gate.held_requests), "vote awaiting dispatch"
                        )
                        # Another device votes first while this device's write is held.
                        await a.ok("vote_poll", choices=2, **args)
                        await ui.wait(
                            "poll_ui", lambda r: r["selection"] == 2, message=mid
                        )
                        for attempt in range(2):
                            await ui.call("poll_ui", open=mid)
                            await ui.wait(
                                "poll_ui", lambda r: r.get("members") == ["a"], choice=2
                            )
                            if not attempt:
                                await ui.call("poll_ui", close=True)
                        gate.hold.add("poll_updated")
                        await gate.release_requests("vote_poll")
                        await ui.wait(
                            "poll_ui", lambda r: r.get("members") == ["a"], choice=1
                        )
                        assert not (await ui.call("poll_ui", choice=2))["members"]
                        assert (await a.ok("get_poll", **args))["summary"][
                            "selection"
                        ] == 1
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
                            "投票已结束" in detail["labels"]
                            and not (await ui.call("poll_ui", message=mid))["pending"]
                        )
                    elif mode == "paging":
                        await ui.call("poll_ui", open=mid)
                        await ui.wait(
                            "poll_ui",
                            lambda r: len(r.get("members", [])) == 10 and r.get("more"),
                        )
                        gate.hold.add("get_poll_voters")
                        await ui.call("poll_ui", more=True)
                        await gate.caught("get_poll_voters")
                        await gate.release("get_poll_voters", error=True)
                        await ui.wait(
                            "poll_ui", lambda r: r.get("moreText") == "重试加载成员"
                        )
                        await ui.call("poll_ui", collapse=True)
                        assert "再显示" in (await ui.call("poll_ui"))["moreText"]
                        await ui.call("poll_ui", more=True)
                        await ui.wait("poll_ui", lambda r: len(r["members"]) == 51)
                        await ui.call("poll_ui", close=True)
                        await ui.call("poll_ui", open=mid)
                        await ui.wait(
                            "poll_ui", lambda r: len(r.get("members", [])) == 10
                        )
                        await ui.wait(
                            "poll_ui", lambda r: r.get("members") == ["a"], choice=2
                        )
                        gate.hold.add("get_poll_voters")
                        await ui.call("poll_ui", more=True)
                        await gate.caught("get_poll_voters")
                        await gate.release("get_poll_voters", error=True)
                        await ui.wait(
                            "poll_ui", lambda r: r.get("moreText") == "重试加载成员"
                        )
                        # A new revision retries automatically. The next click must expand, even
                        # if the user collapsed the section while its replacement page was pending.
                        gate.hold.add("get_poll_voters")
                        await b.ok("vote_poll", choices=0, **args)
                        await gate.caught("get_poll_voters")
                        await ui.call("poll_ui", collapse=True)
                        await gate.release("get_poll_voters")
                        await ui.wait(
                            "poll_ui",
                            lambda r: len(r.get("members", [])) == 10
                            and "再显示" in r.get("moreText", ""),
                        )
                        await b.ok("vote_poll", choices=1, **args)
                        await ui.wait(
                            "poll_ui", lambda r: r.get("moreText") == "再显示 41 位成员"
                        )
                        gate.hold.add("get_poll_voters")
                        await ui.call("poll_ui", more=True)
                        await gate.caught("get_poll_voters")
                        gate.hold.clear()
                        await b.ok("vote_poll", choices=0, **args)
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
                        await ui.call("poll_ui", collapse=True)
                        assert (await ui.call("poll_ui"))["members"] == state[
                            "members"
                        ][:10]
                        await ui.call("poll_ui", more=True)
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
                        await b.ok("vote_poll", choices=2, **args)
                        await seen("update", n)
                        await gate.release("get_messages")
                        await ui.wait(
                            "searchui",
                            lambda r: r.get("ctx")
                            and mid in r.get("contextIds", [])
                            and r.get("pollRevision") == 2,
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
        "unloaded_edit",
        "unloaded_reaction",
        "older",
        "loaded",
        "new",
        "newwithdraw",
        "withdraw",
        "switch",
        "clear",
        "leave",
        "page_position",
        "timeline",
        "detail_recall",
        "detail_clear",
        "detail_loss",
        "detail_disconnect",
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
