"""Contrast creation-age cleanup with Chat-owned multipart retention on loopback."""

import asyncio
import base64
from contextlib import AsyncExitStack
import os
from pathlib import Path
import shutil
import sqlite3
import tempfile
import time

from botocore.exceptions import ClientError

from desktop import until
from isolated import Store, begin, chat, login, put


def part_sizes(api, handle):
    try:
        page = api.list_parts(**handle)
    except ClientError as error:
        if error.response["Error"]["Code"] == "NoSuchUpload":
            return None
        raise
    return [(part["PartNumber"], part["Size"]) for part in page.get("Parts", [])]


def backdate_fixture_upload(volume, handle):
    """Change the pinned Silo disk ID's timestamp in a stopped, disposable volume."""
    encoded = handle["UploadId"]
    prefix, upload_dir = (
        base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4))
        .decode()
        .split(".", 1)
    )
    token, creation = upload_dir.split("x", 1)
    backdated = f"{token}x{int(creation) - 17 * 86400 * 10**9}"
    (path,) = (volume / ".minio.sys/multipart").glob(f"*/{upload_dir}")
    path.rename(path.with_name(backdated))
    handle["UploadId"] = (
        base64.urlsafe_b64encode(f"{prefix}.{backdated}".encode()).decode().rstrip("=")
    )


async def run(root):
    store = Store()
    db = root / "chat.sqlite3"
    volume = root / "retention"
    small = b"shared"
    large = b"T" * (17 * 1024 * 1024)

    def node():
        return store.node(volume, expiry="off", cleanup_interval="1s")

    with node() as (api, env):
        api.create_bucket(Bucket=store.bucket)
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                owner = await login(stack, url, "a", "a", True)
                peer = await login(stack, url, "b", "b", True)
                await owner.ok("send_friend_request", user=2)
                await peer.ok("respond_friend_request", user=1, accept=True)
                group = (await owner.ok("create_group", title="保留期", members=[2]))[
                    "conversation"
                ]
                auth = await begin(owner, group, 10, small)
                await owner.ok(
                    "confirm_attachment",
                    upload=10,
                    number=1,
                    etag=put(auth["url"], small),
                )
                complete = await owner.ok("finish_attachment", upload=10)
                shared_key = complete["attachment"]["object_key"]
                forwarded = await owner.ok(
                    "send_message", conversation=group, client_id=13, forward=complete["id"]
                )
                await owner.ok("delete_message", conversation=group, message=complete["id"])

                lengths, handles = {}, {}
                for ident in (11, 12):
                    auth = await begin(owner, group, ident, large)
                    with sqlite3.connect(db) as conn:
                        key, multipart, length = conn.execute(
                            "select object_key,multipart,part_size from attachment_uploads where client_id=?",
                            (ident,),
                        ).fetchone()
                    assert 0 < length < len(large)
                    await owner.ok(
                        "confirm_attachment",
                        upload=ident,
                        number=1,
                        etag=put(auth["url"], large[:length]),
                    )
                    await owner.ok("pause_attachment", upload=ident)
                    handles[ident] = dict(
                        Bucket=store.bucket, Key=key, UploadId=multipart
                    )
                    lengths[ident] = length

                for ident, handle in handles.items():
                    assert part_sizes(api, handle) == [(1, lengths[ident])]

    for handle in handles.values():
        backdate_fixture_upload(volume, handle)
    control = root / "creation-age-control"
    shutil.copytree(volume, control)
    with store.node(control, cleanup_interval="1s") as (api, _):
        await until(
            lambda: all(part_sizes(api, handle) is None for handle in handles.values()),
            "24h creation-age sweep removed the old fixture uploads",
        )
        assert (
            api.get_object(Bucket=store.bucket, Key=shared_key)["Body"].read() == small
        )

    now = int(time.time() * 1000)
    with sqlite3.connect(db) as conn:
        assert conn.execute(
            "select client_id from attachment_uploads where state='paused' order by client_id"
        ).fetchall() == [(11,), (12,)]
        conn.executemany(
            "update attachment_uploads set multipart=?,created_at=?,touched=? where client_id=?",
            [
                (handles[i]["UploadId"], now - 17 * 86400000, now - days * 86400000, i)
                for i, days in ((11, 14), (12, 16))
            ],
        )

    with node() as (api, env):
        # Same aged fixture and sweep interval as the default 24h control, but expiry is off.
        old_tmp = volume / ".minio.sys/tmp" / "chat-acceptance-aged-tmp"
        fresh_tmp = old_tmp.with_name("chat-acceptance-fresh-tmp")
        for path in (old_tmp, fresh_tmp):
            path.mkdir()
            (path / "fixture").write_bytes(b"tmp")
        aged = time.time() - 48 * 3600
        os.utime(old_tmp, (aged, aged))
        await until(
            lambda: not old_tmp.exists(), "off must preserve stale temp cleanup"
        )
        assert (fresh_tmp / "fixture").read_bytes() == b"tmp"
        assert all(
            part_sizes(api, handle) == [(1, lengths[ident])]
            for ident, handle in handles.items()
        )
        with chat(db, env) as url:
            async with AsyncExitStack() as stack:
                owner = await login(stack, url, "a", "a")
                foreign = await login(stack, url, "a", "c")
                assert "error" in await foreign.ask("resume_attachment", upload=11)
                await until(
                    lambda: part_sizes(api, handles[12]) is None,
                    "Chat aborted activity-expired upload",
                )

                resumed = await owner.ok("resume_attachment", upload=11)
                with sqlite3.connect(db) as conn:
                    multipart, touched = conn.execute(
                        "select multipart,touched from attachment_uploads where client_id=11"
                    ).fetchone()
                    assert multipart == handles[11]["UploadId"] and touched > now
                    assert conn.execute(
                        "select etag from attachment_parts p join attachment_uploads u on u.id=p.upload where u.client_id=11 and p.number=1"
                    ).fetchone()[0]
                assert part_sizes(api, handles[11]) == [(1, lengths[11])]
                await owner.ok(
                    "confirm_attachment",
                    upload=11,
                    number=2,
                    etag=put(resumed["url"], large[lengths[11] :]),
                )
                published = await owner.ok("finish_attachment", upload=11)
                assert published["attachment"]["object_key"] == handles[11]["Key"]
                assert (
                    api.get_object(Bucket=store.bucket, Key=handles[11]["Key"])[
                        "Body"
                    ].read()
                    == large
                )
                assert (
                    api.get_object(Bucket=store.bucket, Key=shared_key)["Body"].read()
                    == small
                )
                assert (
                    await owner.ok(
                        "get_attachment",
                        conversation=group,
                        message=forwarded["id"],
                    )
                )["url"]
    print(
        "PASS creation-age control, ordinary temp sweep with off, protected restart/resume, 15-day Chat activity cleanup and shared object"
    )


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-silo-retention-") as directory:
        asyncio.run(run(Path(directory)))
