"""Probe the real SDK and server with exact frame sizes and controlled response order."""

import asyncio
from contextlib import suppress
import json
from pathlib import Path
import sqlite3
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Gate, until
from fixture import fixture

SAMPLE = '中"反\\\n👩🏽‍💻éÄΣ'
MAX_ID = 2**63 - 1


async def run(probe, scratch):
    async with fixture() as (db, _, a, b, conversation, _):
        await a.ok("set_user_profile", nickname="中👩🏽‍💻", signature="组合 é")
        mid = (
            await a.ok(
                "send_message", conversation=conversation, text="base", client_id=750
            )
        )["id"]
        host, port = a.ws.remote_address[:2]
        gate = Gate(f"ws://{host}:{port}/ws")
        gate.hold.add("get_contacts")
        wire = []
        notification = asyncio.Event()
        rejections = []

        def observe(method, packet):
            if method == "send_message" and "error" in packet:
                rejections.append(packet["error"]["code"])
            if method == "message" and packet.get("params", {}).get("text") == SAMPLE:
                notification.set()

        gate.transform = observe

        class Trace:
            def __init__(self, ws):
                self.ws = ws

            def __getattr__(self, key):
                return getattr(self.ws, key)

            async def __aiter__(self):
                async for frame in self.ws:
                    wire.append((len(frame.encode()), json.loads(frame)))
                    yield frame

        async def handle(ws):
            await gate.handle(Trace(ws))

        async with serve(handle, "127.0.0.1", 0) as relay:
            url = f"ws://127.0.0.1:{relay.sockets[0].getsockname()[1]}/ws"
            proc = await asyncio.create_subprocess_exec(
                probe,
                url,
                scratch,
                str(conversation),
                str(mid),
                stdout=asyncio.subprocess.PIPE,
                stderr=asyncio.subprocess.PIPE,
            )
            try:
                await gate.caught("get_contacts")
                await until(
                    lambda: any(
                        p["method"] == "list_login_sessions" for p in gate.messages
                    ),
                    "profile response triggers a nested SDK request before contacts returns",
                )
                peer = await b.ok(
                    "send_message",
                    conversation=conversation,
                    text=SAMPLE,
                    client_id=751,
                )
                search = await a.ok("search_all_messages", query="äσ")
                assert peer["id"] in {item["id"] for item in search["messages"]}
                await asyncio.wait_for(notification.wait(), 6)
                await gate.release("get_contacts")
                gate.hold.add("get_contacts")
                await gate.caught("get_contacts")
                await gate.down.close()

                out, err = await asyncio.wait_for(proc.communicate(), 12)
                assert proc.returncode == 0, (out.decode(), err.decode())
                edits = [(size, p) for size, p in wire if p["method"] == "edit_message"]
                assert [size for size, _ in edits[:2]] == [65535, 65536]
                assert len(edits) == 3 and edits[2][1]["params"]["text"] == SAMPLE
                sends = [p for _, p in wire if p["method"] == "send_message"]
                assert [p["params"]["client_id"] for p in sends] == [
                    MAX_ID - 65535,
                    MAX_ID - 65536,
                ]
                for p, wanted in zip(sends, (65535, 65536)):
                    assert "reply_to" not in p["params"]
                    p["id"] = MAX_ID
                    data = json.dumps(
                        p, ensure_ascii=False, separators=(",", ":")
                    ).encode()
                    assert len(data) == wanted
                    assert p["params"]["text"].endswith(SAMPLE)
                assert all(
                    p["params"] == {}
                    for _, p in wire
                    if p["method"] in ("get_contacts", "list_login_sessions")
                )
                with sqlite3.connect(db) as conn:
                    got = conn.execute(
                        "select client_id from messages where client_id>=? order by client_id desc",
                        (MAX_ID - 65537,),
                    ).fetchall()
                assert not got and rejections == [-32602, -32602]
                (journal,) = Path(scratch).rglob("tasks.sqlite3")
                with sqlite3.connect(journal) as conn:
                    got = conn.execute(
                        "select client,rejection from text_outbox order by client desc"
                    ).fetchall()
                assert got == [(MAX_ID - 65535, -32602), (MAX_ID - 65536, -32602)]
                print(out.decode().strip())
                print(
                    "PASS actual edit frames 65535/65536; 65537 never sent; Outbox checks use worst-case request ID"
                )
            finally:
                if proc.returncode is None:
                    proc.kill()
                if proc.returncode != 0:
                    with suppress(asyncio.TimeoutError):
                        _, err = await asyncio.wait_for(proc.communicate(), 5)
                        if err:
                            print(err.decode(), file=sys.stderr)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-sdk-json-") as tmp:
        asyncio.run(run(Path(sys.argv[1]).resolve(), tmp))
