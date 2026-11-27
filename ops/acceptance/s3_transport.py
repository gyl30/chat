"""Exercise the real S3 wire, including completion failures and isolated TLS."""

import asyncio
import base64
from collections import Counter
from contextlib import AsyncExitStack, contextmanager
import hashlib
from http.client import HTTPConnection
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
from threading import Event, Thread
from unittest.mock import patch
from urllib.parse import parse_qs, urlsplit
from urllib.request import urlopen

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put


@contextmanager
def proxy(storage, tls=None):
    origin = storage["CHAT_S3_ENDPOINT"]
    faults, seen = {}, Counter()
    get_stalled, get_closed = Event(), Event()
    put_stalled, put_closed = Event(), Event()

    class Request(BaseHTTPRequestHandler):
        def forward(self):
            query = parse_qs(urlsplit(self.path).query, keep_blank_values=True)
            kind = self.command
            if "uploads" in query and kind in ("GET", "POST"):
                kind += " uploads"
            elif "uploadId" in query:
                kind += " multipart"
            elif kind == "PUT" and urlsplit(self.path).path.count("/") == 1:
                kind += " bucket"
            seen[kind] += 1
            headers = {
                k: v for k, v in self.headers.items() if k.lower() != "connection"
            }
            payload = self.rfile.read(int(headers.get("Content-Length", 0)))
            if self.command == "PUT" and "Content-MD5" in self.headers:
                assert (
                    self.headers["Content-MD5"]
                    == base64.b64encode(
                        hashlib.md5(payload, usedforsecurity=False).digest()
                    ).decode()
                )
                seen["Content-MD5"] += 1
            fault = faults.pop(kind, None)
            if fault == "200-error":
                # S3 can stream whitespace before the actual 200 error document.
                body = b'  \r\n<?xml version="1.0"?><Error><Code>InvalidPart</Code></Error>'
                self.send_response(200)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            conn = HTTPConnection(origin, timeout=10)
            try:
                conn.request(self.command, self.path, body=payload, headers=headers)
                res = conn.getresponse()
                body = res.read()
                if fault == "lose-response":
                    assert res.status == 200, res.status
                    return
                if fault == "stall-response":
                    assert res.status == 200, res.status
                    put_stalled.set()
                    self.connection.settimeout(8)
                    if not self.connection.recv(1):
                        put_closed.set()
                    return
                self.send_response(res.status)
                for key, value in res.getheaders():
                    if key.lower() not in {
                        "connection",
                        "transfer-encoding",
                        "server",
                        "date",
                    }:
                        self.send_header(key, value)
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(body[:65536] if fault == "stall" else body)
                    self.wfile.flush()
                if fault == "stall":
                    get_stalled.set()
                    self.connection.settimeout(8)
                    if not self.connection.recv(1):
                        get_closed.set()
            finally:
                conn.close()

        do_GET = do_HEAD = do_POST = do_PUT = do_DELETE = forward

        def log_message(self, *args):
            pass

    with ThreadingHTTPServer(("127.0.0.1", 0), Request) as server:
        if tls:
            server.socket = tls.wrap_socket(server.socket, server_side=True)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield (
                dict(
                    storage,
                    CHAT_S3_ENDPOINT=f"127.0.0.1:{server.server_port}",
                    CHAT_S3_HTTPS=str(int(bool(tls))),
                ),
                faults,
                seen,
                get_stalled,
                get_closed,
                put_stalled,
                put_closed,
            )
        finally:
            server.shutdown()
            thread.join()


async def direct(stack, url):
    a = await login(stack, url, "a", "a", True)
    b = await login(stack, url, "b", "b", True)
    await a.ok("send_friend_request", user=2)
    await b.ok("respond_friend_request", user=1, accept=True)
    conv = (await a.ok("open_direct_conversation", user=2))["conversation"]
    return a, conv


def upload_row(db, cid):
    with sqlite3.connect(db) as conn:
        return conn.execute(
            "select object_key,multipart from attachment_uploads where client_id=?",
            (cid,),
        ).fetchone()


