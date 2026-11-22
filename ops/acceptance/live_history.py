"""Two-device Qt regression for edits, recalls and stale categorized pages."""

import asyncio
from pathlib import Path
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from fixture import fixture


async def run(root, probe, baseline):
    async with fixture() as (_, _, _, peer, conv, _):
        host, port = peer.ws.remote_address[:2]
        gate = Gate(f"ws://{host}:{port}/ws")
        ids = [
            (
                await peer.ok(
                    "send_message",
                    conversation=conv,
                    client_id=1000 + i,
                    text=f"命中 https://example.test/{i}",
                )
            )["id"]
            for i in range(54)
        ]
        async with serve(gate.handle, "127.0.0.1", 0) as proxy:
            ui = Desktop()
            await ui.start(
                probe, root, f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
            )

            async def edit(mid, text):
                gate.messages.clear()
                await peer.ok("edit_message", conversation=conv, message=mid, text=text)
                await until(
                    lambda: any(
                        p["method"] == "get_conversations" for p in gate.messages
                    ),
                    "Qt handled edit notification",
                )

            async def rows(count):
                return (await ui.wait("history", lambda r: r.get("count") == count))[
                    "rows"
                ]

            try:
                await ui.wait("select", lambda r: r["ok"], conv=conv)
                await ui.call("history", open=True, conv=conv)
                await rows(50)
                await edit(ids[-1], "命中 https://example.test/新正文")
                state = await ui.call("history", copy=0)
                if baseline:
                    assert state["rows"][0]["text"].endswith("/53"), state
                    assert state["copied"].endswith("/53"), state
                    print(
                        "RED: actual Qt history display and clipboard kept edited message's old text",
                        flush=True,
                    )
                    return
                assert state["rows"][0]["text"].endswith("新正文"), state
                assert state["copied"].endswith("新正文"), state
                await ui.call("history", tab=3)
                await rows(50)
                await edit(ids[-1], "命中但没有网址")
                await rows(49)
                await ui.call("history", query="命中")
                await rows(50)
                await edit(ids[-2], "https://example.test/不再匹配")
                await rows(49)
                # Reset to a deliberately stale snapshot; remove both its newest and oldest rows.
                await ui.call("history", close=True)
                gate.hold.add("get_messages")
                await ui.call("history", open=True, conv=conv)
                await gate.caught("get_messages")
                page = next(
                    reply for name, reply in gate.held if name == "get_messages"
                )
                raw_ids = [r["id"] for r in page["result"]["messages"]]
                await edit(raw_ids[-1], "在途编辑")
                await peer.ok("delete_message", conversation=conv, message=raw_ids[0])
                await until(
                    lambda: sum(
                        p["method"] == "get_conversations" for p in gate.messages
                    )
                    >= 2,
                    "Qt handled in-flight recall",
                )
                await gate.release("get_messages")
                first = await rows(49)
                assert first[0]["text"] == "在途编辑"
                assert raw_ids[0] not in {r["id"] for r in first}
                before = await ui.call("history", select=5, scroll=120)
                other = (await peer.ok("create_group", title="无关", members=[1]))[
                    "conversation"
                ]
                unrelated = (
                    await peer.ok(
                        "send_message", conversation=other, text="别处", client_id=4000
                    )
                )["id"]
                await peer.ok("get_conversations")
                await edit_other(peer, gate, other, unrelated)
                after = await ui.call("history")
                assert (after["selected"], after["scroll"]) == (
                    before["selected"],
                    before["scroll"],
                )
                gate.messages.clear()
                await ui.call("history", more=True)
                await until(
                    lambda: any(p["method"] == "get_messages" for p in gate.messages),
                    "next page",
                )
                assert (
                    next(
                        p["params"]["before"]
                        for p in gate.messages
                        if p["method"] == "get_messages"
                    )
                    == raw_ids[0]
                )
                final = await rows(53)
                assert [r["id"] for r in final] == list(
                    reversed([i for i in ids if i != raw_ids[0]])
                )
                visible_recall = final[0]["id"]
                await peer.ok(
                    "delete_message", conversation=conv, message=visible_recall
                )
                after_recall = await rows(52)
                assert all(r["id"] != visible_recall for r in after_recall)
                assert (await ui.call("history", copy=0))["copied"] == after_recall[0][
                    "text"
                ]
                # All 50 visible matches can disappear, while an unread older match still exists.
                sparse = [
                    (
                        await peer.ok(
                            "send_message",
                            conversation=conv,
                            client_id=5000 + i,
                            text=f"稀疏-验收 {i}",
                        )
                    )["id"]
                    for i in range(51)
                ]
                await ui.call("history", query="稀疏-验收")
                visible = await rows(50)
                assert [r["id"] for r in visible] == list(reversed(sparse[1:]))
                for mid in sparse[1:]:
                    await peer.ok(
                        "edit_message",
                        conversation=conv,
                        message=mid,
                        text="移除筛选命中",
                    )
                empty = await ui.wait("history", lambda r: r["count"] == 0)
                assert empty["more"], empty
                gate.messages.clear()
                await ui.call("history", more=True)
                tail = await rows(1)
                assert tail[0]["id"] == sparse[0]
                assert (
                    next(
                        p["params"]["before"]
                        for p in gate.messages
                        if p["method"] == "get_messages"
                    )
                    == sparse[1]
                )
                print(
                    "PASS Qt history display/clipboard, category/keyword removal, stale edit/recall, selection/scroll and original cursor even after all matches disappeared",
                    flush=True,
                )
            finally:
                await ui.stop()


async def edit_other(peer, gate, conv, mid):
    gate.messages.clear()
    await peer.ok("edit_message", conversation=conv, message=mid, text="别处编辑")
    await until(
        lambda: any(p["method"] == "get_conversations" for p in gate.messages),
        "Qt handled unrelated edit",
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-live-history-") as path:
        asyncio.run(run(Path(path), sys.argv[1], "--baseline" in sys.argv))
