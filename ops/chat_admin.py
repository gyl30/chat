#!/usr/bin/env python3
"""Local administration for one Chat database. All months and timestamps are UTC."""

import argparse
import calendar
import contextlib
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import sys


def connect(path, mode):
    return sqlite3.connect(path.resolve().as_uri() + "?mode=" + mode, uri=True, timeout=10)


def archive_dir(db):
    return db.parent / (db.name + ".archives")


def archive_path(db, key):
    if not re.fullmatch(r"[0-9a-f]{32}", key):
        raise ValueError("invalid archive key")
    return archive_dir(db) / (key + ".sqlite3")


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def fsync(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def integrity(db):
    result = db.execute("PRAGMA integrity_check").fetchall()
    if result != [("ok",)] or db.execute("PRAGMA foreign_key_check").fetchone():
        raise ValueError("SQLite integrity or foreign key check failed")


def month_bounds(month):
    start = dt.date.fromisoformat(month + "-01")
    if start.strftime("%Y-%m") != month or month >= dt.datetime.now(dt.timezone.utc).strftime("%Y-%m"):
        raise ValueError("choose a completed UTC month in YYYY-MM format")
    end = start + dt.timedelta(days=calendar.monthrange(start.year, start.month)[1])
    return tuple(calendar.timegm(day.timetuple()) * 1000 for day in (start, end))


@contextlib.contextmanager
def archive_lock(db, shared=False):
    directory = archive_dir(db)
    directory.mkdir(mode=0o700, exist_ok=True)
    with (directory / ".lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_SH if shared else fcntl.LOCK_EX)
        yield


def row_hash(digest, row):
    digest.update(json.dumps(row, ensure_ascii=False, separators=(",", ":")).encode() + b"\n")


def create_month(hot, path, bounds, month, key):
    # The hot SELECT holds one WAL read snapshot while new messages can still commit.
    temp = path.with_suffix(".building")
    temp.unlink(missing_ok=True)
    with contextlib.closing(sqlite3.connect(temp)) as cold:
        cold.execute("CREATE TABLE identity(month TEXT PRIMARY KEY, object_key TEXT NOT NULL)")
        cold.execute("INSERT INTO identity VALUES(?,?)", (month, key))
        cold.execute("""CREATE TABLE messages(id INTEGER PRIMARY KEY, conversation_id INTEGER, sender_id INTEGER,
                     body TEXT NOT NULL, created_at INTEGER, reply_to_id INTEGER, edited_at INTEGER, deleted INTEGER,
                     client_id INTEGER, forward_source INTEGER, original_sender INTEGER, object_key TEXT)""")
        source = hot.execute("""SELECT m.id,m.conversation_id,m.sender_id,m.body,m.created_at,m.reply_to_id,
                m.edited_at,m.deleted,m.client_id,m.forward_source,m.original_sender,a.object_key
            FROM messages m LEFT JOIN message_attachments a ON a.message_id=m.id
            WHERE m.created_at>=? AND m.created_at<? ORDER BY m.id""", bounds)
        original = hashlib.sha256()
        for rows in iter(lambda: source.fetchmany(128), []):
            for row in rows:
                row_hash(original, row)
            cold.executemany("INSERT INTO messages VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", rows)
        cold.commit()
        copied = hashlib.sha256()
        for row in cold.execute("SELECT * FROM messages ORDER BY id"):
            row_hash(copied, row)
        if copied.digest() != original.digest():
            raise ValueError("archive rows differ from the source snapshot")
        integrity(cold)
    fsync(temp)
    os.link(temp, path)  # Immutable: never replace an archive a reader might already use.
    temp.unlink()
    fsync(path.parent)


def verify_month(path, month, key, expected):
    digest = file_hash(path)
    if expected is not None and digest != expected:
        raise ValueError(f"{month}: archive SHA-256 mismatch")
    with contextlib.closing(connect(path, "ro")) as cold:
        integrity(cold)
        if cold.execute("SELECT month,object_key FROM identity").fetchall() != [(month, key)]:
            raise ValueError(f"{month}: archive identity mismatch")
        limits = cold.execute("SELECT min(id),max(id),count(*) FROM messages").fetchone()
    return digest, limits


def reclaim_pages(hot):
    # Release free overflow pages in bounded commits, yielding the writer lock between batches.
    budget = hot.execute("PRAGMA freelist_count").fetchone()[0]
    for _ in range(0, budget, 128):
        hot.execute("PRAGMA incremental_vacuum(128)").fetchall()


def archive_month(db, hot, month):
    bounds = month_bounds(month)
    with archive_lock(db):
        with hot:
            hot.execute("""INSERT INTO archives(month,state,updated_at) VALUES(?,'building',CAST(strftime('%s','now') AS INTEGER))
                ON CONFLICT(month) DO NOTHING""", (month,))
        key, expected, state = hot.execute(
            "SELECT object_key,sha256,state FROM archives WHERE month=?", (month,)).fetchone()
        path = archive_path(db, key)
        if state == "complete":
            verify_month(path, month, key, expected)
            print(month, "already complete")
            return
        try:
            with hot:
                hot.execute("UPDATE archives SET state='building',error='',updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE month=?", (month,))
            if not path.exists() and expected is None:
                create_month(hot, path, bounds, month, key)
            digest, (first, last, count) = verify_month(path, month, key, expected)
            with hot:
                hot.execute("""UPDATE archives SET sha256=?,first_id=?,last_id=?,message_count=?,updated_at=CAST(strftime('%s','now') AS INTEGER)
                            WHERE month=?""", (digest, first, last, count, month))
            moved = 0
            with contextlib.closing(connect(path, "ro")) as cold:
                rows = cold.execute("SELECT id,body,edited_at FROM messages WHERE NOT deleted ORDER BY id")
                for batch in iter(lambda: rows.fetchmany(128), []):
                    # An edit, recall or group deletion after the snapshot wins.
                    # Each commit points only at an already verified, durable file.
                    with hot:
                        moved += hot.executemany("""UPDATE messages SET archive_key=?,body=NULL
                            WHERE id=? AND body=? AND edited_at IS ? AND NOT deleted AND archive_key IS NULL""",
                                                 ((key, *row) for row in batch)).rowcount
            with hot:
                hot.execute("UPDATE archives SET state='complete',updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE month=?", (month,))
            reclaim_pages(hot)
            print(f"{month}: complete; snapshot={count}, moved bodies={moved}, SHA-256={digest}")
        except (OSError, sqlite3.Error, ValueError) as error:
            with hot:
                hot.execute("UPDATE archives SET state='failed',error=?,updated_at=CAST(strftime('%s','now') AS INTEGER) WHERE month=?", (str(error), month))
            raise


def archive_status(db, hot, verify=False):
    for month, key, state, digest, first, last, count, error in hot.execute(
            "SELECT month,object_key,state,sha256,first_id,last_id,message_count,error FROM archives ORDER BY month"):
        if verify and state == "complete" and not digest:
            raise ValueError(f"{month}: missing archive checksum")
        if verify and digest:
            _, actual = verify_month(archive_path(db, key), month, key, digest)
            if actual != (first, last, count):
                raise ValueError(f"{month}: manifest range or count mismatch")
            with contextlib.closing(connect(archive_path(db, key), "ro")) as cold:
                ids = iter(cold.execute("SELECT id FROM messages ORDER BY id"))
                current = next(ids, None)
                for target in hot.execute("SELECT id FROM messages WHERE archive_key=? ORDER BY id", (key,)):
                    while current is not None and current < target:
                        current = next(ids, None)
                    if current != target:
                        raise ValueError(f"{month}: missing archived body for message {target[0]}")
        print(json.dumps(dict(month=month, state=state, first=first, last=last, messages=count,
                              file=key+".sqlite3", sha256=digest, error=error), ensure_ascii=False))


def main():
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db", required=True, type=Path)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("archive").add_argument("month", help="completed UTC month, YYYY-MM")
    commands.add_parser("archive-status")
    commands.add_parser("archive-check")
    args = parser.parse_args()
    db = args.db.resolve()
    with contextlib.closing(connect(db, "rw" if args.command == "archive" else "ro")) as hot:
        hot.execute("PRAGMA foreign_keys=ON")
        if args.command == "archive":
            archive_month(db, hot, args.month)
        else:
            archive_status(db, hot, args.command == "archive-check")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, sqlite3.Error) as problem:
        sys.exit(str(problem))