async def missing_bucket(root, store, api, storage):
    db = root / "missing-bucket.sqlite3"
    body = b"resume the original zero-byte task"
    with proxy(storage) as (env, _, seen, *_):
        with chat(db, env):
            assert store.bucket in {bucket["Name"] for bucket in api.list_buckets()["Buckets"]}
            assert seen["PUT bucket"] == 1 and seen["GET uploads"] == 2
        api.put_object(Bucket=store.bucket, Key="existing.txt", Body=b"keep")
        with chat(db, env):
            assert seen["PUT bucket"] == 1 and seen["GET uploads"] == 3
            assert api.get_object(Bucket=store.bucket, Key="existing.txt")["Body"].read() == b"keep"
        bad = dict(env, CHAT_S3_SECRET_KEY="wrong-secret-for-isolated-test")
        with chat(db, bad):
            assert seen["PUT bucket"] == 1
        api.delete_object(Bucket=store.bucket, Key="existing.txt")
    print("PASS startup creates missing bucket, restart preserves it, auth failure never creates a bucket")

    with chat(db, storage) as url:
        # Storage can disappear after startup. Keep exercising real recovery of
        # the previously paused task in the isolated, still empty bucket.
        api.delete_bucket(Bucket=store.bucket)
        async with AsyncExitStack() as stack:
            sender, conv = await direct(stack, url)
            response = await sender.ask(
                "begin_attachment",
                conversation=conv,
                client_id=30,
                filename="recover.bin",
                media_type="application/octet-stream",
                size=len(body),
                checksum=hashlib.sha256(body).hexdigest(),
            )
            assert response["error"]["message"] == "Could not initiate multipart upload"
            await sender.ok("pause_attachment", upload=30)
            status = await sender.ok("query_attachment", upload=30)
            assert status["state"] == "paused" and status["confirmed"] == 0
            key, multipart = upload_row(db, 30)
            assert not multipart

            api.create_bucket(
                Bucket=store.bucket,
                CreateBucketConfiguration={"LocationConstraint": store.region},
            )
            part = await sender.ok("resume_attachment", upload=30)
            assert upload_row(db, 30)[0] == key
            tag = await asyncio.to_thread(put, part["url"], body)
            await sender.ok("confirm_attachment", upload=30, number=part["number"], etag=tag)
            message = await sender.ok("finish_attachment", upload=30)
            assert (await sender.ok("finish_attachment", upload=30))["id"] == message["id"]
            assert api.get_object(Bucket=store.bucket, Key=key)["Body"].read() == body
    print("PASS missing Silo bucket blocks initiation; creating the isolated bucket recovers the original paused zero-byte task")


async def protocol(root, store, api, storage):
    with proxy(storage) as (env, faults, seen, *_):
        db = root / "wire.sqlite3"
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                a, conv = await direct(stack, url)
                body = b"real Silo wire"
                args = dict(
                    conversation=conv,
                    client_id=31,
                    filename="file.bin",
                    media_type="application/octet-stream",
                    size=len(body),
                    checksum=hashlib.sha256(body).hexdigest(),
                )
                faults["POST uploads"] = "lose-response"
                assert "error" in await a.ask("begin_attachment", **args)
                key, mpu = upload_row(db, 31)
                assert not mpu
                listing = api.list_multipart_uploads(Bucket=store.bucket)["Uploads"]
                saved = next(x["UploadId"] for x in listing if x["Key"] == key)
                part = await a.ok("begin_attachment", **args)
                assert upload_row(db, 31) == (key, saved)
                assert seen["POST uploads"] == 1
                args = parse_qs(urlsplit(part["url"]).query)
                assert args["X-Amz-Expires"] == ["900"]
                assert args["uploadId"] == [saved] and args["partNumber"] == ["1"]
                tag = await asyncio.to_thread(put, part["url"], body)
                # Lose the client confirmation after real PUT. ListParts finds it.
                state = await a.ok("resume_attachment", upload=31)
                assert state["number"] == 0
                assert (
                    api.list_parts(Bucket=store.bucket, Key=key, UploadId=saved)[
                        "Parts"
                    ][0]["ETag"].strip('"')
                    == tag
                )
                faults["POST multipart"] = "200-error"
                assert "error" in await a.ask("finish_attachment", upload=31)
                assert upload_row(db, 31) == (key, saved)
                assert not api.list_objects_v2(Bucket=store.bucket, Prefix=key).get(
                    "Contents"
                )
                faults["POST multipart"] = "lose-response"
                assert "error" in await a.ask("finish_attachment", upload=31)
                head = api.head_object(Bucket=store.bucket, Key=key)
                assert (
                    head["ContentLength"] == len(body)
                    and head["Metadata"]["sha256"] == hashlib.sha256(body).hexdigest()
                )
                completed = await a.ok("finish_attachment", upload=31)
                assert (await a.ok("finish_attachment", upload=31))["id"] == completed[
                    "id"
                ]
                assert seen["POST multipart"] == 2, seen
                grant = await a.ok(
                    "get_attachment", conversation=conv, message=completed["id"]
                )
                assert parse_qs(urlsplit(grant["url"]).query)["X-Amz-Expires"] == [
                    "120"
                ]
                assert (
                    await asyncio.to_thread(
                        lambda: urlopen(grant["url"], timeout=5).read()
                    )
                    == body
                )
                await a.ok("delete_message", conversation=conv, message=completed["id"])
                await begin(a, conv, 32, body)
                await a.ok("cancel_attachment", upload=32)
                assert seen["DELETE multipart"] == 1
                with sqlite3.connect(db) as conn:
                    assert conn.execute(
                        "select count(*) from messages where client_id=31"
                    ).fetchone() == (1,)
        with chat(db, env):
            assert not api.list_objects_v2(Bucket=store.bucket, Prefix=key).get(
                "Contents"
            )
            assert seen["DELETE"] >= 1
    print(
        "PASS real Silo: lost Initiate, PUT without confirmation, 200 Error, lost Complete (HEAD before replay), idempotent publish, Abort/Delete, 900/120s"
    )


