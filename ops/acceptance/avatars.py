"""PNG avatar validation via real RPC and Qt normalization in isolated clients."""

import asyncio
from contextlib import AsyncExitStack
import base64
from pathlib import Path
import random
import sqlite3
import struct
import sys
import tempfile
import time
import zlib
from urllib.request import urlopen

from desktop import Desktop, until
from fixture import fixture
from group_profiles import png, prepare_avatar
from isolated import Store, chat, login

SIGNATURE = b"\x89PNG\r\n\x1a\n"
# 7×4 solid green JPEG fixture, generated once with ImageMagick.
JPEG = base64.b64decode(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYF"
    "BgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/2wBDAQICAgICAgUDAwUKBwYHCgoKCgoK"
    "CgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgoKCgr/wAARCAAEAAcDAREA"
    "AhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACP/EABQQAQAAAAAAAAAAAAAAAAAAAAD/xAAVAQEB"
    "AAAAAAAAAAAAAAAAAAAHCP/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/ACeC0hv/"
    "2Q=="
)


def chunk(kind, body=b""):
    return (
        struct.pack(">I", len(body))
        + kind
        + body
        + struct.pack(">I", zlib.crc32(kind + body))
    )


def header(width=256, height=256, color=2):
    return chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, color, 0, 0, 0))


def image(width, height, rows, color=2):
    return (
        SIGNATURE
        + header(width, height, color)
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND")
    )


def png_size(data):
    assert data.startswith(SIGNATURE)
    return struct.unpack_from(">2I", data, 16)


def stored(db, user):
    with sqlite3.connect(db) as conn:
        return conn.execute(
            """select u.avatar_revision,a.media_type,a.data from users u
            left join user_avatars a on a.user_id=u.id where u.id=?""",
            (user,),
        ).fetchone()


