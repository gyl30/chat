"""End-to-end Release comparison of sparse history and global filter changes."""

import argparse
import asyncio
import json
from pathlib import Path
import sqlite3
import time

from fixture import fixture
from search_bench import quantiles, seed


async def compare(binary, baseline, rounds, rows):
    async with fixture(binary) as (db, login, reader, _, direct, _):
        writer = await login("a", "c")
        seed(db, rows)
        with sqlite3.connect(db) as conn:
            target = conn.execute(
                "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(?,1,'https://old.test',0)",
                (direct,),
            ).lastrowid
            conn.executemany(
                "INSERT INTO messages(conversation_id,sender_id,body,created_at) VALUES(?,1,'ordinary text',1)",
                [(direct,)] * 3100,
            )

        async def history():
            if baseline:
                before = None
                # Reproduce the old dialog's initial page and 20 automatic requests.
                for _ in range(21):
                    result = await reader.ok(
                        "get_messages",
                        conversation=direct,
                        **({"before": before} if before else {}),
                    )
                    if any(row["id"] == target for row in result["messages"]):
                        return True
                    before = result["messages"][0]["id"]
                return False
            result = await reader.ok(
                "get_messages", category="links", conversation=direct
            )
            return [row["id"] for row in result["messages"]] == [target]

        async def combined():
            result = await reader.ok(
                "search_all_messages",
                query="没有匹配",
                **({"category": "links", "sender": 1} if not baseline else {}),
            )
            assert not result["messages"]

        async def unchanged_global():
            result = await reader.ok("search_all_messages", query="没有匹配")
            assert not result["messages"]

        send_id = 5000
        for name, query in [
            ("history", history),
            ("global", unchanged_global),
            ("combined", combined),
        ]:
            if baseline and name == "combined":
                continue
            data = []
            for n in range(rounds + 2):
                send_id += 1

                async def timed_query():
                    start = time.perf_counter_ns()
                    result = await query()
                    return (
                        (time.perf_counter_ns() - start) / 1e6,
                        time.perf_counter_ns(),
                        result,
                    )

                request = asyncio.create_task(timed_query())
                await asyncio.sleep(0.002)
                send_start = time.perf_counter_ns()
                await writer.ok(
                    "send_message", conversation=direct, text="perf", client_id=send_id
                )
                send_end = time.perf_counter_ns()
                query_ms, query_end, result = await request
                if name == "history":
                    assert result != baseline
                if n >= 2:
                    data.append(
                        dict(
                            query_ms=query_ms,
                            send_ms=(send_end - send_start) / 1e6,
                            overlap=send_start < query_end,
                        )
                    )
            yield dict(
                phase="baseline" if baseline else "candidate",
                case=name,
                rows=rows,
                samples=data,
                query_p95_p99=quantiles([s["query_ms"] for s in data]),
                send_p95_p99=quantiles([s["send_ms"] for s in data]),
                overlap=sum(s["overlap"] for s in data),
            )


async def run(args):
    output = []
    for binary, baseline in [(args.baseline, True), (args.candidate, False)]:
        async for result in compare(binary, baseline, args.samples, args.rows):
            output.append(result)
            print(
                json.dumps({k: v for k, v in result.items() if k != "samples"}),
                flush=True,
            )
    args.raw.write_text(json.dumps(output, indent=2) + "\n")


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("baseline", type=Path)
    cli.add_argument("candidate", type=Path)
    cli.add_argument("--rows", type=int, default=120000)
    cli.add_argument("--samples", type=int, default=30)
    cli.add_argument("--raw", type=Path, required=True)
    asyncio.run(run(cli.parse_args()))