def certs(root):
    def openssl(*args):
        subprocess.run(["openssl", *args], cwd=root, check=True, capture_output=True)

    openssl(
        "req",
        "-new",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-x509",
        "-days",
        "1",
        "-subj",
        "/CN=Chat acceptance CA",
        "-keyout",
        "ca.key",
        "-out",
        "ca.pem",
    )
    openssl(
        "req",
        "-new",
        "-newkey",
        "rsa:2048",
        "-nodes",
        "-subj",
        "/CN=Chat acceptance",
        "-keyout",
        "node.key",
        "-out",
        "node.csr",
    )
    (root / "ext").write_text(
        "subjectAltName=IP:127.0.0.1\nextendedKeyUsage=serverAuth\n"
    )
    openssl(
        "x509",
        "-req",
        "-in",
        "node.csr",
        "-CA",
        "ca.pem",
        "-CAkey",
        "ca.key",
        "-CAcreateserial",
        "-days",
        "1",
        "-extfile",
        "ext",
        "-out",
        "node.pem",
    )
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(root / "node.pem", root / "node.key")
    return tls


async def save(ui, root, conv, mid, name):
    await ui.call("preview", conv=conv, id=mid, filename=name, inline=False)
    await ui.wait("dialogstate", lambda r: r["shown"])
    await ui.call("dialogstate", save=True, destination=str(root / name))
    await ui.call("dialogstate", close=True)
    await ui.call("downloads", open=True)


