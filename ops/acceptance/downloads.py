"""Real Qt download tasks over an isolated, controllable HTTP stream."""

import asyncio
from collections import Counter
from contextlib import AsyncExitStack
from pathlib import Path
import sys
import tempfile
import time

from websockets.asyncio.server import serve

from desktop import Desktop, Gate, until
from isolated import Store, begin, chat, login, put


async def run(root, probe, stalls=False, baseline=False):
    store = Store()
    payload = b"download verification" * (64 * 1024)
    requests = []
    send_chunks = asyncio.Event()
    disconnected = set()
    final_chunk = asyncio.Event()

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
        if path.endswith("/stall"):
            # Do not send any more bytes or close first. EOF proves the SDK interrupted its socket.
            if not await reader.read(1):
                disconnected.add(path)
        elif not path.endswith("/fail"):
            if path.endswith("/race"):
                await final_chunk.wait()
            elif not path.endswith("/ready"):
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
                        if (
                            not stalls
                            and method == "get_attachment"
                            and actual[method] == 7
                        ):
                            return  # Overwrite from the real Silo URL.
                        suffix = (
                            "fail"
                            if method == "get_attachment"
                            and actual[method] in (2, 4, 5)
                            else "stall"
                            if method == "get_attachment" and actual[method] == 6
                            else "ok"
                        )
                        if stalls:
                            number = actual[method]
                            suffix = (
                                "stall"
                                if number in (1, 2, 6, 7, 8)
                                else "race"
                                if number == 4
                                else "ready"
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
                            if stalls:
                                await stalled_downloads(
                                    root,
                                    ui,
                                    gate,
                                    a,
                                    direct,
                                    originals[0],
                                    requests,
                                    disconnected,
                                    final_chunk,
                                    payload,
                                    baseline,
                                )
                                return
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
                            replacement = b"x" * len(
                                payload
                            )  # Different bytes, same stream length.
                            grant = await begin(
                                b, group, 43, replacement, "replacement.bin"
                            )
                            await b.ok(
                                "confirm_attachment",
                                upload=43,
                                number=1,
                                etag=put(grant["url"], replacement),
                            )
                            new = await b.ok("finish_attachment", upload=43)

                            async def replace():
                                await ui.call(
                                    "preview",
                                    conv=group,
                                    id=new["id"],
                                    filename="replacement.bin",
                                    inline=False,
                                )
                                await ui.wait("dialogstate", lambda r: r["shown"])
                                await ui.call(
                                    "dialogstate",
                                    save=True,
                                    overwrite=True,
                                    destination=str(root / "b.bin"),
                                )
                                await ui.call("dialogstate", close=True)

                            async def assert_original():
                                assert (root / "b.bin").read_bytes() == payload
                                state = await ui.call(
                                    "downloads", row=1, enabled="打开文件"
                                )
                                assert (
                                    state["enabled"]
                                    and state["rows"][1]["status"] == "已完成"
                                )

                            await replace()
                            await ui.wait(
                                "downloads",
                                lambda r: len(r["rows"]) == 5
                                and r["rows"][4]["status"].startswith("失败"),
                            )
                            await assert_original()
                            await replace()
                            await until(
                                lambda: "/get_attachment/6/stall" in requests
                                and any(
                                    p.stat().st_size for p in root.glob("b.bin.part-*")
                                ),
                                "replacement received its first block",
                            )
                            assert not (
                                await ui.call("downloads", row=1, enabled="打开文件")
                            )["enabled"]
                            assert (await ui.call("downloads", row=5, action="取消"))[
                                "triggered"
                            ]
                            await until(
                                lambda: "/get_attachment/6/stall" in disconnected
                                and not list(root.glob("b.bin.part-*")),
                                "replacement cancellation closed the stream and cleaned its temporary file",
                            )
                            await assert_original()
                            await ui.call("download_barrier", hold=True)
                            await replace()
                            await ui.wait("download_barrier", lambda r: r["pending"])
                            assert (root / "b.bin").read_bytes() == replacement
                            assert not (
                                await ui.call("downloads", row=1, enabled="打开文件")
                            )["enabled"]
                            await ui.call("download_barrier", release=True)
                            await ui.wait(
                                "downloads",
                                lambda r: len(r["rows"]) == 7
                                and r["rows"][6]["status"] == "已完成",
                            )
                            replaced = await ui.call(
                                "downloads", row=1, enabled="打开文件"
                            )
                            assert not replaced["enabled"]
                            assert replaced["rows"][1]["status"] == "已被其他下载覆盖"
                            assert (
                                await ui.call(
                                    "downloads", row=1, enabled="打开所在目录"
                                )
                            )["enabled"]
                            assert (
                                await ui.call("downloads", row=6, enabled="打开文件")
                            )["enabled"]
                            assert (
                                await ui.call("downloads", row=2, enabled="打开文件")
                            )["enabled"]
                            print(
                                "PASS isolated SDK downloads, failed/cancelled overwrite preserves original, successful overwrite transfers ownership",
                                flush=True,
                            )

                            await b.ok(
                                "set_group_admin",
                                conversation=group,
                                user=1,
                                admin=True,
                            )
                            await ui.call("group", id=group)
                            await ui.wait(
                                "group_download", lambda r: r.get("count") == 1
                            )
                            await ui.call("group_download", choose=True)
                            await ui.wait("group_download", lambda r: r.get("choosing"))
                            key = originals[1]["attachment"]["object_key"]
                            await ui.call("group_download", group=group, remove_key=key)
                            await ui.wait(
                                "group_download",
                                lambda r: r.get("choosing") and r["count"] == 0,
                            )
                            start = len(gate.messages)
                            await ui.call(
                                "group_download", accept_path=str(root / "removed.bin")
                            )
                            await ui.wait(
                                "group_download", lambda r: not r.get("choosing")
                            )
                            await ui.call("group_download", close=True)
                            tasks = await ui.call("downloads")
                            assert len(tasks["rows"]) == 7, tasks
                            assert not any(
                                m["method"] == "get_conversation_file"
                                for m in gate.messages[start:]
                            )
                            assert not (root / "removed.bin").exists()
                            print(
                                "PASS the selected group file disappeared while the save dialog was open; no stale task",
                                flush=True,
                            )
                        finally:
                            send_chunks.set()
                            final_chunk.set()
                            await ui.stop()


async def stalled_downloads(
    root,
    ui,
    gate,
    account,
    conv,
    original,
    requests,
    disconnected,
    final_chunk,
    payload,
    baseline,
):
    await ui.wait("select", lambda r: r["ok"], conv=conv)

    async def save(name):
        await ui.call(
            "preview", conv=conv, id=original["id"], filename="同名.bin", inline=False
        )
        await ui.wait("dialogstate", lambda r: r["shown"])
        await ui.call("dialogstate", save=True, destination=str(root / name))
        await ui.call("dialogstate", close=True)

    def received(name):
        return any(p.stat().st_size > 0 for p in root.glob(f"{name}.part-*"))

    def ended(number):
        return f"/get_attachment/{number}/stall" in disconnected

    async def cancel(row):
        return (await ui.call("downloads", row=row, action="取消")).get(
            "triggered", False
        )

    await save("stall-a.bin")
    await save("stall-b.bin")
    await until(
        lambda: received("stall-a.bin") and received("stall-b.bin"),
        "both worker downloads have received first block",
    )
    await save("next.bin")
    path = root / "待发送文件.bin"
    path.write_bytes(b"download workers must release the shared upload slot")
    await ui.call("upload", conv=conv, path=str(path))
    await ui.call("downloads", open=True)
    assert len(requests) == 2 and not (root / "next.bin").exists()
    mark = time.monotonic()
    assert await cancel(0)
    if baseline:
        try:
            await asyncio.wait_for(
                until(lambda: ended(1), "old cancelled socket unexpectedly closed"), 2
            )
        except asyncio.TimeoutError:
            assert received("stall-a.bin") and len(requests) == 2
            print(
                "RED: Qt marked both-worker stall cancelled but after 2s the socket and partial file remained",
                flush=True,
            )
            return
        raise AssertionError("baseline unexpectedly released its stalled TCP stream")

    await asyncio.wait_for(
        until(
            lambda: ended(1) and not list(root.glob("stall-a.bin.part-*")),
            "first socket EOF and temp cleaned",
        ),
        3,
    )
    first_ms = (time.monotonic() - mark) * 1000
    await ui.wait("downloads", lambda r: r["rows"][2]["status"] == "已完成")
    assert (root / "next.bin").read_bytes() == payload
    assert not ended(2) and received("stall-b.bin")

    async def uploaded():
        page = await account.ok("get_messages", conversation=conv)
        return any(
            m.get("attachment", {}).get("filename") == path.name
            for m in page["messages"]
        )

    await until(
        uploaded, "actual upload published while the other download stays stalled"
    )
    mark = time.monotonic()
    assert await cancel(1)
    await asyncio.wait_for(
        until(
            lambda: ended(2) and not list(root.glob("stall-b.bin.part-*")),
            "second socket EOF and temp cleaned",
        ),
        3,
    )
    second_ms = (time.monotonic() - mark) * 1000
    assert not any((root / name).exists() for name in ("stall-a.bin", "stall-b.bin"))

    await save("race.bin")
    await until(lambda: received("race.bin"), "race first block reached SDK")
    final_chunk.set()
    await cancel(3)
    result = await ui.wait(
        "downloads",
        lambda r: r["rows"][3]["status"] in ("已取消", "已完成"),
    )
    status = result["rows"][3]["status"]
    completed = status == "已完成"
    await until(lambda: not list(root.glob("race.bin.part-*")), "race temp removed")
    assert (root / "race.bin").exists() == completed
    if completed:
        assert (root / "race.bin").read_bytes() == payload
    await ui.call("download_barrier", hold=True)
    await save("published.bin")
    await ui.wait("download_barrier", lambda r: r["pending"])
    assert (root / "published.bin").read_bytes() == payload
    # File is already published while the UI still thinks the task is running.
    await cancel(4)  # A click is possible, but the phase guard must refuse it.
    assert (await ui.call("downloads"))["rows"][4]["status"] != "已取消"
    await ui.call("download_barrier", release=True)
    await ui.wait("downloads", lambda r: r["rows"][4]["status"] == "已完成")

    async def cleaned(number, name):
        await asyncio.wait_for(
            until(
                lambda: ended(number) and not list(root.glob(f"{name}.part-*")),
                f"{name}: socket EOF and temp cleaned",
            ),
            3,
        )
        assert not (root / name).exists()

    await save("disconnect.bin")
    await until(
        lambda: received("disconnect.bin"), "disconnect case received first block"
    )
    await gate.down.close()
    await cleaned(6, "disconnect.bin")
    await ui.wait("select", lambda r: r["ok"], conv=conv)
    await until(
        lambda: any(p["method"] == "resume_session" for p in gate.messages),
        "actual reconnect",
    )
    await ui.wait("state", lambda r: r["enabled"])

    await save("logout.bin")
    await until(lambda: received("logout.bin"), "logout case received first block")
    await ui.call("logout")
    await cleaned(7, "logout.bin")
    await ui.call("login", user="a")
    await ui.wait("state", lambda r: r["user"] == 1)
    await ui.wait("select", lambda r: r["ok"], conv=conv)

    await save("quit.bin")
    await until(lambda: received("quit.bin"), "exit case received first block")
    await ui.call("quit")
    assert await asyncio.wait_for(ui.proc.wait(), 3) == 0
    await cleaned(8, "quit.bin")

    print(
        f"PASS two Qt/SDK stalled sockets interrupted and temp cleaned in {first_ms:.1f}/{second_ms:.1f} ms; "
        f"upload and unaffected download advanced; race={status}; late cancel refused; disconnect/logout/quit cleaned",
        flush=True,
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-downloads-") as path:
        asyncio.run(
            run(
                Path(path),
                sys.argv[1],
                "--stalls" in sys.argv,
                "--baseline" in sys.argv,
            )
        )
