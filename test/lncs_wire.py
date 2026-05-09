# -*- coding: utf-8 -*-
"""LNCS TCP 成帧：4 字节大端长度 + UTF-8 JSON（与 vs-server `wire.hpp` 一致）。"""

from __future__ import annotations

import json
import socket
import struct
from typing import Any, Dict

MAX_FRAME_PAYLOAD = 256 * 1024


def encode_frame(obj: Dict[str, Any]) -> bytes:
    raw = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(raw) > MAX_FRAME_PAYLOAD:
        raise ValueError("JSON payload exceeds 256 KiB")
    return struct.pack(">I", len(raw)) + raw


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed before %d bytes" % n)
        buf += chunk
    return bytes(buf)


def read_frame(sock: socket.socket) -> Dict[str, Any]:
    hdr = recv_exact(sock, 4)
    (length,) = struct.unpack(">I", hdr)
    if length > MAX_FRAME_PAYLOAD:
        raise ValueError("peer claims frame length %d (> max)" % length)
    body = recv_exact(sock, length)
    return json.loads(body.decode("utf-8"))


class LncsSession:
    """单连接：发送 dict，阻塞读取一帧 JSON。"""

    def __init__(self, host: str, port: int, timeout: float = 30.0) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.settimeout(timeout)

    def __enter__(self) -> LncsSession:
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.close()

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    def send_obj(self, obj: Dict[str, Any]) -> None:
        self._sock.sendall(encode_frame(obj))

    def recv_obj(self) -> Dict[str, Any]:
        return read_frame(self._sock)

    def handshake(self) -> Dict[str, Any]:
        self.send_obj({"type": "hello", "magic": "LNCS", "version": 1})
        return self.recv_obj()

    def heartbeat_roundtrip(self) -> Dict[str, Any]:
        self.send_obj({"type": "heartbeat"})
        return self.recv_obj()

    def login_email(self, email: str, password: str) -> Dict[str, Any]:
        self.send_obj({"type": "auth_login", "email": email, "password": password})
        return self.recv_obj()


