# -*- coding: utf-8 -*-
"""LNCS TCP 成帧：4 字节大端长度 + UTF-8 JSON，或 LNCB 二进制负载（与 vs-server `wire.hpp` / `protocol.hpp` 一致）。"""

from __future__ import annotations

import json
import socket
import struct
from typing import Any, Dict, List, Optional

MAX_FRAME_PAYLOAD = 256 * 1024
LN_CB_SEND_HDR = 86
LN_CB_PUSH_HDR = 24


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


def read_frame_body(sock: socket.socket) -> bytes:
    hdr = recv_exact(sock, 4)
    (length,) = struct.unpack(">I", hdr)
    if length > MAX_FRAME_PAYLOAD:
        raise ValueError("peer claims frame length %d (> max)" % length)
    return recv_exact(sock, length)


def read_frame(sock: socket.socket) -> Dict[str, Any]:
    body = read_frame_body(sock)
    return json.loads(body.decode("utf-8"))


def build_ln_cb_sender_chunk(transfer_id: int, seq: int, token_hex_64: str, plain: bytes) -> bytes:
    if len(token_hex_64) != 64:
        raise ValueError("token must be 64 hex chars")
    if len(plain) > MAX_FRAME_PAYLOAD - LN_CB_SEND_HDR:
        raise ValueError("plain chunk too large")
    out = bytearray()
    out += b"LNCB"
    out += struct.pack(">H", 1)
    out += struct.pack(">q", transfer_id)
    out += struct.pack(">I", seq & 0xFFFFFFFF)
    out += token_hex_64.encode("ascii")
    out += struct.pack(">I", len(plain))
    out += plain
    return bytes(out)


def parse_ln_cb_chunk_push(body: bytes) -> Optional[bytes]:
    """解析 S→C LNCB file_chunk_push；失败返回 None。"""
    if len(body) < LN_CB_PUSH_HDR or body[:4] != b"LNCB":
        return None
    kind = struct.unpack(">H", body[6:8])[0]
    if kind != 1:
        return None
    plen = struct.unpack(">I", body[20:24])[0]
    if len(body) != LN_CB_PUSH_HDR + plen:
        return None
    return body[LN_CB_PUSH_HDR:]


class LncsSession:
    """单连接：发送 dict / 原始负载，阻塞读取 JSON 或帧字节。"""

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

    def send_raw_payload(self, payload: bytes) -> None:
        if len(payload) > MAX_FRAME_PAYLOAD:
            raise ValueError("payload exceeds max frame size")
        self._sock.sendall(struct.pack(">I", len(payload)) + payload)

    def recv_obj(self) -> Dict[str, Any]:
        return read_frame(self._sock)

    def recv_frame_body(self) -> bytes:
        return read_frame_body(self._sock)

    def handshake(self, capabilities: Optional[List[str]] = None) -> Dict[str, Any]:
        obj: Dict[str, Any] = {"type": "hello", "magic": "LNCS", "version": 1}
        if capabilities:
            obj["capabilities"] = capabilities
        self.send_obj(obj)
        return self.recv_obj()

    def heartbeat_roundtrip(self) -> Dict[str, Any]:
        self.send_obj({"type": "heartbeat"})
        return self.recv_obj()

    def login_email(self, email: str, password: str) -> Dict[str, Any]:
        self.send_obj({"type": "auth_login", "email": email, "password": password})
        return self.recv_obj()

