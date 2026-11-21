"""Check personal watermarks and hidden idempotent acknowledgement over real RPC."""

import asyncio
import sqlite3

from fixture import fixture


async def run():
    async with fixture() as (db, login, sender, peer, direct, _):
        second = await login("a", "c")
        params = dict(conversation=direct, text="确认丢失", client_id=75)
        cutoff = (await sender.ok("send_message", **params))["id"]

        def cursor():
            with sqlite3.connect(db) as conn:
                return conn.execute(
                    "select last_read_message_id from conversation_members where conversation_id=? and user_id=1",
                    (direct,),
                ).fetchone()[0]

        read = cursor()

        async def clear(through):
            return (await second.ok("clear_history", conversation=direct, through=through))["through"]

        assert await clear(cutoff) == cutoff
        new = (await peer.ok("send_message", conversation=direct, text="之后", client_id=5))["id"]
        await sender.ok("set_unread_mark", conversation=direct, marked=True)
        assert await clear(cutoff) == cutoff
        (snapshot,) = (await sender.ok("get_conversations"))["conversations"]
        assert snapshot["marked_unread"] and snapshot["unread"] == 1 and cursor() == read
        assert await sender.ok("send_message", **params) == {
            "conversation": direct,
            "from": 1,
            "id": cutoff,
            "client_id": 75,
            "hidden": True,
        }
        assert await clear(new) == new and await clear(cutoff) == new and cursor() == read
        print("PASS fixed cutoff retry, true read position, later unread and manual mark, hidden ACK")


if __name__ == "__main__":
    asyncio.run(run())
