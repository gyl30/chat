"""Real loopback Chat, SDK, Qt and Silo login-session acceptance."""

import asyncio
from contextlib import AsyncExitStack
import hashlib
from pathlib import Path
import sqlite3
import stat
import sys
import tempfile

from websockets.asyncio.client import connect
from websockets.asyncio.server import serve
from websockets.exceptions import ConnectionClosed

from desktop import Desktop, Gate, until
from fixture import Client
from group_moderation import hold_silo_completion, read
from isolated import Store, begin, chat, put

INVALID = -32016
PASSWORD = -32017


async def socket(stack, url):
    return Client(await stack.enter_async_context(connect(url)))


async def authenticate(rpc, device, password="pass", user="a"):
    return await rpc.ok(
        "authenticate",
        username=user,
        password=password,
        device_secret=device * 64,
        device_name=f"Fixture {device}",
        client_type="RPC",
    )


async def resume(rpc, device, token):
    return await rpc.ask("resume_session", token=token, device_secret=device * 64)


async def rejected(stack, url, device, token):
    client = await socket(stack, url)
    assert (await resume(client, device, token))["error"]["code"] == INVALID
    assert (await client.ask("list_login_sessions"))["error"]["code"] == -32001


async def rpc_acceptance(db, storage):
    async with AsyncExitStack() as stack:
        with chat(db, storage) as url:
            a, b, peer = [await socket(stack, url) for _ in range(3)]
            assert (await a.ok("register", username="a", password="pass"))["user"] == 1
            assert (await peer.ok("register", username="b", password="pass"))[
                "user"
            ] == 2
            first, second = await authenticate(a, "a"), await authenticate(b, "b")
            other = await authenticate(peer, "d", user="b")
            assert first["session"] != second["session"]
            assert read(
                db, "SELECT token_hash FROM login_sessions WHERE id=?", first["session"]
            ) == [(hashlib.sha256(first["token"].encode()).hexdigest(),)]
            rows = (await a.ok("list_login_sessions"))["sessions"]
            assert {r["id"] for r in rows} == {first["session"], second["session"]}
            assert len([r for r in rows if r["current"]]) == 1
            assert (
                rows[0]["id"] == first["session"]
                and rows[0]["device_name"] == "Fixture a"
            )
            assert set(rows[0]) == {
                "id",
                "device_name",
                "client_type",
                "created_at",
                "last_seen",
                "current",
            }
            assert not (await a.ok("revoke_login_session", session=other["session"]))[
                "changed"
            ]
            await rejected(stack, url, "c", second["token"])
            await a.ok("send_friend_request", user=2)
            await peer.ok("respond_friend_request", user=1, accept=True)
            direct = (await a.ok("open_direct_conversation", user=2))["conversation"]
            for n, client in enumerate((a, b), start=100):
                await client.ok(
                    "send_message",
                    conversation=direct,
                    client_id=n,
                    text=f"session {n}",
                )

            # Two actual WebSockets share the one revocable session.
            duplicate = await socket(stack, url)
            assert (await resume(duplicate, "b", second["token"]))["result"][
                "session"
            ] == second["session"]
            assert (await a.ok("revoke_login_session", session=second["session"]))[
                "changed"
            ]
            for client in (b, duplicate):
                await asyncio.wait_for(client.ws.wait_closed(), 3)
            # Treat the first success as a lost acknowledgement. Retrying is harmless.
            assert not (await a.ok("revoke_login_session", session=second["session"]))[
                "changed"
            ]
            await rejected(stack, url, "b", second["token"])

            assert (
                await a.ask(
                    "change_password",
                    current_password="wrong",
                    new_password="next-pass",
                )
            )["error"]["code"] == PASSWORD
            c, concurrent = await socket(stack, url), await socket(stack, url)
            third = await authenticate(c, "c")
            changed, racing = await asyncio.gather(
                a.ok(
                    "change_password", current_password="pass", new_password="next-pass"
                ),
                authenticate(concurrent, "e"),
                return_exceptions=True,
            )
            assert isinstance(changed, dict) and changed["changed"]
            await asyncio.wait_for(c.ws.wait_closed(), 3)
            await rejected(stack, url, "c", third["token"])
            if isinstance(racing, dict) and racing["authenticated"]:
                # It was issued first, so the same atomic password change must revoke it.
                await asyncio.wait_for(concurrent.ws.wait_closed(), 3)
                await rejected(stack, url, "e", racing["token"])
            else:
                assert isinstance(racing, (dict, ConnectionClosed))
            assert not (await authenticate(await socket(stack, url), "f"))[
                "authenticated"
            ]
            assert (await a.ok("list_login_sessions"))["sessions"][0]["id"] == first[
                "session"
            ]
            await a.ok(
                "send_message",
                conversation=direct,
                client_id=103,
                text="current survives password change",
            )

        with chat(db, storage) as url:
            surviving = await socket(stack, url)
            assert (await resume(surviving, "a", first["token"]))["result"][
                "session"
            ] == first["session"]
            await rejected(stack, url, "b", second["token"])
            await rejected(stack, url, "c", third["token"])
            rest = [await socket(stack, url) for _ in range(2)]
            others = [
                (device, await authenticate(rpc, device, password="next-pass"))
                for device, rpc in zip(("c", "d"), rest)
            ]
            assert (await surviving.ok("revoke_other_login_sessions"))["changed"]
            for (device, result), rpc in zip(others, rest):
                await asyncio.wait_for(rpc.ws.wait_closed(), 3)
                await rejected(stack, url, device, result["token"])
            assert [
                r["id"] for r in (await surviving.ok("list_login_sessions"))["sessions"]
            ] == [first["session"]]

            expired = await authenticate(
                await socket(stack, url), "f", password="next-pass"
            )
            with sqlite3.connect(db) as conn:
                conn.execute(
                    "UPDATE login_sessions SET created_at=0 WHERE id=?",
                    (expired["session"],),
                )
            await rejected(stack, url, "f", expired["token"])
            assert len((await surviving.ok("list_login_sessions"))["sessions"]) == 1
            assert (await surviving.ok("logout_session"))["changed"]
            await asyncio.wait_for(surviving.ws.wait_closed(), 3)
            await rejected(stack, url, "a", first["token"])
    print(
        "PASS real session identity, digest only, scopes, same-session sockets, password race, restart, expiry, revoke others and logout",
        flush=True,
    )


