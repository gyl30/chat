"""Disposable loopback endpoints for the Silo recovery drill and UI acceptance."""

from contextlib import contextmanager
import hashlib
import os
import secrets
import socket
import subprocess
import time
import urllib.request

import boto3
from botocore.exceptions import ClientError

from fixture import Client, connect


def port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@contextmanager
def process(command, env, health):
    proc = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(150):
            if proc.poll() is not None:
                raise RuntimeError(f"{command[0]} exited with {proc.returncode}")
            try:
                with urllib.request.urlopen(health, timeout=0.25) as response:
                    if response.status == 200:
                        break
            except OSError:
                time.sleep(0.05)
        else:
            raise TimeoutError(health)
        yield
    finally:
        proc.terminate()
        proc.wait(timeout=15)


class Store:
    def __init__(self, *, region="us-east-1"):
        self.region = region
        self.secret = secrets.token_hex(24)
        self.bucket = "chat-acceptance"

    @contextmanager
    def node(self, volume, *, expiry=None, cleanup_interval=None):
        endpoint = f"127.0.0.1:{port()}"
        env = dict(
            os.environ,
            SILO_ROOT_USER="chat-accept",
            MINIO_ROOT_USER="chat-accept",
            SILO_ROOT_PASSWORD=self.secret,
            MINIO_ROOT_PASSWORD=self.secret,
            MINIO_SITE_REGION=self.region,
        )
        # Only the explicit test knobs may override the isolated node's defaults.
        for name, value in (
            ("MINIO_API_STALE_UPLOADS_EXPIRY", expiry),
            ("MINIO_API_STALE_UPLOADS_CLEANUP_INTERVAL", cleanup_interval),
            ("MINIO_CONFIG_ENV_FILE", None),
        ):
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
        url = "http://" + endpoint
        command = [
            os.getenv("SILO_BIN", "silo"),
            "server",
            str(volume),
            "--address",
            endpoint,
            "--console-address",
            f"127.0.0.1:{port()}",
        ]
        with process(command, env, url + "/minio/health/ready"):
            api = boto3.client(
                "s3",
                endpoint_url=url,
                aws_access_key_id="chat-accept",
                aws_secret_access_key=self.secret,
                region_name=self.region,
            )
            # A newly started Silo can answer the health check before its S3 API is ready.
            for attempt in range(8):
                try:
                    api.list_buckets()
                    break
                except ClientError as error:
                    if error.response["Error"]["Code"] != "XMinioServerNotInitialized" or attempt == 7:
                        raise
                    time.sleep(0.25)
            yield (
                api,
                dict(
                    CHAT_S3_ENDPOINT=endpoint,
                    CHAT_S3_BUCKET=self.bucket,
                    CHAT_S3_ACCESS_KEY="chat-accept",
                    CHAT_S3_SECRET_KEY=self.secret,
                    CHAT_S3_HTTPS="0",
                    CHAT_S3_REGION=self.region,
                ),
            )


@contextmanager
def chat(db, storage, *, server=None):
    listen = port()
    url = f"127.0.0.1:{listen}"
    with process(
        [server or os.getenv("CHAT_TEST_SERVER", "build/server"), str(listen), "8", str(db)],
        dict(os.environ, **storage),
        "http://" + url + "/health",
    ):
        yield "ws://" + url + "/ws"


async def login(stack, url, name, device, register=False):
    rpc = Client(await stack.enter_async_context(connect(url)))
    if register:
        await rpc.ok("register", username=name, password="pass")
    assert (await rpc.ok("authenticate", username=name, password="pass", device_secret=device * 64))["authenticated"]
    return rpc


def put(url, data):
    with urllib.request.urlopen(urllib.request.Request(url, data=data, method="PUT")) as response:
        return response.headers["ETag"].strip('"')


async def begin(user, group, client_id, data, filename="restore.bin", media_type="application/octet-stream"):
    return await user.ok(
        "begin_attachment",
        conversation=group,
        client_id=client_id,
        filename=filename,
        media_type=media_type,
        size=len(data),
        checksum=hashlib.sha256(data).hexdigest(),
    )
