"""Check recipient-specific poll data on actual history, RPC and notification paths."""

from contextlib import AsyncExitStack

from isolated import login


async def verify_modes(url, creator, voter, observer, group):
    async with AsyncExitStack() as stack:
        second = await login(stack, url, "b", "d")
        for n, (anonymous, multiple, quiz) in enumerate(
            (a, m, q)
            for a in (True, False)
            for m, q in ((False, False), (True, False), (False, True))
        ):
            settings = dict(
                anonymous=anonymous,
                multiple=multiple,
                correct=2 if quiz else 0,
                explanation="因为有两个端点" if quiz else "",
            )
            message = await creator.ok(
                "send_message",
                conversation=group,
                client_id=8000 + n,
                text="选择结果",
                poll_options=["甲", "乙", "丙"],
                poll_settings=settings,
            )
            params = dict(conversation=group, message=message["id"])
            initial = message["poll"]
            assert (
                initial["anonymous"] == anonymous
                and initial["quiz"] == quiz
                and initial["multiple"] == multiple
            )
            assert (
                not initial["correct"]
                and not initial["explanation"]
                and not initial["recent"]
            )
            mask = 3 if multiple else 1
            cast = (await voter.ok("vote_poll", **params, choices=mask))["summary"]
            assert cast["selection"] == mask and cast["participants"] == 1
            assert bool(cast["recent"]) != anonymous and (cast["correct"] == 2) == quiz
            same = (await voter.ok("vote_poll", **params, choices=mask))["summary"]
            assert same == cast
            for client, own in ((second, True), (observer, False)):
                state = (await client.ok("get_poll", **params))["summary"]
                history = await client.ok(
                    "get_messages", conversation=group, around=message["id"]
                )
                row = next(m for m in history["messages"] if m["id"] == message["id"])
                assert row["poll"] == state
                events = [
                    e["params"]["summary"]
                    for e in client.events
                    if e.get("method") == "poll_updated"
                    and e["params"]["message"] == message["id"]
                ]
                assert events[-1] == state
                assert state["selection"] == (mask if own else 0)
                assert (
                    bool(state["correct"])
                    == bool(state["explanation"])
                    == (own and quiz)
                )
                assert state["recent"] == ([] if anonymous else cast["recent"])
            page = await observer.ask("get_poll_voters", **params, choice=1)
            if anonymous:
                assert "error" in page
            else:
                assert [v["username"] for v in page["result"]["members"]] == ["b"]
            if quiz:
                for invalid in (0, 2, 3):
                    assert "error" in await voter.ask(
                        "vote_poll", **params, choices=invalid
                    )
                right = (await observer.ok("vote_poll", **params, choices=2))["summary"]
                assert right["selection"] == 2 and right["correct"] == 2
            else:
                changed = (
                    await voter.ok("vote_poll", **params, choices=6 if multiple else 2)
                )["summary"]
                assert changed["participants"] == 1
                assert [o["votes"] for o in changed["options"]] == [0, 1, int(multiple)]
                cleared = (await voter.ok("vote_poll", **params, choices=0))["summary"]
                assert not cleared["participants"] and not cleared["recent"]
            closed = (await creator.ok("close_poll", **params))["summary"]
            assert closed["closed"] and bool(closed["correct"]) == quiz
            assert "error" in await voter.ask("vote_poll", **params, choices=1)
            await creator.ok("delete_message", **params)
    print(
        "PASS six poll modes: atomic ballots, distinct participants, quiz disclosure, anonymous RPC/history/events and second-device selection"
    )


async def verify_composer(ui, gate, creator, room, database):
    from desktop import until
    import sqlite3

    for kind in ("single", "multiple", "quiz"):
        await ui.call("poll_ui", create=True)
        await ui.wait("poll_ui", lambda r: r.get("title") == "发起群投票")
        question = "正式表单 🗳 " + kind
        fields = dict(
            question=question,
            options="甲\n乙\n丙",
            multiple=kind == "multiple",
            quiz=kind == "quiz",
        )
        if kind == "quiz":
            invalid = await ui.call("poll_ui", **fields)
            assert not invalid["valid"] and "正确答案" in invalid["feedback"]
            fields.update(correct=2, explanation="答案为乙。")
        if kind == "single":
            assert (await ui.call("poll_ui", submit=False, **fields))["valid"]
            assert (await ui.call("poll_ui", nextOption=True))["advanced"]
            gate.hold.add("send_message")
        assert (await ui.call("poll_ui", **fields))["valid"]
        if kind == "single":
            await gate.caught("send_message")
            await gate.release("send_message", error=True)
            failed = await ui.wait(
                "poll_ui", lambda r: "QA search failure" in r.get("feedback", "")
            )
            assert failed["question"] == question
            with sqlite3.connect(database) as conn:
                acknowledged_late = conn.execute(
                    "SELECT id FROM messages WHERE body=?", (question,)
                ).fetchone()[0]
            await ui.call("poll_ui", retry=True)
            await ui.wait(
                "poll_ui", lambda r: "title" not in r, message=acknowledged_late
            )
            sent = [
                request["params"]["client_id"]
                for request in gate.messages
                if request["method"] == "send_message"
                and request["params"].get("text") == question
            ]
            assert len(sent) == 2 and sent[0] == sent[1]
            with sqlite3.connect(database) as conn:
                assert conn.execute(
                    "SELECT count(*) FROM messages WHERE body=?", (question,)
                ).fetchone() == (1,)

        def created():
            with sqlite3.connect(database) as conn:
                found = conn.execute(
                    "SELECT id FROM messages WHERE body=?", (question,)
                ).fetchone()
                return found and found[0]

        mid = await until(created, "Qt form persisted")
        params = dict(conversation=room, message=mid)
        state = (await creator.ok("get_poll", **params))["summary"]
        assert (
            state["anonymous"]
            and state["multiple"] == (kind == "multiple")
            and state["quiz"] == (kind == "quiz")
        )
        assert not state["correct"] and not state["explanation"]
        await ui.wait("state", lambda r: any(x["id"] == mid for x in r["rows"]))
        if kind == "multiple":
            for choice, draft in ((1, 1), (2, 3), (1, 2), (3, 6)):
                await ui.call("poll_ui", message=mid, vote=choice)
                row = await ui.call("poll_ui", message=mid)
                assert (
                    row["draftChoices"] == draft
                    and not row["selection"]
                    and not row["pending"]
                )
            assert not (await creator.ok("get_poll", **params))["summary"][
                "participants"
            ]
            await ui.call("poll_ui", message=mid, vote=-2)
            await ui.wait(
                "poll_ui",
                lambda r: r["selection"] == 6 and not r["draftChoices"],
                message=mid,
            )
        else:
            gate.hold_requests.add("vote_poll")
            await ui.call("poll_ui", message=mid, vote=1)
            await until(lambda: bool(gate.held_requests), "pending option")
            if kind == "single":
                await creator.ok("close_poll", **params)
            await gate.release_requests("vote_poll")
            await ui.wait("poll_ui", lambda r: not r["pending"], message=mid)
            summary = (await creator.ok("get_poll", **params))["summary"]
            assert summary["selection"] == (1 if kind == "quiz" else 0)
            assert summary["correct"] == (2 if kind == "quiz" else 0)
        await creator.ok("delete_message", **params)
    print(
        "PASS Qt anonymous defaults, quiz validation and disclosure, multi-select staging/toggle/commit and concurrent close during submission"
    )
