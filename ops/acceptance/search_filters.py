"""Structured global search, combined with literal content and live visibility."""

import asyncio
import sqlite3

from fixture import fixture


async def run():
    async with fixture() as (db, login, a, b, direct, _):
        c = await login("c", "c", True)
        await b.ok("send_friend_request", user=3)
        await c.ok("respond_friend_request", user=2, accept=True)
        group = (await b.ok("create_group", title="权限验证", members=[3]))[
            "conversation"
        ]
        with sqlite3.connect(db) as conn:
            start = 1767225600000
            conn.executemany(
                "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(?,?,?,?)",
                [(direct, 2, '中 文 %_ "sql" https://example', start)] * 51
                + [(direct, 2, "only uppercase HTTPS://example", start)]
                + [(direct, 2, "https://at-end", start + 86400000)]
                + [(group, 2, '中 文 %_ "sql" https://not-visible', start)] * 500,
            )
            ids = [
                r[0]
                for r in conn.execute(
                    "SELECT id FROM messages WHERE conversation_id=? AND body LIKE '中%' ORDER BY id DESC",
                    (direct,),
                )
            ]
        await a.ok("hide_conversation", conversation=direct)
        scopes = await a.ok("message_search_scopes")
        assert any(row["id"] == direct and row["name"] == "b" for row in scopes)
        filters = dict(
            conversation=direct,
            sender=2,
            category="links",
            start=start,
            end=start + 86400000,
        )
        first = await a.ok("search_all_messages", **filters)
        assert [r["id"] for r in first["messages"]] == ids[:50]
        assert first["next_before"] == ids[49]
        second = await a.ok(
            "search_all_messages", before=first["next_before"], **filters
        )
        assert [r["id"] for r in second["messages"]] == ids[50:] and not second.get(
            "next_before"
        )
        history = await a.ok(
            "get_messages", category="links", conversation=direct, query="中 文"
        )
        assert [r["id"] for r in history["messages"]] == ids[:50][::-1] and history[
            "has_more"
        ]
        last = await a.ok(
            "get_messages",
            category="links",
            conversation=direct,
            query="中 文",
            before=ids[49],
        )
        assert [r["id"] for r in last["messages"]] == ids[50:] and not last["has_more"]
        for literal in ("%_", '"sql"', "中", " 文", " "):
            assert (await a.ok("search_all_messages", query=literal, **filters))[
                "messages"
            ]
        assert not (await a.ok("search_all_messages", query="中文", **filters))[
            "messages"
        ]
        assert not (
            await a.ok("search_all_messages", conversation=group, category="links")
        )["messages"]
        assert "error" in await a.ask("search_all_messages", query="")
        assert "error" in await a.ask("search_all_messages")
        await b.ok("delete_message", conversation=direct, message=ids[0])
        assert ids[0] not in [
            r["id"] for r in (await a.ok("search_all_messages", **filters))["messages"]
        ]
        await a.ok("clear_history", conversation=direct, through=ids[0])
        assert not (await a.ok("search_all_messages", **filters))["messages"]
        await b.ok("invite_group_members", conversation=group, members=[1])
        assert not (await a.ok("get_messages", conversation=group, category="links"))[
            "messages"
        ]
        fresh = (
            await b.ok(
                "send_message", conversation=group, text="https://joined", client_id=920
            )
        )["id"]
        assert [
            r["id"]
            for r in (
                await a.ok("search_all_messages", conversation=group, category="links")
            )["messages"]
        ] == [fresh]
        await b.ok("remove_group_member", conversation=group, user=1)
        assert "error" in await a.ask("get_messages", conversation=group, around=fresh)
        assert not (
            await a.ok("search_all_messages", conversation=group, category="links")
        )["messages"]
        await b.ok("invite_group_members", conversation=group, members=[1])
        assert not (await a.ok("get_messages", conversation=group, category="links"))[
            "messages"
        ]
        assert "error" in await a.ask("get_messages", conversation=group, around=fresh)
        print(
            "PASS combined and filter-only global search, literal spaces/%/_/quotes, hidden scope, dates, 50/51 and permission",
            flush=True,
        )


if __name__ == "__main__":
    asyncio.run(run())
