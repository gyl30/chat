"""Read actual membership watermarks and events after one RPC per read scope."""
import asyncio
import sqlite3

from fixture import fixture


async def main():
    async with fixture() as (path, login, a, b, dm, _):
        other = await login("a", "c")
        g = (await a.ok("create_group", title="工作", members=[2]))["conversation"]
        hidden = (await a.ok("create_group", title="隐藏", members=[2]))["conversation"]
        await a.ok("create_conversation_folder", name="文件夹")
        folder = (await a.ok("get_conversations"))["folders"][0]["id"]
        await a.ok("move_conversation_to_folder", conversation=g, folder=folder)
        await a.ok("set_notification_level", conversation=g, level=2, until=0)
        async def send(room, cid):
            return (await b.ok("send_message", conversation=room, text="消息", client_id=cid))["id"]
        d1, g1, h1 = [await send(room, i) for i, room in enumerate((dm,g,hidden),1)]
        await a.ok("hide_conversation", conversation=hidden)
        await a.ok("set_unread_mark", conversation=g, marked=True)
        def position(room):
            with sqlite3.connect(path) as db:
                return db.execute("SELECT last_read_message_id,unread_mark_message_id FROM conversation_members WHERE user_id=1 AND conversation_id=?", (room,)).fetchone()
        await a.ok("mark_conversations_read", folder=folder)
        assert position(g)==(g1,None) and position(dm)==(0,None) and position(hidden)==(0,None)
        await b.ok("get_conversations")
        assert any(e.get("method")=="read" and e["params"]==dict(user=1,conversation=g,message=g1) for e in b.events)
        await other.ok("get_conversations")
        assert any(e.get("method")=="conversation_view" and e["params"]["action"]=="read" for e in other.events)
        await a.ok("mark_conversations_read", folder=0)
        assert position(dm)==(d1,None)
        new = await send(dm, 100)
        assert position(dm)==(d1,None)
        await a.ok("clear_history", conversation=dm, through=new)
        await a.ok("mark_conversations_read", conversation=dm)
        assert position(dm)==(d1,None)
        g2=await send(g,101)
        await a.ok("mark_conversations_read")
        assert position(g)==(g2,None) and position(hidden)==(0,None)
        await a.ok("set_unread_mark", conversation=g, marked=True)
        await a.ok("mark_conversations_read",conversation=g)
        assert position(g)==(g2,None)
        # An entire extra page of visible conversations must be included.
        for n in range(51):
            room=(await b.ok("create_group", title=f"组{n}", members=[1]))["conversation"]
            await send(room,200+n)
        assert (await a.ok("get_conversations"))["next"]
        await a.ok("mark_conversations_read")
        with sqlite3.connect(path) as db:
            assert db.execute("SELECT count(*) FROM conversation_members WHERE user_id=1 AND conversation_id>? AND last_read_message_id>0",(hidden,)).fetchone()==(51,)
        print("PASS single/folder/default/all RPC; SQLite watermarks, events, mute, hidden, clear, later arrivals and >50 conversations")


if __name__=="__main__":
    asyncio.run(main())
