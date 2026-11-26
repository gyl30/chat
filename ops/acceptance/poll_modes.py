"""Check recipient-specific poll data on actual history, RPC and notification paths."""

from contextlib import AsyncExitStack

from isolated import login


async def verify_modes(url, creator, voter, observer, group):
    async with AsyncExitStack() as stack:
        second = await login(stack, url, "b", "d")
        for n, (anonymous, multiple, quiz) in enumerate(
            (a, m, q) for a in (True, False) for m, q in ((False, False), (True, False), (False, True))
        ):
            settings = dict(anonymous=anonymous, multiple=multiple, correct=2 if quiz else 0,
                            explanation="因为有两个端点" if quiz else "")
            message = await creator.ok("send_message", conversation=group, client_id=8000+n, text="选择结果",
                                       poll_options=["甲", "乙", "丙"], poll_settings=settings)
            params = dict(conversation=group, message=message["id"])
            initial = message["poll"]
            assert initial["anonymous"] == anonymous and initial["quiz"] == quiz and initial["multiple"] == multiple
            assert not initial["correct"] and not initial["explanation"] and not initial["recent"]
            mask = 3 if multiple else 1
            cast = (await voter.ok("vote_poll", **params, choices=mask))["summary"]
            assert cast["selection"] == mask and cast["participants"] == 1
            assert bool(cast["recent"]) != anonymous and (cast["correct"] == 2) == quiz
            same = (await voter.ok("vote_poll", **params, choices=mask))["summary"]
            assert same == cast
            for client, own in ((second, True), (observer, False)):
                state = (await client.ok("get_poll", **params))["summary"]
                history = await client.ok("get_messages", conversation=group, around=message["id"])
                row = next(m for m in history["messages"] if m["id"] == message["id"])
                assert row["poll"] == state
                events = [e["params"]["summary"] for e in client.events if e.get("method") == "poll_updated"
                          and e["params"]["message"] == message["id"]]
                assert events[-1] == state
                assert state["selection"] == (mask if own else 0)
                assert bool(state["correct"]) == bool(state["explanation"]) == (own and quiz)
                assert state["recent"] == ([] if anonymous else cast["recent"])
            page = await observer.ask("get_poll_voters", **params, choice=1)
            if anonymous:
                assert "error" in page
            else:
                assert [v["username"] for v in page["result"]["members"]] == ["b"]
            if quiz:
                for invalid in (0, 2, 3):
                    assert "error" in await voter.ask("vote_poll", **params, choices=invalid)
                right = (await observer.ok("vote_poll", **params, choices=2))["summary"]
                assert right["selection"] == 2 and right["correct"] == 2
            else:
                changed = (await voter.ok("vote_poll", **params, choices=6 if multiple else 2))["summary"]
                assert changed["participants"] == 1
                assert [o["votes"] for o in changed["options"]] == [0, 1, int(multiple)]
                cleared = (await voter.ok("vote_poll", **params, choices=0))["summary"]
                assert not cleared["participants"] and not cleared["recent"]
            closed = (await creator.ok("close_poll", **params))["summary"]
            assert closed["closed"] and bool(closed["correct"]) == quiz
            assert "error" in await voter.ask("vote_poll", **params, choices=1)
            await creator.ok("delete_message", **params)
    print("PASS six poll modes: atomic ballots, distinct participants, quiz disclosure, anonymous RPC/history/events and second-device selection")
