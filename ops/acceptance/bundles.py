"""Drive merged forwarding with Qt, archived source text and real Silo file references."""
import asyncio
from contextlib import AsyncExitStack
from pathlib import Path
import sqlite3
import sys
import tempfile
import urllib.request

from desktop import Desktop, until
from isolated import Store, begin, chat, login, put
from message_selection import PNG
from recovery import admin


async def run(root, probe):
    store = Store()
    db = root / "bundle.sqlite"
    with store.node(root / "silo") as (api, env):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                a,b,c = [await login(stack,url,name,name,True) for name in "abc"]
                for peer, uid in [(b,2),(c,3)]:
                    await a.ok("send_friend_request",user=uid)
                    await peer.ok("respond_friend_request",user=1,accept=True)
                source = (await a.ok("open_direct_conversation",user=2))["conversation"]
                target = (await a.ok("create_group",title="合并目标",members=[3]))["conversation"]
                async def text(cid, body, markdown=False):
                    return (await b.ok("send_message",conversation=source,client_id=cid,text=body,markdown=markdown))["id"]
                first = await text(10,"**原始正文**",True)
                async def attach(cid,payload,name,media):
                    part = await begin(b,source,cid,payload,name,media)
                    await b.ok("confirm_attachment",upload=cid,number=1,etag=put(part["url"],payload))
                    return await b.ok("finish_attachment",upload=cid)
                image = await attach(20,PNG,"图.png","image/png")
                file = await attach(21,b"snapshot","文件.bin","application/octet-stream")
                ids=[first,image["id"],file["id"]]
                with sqlite3.connect(db) as con:
                    con.execute("update messages set created_at=1736942400000 where id=?", (first,))
                admin("--db",db,"archive","2025-01")
                ui=Desktop()
                await ui.start(probe,root,url)
                stack.push_async_callback(ui.stop)
                await ui.wait("select",lambda r:r["ok"],conv=source)
                await ui.wait("state",lambda r:file["id"] in [x["id"] for x in r["rows"]])
                await ui.call("multi",start=first)
                for mid in ids[1:]:
                    await ui.call("multi",click=mid,modifier="ctrl")
                await ui.call("forward_picker",open=True)
                await ui.wait("forward_picker",lambda r:r["open"])
                assert (await ui.call("forward_picker",target=target,bundle=True))["confirmed"]
                def placed():
                    with sqlite3.connect(db) as con:
                        return con.execute("select id,client_id from messages where content='bundle'").fetchone()
                mid,cid=await until(placed,"one merged message from Qt")
                args=dict(conversation=target,message=mid)
                page=await c.ok("get_forwarded_messages",**args)
                assert [x["position"] for x in page["items"]]==[1,2,3] and page["items"][0]["markdown"]
                assert (await a.ok("send_message",conversation=target,client_id=cid,bundle=ids))["id"]==mid
                assert "error" in await c.ask("get_messages",conversation=source,around=first)
                assert "error" in await c.ask("send_message",conversation=target,client_id=9,bundle=ids)
                assert "error" in await a.ask("send_message",conversation=target,client_id=19,forward=mid)
                assert "error" in await a.ask("send_message",conversation=target,client_id=19,bundle=[mid,first])
                await ui.wait("select",lambda r:r["ok"],conv=target)
                await ui.call("bundle_view",open=mid)
                await ui.wait("bundle_view",lambda r:r.get("count")==3 and r["body"]=="原始正文")
                await ui.call("bundle_view",close=True)
                long_ids=[await text(101+i,"a"*58000) for i in range(12)]
                long=await a.ok("send_message",conversation=target,bundle=long_ids,client_id=71)
                after=0
                positions=[]
                while True:
                    chunk=await c.ok("get_forwarded_messages",conversation=target,message=long["id"],after=after)
                    assert all(i["body"]=="a"*58000 for i in chunk["items"])
                    positions.extend(i["position"] for i in chunk["items"])
                    after=chunk["next"]
                    if not after: break
                assert positions==list(range(1,13))
                await b.ok("edit_message",conversation=source,message=first,text="编辑后")
                for src in ids: await b.ok("delete_message",conversation=source,message=src)
                assert (await c.ok("get_forwarded_messages",**args))==page
                for i,payload in [(2,PNG),(3,b"snapshot")]:
                    download=await c.ok("get_attachment",**args,item=i)
                    assert urllib.request.urlopen(download["url"]).read()==payload
                await c.ok("clear_history",conversation=target,through=mid)
                assert "error" in await c.ask("get_attachment",**args,item=2)
                assert "error" in await c.ask("get_forwarded_messages",**args)
                await a.ok("invite_group_members",conversation=target,members=[2])
                assert "error" in await b.ask("get_forwarded_messages",**args)
                await a.ok("delete_message",**args)
                assert "error" in await a.ask("get_attachment",**args,item=2)
                with sqlite3.connect(db) as con:
                    assert con.execute("select count(*) from bundle_items where parent=?",(mid,)).fetchone()==(0,)
        # Startup drains outstanding object cleanup in the same disposable bucket.
        with chat(db,env):
            await until(lambda: not api.list_objects_v2(Bucket=store.bucket).get("Contents"),"last bundle files reclaimed")
    print("PASS Qt/SDK/server merged selection, isolated Silo shared references and last-reference GC, archived sources, snapshots, keysets and permission floors")


if __name__=="__main__":
    with tempfile.TemporaryDirectory(prefix="chat-bundle-") as tmp:
        asyncio.run(run(Path(tmp),sys.argv[1]))
