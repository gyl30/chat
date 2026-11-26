"""Hold real WebSocket responses until the Qt event under test has been processed."""

import asyncio
import json
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
            mid = None
            if mode in ("nav_retry", "inflight_layout", "overlap"):
                series = []
                for n in range(100):
                    if mode == "inflight_layout" and n == 64:
                        mid = await create(1)
                    series.append(
                        (
                            await b.ok(
                                "send_message",
                                conversation=g,
                                text=f"{n} " + "历史锚点 " * 130,
                                client_id=1200 + n,
                            )
                        )["id"]
                    )
                await a.ok("mark_read", conversation=g, message=series[-1])
            mid = mid or await create(1)
            args = dict(conversation=g, message=mid)
            other = (
                await b.ok("send_message", conversation=g, text="来源", client_id=2)
            )["id"]
            if mode in ("nav_retry", "inflight_layout", "overlap"):
                await a.ok("mark_read", conversation=g, message=other)
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

                    def watermark():
                        with sqlite3.connect(db) as conn:
                            return conn.execute(
                                "SELECT last_read_message_id FROM conversation_members WHERE conversation_id=? AND user_id=1",
                                (g,),
                            ).fetchone()[0]

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
                    elif mode == "poll_retry":
                        await ui.call("poll_ui", create=True)
                        await ui.wait("poll_ui", lambda r: r.get("editable"))
                        gate.hold.add("send_message")
                        fields = dict(
                            question="待确认问答",
                            options="甲\n乙",
                            quiz=True,
                            correct=2,
                            explanation="解释",
                            anonymous=False,
                        )
                        assert (await ui.call("poll_ui", **fields))["valid"]
                        await gate.caught("send_message")
                        first = next(
                            m["params"]
                            for m in gate.messages
                            if m["method"] == "send_message"
                        )
                        mid = gate.held[-1][1]["result"]["id"]
                        gate.hold.discard("send_message")
                        gate.held.clear()
                        gate.hold_requests.add("resume_session")
                        await gate.down.close()
                        await until(
                            lambda: gate.held_requests, "disconnected after commit"
                        )
                        offline = await ui.call("poll_ui", tryEdit=True)
                        print("poll after disconnect", offline, flush=True)
                        assert (
                            not offline["editable"]
                            and not offline["edited"]
                            and not offline["canSubmit"]
                        ), offline
                        await gate.release_requests("resume_session")
                        await ui.wait("poll_ui", lambda r: r.get("canSubmit"))
                        assert not (await ui.call("poll_ui", tryEdit=True))["edited"]
                        await ui.call("poll_ui", retry=True)
                        await ui.wait(
                            "poll_ui", lambda r: "title" not in r, message=mid
                        )
                        replay = [
                            m["params"]
                            for m in gate.messages
                            if m["method"] == "send_message"
                        ]
                        assert replay == [first, first], replay
                        with sqlite3.connect(db) as conn:
                            assert conn.execute(
                                "SELECT id,body FROM messages WHERE sender_id=1 AND client_id=?",
                                (first["client_id"],),
                            ).fetchall() == [(mid, fields["question"])]
                        await ui.call("poll_ui", create=True)
                        await ui.wait("poll_ui", lambda r: r.get("editable"))
                        gate.hold_requests.add("send_message")
                        await ui.call("poll_ui", **fields)
                        await until(
                            lambda: gate.held_requests,
                            "rejected request held before publication",
                        )
                        _, request = gate.held_requests[0]
                        rejected_id = request["params"]["client_id"]
                        # The server rejects an invalid body before writing; exercise the real error response.
                        request["params"]["text"] = ""
                        await gate.release_requests("send_message")
                        await ui.wait(
                            "poll_ui",
                            lambda r: r.get("editable") and r.get("canSubmit"),
                        )
                        approved = dict(
                            fields, question="修改后重新创建", options="丙\n丁"
                        )
                        await ui.call("poll_ui", **approved)
                        await ui.wait(
                            "poll_ui", lambda r: "title" not in r, message=mid
                        )
                        latest = [
                            m["params"]
                            for m in gate.messages
                            if m["method"] == "send_message"
                        ][-1]
                        assert (
                            latest["client_id"] != rejected_id
                            and latest["text"] == approved["question"]
                        )
                        with sqlite3.connect(db) as conn:
                            assert conn.execute(
                                "SELECT count(*) FROM messages WHERE sender_id=1 AND client_id=?",
                                (rejected_id,),
                            ).fetchone() == (0,)
                        # Remove the sender while the authenticated reconnect is parked.
                        owned = (
                            await b.ok("create_group", title="失权投票", members=[1])
                        )["conversation"]
                        await ui.wait("select", lambda r: r["ok"], conv=owned)
                        await ui.wait("reading", lambda r: r["ready"])
                        await ui.call("poll_ui", create=True)
                        await ui.wait("poll_ui", lambda r: r.get("editable"))
                        gate.hold.add("send_message")
                        await ui.call("poll_ui", **dict(fields, question="重连时失权"))
                        await gate.caught("send_message")
                        gate.hold.clear()
                        gate.held.clear()
                        gate.hold_requests.add("resume_session")
                        await gate.down.close()
                        await until(
                            lambda: gate.held_requests, "permission check on reconnect"
                        )
                        await b.ok("remove_group_member", conversation=owned, user=1)
                        await gate.release_requests("resume_session")
                        await ui.wait("state", lambda r: r["conv"] != owned)
                        assert not (
                            await ui.call("poll_ui", retry=True, message=mid)
                        ).get("canSubmit")
                        assert (
                            sum(
                                m["method"] == "send_message"
                                and m["params"].get("text") == "重连时失权"
                                for m in gate.messages
                            )
                            == 1
                        )
                        await ui.wait("select", lambda r: r["ok"], conv=g)
                        await ui.wait("reading", lambda r: r["ready"])
                        await ui.call("poll_ui", create=True)
                        await ui.wait("poll_ui", lambda r: r.get("editable"))
                        await ui.call("poll_ui", submit=False, **fields)
                        await ui.call("poll_ui", closeComposer=True)
                        await ui.wait("confirm", lambda r: r["open"], accept=False)
                        await ui.call("confirm", cancel=True)
                        await ui.wait(
                            "poll_ui", lambda r: r.get("question") == fields["question"]
                        )
                        await ui.call("poll_ui", closeComposer=True)
                        await ui.wait("confirm", lambda r: r["open"], accept=False)
                        await ui.call("confirm")
                        await ui.wait(
                            "poll_ui", lambda r: "title" not in r, message=mid
                        )
                        await ui.call("poll_ui", create=True)
                        await ui.wait("poll_ui", lambda r: r.get("editable"))
                        await ui.call("poll_ui", submit=False, **fields)
                        await ui.call("logout")
                        await ui.wait("state", lambda r: r["user"] == 0)
                        await ui.wait(
                            "poll_ui",
                            lambda r: r.get("title") != "发起群投票",
                            message=mid,
                        )
                    elif mode == "nav_retry":
                        await ui.call("reading", jumpReply=series[12])
                        await ui.wait(
                            "reading", lambda r: r["ready"] and r["last"] < series[60]
                        )
                        gate.hold.add("get_messages")
                        await ui.call("reading", latest=True)
                        await gate.caught("get_messages")
                        print(
                            "first latest",
                            gate.held[0][1]["result"]["messages"][-1]["id"],
                            flush=True,
                        )
                        for _ in range(2):
                            gate.held.clear()
                            await gate.down.close()
                            await gate.caught("get_messages")
                            request = next(
                                r["params"]
                                for r in reversed(gate.messages)
                                if r["method"] == "get_messages"
                            )
                            assert (
                                "after" not in request and "around" not in request
                            ), request
                        await gate.release("get_messages")
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and r["tailLoaded"]
                            and r["scroll"] == r["maximum"],
                        )

                        # An around request, and its return trip to a disjoint historical interval,
                        # each survive the loss of their first WebSocket.
                        async def reconnect_held(expected):
                            gate.held.clear()
                            await gate.down.close()
                            await gate.caught("get_messages")
                            req = next(
                                m["params"]
                                for m in reversed(gate.messages)
                                if m["method"] == "get_messages"
                            )
                            assert expected.items() <= req.items(), req
                            await gate.release("get_messages")

                        await ui.call("reading", jumpReply=series[12])
                        await ui.wait(
                            "reading", lambda r: r["ready"] and r["first"] == series[0]
                        )
                        historical = await ui.call("reading")
                        gate.hold.add("get_messages")
                        await ui.call("reading", jumpReply=series[75])
                        await gate.caught("get_messages")
                        await reconnect_held({"around": series[75]})
                        await ui.wait(
                            "reading", lambda r: r["ready"] and r["first"] > series[0]
                        )
                        gate.hold.add("get_messages")
                        await ui.call("reading", unread=True)
                        await gate.caught("get_messages")
                        await reconnect_held({"around": historical["topId"]})
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and (r["topId"], r["offset"])
                            == (historical["topId"], historical["offset"]),
                        )
                        # Clear the older reply return entry by visiting another room.
                        await ui.call("select", conv=direct)
                        await ui.wait("reading", lambda r: r["ready"])
                        await ui.call("select", conv=g)
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"] and r["topId"] == historical["topId"],
                        )
                        unchanged = await ui.call("reading")
                        gate.hold.add("get_messages")
                        await gate.down.close()
                        await gate.caught("get_messages")
                        await reconnect_held({"after": unchanged["first"] - 1})
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and (r["topId"], r["offset"])
                            == (unchanged["topId"], unchanged["offset"]),
                        )
                        # The actual unread frontier is outside the currently loaded interval.
                        newest = (
                            await b.ok(
                                "send_message",
                                conversation=g,
                                text="未读请求断线",
                                client_id=2041,
                            )
                        )["id"]
                        await ui.wait("reading", lambda r: r["count"] == 1)
                        gate.hold.add("get_messages")
                        await ui.call("reading", unread=True)
                        await gate.caught("get_messages")
                        await reconnect_held({"capture_unread": True})
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and r["tailLoaded"]
                            and r["last"] == newest,
                        )
                        await until(
                            lambda: watermark() == newest,
                            "unread replay advances actual SQLite receipt",
                        )
                        gate.hold.add("get_messages")
                        await ui.call("reading", jumpReply=series[12])
                        await gate.caught("get_messages")
                        gate.hold.clear()
                        await ui.call("select", conv=direct)
                        await ui.wait("reading", lambda r: r["ready"])
                        await gate.release("get_messages")
                        after = await ui.call("state")
                        assert after["conv"] == direct and all(
                            r["id"] != series[12] for r in after["rows"]
                        )
                    elif mode == "inflight_layout":
                        row = series[65]
                        initial = await ui.call("reading", inspect=row, atY=-2)
                        assert (initial["topId"], initial["offset"]) == (
                            row,
                            -2,
                        ), initial
                        gate.hold.add("get_messages")
                        await ui.call("reading", requestOlder=True)
                        await gate.caught("get_messages")
                        n = await count("edit")
                        await b.ok(
                            "edit_message",
                            conversation=g,
                            message=series[64],
                            text="短消息",
                        )
                        await seen("edit", n)
                        after = await ui.call("reading")
                        assert (after["topId"], after["offset"]) == (row, -2), after
                        for width in (980, 1280):
                            await ui.call("poll_ui", width=width)
                            after = await ui.call("reading")
                            assert (after["topId"], after["offset"]) == (row, -2), after
                        for emoji in ("👍", ""):
                            n = await count("reaction")
                            await b.ok(
                                "set_message_reaction",
                                conversation=g,
                                message=series[64],
                                emoji=emoji,
                            )
                            await seen("reaction", n)
                            after = await ui.call("reading")
                            assert (after["topId"], after["offset"]) == (row, -2), after
                        n = await count("update")
                        await b.ok("vote_poll", choices=1, **args)
                        await b.ok("close_poll", **args)
                        await seen("update", n)
                        await ui.wait("poll_ui", lambda r: r["closed"], message=mid)
                        assert (await ui.call("reading"))["topId"] == row
                        await ui.call("poll_ui", open=mid)
                        await ui.wait("poll_ui", lambda r: r.get("members") == ["b"])
                        await ui.call("poll_ui", close=True)
                        after = await ui.call("reading")
                        assert (after["topId"], after["offset"]) == (row, -2), after
                        assert (await ui.call("multi", start=row))["valid"] == [row]
                        selected = await ui.call("reading", inspect=row, atY=-2)
                        assert (selected["topId"], selected["offset"]) == (row, -2)
                        await ui.call("multi", key="esc")
                        after = await ui.call("reading")
                        assert (after["topId"], after["offset"]) == (row, -2), after
                        n = await count("edit")
                        await b.ok("delete_message", conversation=g, message=series[64])
                        await seen("edit", n)
                        after = await ui.call("reading")
                        assert (after["topId"], after["offset"], watermark()) == (
                            row,
                            -2,
                            other,
                        ), after
                        await gate.release("get_messages")
                        await ui.wait(
                            "reading",
                            lambda r: r["ready"]
                            and (r["topId"], r["offset"]) == (row, -2),
                        )
                    elif mode == "overlap":
                        await ui.call("reading", latest=True)
                        await ui.wait(
                            "reading", lambda r: r["ready"] and r["tailLoaded"]
                        )
                        await ui.call("reading", jumpReply=series[43])
                        merged = await ui.wait(
                            "reading", lambda r: r["ready"] and r["first"] == series[23]
                        )
                        assert merged["last"] == other and merged["tailLoaded"], merged
                        await ui.call("reading", loadAround=series[8])
                        whole = await ui.wait(
                            "reading", lambda r: r["ready"] and r["first"] == series[0]
                        )
                        assert (
                            whole["tailLoaded"]
                            and whole["oldestLoaded"]
                            and whole["last"] == other
                        ), whole
                        await ui.call("reading", loadAround=series[43])
                        inner = await ui.wait("reading", lambda r: r["ready"])
                        assert (
                            inner["first"] == series[0]
                            and inner["last"] == other
                            and inner["tailLoaded"]
                            and inner["oldestLoaded"]
                        ), inner
                        tail = (
                            await b.ok(
                                "send_message",
                                conversation=g,
                                text="重叠后实时来信",
                                client_id=2460,
                            )
                        )["id"]
                        await ui.wait(
                            "reading", lambda r: r["last"] == tail and r["tailLoaded"]
                        )
                        gate.hold_requests.add("send_message")
                        await ui.call("send", text="重叠后的待发")
                        await ui.wait(
                            "state",
                            lambda r: any(
                                x["text"] == "重叠后的待发" for x in r["rows"]
                            ),
                            pendingOnly=True,
                        )
                    elif mode == "pending_anchor":
                        gate.hold_requests.add("send_message")
                        for n in range(3):
                            await ui.call(
                                "send", text=f"排队 {n} " + "待发送锚点 " * 300
                            )
                            await ui.wait(
                                "state",
                                lambda r: sum(not x["id"] for x in r["rows"]) == n + 1,
                            )
                        before = await ui.call("reading", pendingAt="排队 1", atY=-7)
                        assert (
                            before["topText"].startswith("排队 1")
                            and before["offset"] == -7
                        ), before
                        await ui.call("poll_ui", width=980)
                        after = await ui.call("reading")
                        assert (after["topClient"], after["offset"]) == (
                            before["topClient"],
                            -7,
                        ), after
                        # Send the first queued request, then observe the remaining pending row at its old pixel.
                        gate.hold.add("send_message")
                        server, request = gate.held_requests.pop(0)
                        await server.send(json.dumps(request))
                        await gate.caught("send_message")
                        await gate.release("send_message")
                        await ui.wait(
                            "state", lambda r: sum(not x["id"] for x in r["rows"]) == 2
                        )
                        assert (await ui.call("reading"))["topClient"] == before[
                            "topClient"
                        ]
                        await until(
                            lambda: gate.held_requests, "second pending message queued"
                        )
                        server, request = gate.held_requests.pop(0)
                        await server.send(json.dumps(request))
                        acknowledged = await ui.wait(
                            "reading", lambda r: r["topId"] > other
                        )
                        assert (acknowledged["topClient"], acknowledged["offset"]) == (
                            before["topClient"],
                            -7,
                        ), acknowledged
                        await until(
                            lambda: gate.held_requests,
                            "third pending message in flight",
                        )
                        last_pending = await ui.call(
                            "reading", pendingAt="排队 2", atY=-7
                        )
                        assert (
                            last_pending["topClient"] and last_pending["offset"] == -7
                        ), last_pending
                        _, rejected = gate.held_requests.pop(0)
                        await gate.down.send(
                            json.dumps(
                                dict(
                                    jsonrpc="2.0",
                                    id=rejected["id"],
                                    error=dict(code=-32602, message="Invalid params"),
                                )
                            )
                        )
                        failure = await ui.wait("reading", lambda r: r["topFailed"])
                        assert (failure["topClient"], failure["offset"]) == (
                            last_pending["topClient"],
                            -7,
                        ), failure
                        await ui.call("message_menu", open=0)
                        await ui.wait(
                            "message_menu",
                            lambda r: "删除待发消息" in r.get("actions", []),
                        )
                        await ui.call("message_menu", action="删除待发消息")
                        await ui.wait(
                            "state", lambda r: not r["rows"], pendingOnly=True
                        )
                        assert not (await ui.call("reading"))["topFailed"]
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
        "poll_retry",
        "nav_retry",
        "inflight_layout",
        "overlap",
        "pending_anchor",
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