async def revoke_during_publish(root, storage, api, bucket):
    with hold_silo_completion(storage) as (endpoint, armed, completed, release):
        db = root / "publish.sqlite3"
        with chat(db, endpoint) as url:
            async with AsyncExitStack() as stack:
                a, b, peer = [await socket(stack, url) for _ in range(3)]
                await a.ok("register", username="a", password="pass")
                await peer.ok("register", username="b", password="pass")
                await authenticate(a, "a")
                target = await authenticate(b, "b")
                await authenticate(peer, "d", user="b")
                await a.ok("send_friend_request", user=2)
                await peer.ok("respond_friend_request", user=1, accept=True)
                conv = (await a.ok("open_direct_conversation", user=2))["conversation"]
                payload = b"isolated Silo finalization before login revocation"
                part = await begin(b, conv, 901, payload)
                await b.ok(
                    "confirm_attachment",
                    upload=901,
                    number=1,
                    etag=await asyncio.to_thread(put, part["url"], payload),
                )
                [(key,)] = read(
                    db, "SELECT object_key FROM attachment_uploads WHERE client_id=901"
                )
                armed.set()
                pending = asyncio.create_task(b.ask("finish_attachment", upload=901))
                try:
                    await until(
                        completed.is_set,
                        "real Silo Complete succeeds before Chat finalization",
                    )
                    assert api.head_object(Bucket=bucket, Key=key)[
                        "ContentLength"
                    ] == len(payload)
                    assert (
                        await a.ok("revoke_login_session", session=target["session"])
                    )["changed"]
                    try:
                        await asyncio.wait_for(pending, 3)
                        assert False, "revoked in-flight socket did not close"
                    except ConnectionClosed:
                        pass
                finally:
                    release.set()
                await until(
                    lambda: read(
                        db, "SELECT state FROM attachment_uploads WHERE client_id=901"
                    )
                    == [("paused",)],
                    "revoked upload pauses but stays on original installation",
                )
                assert not read(db, "SELECT id FROM messages WHERE client_id=901")
                assert not read(
                    db, "SELECT object_key FROM files WHERE object_key=?", key
                )
                await rejected(stack, url, "b", target["token"])
    print(
        "PASS revoke closes an in-flight WS while Silo reply is held, and Chat final write cannot publish",
        flush=True,
    )


