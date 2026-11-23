"""Real heartbeat and Qt recovery over a loopback TCP relay that never speaks WebSocket."""

import argparse
import asyncio
from contextlib import AsyncExitStack, suppress
from pathlib import Path
import sqlite3
import tempfile
import time
from urllib.parse import urlsplit

from desktop import Desktop, until
from isolated import Store, chat, login


class Relay:
    def __init__(self, upstream):
        self.host = urlsplit(upstream).hostname
        self.port = urlsplit(upstream).port
        self.live = {}
        self.started = 0
        self.accepting = asyncio.Event()
        self.accepting.set()

    def stall(self, *directions):
        assert len(self.live) == 1, self.live
        pair = next(iter(self.live.values()))
        pair["blocked"] = set(directions)
        self.accepting.clear()
        assert pair["first_endpoint_end"] is None
        return pair

    async def handle(self, client, down):
        await self.accepting.wait()
        server, up = await asyncio.open_connection(self.host, self.port)
        ident = self.started
        self.started += 1
        pair = self.live[ident] = {
            "blocked": set(),
            "dropped": {"up": 0, "down": 0},
            "first_endpoint_end": None,
        }

        async def copy(source, target, direction):
            try:
                while block := await source.read(65536):
                    if direction in pair["blocked"]:
                        pair["dropped"][direction] += len(block)
                    else:
                        target.write(block)
                        await target.drain()
            except OSError:
                pass
            finally:
                if pair["first_endpoint_end"] is None:
                    pair["first_endpoint_end"] = (direction, time.monotonic())
            # Don't propagate an EOF across a stalled pair. Each endpoint must
            # discover the transport stop itself; the relay never sends a frame.
            if not pair["blocked"]:
                target.close()

        try:
            await asyncio.gather(copy(client, up, "up"), copy(server, down, "down"))
        finally:
            for writer in (up, down):
                writer.close()
                with suppress(OSError):
                    await writer.wait_closed()
            self.live.pop(ident, None)


def sql(db, query, *par):
    with sqlite3.connect(db) as conn:
        return conn.execute(query, par).fetchall()


