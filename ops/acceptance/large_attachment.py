"""Send and save an exact 4 GiB attachment through real Qt, SDK, Chat and Silo."""

import asyncio
from contextlib import AsyncExitStack
import hashlib
from pathlib import Path
import sqlite3
import sys
import tempfile
import time

from websockets.asyncio.server import serve

from desktop import Desktop, Gate
from isolated import Store, chat, login

GIB = 1024**3


def sha256(path):
    with path.open("rb") as file:
        digest = hashlib.sha256()
        for chunk in iter(lambda: file.read(4 * 1024**2), b""):
            digest.update(chunk)
    return digest.hexdigest()


async def run(root, probe):
    db = root / "chat.sqlite3"
    src, dst = root / "four-gib.bin", root / "received-four-gib.bin"
    with src.open("wb") as file:
        file.truncate(4 * GIB)
        for index in range(256):
            file.seek((index + 1) * 16 * 1024**2 - 4)
            file.write(index.to_bytes(4, "big"))
    digest = sha256(src)
    started = time.monotonic()
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, storage) as upstream:
            gate = Gate(upstream)
            async with (
                serve(gate.handle, "127.0.0.1", 0) as ws,
                AsyncExitStack() as stack,
            ):
                url = f"ws://127.0.0.1:{ws.sockets[0].getsockname()[1]}/ws"
                a = await login(stack, upstream, "a", "a", True)
                b = await login(stack, upstream, "b", "b", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                conv = (await a.ok("open_direct_conversation", user=2))["conversation"]
                ui = Desktop()
                await ui.start(probe, root, url)
                stack.push_async_callback(ui.stop)
                await ui.wait("select", lambda r: r["ok"], conv=conv)
                await ui.call("upload", conv=conv, path=str(src))

                async def published():
                    while True:
                        with sqlite3.connect(db) as conn:
                            row = conn.execute(
                                "select message_id,object_key from message_attachments"
                            ).fetchone()
                        if row:
                            return row
                        await asyncio.sleep(0.1)

                mid, key = await asyncio.wait_for(published(), 300)
                confirms = [
                    p["params"]["number"]
                    for p in gate.messages
                    if p["method"] == "confirm_attachment"
                ]
                assert confirms == list(range(1, 257)), confirms
                head = api.head_object(Bucket=store.bucket, Key=key)
                assert (
                    head["ContentLength"] == 4 * GIB
                    and head["Metadata"]["sha256"] == digest
                )
                sent = time.monotonic()
                await ui.call(
                    "preview", conv=conv, id=mid, filename=src.name, inline=False
                )
                await ui.wait("dialogstate", lambda r: r["shown"])
                await ui.call("dialogstate", save=True, destination=str(dst))
                await ui.call("dialogstate", close=True)
                await ui.call("downloads", open=True)

                async def downloaded():
                    while True:
                        rows = (await ui.call("downloads"))["rows"]
                        if rows and rows[0]["status"] == "已完成":
                            return
                        await asyncio.sleep(0.1)

                await asyncio.wait_for(downloaded(), 300)
                assert dst.stat().st_size == 4 * GIB
                assert sha256(dst) == digest and not list(root.glob("*.part-*"))
                print(
                    f"PASS Qt 4 GiB, 256 confirmed PUTs, SHA256={digest}; "
                    f"upload/publish {sent - started:.1f}s; download/check {time.monotonic() - sent:.1f}s"
                )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-s3-large-") as path:
        asyncio.run(run(Path(path), sys.argv[1]))