async def validation(db, a):
    pixels = random.Random(2026).randbytes(256 * 256 * 3)
    rgb = image(
        256,
        256,
        b"".join(b"\0" + pixels[n : n + 768] for n in range(0, len(pixels), 768)),
    )
    rgba = image(256, 256, (b"\0" + bytes((255, 0, 0, 128)) * 256) * 256, 6)
    assert len(rgb) > 32768
    for value in (rgb, rgba):
        reply = await a.ok(
            "finish_avatar_upload", upload=await prepare_avatar(a, value)
        )
        assert stored(db, 1) == (reply["avatar_revision"], "image/png", value)
    before = stored(db, 1)
    assert "error" in await a.ask("begin_avatar_upload", size=1024 * 1024 + 1)
    rows = (b"\0" + b"\x88" * 768) * 256
    encoded = zlib.compress(rows)
    prefix = SIGNATURE + header()
    end = chunk(b"IEND")
    crc = chunk(b"IDAT", encoded)
    ancillary = chunk(b"tEXt", b"Title\0Local regression")
    corruptions = {
        "signature": b"B" + rgb[1:],
        "JPEG": JPEG,
        "dimensions": png((0, 0, 0), 4),
        "IHDR": SIGNATURE + chunk(b"IHDR", b"short") + crc + end,
        "IDAT CRC": prefix + crc[:-1] + bytes([crc[-1] ^ 1]) + end,
        "ancillary CRC": prefix
        + ancillary[:-1]
        + bytes([ancillary[-1] ^ 1])
        + crc
        + end,
        "Adler32": prefix
        + chunk(b"IDAT", encoded[:-1] + bytes([encoded[-1] ^ 1]))
        + end,
        "incomplete stream": prefix
        + chunk(b"IDAT", encoded[: len(encoded) // 2])
        + end,
        "missing IEND": prefix + crc,
        "bad IEND": prefix + crc + end[:-1] + bytes([end[-1] ^ 1]),
        "trailing data": prefix + crc + end + b"more",
        "animated PNG": prefix + chunk(b"acTL", struct.pack(">II", 1, 0)) + crc + end,
        "excess scanlines (compressed)": prefix
        + chunk(b"IDAT", zlib.compress(rows * 24))
        + end,
        "excess scanlines (stored blocks)": prefix
        + chunk(b"IDAT", zlib.compress(rows * 4, 0))
        + end,
    }
    for label, value in corruptions.items():
        key = await prepare_avatar(a, value)
        started = time.monotonic()
        result = await asyncio.wait_for(a.ask("finish_avatar_upload", upload=key), 3)
        assert result.get("error", {}).get("code") == -32602, (label, result)
        assert stored(db, 1) == before
        if label.startswith("excess"):
            print(
                f"{label}: {len(value)} B file rejected in {(time.monotonic() - started) * 1000:.2f} ms"
            )
    print(
        "PASS real RPC: RGB/RGBA, 32 KiB chunks, size and integrity checks; reject without replacing avatar/revision"
    )


def is_green(name):
    return name and int(name[5:7], 16) > 110 and int(name[3:5], 16) < 50


async def qt_run(db, a, probe, root):
    # Seed old blobs directly: these formats are readable, not uploadable.
    with sqlite3.connect(db) as conn:
        for user, media, data in [
            (1, "image/jpeg", JPEG),
            (2, "image/png", png((0, 0, 255), 4)),
        ]:
            conn.execute("update users set avatar_revision=10 where id=?", (user,))
            conn.execute(
                "insert or replace into user_avatars values(?,?,?,?,0)",
                (user, media, len(data), data),
            )

    url = f"ws://127.0.0.1:{a.ws.remote_address[1]}/ws"
    src_png, src_jpg, high, bad = [
        root / name for name in ("crop.png", "small.jpg", "high.png", "not.png")
    ]
    center = bytes((255, 0, 0, 128))
    edge = bytes((0, 255, 0, 255))
    src_png.write_bytes(image(12, 4, (b"\0" + edge * 4 + center * 4 + edge * 4) * 4, 6))
    src_jpg.write_bytes(JPEG)
    high.write_bytes(image(2048, 1024, (b"\0" + bytes((0, 0, 255)) * 2048) * 1024))
    bad.write_bytes(b"not an image")
    originals = {path: path.read_bytes() for path in (src_png, src_jpg, high, bad)}
    actor, peer = Desktop(), Desktop()

    async def profile(ui, directory, user="a", user_id=1):
        await ui.start(probe, directory, url, user=user, user_id=user_id)
        await ui.call("contacts", open=True)
        await ui.call("contacts", selfProfile=True)
        await ui.wait("contacts", lambda r: r.get("avatarEditable"))

    async def observed(ui, uid, check):
        return await ui.wait(
            "contacts", lambda r: check(r.get("avatarPixel")), avatarUser=uid
        )

    try:
        await profile(actor, root / "actor")
        await profile(peer, root / "peer", "b", 2)
        await observed(actor, 1, is_green)
        await observed(actor, 2, lambda c: c == "#ff0000ff")
        for rev, path, check in (
            (11, src_png, lambda c: c == "#80ff0000"),
            (12, src_jpg, is_green),
            (13, high, lambda c: c == "#ff0000ff"),
        ):
            await actor.call("contacts", avatar=str(path))
            await until(lambda: stored(db, 1)[0] == rev, "normalized upload saved")
            _, media, data = stored(db, 1)
            assert media == "image/png" and png_size(data) == (256, 256)
            assert data[25] == (6 if path == src_png else 2)
            await observed(peer, 1, check)
        before = stored(db, 1)
        await actor.call("contacts", avatar="")
        await actor.wait("contacts", lambda r: r.get("avatarEditable"))
        assert stored(db, 1) == before
        await actor.call("contacts", avatar=str(bad))
        await actor.wait(
            "contacts", lambda r: "请选择完整" in r.get("avatarStatus", "")
        )
        assert stored(db, 1) == before
        assert all(path.read_bytes() == data for path, data in originals.items())

        await actor.stop()
        await profile(actor, root / "actor")
        await observed(actor, 1, lambda c: c == "#ff0000ff")
        await actor.call("contacts", clearAvatar=True)
        await until(lambda: stored(db, 1) == (14, None, None), "Qt clears avatar")
        await observed(peer, 1, lambda c: c is None)
    finally:
        await actor.stop()
        await peer.stop()
    print(
        "PASS Qt real chooser: legacy JPEG and PNG, crop/alpha, small JPEG, high-res PNG, cancel, invalid, peer refresh, restart, and clear"
    )


async def attachments(root, probe):
    store = Store()
    with store.node(root / "silo") as (api, storage):
        api.create_bucket(Bucket=store.bucket)
        with chat(root / "attachments.sqlite3", storage) as url:
            async with AsyncExitStack() as stack:
                a = await login(stack, url, "a", "a", True)
                b = await login(stack, url, "b", "b", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                conv = (await a.ok("open_direct_conversation", user=2))["conversation"]
                ui = Desktop()
                await ui.start(probe, root / "attachments-ui", url)
                stack.push_async_callback(ui.stop)
                await ui.wait("select", lambda r: r["ok"], conv=conv)
                for name, media, data in (
                    ("message.png", "image/png", png((0, 0, 255), 4)),
                    ("message.jpg", "image/jpeg", JPEG),
                ):
                    path = root / name
                    path.write_bytes(data)
                    await ui.call("upload", conv=conv, path=str(path))

                    async def published():
                        page = await b.ok("get_messages", conversation=conv)
                        return next(
                            (
                                m
                                for m in page["messages"]
                                if m.get("attachment", {}).get("filename") == name
                            ),
                            None,
                        )

                    saved = await until(published, f"{name} published by actual SDK")
                    assert saved["attachment"]["media_type"] == media
                    grant = await b.ok(
                        "get_attachment", conversation=conv, message=saved["id"]
                    )
                    with urlopen(grant["url"], timeout=5) as stream:
                        assert stream.read() == data
                    await ui.call("preview", conv=conv, id=saved["id"], filename=name)
                    await ui.wait("dialogstate", lambda r: r.get("image"))
                    await ui.call("dialogstate", close=True)
    print(
        "PASS Qt/SDK/Silo: small PNG and JPEG message attachments retain original bytes and preview"
    )


async def run(probe):
    with tempfile.TemporaryDirectory(prefix="chat-avatar-") as scratch:
        async with fixture() as (db, _, a, _, _, _):
            await validation(db, a)
            await qt_run(db, a, probe, Path(scratch))
        await attachments(Path(scratch), probe)


if __name__ == "__main__":
    asyncio.run(run(sys.argv[1]))