async def run(root, args):
    db = root / "chat.sqlite3"
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, storage) as url:
            proxy = Relay(url)
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                b = await login(stack, url, "b", "b", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                direct = (await a.ok("open_direct_conversation", user=2))[
                    "conversation"
                ]
                listener = await stack.enter_async_context(
                    await asyncio.start_server(proxy.handle, "127.0.0.1", 0)
                )
                entry = f"ws://127.0.0.1:{listener.sockets[0].getsockname()[1]}/ws"
                ui = Desktop()
                await ui.start(
                    args.probe, root, entry, native=args.native, platform=args.platform
                )
                try:
                    print(
                        "Qt runtime", await ui.call("desktop_environment"), flush=True
                    )
                    await ui.wait("select", lambda r: r["ok"], conv=direct)
                    await ui.wait("reading", lambda r: r["ready"])
                    (journal,) = (root / "data").rglob("tasks.sqlite3")
                    ids = (await a.ok("list_login_sessions"))["sessions"]
                    app = next(item["id"] for item in ids if not item["current"])

                    def outbox(text):
                        return sql(
                            journal, "SELECT client FROM text_outbox WHERE body=?", text
                        )

                    def sent(cid):
                        return sql(db, "SELECT id FROM messages WHERE client_id=?", cid)

                    async def wait_timeout(pair, start):
                        state = await asyncio.wait_for(
                            poll(
                                ui,
                                "login_state",
                                lambda r: r["reconnecting"] and r["connectionText"],
                            ),
                            65,
                        )
                        elapsed = time.monotonic() - start
                        assert elapsed >= 8, "unexpected immediate disconnect"
                        print(
                            f"heartbeat reconnect after {elapsed:.2f}s, "
                            f"relay dropped={pair['dropped']}, first observed endpoint end={pair['first_endpoint_end']}, client={state}",
                            flush=True,
                        )

                    async def recover():
                        proxy.accepting.set()
                        await asyncio.wait_for(
                            poll(
                                ui,
                                "login_state",
                                lambda r: not r["reconnecting"] and r["user"] == 1,
                            ),
                            40,
                        )
                        await ui.wait("reading", lambda r: r["ready"])
                        await until(lambda: len(proxy.live) == 1, "only recovered link")
                        assert not (await ui.call("login_state"))["reconnectTimers"]
                        assert {
                            r["id"]
                            for r in (await a.ok("list_login_sessions"))["sessions"]
                        } == {r["id"] for r in ids}

                    # Downlink only: server persists the original send and starts
                    # the original upload; neither acknowledgement reaches Qt.
                    pair = proxy.stall("down")
                    start = time.monotonic()
                    await ui.call("send", text="accepted before downlink stalled")
                    await until(
                        lambda: outbox("accepted before downlink stalled"), "Outbox"
                    )
                    [(cid,)] = outbox("accepted before downlink stalled")
                    await until(lambda: len(sent(cid)) == 1, "server accepted original")
                    original = sent(cid)
                    file = root / "paused.bin"
                    file.write_bytes(b"original installation upload after stall")
                    await ui.call("queue_file", conv=direct, path=str(file))
                    await until(
                        lambda: sql(
                            db,
                            "SELECT client_id FROM attachment_uploads WHERE multipart IS NOT NULL AND multipart<>''",
                        ),
                        "server registered upload",
                    )
                    [(upload,)] = sql(db, "SELECT client_id FROM attachment_uploads")
                    [(key, multipart)] = sql(
                        db,
                        "SELECT object_key,multipart FROM attachment_uploads WHERE client_id=?",
                        upload,
                    )
                    assert key and multipart
                    await wait_timeout(pair, start)
                    assert outbox("accepted before downlink stalled") == [(cid,)]
                    await ui.call("send", text="queued while offline")
                    await until(lambda: outbox("queued while offline"), "offline queue")
                    [(offline,)] = outbox("queued while offline")
                    assert not sent(offline)
                    await ui.call("draft", text="draft kept during reconnect")
                    beat = (
                        await b.ok(
                            "send_message",
                            conversation=direct,
                            client_id=8840,
                            text="backfill during stall",
                        )
                    )["id"]
                    await recover()
                    await ui.wait(
                        "state",
                        lambda r: r["draft"] == "draft kept during reconnect"
                        and any(m["id"] == beat for m in r["rows"]),
                    )
                    await until(
                        lambda: not outbox("accepted before downlink stalled")
                        and not outbox("queued while offline"),
                        "replayed queue settled",
                    )
                    assert sent(cid) == original and len(sent(offline)) == 1
                    assert sql(
                        db,
                        "SELECT object_key,multipart FROM attachment_uploads WHERE client_id=?",
                        upload,
                    ) == [(key, multipart)]
                    await ui.wait(
                        "transfers", lambda r: any(file.name in v for v in r["labels"])
                    )
                    label = next(
                        v
                        for v in (await ui.call("transfers"))["labels"]
                        if file.name in v
                    )
                    assert (await ui.call("transfers", label=label, action="继续上传"))[
                        "triggered"
                    ]
                    await until(lambda: len(sent(upload)) == 1, "same upload published")
                    print(
                        "PASS downlink stall, original ACK/idempotency, offline Outbox, draft, history, original upload resume",
                        flush=True,
                    )

                    # Two-way stall: there is no forwarded revoke notification.
                    pair = proxy.stall("up", "down")
                    start = time.monotonic()
                    await ui.call("draft", text="draft survives revoked login")
                    assert (await a.ok("revoke_login_session", session=app))["changed"]
                    await wait_timeout(pair, start)
                    proxy.accepting.set()
                    await asyncio.wait_for(
                        poll(ui, "login_state", lambda r: r["user"] == 0), 40
                    )
                    assert sql(
                        journal,
                        "SELECT body FROM compose_drafts WHERE conversation=?",
                        direct,
                    ) == [("draft survives revoked login",)]
                    assert not sql(journal, "SELECT token FROM login_credentials")
                    await until(lambda: not proxy.live, "all retired links removed")
                    assert not (await ui.call("login_state"))["reconnectTimers"]
                    print(
                        "PASS bidirectional stall, missed revoke, rejected resume, preserved draft; "
                        f"relay open={len(proxy.live)} total accepted={proxy.started}",
                        flush=True,
                    )
                finally:
                    await ui.stop()


async def poll(ui, op, check):
    while True:
        state = await ui.call(op)
        if check(state):
            return state
        await asyncio.sleep(0.1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe")
    parser.add_argument("--native", action="store_true")
    parser.add_argument("--platform", choices=("xcb", "wayland"))
    args = parser.parse_args()
    if args.platform and not args.native:
        parser.error("--platform requires --native")
    with tempfile.TemporaryDirectory(prefix="chat-raw-stall-") as directory:
        asyncio.run(run(Path(directory), args))
