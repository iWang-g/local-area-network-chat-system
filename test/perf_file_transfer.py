# -*- coding: utf-8 -*-
"""
双连接文件中继性能测试（发送方 + 接收方均在线，走 file_offer → accept → chunk 中继）。

默认使用 **LNCB 二进制分片**（hello 时声明 `file_chunk_binary_v1`）；`--legacy-json` 可回退 JSON+Base64。

示例：
  cd test
  python perf_file_transfer.py --host 127.0.0.1 \\
    --sender-email a@x.com --sender-password p1 \\
    --receiver-email b@x.com --receiver-password p2 \\
    --peer 2 --size 10485760
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import threading
import time
from typing import List, Optional, Tuple

from lncs_wire import LncsSession, build_ln_cb_sender_chunk, parse_ln_cb_chunk_push

FILE_TRANSFER_MAX_BYTES = 512 * 1024 * 1024
DEFAULT_CHUNK_PLAIN_MAX = 65536


def _plain_byte_at(global_offset: int) -> int:
    return global_offset % 251


def _sha256_hex_full_file(total: int) -> str:
    h = hashlib.sha256()
    sent = 0
    block = 1024 * 1024
    while sent < total:
        n = min(block, total - sent)
        chunk = bytes(_plain_byte_at(sent + i) for i in range(n))
        h.update(chunk)
        sent += n
    return h.hexdigest()


def _iter_send_chunks(total: int, chunk_plain_max: int):
    sent = 0
    while sent < total:
        n = min(chunk_plain_max, total - sent)
        yield bytes(_plain_byte_at(sent + i) for i in range(n))
        sent += n


def _login(
    sess: LncsSession, email: str, password: str, *, use_binary: bool
) -> Tuple[str, int]:
    caps = ["file_chunk_binary_v1"] if use_binary else None
    h = sess.handshake(caps)
    if h.get("type") != "hello_ok":
        raise RuntimeError("handshake: %r" % (h,))
    if use_binary and not h.get("file_chunk_binary"):
        raise RuntimeError("server did not enable file_chunk_binary in hello_ok")
    sess.send_obj({"type": "auth_login", "email": email, "password": password})
    r = sess.recv_obj()
    if r.get("type") != "auth_ok":
        raise RuntimeError("login failed: %r" % (r,))
    token = r.get("token")
    uid = r.get("user_id")
    if not token or uid is None:
        raise RuntimeError("auth_ok missing token/user_id")
    if use_binary and len(str(token)) != 64:
        raise RuntimeError("auth_ok token must be 64 hex chars for LNCB path")
    return str(token), int(uid)


def run_transfer(
    host: str,
    port: int,
    sender_email: str,
    sender_password: str,
    receiver_email: str,
    receiver_password: str,
    peer_user_id: int,
    size: int,
    file_name: str,
    timeout: float,
    use_binary: bool,
) -> None:
    if size <= 0 or size > FILE_TRANSFER_MAX_BYTES:
        raise SystemExit(
            "size must be in (0, %d] bytes (512 MiB cap)" % FILE_TRANSFER_MAX_BYTES
        )

    sha_hex = _sha256_hex_full_file(size)
    err_box: List[BaseException] = []

    def rx_thread() -> None:
        try:
            with LncsSession(host, port, timeout=timeout) as rx:
                tok, _uid = _login(rx, receiver_email, receiver_password, use_binary=use_binary)
                tid_incoming: Optional[int] = None
                while tid_incoming is None:
                    body = rx.recv_frame_body()
                    if body.startswith(b"LNCB"):
                        raise RuntimeError("unexpected LNCB before file_incoming")
                    obj = json.loads(body.decode("utf-8"))
                    typ = obj.get("type")
                    if typ == "file_incoming":
                        tid_incoming = int(obj["transfer_id"])
                    elif typ in ("friend_notify", "msg_push"):
                        continue
                    elif typ == "error":
                        raise RuntimeError("receiver got error: %r" % (obj,))
                rx.send_obj({"type": "file_accept", "token": tok, "transfer_id": tid_incoming})
                acc = rx.recv_obj()
                if acc.get("type") != "file_accept_ok":
                    raise RuntimeError("file_accept: %r" % (acc,))
                received = 0
                done = False
                while not done:
                    body = rx.recv_frame_body()
                    if body.startswith(b"LNCB"):
                        if use_binary:
                            plain = parse_ln_cb_chunk_push(body)
                            if plain is None:
                                raise RuntimeError("bad LNCB push frame")
                            received += len(plain)
                        else:
                            raise RuntimeError("unexpected LNCB in legacy mode")
                        continue
                    obj = json.loads(body.decode("utf-8"))
                    typ = obj.get("type")
                    if typ == "file_chunk_push":
                        b64 = obj.get("data_b64") or ""
                        raw = base64.b64decode(b64, validate=True)
                        received += len(raw)
                    elif typ == "file_transfer_done":
                        done = True
                    elif typ in ("friend_notify", "msg_push"):
                        continue
                    elif typ == "error":
                        raise RuntimeError("receiver mid xfer: %r" % (obj,))
                if received != size:
                    raise RuntimeError("size mismatch: got %d expected %d" % (received, size))
        except BaseException as e:
            err_box.append(e)

    th = threading.Thread(target=rx_thread, daemon=True)
    th.start()
    time.sleep(0.15)

    wall0 = time.perf_counter()
    with LncsSession(host, port, timeout=timeout) as tx:
        stok, _ = _login(tx, sender_email, sender_password, use_binary=use_binary)
        tx.send_obj(
            {
                "type": "file_offer",
                "token": stok,
                "peer_user_id": peer_user_id,
                "file_name": file_name,
                "file_size": size,
                "sha256_hex": sha_hex,
            }
        )

        transfer_id: Optional[int] = None
        chunk_max = DEFAULT_CHUNK_PLAIN_MAX
        ready = False
        while not ready:
            obj = tx.recv_obj()
            typ = obj.get("type")
            if typ == "file_offer_ok":
                transfer_id = int(obj["transfer_id"])
                if use_binary and obj.get("file_chunk_binary"):
                    chunk_max = int(obj.get("chunk_plain_max_binary") or DEFAULT_CHUNK_PLAIN_MAX)
                else:
                    chunk_max = int(obj.get("chunk_plain_max") or DEFAULT_CHUNK_PLAIN_MAX)
            elif typ == "file_offer_delivered":
                continue
            elif typ == "file_send_ready":
                ready = True
            elif typ == "error":
                raise RuntimeError("file_offer phase: %r" % (obj,))
            else:
                continue

        if transfer_id is None:
            raise RuntimeError("missing file_offer_ok")

        xfer_start = time.perf_counter()
        seq = 0
        for plain in _iter_send_chunks(size, chunk_max):
            if use_binary:
                raw = build_ln_cb_sender_chunk(transfer_id, seq, stok, plain)
                tx.send_raw_payload(raw)
            else:
                b64 = base64.b64encode(plain).decode("ascii")
                tx.send_obj(
                    {
                        "type": "file_chunk",
                        "token": stok,
                        "transfer_id": transfer_id,
                        "seq": seq,
                        "data_b64": b64,
                    }
                )
            seq += 1

        tx.send_obj({"type": "file_sender_done", "token": stok, "transfer_id": transfer_id})
        done_ack = tx.recv_obj()
        if done_ack.get("type") != "file_sender_done_ok":
            raise RuntimeError("file_sender_done: %r" % (done_ack,))
        xfer_end = time.perf_counter()

    wall1 = time.perf_counter()
    th.join(timeout=max(30.0, timeout))
    if err_box:
        raise err_box[0]

    payload_sec = xfer_end - xfer_start
    total_sec = wall1 - wall0
    mib = size / (1024 * 1024)
    mib_s = mib / payload_sec if payload_sec > 0 else 0

    mode = "LNCB binary chunk" if use_binary else "JSON+Base64"
    print("=== file relay throughput (online peer, %s) ===" % mode)
    print("file_size_bytes: %d (%.4f MiB)" % (size, mib))
    print("file_name: %s" % file_name)
    print("chunks: %d (chunk_plain_max=%d)" % (seq, chunk_max))
    print("xfer_wall_sec (offer→sender_done_ok): %.4f" % payload_sec)
    print("total_wall_sec (threads login+xfer): %.4f" % total_sec)
    print("throughput_MiB_s (payload/xfer_wall): %.4f" % mib_s)
    print("equivalent_Mibit_s (MiB/s × 8): %.4f" % (mib_s * 8))


def main() -> None:
    p = argparse.ArgumentParser(description="LNCS file relay performance (two TCP connections)")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=28888)
    p.add_argument("--sender-email", required=True)
    p.add_argument("--sender-password", required=True)
    p.add_argument("--receiver-email", required=True)
    p.add_argument("--receiver-password", required=True)
    p.add_argument("--peer", type=int, required=True, help="接收方 user_id")
    p.add_argument(
        "--size",
        type=int,
        default=10 * 1024 * 1024,
        help="字节数，默认 10 MiB；上限 %d (512 MiB)" % FILE_TRANSFER_MAX_BYTES,
    )
    p.add_argument("--name", default="perf.bin", help="文件名（展示用）")
    p.add_argument("--timeout", type=float, default=7200.0, help="套接字超时（秒）")
    p.add_argument(
        "--legacy-json",
        action="store_true",
        help="使用 JSON+Base64 分片（旧路径），默认启用 LNCB 二进制",
    )
    args = p.parse_args()
    run_transfer(
        args.host,
        args.port,
        args.sender_email,
        args.sender_password,
        args.receiver_email,
        args.receiver_password,
        args.peer,
        args.size,
        args.name,
        args.timeout,
        use_binary=not args.legacy_json,
    )


if __name__ == "__main__":
    main()
