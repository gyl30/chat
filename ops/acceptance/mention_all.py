"""Verify structured group mentions through the server, including permission and visibility changes."""

import asyncio
import sqlite3

from fixture import fixture


async def run():
    async with fixture() as (db, login, a, b, direct, _):
        group = (await a.ok("create_group", members=[2], title="提及"))["conversation"]
        b2 = await login("b", "d")

        async def send(actor, cid, **fields):
            return await actor.ask(
                "send_message",
                conversation=group,
                client_id=cid,
                text="@所有人",
                **fields
            )

        async def hits(actor):
            return [
                r["id"]
                for r in (await actor.ok("search_all_messages", mentions_me=True))[
                    "messages"
                ]
            ]

        async def unread(actor):
            rooms = (await actor.ok("get_conversations"))["conversations"]
            return next(r for r in rooms if r["id"] == group)["unread_mention"]

        assert "error" in await b.ask(
            "send_message", conversation=direct, client_id=1, text="x", mention_all=True
        )
        assert "error" in await send(b, 10, mention_all=True)
        assert not (await send(a, 11))["result"]["mention_all"] and not await hits(b)
        m = (await send(a, 12, mention_all=True))["result"]
        mid = m["id"]
        assert m["mention_all"] and not m["mentions"]
        assert (await send(a, 12, mention_all=True))["result"]["id"] == mid
        assert await hits(b2) == [mid] and await unread(b)
        await b2.ok("mark_read", conversation=group, message=mid)
        assert not await unread(b)
        await a.ok("edit_message", conversation=group, message=mid, text="修改")
        assert await hits(b) == [mid]
        forwarded = await b.ok(
            "send_message", conversation=direct, client_id=31, forward=mid
        )
        assert not forwarded["mention_all"]
        await b.ok("clear_history", conversation=group, through=mid)
        assert not await hits(b2)
        await a.ok("set_group_admin", conversation=group, user=2, admin=True)
        admin = (await send(b, 13, mention_all=True))["result"]["id"]
        assert await hits(a) == [admin]
        await a.ok("set_group_admin", conversation=group, user=2, admin=False)
        await a.ok("set_group_member_mute", conversation=group, user=2, seconds=-1)
        assert "error" in await send(b, 14, mention_all=True)
        await b.ok("delete_message", conversation=group, message=admin)
        assert not await hits(a)
        await b.ok("leave_group", conversation=group)
        other = (await send(a, 15, mention_all=True))["result"]["id"]
        await a.ok("invite_group_members", conversation=group, members=[2])
        assert not await hits(b)
        with sqlite3.connect(db) as conn:
            assert conn.execute(
                "SELECT count(*) FROM messages WHERE sender_id=1 AND client_id=12"
            ).fetchone() == (1,)
            assert conn.execute(
                "SELECT count(*) FROM message_mentions WHERE message_id IN (?,?)",
                (mid, other),
            ).fetchone() == (0,)
    print(
        "PASS structured group mention roles, retry, literal text, edit, forwarding, mute, recall, read, clear and rejoin"
    )


if __name__ == "__main__":
    asyncio.run(run())
