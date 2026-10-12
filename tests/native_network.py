#!/usr/bin/env python3
"""Per-client TCP relay used to blackhole the network path of one native client.

The relay copies bytes in both directions and, while paused, stops copying without
closing either socket. The client therefore sees a silent link: its own writes are
accepted by the kernel, no answer arrives, and only its own heartbeat gives up.
Every accepted client connection dials its own upstream connection, so a client that
reconnects after the link is restored is accepted again on the same address.
"""
from __future__ import annotations

import contextlib
import selectors
import socket
import threading


class Blackhole:
    def __init__(self, host, port):
        self.target = (host, int(port))
        self.relayed = 0
        self.paused = False
        self._condition = threading.Condition()
        self._stopped = threading.Event()
        self._pairs = []
        self._listener = socket.socket()
        self._listener.bind(('127.0.0.1', 0))
        self._listener.listen(8)
        self.port = self._listener.getsockname()[1]
        threading.Thread(target=self._accept_loop, daemon=True).start()

    @property
    def url(self):
        return f'ws://127.0.0.1:{self.port}/ws'

    def pause(self):
        with self._condition:
            self.paused = True

    def resume(self):
        with self._condition:
            self.paused = False
            self._condition.notify_all()

    def sever(self):
        """Close both ends of every open pair so the client reconnects immediately."""
        with self._condition:
            pairs, self._pairs = list(self._pairs), []
        for pair in pairs:
            self._shutdown(pair)

    def close(self):
        self._stopped.set()
        self.resume()
        with self._condition:
            pairs, self._pairs = list(self._pairs), []
        for pair in pairs:
            self._shutdown(pair)
        with contextlib.suppress(OSError):
            self._listener.close()

    def _shutdown(self, pair):
        for sock in pair:
            with contextlib.suppress(OSError):
                sock.close()

    def _accept_loop(self):
        while not self._stopped.is_set():
            try:
                client, _ = self._listener.accept()
            except OSError:
                return
            try:
                upstream = socket.create_connection(self.target)
            except OSError:
                with contextlib.suppress(OSError):
                    client.close()
                continue
            pair = [client, upstream]
            with self._condition:
                self._pairs.append(pair)
            threading.Thread(target=self._pump, args=(pair,), daemon=True).start()

    def _pump(self, pair):
        client, upstream = pair
        peers = {client: upstream, upstream: client}
        poller = selectors.DefaultSelector()
        for sock in pair:
            poller.register(sock, selectors.EVENT_READ, peers[sock])
        try:
            while not self._stopped.is_set():
                # A paused link is never read, so the kernel buffer fills and the client
                # experiences a silent network instead of an error.
                with self._condition:
                    while self.paused and not self._stopped.is_set():
                        self._condition.wait(0.2)
                if self._stopped.is_set():
                    break
                for key, _ in poller.select(0.2):
                    try:
                        data = key.fileobj.recv(65536)
                        if not data:
                            return
                        self.relayed += len(data)
                        key.data.sendall(data)
                    except OSError:
                        return
        finally:
            with contextlib.suppress(OSError):
                poller.close()
            with self._condition:
                if pair in self._pairs:
                    self._pairs.remove(pair)
            self._shutdown(pair)