async def qt_acceptance(root, db, storage, probe):
    with chat(db, storage) as url:
        async with AsyncExitStack() as stack:
            provision, peer = await socket(stack, url), await socket(stack, url)
            await provision.ok("register", username="a", password="pass")
            await peer.ok("register", username="b", password="pass")
            temporary = await authenticate(provision, "a")
            await authenticate(peer, "d", user="b")
            await provision.ok("send_friend_request", user=2)
            await peer.ok("respond_friend_request", user=1, accept=True)
            conv = (await provision.ok("open_direct_conversation", user=2))[
                "conversation"
            ]
            anchor = (
                await peer.ok(
                    "send_message",
                    conversation=conv,
                    client_id=951,
                    text="草稿回复锚点",
                )
            )["id"]
            assert temporary["session"]
            await provision.ok("logout_session")
            ga, gb = Gate(url), Gate(url)
            urls = []
            for gate in (ga, gb):
                proxy = await stack.enter_async_context(
                    serve(gate.handle, "127.0.0.1", 0)
                )
                urls.append(f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws")
            a, b = Desktop(), Desktop()
            homes = (root / "a", root / "b")
            await a.start(probe, homes[0], urls[0])
            await b.start(probe, homes[1], urls[1])
            try:
                (journal,) = homes[1].rglob("tasks.sqlite3")
                assert stat.S_IMODE(journal.stat().st_mode) == 0o600
                assert stat.S_IMODE(journal.parent.stat().st_mode) == 0o700
                [(token,)] = read(
                    journal, "SELECT token FROM login_credentials WHERE username='a'"
                )
                [(install,)] = read(journal, "SELECT secret FROM installation")
                assert token != install and len(token) == len(install) == 64

                await a.call("devices", open=True)
                await b.call("devices", open=True)
                state = await a.wait("devices", lambda r: len(r.get("rows", [])) == 2)
                aid = next(r["id"] for r in state["rows"] if r["current"])
                bid = next(r["id"] for r in state["rows"] if not r["current"])
                assert all(
                    r["name"] and r["type"] and r["created"] and r["seen"]
                    for r in state["rows"]
                )
                await b.wait(
                    "devices",
                    lambda r: any(
                        x["id"] == bid and x["current"] for x in r.get("rows", [])
                    ),
                )
                assert not (await a.call("devices", select=aid))["revoke_enabled"]
                await b.call("devices", close=True)

                await gb.down.close()
                await until(
                    lambda: any(p["method"] == "resume_session" for p in gb.messages),
                    "Qt resumes with token",
                )
                await b.wait("select", lambda r: r["ok"], conv=conv)
                await b.wait(
                    "state", lambda r: any(m["id"] == anchor for m in r["rows"])
                )
                assert [p["method"] for p in gb.messages].count("authenticate") == 1
                assert await a.wait(
                    "devices", lambda r: any(x["id"] == bid for x in r.get("rows", []))
                )

                gb.hold.add("send_message")
                await b.call("send", text="登录撤销前已提交但确认丢失")
                await gb.caught("send_message")
                [(cid,)] = read(journal, "SELECT client FROM text_outbox")
                assert read(
                    db, "SELECT count(*) FROM messages WHERE client_id=?", cid
                ) == [(1,)]
                file = root / "unfinished.bin"
                file.write_bytes(b"x" * (16 * 1024 * 1024) + b"tail")
                gb.hold.add("confirm_attachment")
                await b.call("upload", conv=conv, path=str(file))
                await gb.caught("confirm_attachment")
                [(upload, object_key, multipart)] = read(
                    db,
                    "SELECT client_id,object_key,multipart FROM attachment_uploads WHERE conversation=?",
                    conv,
                )
                assert multipart and object_key
                assert (await b.call("reply_to", id=anchor))["reply"]
                await b.call("draft_debounce", hold=True)
                await b.call("draft", text="撤销瞬间尚未落盘的草稿")
                assert (await b.call("draft_debounce"))["pending"]
                assert not read(
                    journal, "SELECT 1 FROM compose_drafts WHERE conversation=?", conv
                )

                await b.call("devices", open=True)
                await b.wait("devices", lambda r: len(r.get("rows", [])) == 2)
                assert (await b.call("devices", select=aid, revoke=True))[
                    "revoke_enabled"
                ]
                # Leave B's conflicting confirmation open; only A's confirmation is accepted.
                await b.wait("confirm", lambda r: r["open"], accept=False)
                assert (await a.call("devices", select=bid, revoke=True))[
                    "revoke_enabled"
                ]
                await a.wait("confirm", lambda r: r["open"])
                await b.wait(
                    "login_state",
                    lambda r: r["user"] == 0 and "其他设备" in r["status"],
                )
                assert not (await b.call("login_state"))["reconnecting"]
                assert not (await b.call("devices")).get("rows")
                assert not (await b.call("confirm", accept=False))["open"]
                await a.wait(
                    "devices", lambda r: [x["id"] for x in r.get("rows", [])] == [aid]
                )
                await until(
                    lambda: read(
                        journal,
                        "SELECT body,reply FROM compose_drafts WHERE conversation=?",
                        conv,
                    )
                    == [("撤销瞬间尚未落盘的草稿", anchor)],
                    "Qt flushes active debounce before retiring local owner",
                )
                assert read(journal, "SELECT client FROM text_outbox") == [(cid,)]
                assert read(journal, "SELECT client FROM tasks") == [(upload,)]
                assert not read(journal, "SELECT token FROM login_credentials")
                assert read(journal, "SELECT secret FROM installation") == [(install,)]
                await until(
                    lambda: read(
                        db,
                        "SELECT state,object_key,multipart FROM attachment_uploads WHERE client_id=?",
                        upload,
                    )
                    == [("paused", object_key, multipart)],
                    "same multipart retained after remote logout",
                )
                probe_socket = await socket(stack, url)
                assert (
                    await probe_socket.ask(
                        "resume_session", token=token, device_secret=install
                    )
                )["error"]["code"] == INVALID

                (a_journal,) = homes[0].rglob("tasks.sqlite3")
                [(a_token,)] = read(a_journal, "SELECT token FROM login_credentials")
                [(a_install,)] = read(a_journal, "SELECT secret FROM installation")
                other_installation = await socket(stack, url)
                assert (
                    await other_installation.ok(
                        "resume_session", token=a_token, device_secret=a_install
                    )
                )["session"] == aid
                assert (
                    await other_installation.ask("resume_attachment", upload=upload)
                )["error"]["code"] == -32008
                await other_installation.ws.close()

                gb.hold.clear()
                gb.held.clear()
                await b.call("login", user="a", password="pass")
                await b.wait("state", lambda r: r["user"] == 1)
                await b.wait("select", lambda r: r["ok"], conv=conv)
                await b.wait("state", lambda r: r["draft"] == "撤销瞬间尚未落盘的草稿")
                await until(
                    lambda: not read(journal, "SELECT client FROM text_outbox"),
                    "new session reconciles old accepted message",
                )
                assert read(
                    db, "SELECT count(*) FROM messages WHERE client_id=?", cid
                ) == [(1,)]
                transfers = await b.wait(
                    "transfers",
                    lambda r: any(file.name in label for label in r["labels"]),
                )
                label = next(x for x in transfers["labels"] if file.name in x)
                assert (await b.call("transfers", label=label, action="继续上传"))[
                    "triggered"
                ]
                await until(
                    lambda: read(
                        db, "SELECT count(*) FROM messages WHERE client_id=?", upload
                    )
                    == [(1,)],
                    "original installation publishes",
                )
                assert any(
                    p["method"] == "resume_attachment"
                    and p["params"]["upload"] == upload
                    for p in gb.messages
                )

                # Suppress delivery of the revocation event. The client must fail closed using its old token.
                state = await a.wait("devices", lambda r: len(r.get("rows", [])) == 2)
                bid = next(r["id"] for r in state["rows"] if not r["current"])
                gb.hold.add("login_session_revoked")
                ga.hold.add("revoke_login_session")
                before = sum(p["method"] == "resume_session" for p in gb.messages)
                await a.call("devices", select=bid, revoke=True)
                await a.wait("confirm", lambda r: r["open"])
                await ga.caught("revoke_login_session")
                # The actual successful acknowledgement is withheld. Retrying from
                # another connection of A's same login finds no additional change.
                retry = await socket(stack, url)
                assert (
                    await retry.ok(
                        "resume_session", token=a_token, device_secret=a_install
                    )
                )["session"] == aid
                assert not (await retry.ok("revoke_login_session", session=bid))[
                    "changed"
                ]
                await retry.ws.close()
                await ga.release("revoke_login_session")
                await until(
                    lambda: sum(p["method"] == "resume_session" for p in gb.messages)
                    > before,
                    "missed notification must test old token",
                )
                await b.wait(
                    "login_state", lambda r: r["user"] == 0 and "失效" in r["status"]
                )
                assert not read(journal, "SELECT token FROM login_credentials")

                gb.hold.clear()
                gb.held.clear()
                await b.call("login", user="a", password="pass")
                await b.wait("state", lambda r: r["user"] == 1)

                # Put the target in an actual reconnect before committing revocation.
                state = await a.wait("devices", lambda r: len(r.get("rows", [])) == 2)
                bid = next(x["id"] for x in state["rows"] if not x["current"])
                gb.hold_requests.add("resume_session")
                await gb.down.close()
                await until(
                    lambda: any(
                        r["method"] == "resume_session" for _, r in gb.held_requests
                    ),
                    "reconnect is waiting before upstream authentication",
                )
                await a.call("devices", select=bid, revoke=True)
                await a.wait("confirm", lambda r: r["open"])
                await a.wait("devices", lambda r: len(r.get("rows", [])) == 1)
                await gb.release_requests("resume_session")
                await b.wait(
                    "login_state", lambda r: r["user"] == 0 and "失效" in r["status"]
                )
                assert not read(journal, "SELECT token FROM login_credentials")

                await b.call("login", user="a", password="pass")
                await b.wait("state", lambda r: r["user"] == 1)
                await b.call("devices", open=True)
                await b.wait("devices", lambda r: len(r.get("rows", [])) == 2)
                await a.call("devices", password="wrong", new="next-pass")
                await a.wait("devices", lambda r: "不正确" in r.get("status", ""))
                assert (await b.call("login_state"))["user"] == 1
                await a.call("devices", password="pass", new="next-pass")
                await a.wait(
                    "devices",
                    lambda r: "密码已修改" in r.get("status", "")
                    and len(r["rows"]) == 1,
                )
                await b.wait(
                    "login_state",
                    lambda r: r["user"] == 0 and "其他设备" in r["status"],
                )
                assert (await a.call("login_state"))["user"] == 1

                # Process restart uses the saved credential explicitly, without a password.
                await a.call("devices", close=True)
                await a.call("quit")
                assert await asyncio.wait_for(a.proc.wait(), 8) == 0
                await a.start(probe, homes[0], urls[0], password="")
                await a.call("devices", open=True)
                await a.wait(
                    "devices",
                    lambda r: len(r.get("rows", [])) == 1 and r["rows"][0]["id"] == aid,
                )
                await b.call("login", user="a", password="next-pass")
                await b.wait("state", lambda r: r["user"] == 1)
                await a.wait("devices", lambda r: r.get("other_enabled"))
                await a.call("devices", others=True)
                await a.wait("confirm", lambda r: r["open"])
                await b.wait("login_state", lambda r: r["user"] == 0)
                await a.wait("devices", lambda r: len(r.get("rows", [])) == 1)
                await a.call("devices", close=True)
                await a.call("logout")
                await a.wait("login_state", lambda r: r["user"] == 0)
                assert not (await a.call("login_state"))["reconnecting"]
                assert read(
                    db,
                    "SELECT revoked_at IS NOT NULL FROM login_sessions WHERE id=?",
                    aid,
                ) == [(1,)]
                print(
                    "PASS real Qt list and confirmations, resume without password, remote and missed-event logout, password change, saved draft/Outbox/MPU",
                    flush=True,
                )
            finally:
                await a.stop()
                await b.stop()


async def run(root, probe):
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        await rpc_acceptance(root / "rpc.sqlite3", storage)
        await revoke_during_publish(root, storage, api, store.bucket)
        if probe:
            await qt_acceptance(root, root / "qt.sqlite3", storage, probe)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-login-sessions-") as directory:
        asyncio.run(run(Path(directory), sys.argv[1] if len(sys.argv) > 1 else None))
