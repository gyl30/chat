"""Verify the offline retirement and the surviving message-attachment permissions."""

import asyncio
from contextlib import AsyncExitStack
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import urllib.request

from desktop import Desktop
from isolated import Store, begin, chat, login, port, put

OPS = Path(__file__).resolve().parents[1]
CHECKSUM = "0" * 64


def migrate(db, component):
    sql = (OPS / f"retire_group_files_{component}.sql").read_text()
    with sqlite3.connect(db) as conn:
        conn.executescript(sql)
        assert not conn.execute("PRAGMA foreign_key_check").fetchall()


async def upload(owner, group, client, filename, data):
    grant = await begin(owner, group, client, data, filename)
    await owner.ok(
        "confirm_attachment", upload=client, number=1, etag=put(grant["url"], data)
    )
    return await owner.ok("finish_attachment", upload=client)


async def assert_rejected(user, method, code, **params):
    reply = await user.ask(method, **params)
    assert reply.get("error", {}).get("code") == code, (method, reply.get("error"))


async def verify_client(probe, root, url, group):
    ui = Desktop()
    await ui.start(probe, root, url)
    await ui.stop()
    (journal,) = (root / "data").rglob("tasks.sqlite3")
    with sqlite3.connect(journal) as db:
        device = db.execute("SELECT secret FROM installation").fetchone()
        db.executescript("""
            CREATE TABLE group_file_tasks(user INTEGER NOT NULL,client INTEGER NOT NULL,
              PRIMARY KEY(user,client), FOREIGN KEY(user,client) REFERENCES tasks(user,client) ON DELETE CASCADE);
            INSERT INTO compose_drafts(user,conversation,revision,reply,body,updated)
            VALUES(1,1,1,0,'draft stays',1);
            INSERT INTO text_outbox(user,client,conversation,body,created,rejection)
            VALUES(1,10,1,'outbox stays',1,-32006);
        """)
        for client, filename in ((10, "standalone.bin"), (11, "normal.bin")):
            db.execute(
                "INSERT INTO tasks(user,client,conversation,path,size,stamp,reply,checksum) VALUES(1,?,?,?,0,0,0,?)",
                (client, group, str(root / filename), CHECKSUM),
            )
        db.execute("INSERT INTO group_file_tasks VALUES(1,10)")
    await ui.start(probe, root, url, user_id=0)
    try:
        blocked = await ui.wait("login_state", lambda r: "迁移旧版上传记录" in r["status"])
        assert blocked["user"] == 0
    finally:
        await ui.stop()
    migrate(journal, "client")
    with sqlite3.connect(journal) as db:
        assert db.execute("SELECT secret FROM installation").fetchone() == device
        assert db.execute("SELECT body FROM compose_drafts").fetchall() == [("draft stays",)]
        assert db.execute("SELECT body FROM text_outbox").fetchall() == [("outbox stays",)]
        assert db.execute("SELECT client FROM tasks").fetchall() == [(11,)]
        assert not db.execute("SELECT 1 FROM sqlite_schema WHERE name='group_file_tasks'").fetchone()
    await ui.start(probe, root, url)
    try:
        await ui.wait(
            "transfers",
            lambda r: len(r["labels"]) == 1 and "normal.bin" in r["labels"][0],
        )
    finally:
        await ui.stop()
    print("PASS legacy local journal blocked, offline upgrade preserves device, drafts, Outbox and ordinary paused upload")


