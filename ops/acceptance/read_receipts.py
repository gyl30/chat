"""Read receipts from real history and roster RPCs, SQLite and the Qt timeline."""

import asyncio
from pathlib import Path
import sqlite3
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from fixture import fixture


async def run(root, probe, case):
    async with fixture() as (db, login, a, b, direct, _):
        host, port = a.ws.remote_address[:2]
        gate = Gate(f"ws://{host}:{port}/ws")
        async with serve(gate.handle, "127.0.0.1", 0) as proxy:
            entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            ui = Desktop()

            async def send(room, number):
                return (await a.ok(
                    "send_message", conversation=room, client_id=7000 + number,
                    text=f"回执 {number} " + "验证历史阅读位置 " * 16,
                ))["id"]

            async def select(room):
                await ui.wait("select", lambda res: res["ok"], conv=room)
                await ui.wait("reading", lambda res: res["ready"] and res["last"])

            async def check(messages, expected):
                def match(state):
                    return [
                        (row["read"], row["readers"]) for row in state["rows"] if row["shown"]
                    ] == expected
                return await ui.wait("receipts", match, messages=messages)

            def watermark(room, user):
                with sqlite3.connect(db) as conn:
                    return conn.execute(
                        "SELECT joined_message_id,last_read_message_id,cleared_message_id "
                        "FROM conversation_members WHERE conversation_id=? AND user_id=?",
                        (room, user),
                    ).fetchone()

            try:
                if case in ("all", "paging"):
                    ids = [await send(direct, i) for i in range(55)]
                    await ui.start(probe, root, entry)
                    await select(direct)
                    before = await ui.call("reading")
                    assert before["first"] > ids[0]
                    await ui.call("receipts", trace=True)
                    await ui.call("reading", requestOlder=True)
                    after = await ui.wait("reading", lambda s: s["ready"] and s["first"] == ids[0])
                    measured = (await ui.call("receipts"))["changes"]
                    print(f"unchanged get_messages(before) Model notifications: {measured}", flush=True)
                    assert not measured
                    assert (after["topId"], after["offset"]) == (before["topId"], before["offset"])
                    await a.ok("mark_read", conversation=direct, message=ids[-1])
                    await ui.wait("reading", lambda s: s["read"] == ids[-1])
                    assert not (await ui.call("receipts"))["changes"]
                    await b.ok("mark_read", conversation=direct, message=ids[1])
                    observed = await check(ids[:3], [(True, []), (True, []), (False, [])])
                    assert observed["changes"] == [[ids[0], ids[1], 2]], observed
                    for through in (ids[1], ids[0]):
                        received = await ui.call("receipts", reader=2, through=through)
                        assert received["changes"] == observed["changes"]
                    await ui.stop()
                    print("PASS full history layout anchor, silent self/duplicate/old reads, and two-row private receipt update", flush=True)

                if case in ("all", "membership"):
                    c = await login("c", "c", True)
                    await a.ok("send_friend_request", user=3)
                    await c.ok("respond_friend_request", user=1, accept=True)
                    room = (await a.ok("create_group", title="成员回执", members=[2]))["conversation"]
                    old = await send(room, 100)
                    await a.ok("invite_group_members", conversation=room, members=[3])
                    new = await send(room, 101)
                    assert watermark(room, 3)[:2] == (old, 0)
                    await c.ok("mark_read", conversation=room, message=new)
                    await ui.start(probe, root, entry)
                    await select(room)
                    observed = await ui.wait("receipts", lambda s: s["rows"][1]["readers"] == [3], messages=[old, new])
                    print(f"later member, old/new Qt receipts: {observed['rows']!r}", flush=True)
                    assert observed["rows"][0] == {"shown": True, "read": False, "readers": []}
                    members = (await a.ok("get_members", conversation=room))["members"]
                    assert [(row["id"], row["joined_message_id"]) for row in members] == [(1, 0), (2, 0), (3, old)]
                    await ui.call("receipts", trace=True)
                    deliveries = (await ui.call("deliveries"))["members"]
                    await ui.call("receipts", refreshMembers=True)
                    await ui.wait("deliveries", lambda r: r["members"] > deliveries)
                    assert not (await ui.call("receipts"))["changes"]
                    await ui.call("receipts", show=new)
                    await ui.wait("receipts", lambda r: r.get("dialogReaders") == [3])
                    b2 = await login("b", "d")
                    await b2.ok("mark_read", conversation=room, message=new)
                    observed = await check([old, new], [(True, [2]), (True, [2, 3])])
                    assert observed["changes"] == [[old, old, 1]], observed
                    await ui.wait("receipts", lambda r: r.get("dialogReaders") == [2, 3])
                    await ui.call("receipts", close=True)
                    await c.ok("clear_history", conversation=room, through=new)
                    assert watermark(room, 3) == (old, new, new)
                    await check([old, new], [(True, [2]), (True, [2, 3])])
                    await b.ok("leave_group", conversation=room)
                    await check([old, new], [(False, []), (True, [3])])

                    gate.hold.add("get_members")
                    await a.ok("invite_group_members", conversation=room, members=[2])
                    await gate.caught("get_members")
                    again = await send(room, 102)
                    await b2.ok("mark_read", conversation=room, message=again)
                    assert watermark(room, 2)[:2] == (new, again)
                    await ui.wait("receipts", lambda r: r["watermark"] == again, peer=2)
                    await check([new, again], [(False, []), (False, [])])
                    await gate.release("get_members")
                    await check([old, new, again], [(False, []), (True, [3]), (True, [2])])

                    # The reply for the previous roster is released after the newest roster.
                    gate.hold.add("get_members")
                    await a.ok("remove_group_member", conversation=room, user=3)
                    await gate.caught("get_members")
                    await a.ok("invite_group_members", conversation=room, members=[3])
                    await until(lambda: len(gate.held) >= 2, "two roster replies")
                    latest = await send(room, 103)
                    await c.ok("mark_read", conversation=room, message=latest)
                    assert watermark(room, 3)[:2] == (again, latest)
                    gate.held.reverse()
                    await gate.release("get_members")
                    marker = await send(room, 104)
                    await ui.wait("reading", lambda r: r["last"] == marker)
                    await check([old, new, again, latest], [(False, []), (False, []), (True, [2]), (True, [3])])
                    print("PASS current roster, multi-device read, clear, leave/rejoin, kick/reinvite and stale reply ordering", flush=True)
            finally:
                await ui.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-read-receipts-") as tmp:
        asyncio.run(run(Path(tmp), sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "all"))