async def https(root, probe, storage):
    def seen_http(seen):
        return sum(seen.values())

    with proxy(storage, certs(root)) as (
        env,
        faults,
        seen,
        get_stalled,
        get_closed,
        put_stalled,
        put_closed,
    ):
        for origin, trust in (
            (env, ""),
            (
                dict(
                    env,
                    CHAT_S3_ENDPOINT=env["CHAT_S3_ENDPOINT"].replace(
                        "127.0.0.1", "localhost"
                    ),
                ),
                str(root / "ca.pem"),
            ),
        ):
            db = root / f"rejected-{bool(trust)}.sqlite3"
            with patch.dict(os.environ, SSL_CERT_FILE=trust), chat(db, origin) as url:
                async with AsyncExitStack() as stack:
                    a, conv = await direct(stack, url)
                    reply = await a.ask(
                        "begin_attachment",
                        conversation=conv,
                        client_id=41,
                        filename="forbidden.bin",
                        media_type="application/octet-stream",
                        size=1,
                        checksum=hashlib.sha256(b"x").hexdigest(),
                    )
                    assert "error" in reply
                    assert not seen_http(seen)

        db = root / "tls.sqlite3"
        payload = b"TLS, not an unverified HTTP substitute" * 10000
        path = root / "tls.bin"
        path.write_bytes(payload)
        with (
            patch.dict(os.environ, SSL_CERT_FILE=str(root / "ca.pem")),
            chat(db, env) as url,
        ):
            async with AsyncExitStack() as stack:
                _, conv = await direct(stack, url)
                gate = Gate(url)
                relay = await stack.enter_async_context(
                    serve(gate.handle, "127.0.0.1", 0)
                )
                entry = f"ws://127.0.0.1:{relay.sockets[0].getsockname()[1]}/ws"
                ui = Desktop()
                await ui.start(probe, root / "trusted", entry)
                stack.push_async_callback(ui.stop)
                await ui.wait("select", lambda r: r["ok"], conv=conv)
                await ui.call("upload", conv=conv, path=str(path))

                def first_message():
                    with sqlite3.connect(db) as conn:
                        return conn.execute(
                            "select message_id from message_attachments"
                        ).fetchone()

                (mid,) = await until(first_message, "real HTTPS upload and publication")
                assert seen["Content-MD5"] == 1
                await save(ui, root, conv, mid, "trusted.bin")
                await ui.wait("downloads", lambda r: r["rows"][0]["status"] == "已完成")
                assert (root / "trusted.bin").read_bytes() == payload

                before = seen["GET"]
                with patch.dict(os.environ, SSL_CERT_FILE=""):
                    denied = Desktop()
                    await denied.start(probe, root / "untrusted", url)
                    stack.push_async_callback(denied.stop)
                    await denied.wait("select", lambda r: r["ok"], conv=conv)
                    await save(denied, root, conv, mid, "untrusted.bin")
                    await denied.wait(
                        "downloads",
                        lambda r: r["rows"][0]["status"].startswith("失败："),
                    )
                    assert (
                        not (root / "untrusted.bin").exists() and seen["GET"] == before
                    )
                    await denied.stop()

                def destination(netloc):
                    def rewrite(method, response):
                        if method == "get_attachment" and (
                            grant := response.get("result")
                        ):
                            grant["url"] = (
                                urlsplit(grant["url"])._replace(netloc=netloc).geturl()
                            )

                    return rewrite

                before = seen["GET"]
                wrong_host = env["CHAT_S3_ENDPOINT"].replace("127.0.0.1", "localhost")
                with socket.socket() as unopened:
                    unopened.bind(
                        ("127.0.0.1", 0)
                    )  # This port deliberately does not listen.
                    no_listener = f"127.0.0.1:{unopened.getsockname()[1]}"
                    for idx, name, addr in (
                        (1, "wrong-name.bin", wrong_host),
                        (2, "refused.bin", no_listener),
                    ):
                        gate.transform = destination(addr)
                        await save(ui, root, conv, mid, name)
                        await ui.wait(
                            "downloads",
                            lambda r: r["rows"][idx]["status"].startswith("失败："),
                        )
                        assert not (root / name).exists() and seen["GET"] == before
                gate.transform = None

                large = root / "paused-tls.bin"
                with large.open("wb") as part_data:
                    part_data.truncate(17 * 1024**2)
                before = seen["PUT multipart"]
                faults["PUT multipart"] = "stall-response"
                await ui.call("upload", conv=conv, path=str(large))
                await until(
                    put_stalled.is_set, "Silo persisted the first PUT before pause"
                )
                state = await ui.wait(
                    "transfers",
                    lambda r: any(large.name in item for item in r["labels"]),
                )
                label = next(item for item in state["labels"] if large.name in item)
                assert (await ui.call("transfers", label=label, action="暂停上传"))[
                    "triggered"
                ]
                await asyncio.wait_for(
                    until(put_closed.is_set, "paused TLS PUT closed"), 3
                )

                def paused():
                    with sqlite3.connect(db) as conn:
                        return conn.execute(
                            "select 1 from attachment_uploads where state='paused'"
                        ).fetchone()

                await until(
                    paused, "S3 upload pauses while its response is still withheld"
                )
                state = await ui.wait(
                    "transfers",
                    lambda r: any(large.name in item for item in r["labels"]),
                )
                label = next(item for item in state["labels"] if large.name in item)
                gate.messages.clear()
                assert (await ui.call("transfers", label=label, action="继续上传"))[
                    "triggered"
                ]

                def published():
                    with sqlite3.connect(db) as conn:
                        return conn.execute(
                            "select 1 from message_attachments where message_id>?",
                            (mid,),
                        ).fetchone()

                await until(published, "TLS upload resumes and publishes")
                confirms = [
                    r["params"]["number"]
                    for r in gate.messages
                    if r["method"] == "confirm_attachment"
                ]
                assert confirms == [2], confirms
                assert seen["PUT multipart"] == before + 2
                print(
                    "PASS Qt pauses a stalled TLS PUT response, closes its socket, resumes with part 2 only"
                )

                faults["GET"] = "stall"
                await save(ui, root, conv, mid, "exit.bin")
                await until(get_stalled.is_set, "first TLS body block")
                await ui.call("quit")
                assert await asyncio.wait_for(ui.proc.wait(), 3) == 0
                await asyncio.wait_for(until(get_closed.is_set, "TLS socket closed"), 3)
                assert not (root / "exit.bin").exists() and not list(
                    root.glob("*.part-*")
                )
    print(
        "PASS HTTPS: server rejects untrusted CA and wrong host; Qt rejects untrusted CA, wrong host and refused connection; trusted PUT MD5 + download; exit closes stalled TLS socket and cleans temp"
    )


async def run(root, probe):
    store = Store(region="us-west-2")
    store.bucket = "chat-wire-test"
    with store.node(root / "silo") as (api, storage):
        await missing_bucket(root, store, api, storage)
        await protocol(root, store, api, storage)
        await https(root, probe, storage)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-s3-wire-") as path:
        asyncio.run(run(Path(path), sys.argv[1]))
