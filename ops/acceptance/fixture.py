"""Loopback-only Chat acceptance fixture. All accounts and SQLite files are disposable."""

import asyncio
import contextlib
import json
import os
from pathlib import Path
import socket
import tempfile

from websockets.asyncio.client import connect


class Client:
    def __init__(self, ws):
        self.ws = ws
        self.ident = 0
        self.events = []

    async def ask(self, method, **params):
        self.ident += 1
        await self.ws.send(json.dumps(dict(jsonrpc="2.0", id=self.ident, method=method, params=params)))
        while True:
            response = json.loads(await asyncio.wait_for(self.ws.recv(), 60))
            if response.get("id") == self.ident:
                return response
            self.events.append(response)

    async def ok(self, method, **params):
        response = await self.ask(method, **params)
        assert "error" not in response, (method, response)
        return response["result"]


@contextlib.asynccontextmanager
async def fixture(binary=None, stderr=None):
    with tempfile.TemporaryDirectory(prefix="chat-accept-") as directory:
        db = Path(directory) / "chat.sqlite"
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        url = f"ws://127.0.0.1:{port}/ws"
        env = dict(
            os.environ,
            CHAT_S3_ENDPOINT="127.0.0.1:1",
            CHAT_S3_BUCKET="isolated",
            CHAT_S3_ACCESS_KEY="local",
            CHAT_S3_SECRET_KEY="local",
            CHAT_S3_HTTPS="0",
        )
        server = await asyncio.create_subprocess_exec(
            binary or os.environ.get("CHAT_TEST_SERVER", "build/server"),
            str(port),
            "16",
            str(db),
            env=env,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=stderr,
        )
        try:
            async with contextlib.AsyncExitStack() as stack:

                async def login(name, device, register=False):
                    client = Client(await stack.enter_async_context(connect(url)))
                    if register:
                        await client.ok("register", username=name, password="pass")
                    result = await client.ok("authenticate", username=name, password="pass", device_secret=device * 64)
                    assert result["authenticated"]
                    return client

                for _ in range(200):
                    try:
                        async with connect(url):
                            break
                    except OSError:
                        if server.returncode is not None:
                            raise RuntimeError("server exited during startup")
                        await asyncio.sleep(0.025)
                a = await login("a", "a", True)
                b = await login("b", "b", True)
                await a.ok("send_friend_request", user=2)
                await b.ok("respond_friend_request", user=1, accept=True)
                direct = (await a.ok("open_direct_conversation", user=2))["conversation"]
                yield db, login, a, b, direct, server.pid
        finally:
            if server.returncode is None:
                server.terminate()
                assert await asyncio.wait_for(server.wait(), 10) == 0