async def check(root, probe):
    store = Store()
    db = root / "chat.sqlite3"
    with store.node(root / "silo") as (api, storage):
        with chat(db, storage) as url:
            async with AsyncExitStack() as stack:
                owner = await login(stack, url, "a", "a", True)
                member = await login(stack, url, "b", "b", True)
                newcomer = await login(stack, url, "c", "c", True)
                await owner.ok("send_friend_request", user=2)
                await member.ok("respond_friend_request", user=1, accept=True)
                await owner.ok("send_friend_request", user=3)
                await newcomer.ok("respond_friend_request", user=1, accept=True)
                room = (await owner.ok("create_group", title="消息附件", members=[2]))["conversation"]
                keeping = await upload(owner, room, 41, "normal.bin", b"ordinary attachment")
                standalone = await upload(owner, room, 42, "retired.bin", b"standalone attachment")
                key = keeping["attachment"]["object_key"]
                retired_key = standalone["attachment"]["object_key"]
                await owner.ok("invite_group_members", conversation=room, members=[3])
                await assert_rejected(newcomer, "get_attachment", -32007, conversation=room, message=keeping["id"])
                for method in (
                    "list_conversation_files", "add_conversation_file", "remove_conversation_file",
                    "get_conversation_file", "complete_file",
                ):
                    await assert_rejected(owner, method, -32601)
                await assert_rejected(owner, "get_attachment", -32602, conversation=room, message=keeping["id"], object_key=key)
                await assert_rejected(owner, "send_message", -32602, conversation=room, client_id=51, object_key=key)
                assert api.head_object(Bucket=store.bucket, Key=retired_key)["ContentLength"]
                with sqlite3.connect(db) as conn:
                    assert not conn.execute("SELECT 1 FROM sqlite_schema WHERE name='conversation_files'").fetchone()

        with sqlite3.connect(db) as conn:
            conn.execute("PRAGMA foreign_keys=ON")
            conn.execute("DELETE FROM messages WHERE id=?", (standalone["id"],))
            conn.execute("DELETE FROM attachment_cleanup WHERE object_key=?", (retired_key,))
            conn.executescript("""
                ALTER TABLE files ADD COLUMN device TEXT NOT NULL DEFAULT '';
                CREATE TABLE conversation_files (
                    conversation_id INTEGER NOT NULL REFERENCES conversations(id),
                    object_key TEXT NOT NULL REFERENCES files(object_key),
                    created_at INTEGER NOT NULL, PRIMARY KEY(conversation_id,object_key));
                CREATE INDEX group_files_object ON conversation_files(object_key);
                CREATE INDEX group_files_page ON conversation_files(conversation_id,created_at DESC,object_key DESC);
                CREATE TRIGGER release_conversation_files AFTER DELETE ON conversation_files
                BEGIN SELECT 1; END;
                CREATE TRIGGER claim_conversation_files AFTER INSERT ON conversation_files
                BEGIN SELECT 1; END;
            """)
            conn.executemany(
                "INSERT INTO conversation_files VALUES(?,?,0)", ((room, key), (room, retired_key))
            )
        blocked = subprocess.run(
            [os.environ["CHAT_TEST_SERVER"], str(port()), "2", str(db)],
            env=dict(os.environ, **storage), capture_output=True, text=True, timeout=12,
        )
        assert blocked.returncode != 0 and "offline migration" in blocked.stderr + blocked.stdout
        migrate(db, "server")
        with sqlite3.connect(db) as conn:
            assert conn.execute("SELECT object_key FROM attachment_cleanup").fetchall() == [(retired_key,)]
            assert not conn.execute(
                "SELECT 1 FROM sqlite_schema WHERE name IN ('conversation_files','group_files_object','group_files_page')"
            ).fetchone()
        with chat(db, storage) as url:
            await verify_client(probe, root / "client", url, room)
            async with AsyncExitStack() as stack:
                owner = await login(stack, url, "a", "a")
                auth = await owner.ok("get_attachment", conversation=room, message=keeping["id"])
                with urllib.request.urlopen(auth["url"]) as response:
                    assert response.read() == b"ordinary attachment"
                await owner.ok("delete_message", conversation=room, message=keeping["id"])
        assert not api.list_objects_v2(Bucket=store.bucket).get("Contents")
    print("PASS retired RPCs and key-only requests rejected; offline server upgrade preserves shared objects and reclaims last references")


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="chat-retire-files-") as directory:
        asyncio.run(check(Path(directory), sys.argv[1]))
