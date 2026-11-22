"""Real Qt download tasks over an isolated, controllable HTTP stream."""

import asyncio
from collections import Counter
from contextlib import AsyncExitStack
from pathlib import Path
import sys
import tempfile

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put


async def run(root, probe):
    store = Store()
    payload = b"download verification" * (64 * 1024)
    requests = []
    send_chunks = asyncio.Event()

    async def stream(reader, writer):
        request = (await reader.readline()).decode().split()
        path = request[1]
        while await reader.readline() not in (b"\r\n", b""):
            pass
        requests.append(path)
        writer.write(
            f"HTTP/1.1 200 OK\r\nContent-Length: {len(payload)}\r\nConnection: close\r\n\r\n".encode()
        )
        writer.write(payload[:65536])
        await writer.drain()
        if not path.endswith("/fail"):
            await send_chunks.wait()
            try:
                writer.write(payload[65536:])
                await writer.drain()
            except ConnectionError:
                pass
        writer.close()
        try:
            await writer.wait_closed()
        except ConnectionError:
            pass

    async with await asyncio.start_server(stream, "127.0.0.1", 0) as data:
        endpoint = f"http://127.0.0.1:{data.sockets[0].getsockname()[1]}"
        with store.node(root / "silo") as (api, storage):
            api.create_bucket(Bucket=store.bucket)
            with chat(root / "chat.sqlite3", storage) as upstream:
                async with AsyncExitStack() as stack:
                    a = await login(stack, upstream, "a", "a", True)
                    b = await login(stack, upstream, "b", "b", True)
                    await a.ok("send_friend_request", user=2)
                    await b.ok("respond_friend_request", user=1, accept=True)
                    direct = (await a.ok("open_direct_conversation", user=2))[
                        "conversation"
                    ]
                    group = (await b.ok("create_group", title="下载组", members=[1]))[
                        "conversation"
                    ]
                    originals = []
                    for conv, ident in ((direct, 41), (group, 42)):
                        grant = await begin(
                            b,
                            conv,
                            ident,
                            payload,
                            "同名.bin",
                            "application/octet-stream",
                        )
                        await b.ok(
                            "confirm_attachment",
                            upload=ident,
                            number=1,
                            etag=put(grant["url"], payload),
                        )
                        originals.append(await b.ok("finish_attachment", upload=ident))
                    await b.ok(
                        "add_conversation_file",
                        conversation_id=group,
                        object_key=originals[1]["attachment"]["object_key"],
                    )
                    actual = Counter()
                    gate = Gate(upstream)

                    def transform(method, result):
                        if (
                            method not in ("get_attachment", "get_conversation_file")
                            or "result" not in result
                        ):
                            return
                        actual[method] += 1
                        suffix = (
                            "fail"
                            if method == "get_attachment" and actual[method] in (2, 4)
                            else "ok"
                        )
                        result["result"]["url"] = (
                            f"{endpoint}/{method}/{actual[method]}/{suffix}"
                        )

                    gate.transform = transform
                    async with serve(gate.handle, "127.0.0.1", 0) as proxy:
                        ui = Desktop()
                        entry = f"ws://127.0.0.1:{proxy.sockets[0].getsockname()[1]}/ws"
                        await ui.start(probe, root, entry)
                        try:
                            for f, conv, path in zip(
                                originals, [direct, group], ["a.bin", "b.bin"]
                            ):
                                await ui.call(
                                    "preview",
                                    conv=conv,
                                    id=f["id"],
                                    filename="同名.bin",
                                    inline=False,
                                )
                                await ui.wait("dialogstate", lambda r: r["shown"])
                                await ui.call(
                                    "dialogstate",
                                    save=True,
                                    destination=str(root / path),
                                )
                                await ui.call("dialogstate", close=True)
                            await ui.call("group", id=group)
                            await ui.wait(
                                "group_download", lambda r: r.get("count", 0) == 1
                            )
                            await ui.call(
                                "group_download", destination=str(root / "c.bin")
                            )
                            await ui.call("group_download", close=True)
                            await ui.call("downloads", open=True)
                            rows = (
                                await ui.wait(
                                    "downloads", lambda r: len(r.get("rows", [])) == 3
                                )
                            )["rows"]
                            assert len({v["id"] for v in rows}) == 3
                            assert {v["filename"] for v in rows} == {"同名.bin"}
                            assert (
                                "群文件" in rows[2]["source"]
                                and rows[0]["source"] != rows[1]["source"]
                            )
                            await until(
                                lambda: len(requests) >= 2,
                                "two distinct HTTP downloads started",
                            )
                            await ui.wait(
                                "downloads",
                                lambda r: r["rows"][1]["status"].startswith("失败"),
                            )
                            assert (await ui.call("downloads", row=0, action="取消"))[
                                "triggered"
                            ]
                            send_chunks.set()
                            assert (await ui.call("downloads", row=1, action="重试"))[
                                "triggered"
                            ]
                            rows = (
                                await ui.wait(
                                    "downloads",
                                    lambda r: [v["status"] for v in r["rows"]]
                                    == ["已取消", "已完成", "已完成"],
                                )
                            )["rows"]
                            assert rows[1]["id"] and rows[2]["progress"]
                            assert actual["get_attachment"] == 3
                            assert not (root / "a.bin").exists()
                            assert (
                                (root / "b.bin").read_bytes()
                                == (root / "c.bin").read_bytes()
                                == payload
                            )
                            await until(
                                lambda: not list(root.glob("*.part-*")),
                                "no partial files",
                            )
                            await ui.call(
                                "preview",
                                conv=group,
                                id=originals[1]["id"],
                                filename="同名.bin",
                                inline=False,
                            )
                            await ui.wait("dialogstate", lambda r: r["shown"])
                            await ui.call(
                                "dialogstate",
                                save=True,
                                destination=str(root / "denied.bin"),
                            )
                            await ui.call("dialogstate", close=True)
                            await ui.wait(
                                "downloads",
                                lambda r: len(r.get("rows", [])) == 4
                                and r["rows"][3]["status"].startswith("失败"),
                            )
                            await b.ok(
                                "delete_message",
                                conversation=group,
                                message=originals[1]["id"],
                            )
                            assert (await ui.call("downloads", row=3, action="重试"))[
                                "triggered"
                            ]
                            await ui.wait(
                                "downloads",
                                lambda r: r["rows"][3]["status"].startswith("失败"),
                            )
                            assert (
                                actual["get_attachment"] == 4
                                and not (root / "denied.bin").exists()
                            )
                            assert "url" in await a.ok(
                                "get_conversation_file",
                                conversation_id=group,
                                object_key=originals[1]["attachment"]["object_key"],
                            )
                            print(
                                "PASS three isolated SDK downloads, same-name IDs, targeted cancellation, failed stream, fresh authorized retry and denied revoked reference",
                                flush=True,
                            )
                        finally:
                            send_chunks.set()
                            await ui.stop()


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-downloads-") as path:
        asyncio.run(run(Path(path), sys.argv[1]))
