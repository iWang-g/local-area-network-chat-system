# -*- coding: utf-8 -*-
"""
性能测试脚本（需本机已启动 vs-server，默认端口 28888）。

示例：
  cd test
  python perf_tcp_load.py heartbeat --host 127.0.0.1 --count 500
  python perf_tcp_load.py concurrent --host 127.0.0.1 --connections 80

认证消息吞吐（需数据库中已有账号，且与 peer 已为好友）：
  python perf_tcp_load.py msg --host 127.0.0.1 --email you@ex.com --password secret --peer 2 --messages 200
"""

from __future__ import annotations

import argparse
import statistics
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import List, Tuple

from lncs_wire import LncsSession


def pct(xs: List[float], q: float) -> float:
    if not xs:
        return float("nan")
    xs = sorted(xs)
    i = min(len(xs) - 1, max(0, int(round((len(xs) - 1) * q))))
    return xs[i]


def run_heartbeat_rtt(host: str, port: int, count: int, timeout: float) -> None:
    lat: List[float] = []
    with LncsSession(host, port, timeout=timeout) as s:
        r0 = s.handshake()
        if r0.get("type") != "hello_ok":
            raise SystemExit("handshake failed: %r" % (r0,))
        t0 = time.perf_counter()
        for _ in range(count):
            t1 = time.perf_counter()
            ack = s.heartbeat_roundtrip()
            t2 = time.perf_counter()
            if ack.get("type") != "heartbeat_ack":
                raise SystemExit("unexpected: %r" % (ack,))
            lat.append((t2 - t1) * 1000.0)
        wall = time.perf_counter() - t0

    print("=== heartbeat RTT (%d samples, same connection) ===" % count)
    print("wall_clock_sec: %.4f" % wall)
    print("throughput_hz: %.2f" % (count / wall,))
    print("rtt_ms min/max: %.3f / %.3f" % (min(lat), max(lat)))
    print("rtt_ms mean: %.3f" % (statistics.mean(lat),))
    if len(lat) > 1:
        print("rtt_ms stdev: %.3f" % (statistics.stdev(lat),))
    print("rtt_ms p50/p95/p99: %.3f / %.3f / %.3f" % (pct(lat, 0.50), pct(lat, 0.95), pct(lat, 0.99)))


def _one_concurrent_hello(host: str, port: int, timeout: float) -> Tuple[bool, float]:
    t0 = time.perf_counter()
    try:
        with LncsSession(host, port, timeout=timeout) as s:
            r = s.handshake()
            ok = r.get("type") == "hello_ok"
    except OSError:
        ok = False
    dt = time.perf_counter() - t0
    return ok, dt


def run_concurrent_hello(host: str, port: int, connections: int, timeout: float) -> None:
    oks = 0
    dts: List[float] = []
    t_wall0 = time.perf_counter()
    with ThreadPoolExecutor(max_workers=min(connections, 256)) as ex:
        futs = [
            ex.submit(_one_concurrent_hello, host, port, timeout) for _ in range(connections)
        ]
        for fu in as_completed(futs):
            ok, dt = fu.result()
            dts.append(dt)
            if ok:
                oks += 1
    wall = time.perf_counter() - t_wall0

    print("=== concurrent hello_ok (%d parallel connects) ===" % connections)
    print("wall_clock_sec: %.4f" % wall)
    print("success_count: %d / %d" % (oks, connections))
    print("connect+hello_ms p50/p95: %.3f / %.3f" % (pct(dts, 0.50) * 1000.0, pct(dts, 0.95) * 1000.0))


def run_msg_throughput(
    host: str, port: int, email: str, password: str, peer: int, messages: int, timeout: float
) -> None:
    lat: List[float] = []
    t0 = time.perf_counter()
    with LncsSession(host, port, timeout=timeout) as s:
        h = s.handshake()
        if h.get("type") != "hello_ok":
            raise SystemExit("handshake failed: %r" % (h,))
        login = s.login_email(email, password)
        if login.get("type") != "auth_ok":
            raise SystemExit("login failed: %r" % (login,))
        token = login.get("token")
        if not token:
            raise SystemExit("auth_ok missing token")
        text = "perf"
        for _ in range(messages):
            t1 = time.perf_counter()
            s.send_obj(
                {"type": "msg_send", "token": token, "peer_user_id": peer, "text": text}
            )
            while True:
                fr = s.recv_obj()
                typ = fr.get("type")
                if typ == "msg_send_ok":
                    break
                # 忽略好友下推等异步帧
                if typ in ("friend_notify", "msg_push"):
                    continue
                raise SystemExit("unexpected frame during msg_send: %r" % (fr,))
            t2 = time.perf_counter()
            lat.append((t2 - t1) * 1000.0)
    wall = time.perf_counter() - t0

    print("=== msg_send round-trip (%d messages, peer_user_id=%d) ===" % (messages, peer))
    print("wall_clock_sec: %.4f" % wall)
    print("throughput_msg_per_s: %.2f" % (messages / wall,))
    print("msg_rt_ms mean/p95/p99: %.3f / %.3f / %.3f" % (
        statistics.mean(lat), pct(lat, 0.95), pct(lat, 0.99)))


def main() -> None:
    p = argparse.ArgumentParser(description="LNCS TCP performance probes")
    sub = p.add_subparsers(dest="cmd", required=True)

    ph = sub.add_parser("heartbeat", help="单连接 heartbeat 往返延迟与频率")
    ph.add_argument("--host", default="127.0.0.1")
    ph.add_argument("--port", type=int, default=28888)
    ph.add_argument("--count", type=int, default=200)
    ph.add_argument("--timeout", type=float, default=30.0)

    pc = sub.add_parser("concurrent", help="并发新建连接并完成 hello")
    pc.add_argument("--host", default="127.0.0.1")
    pc.add_argument("--port", type=int, default=28888)
    pc.add_argument("--connections", type=int, default=50)
    pc.add_argument("--timeout", type=float, default=30.0)

    pm = sub.add_parser("msg", help="登录后对指定好友发送 msg_send（需已为好友）")
    pm.add_argument("--host", default="127.0.0.1")
    pm.add_argument("--port", type=int, default=28888)
    pm.add_argument("--email", required=True)
    pm.add_argument("--password", required=True)
    pm.add_argument("--peer", type=int, required=True, help="对方 user_id")
    pm.add_argument("--messages", type=int, default=100)
    pm.add_argument("--timeout", type=float, default=60.0)

    args = p.parse_args()
    if args.cmd == "heartbeat":
        run_heartbeat_rtt(args.host, args.port, args.count, args.timeout)
    elif args.cmd == "concurrent":
        run_concurrent_hello(args.host, args.port, args.connections, args.timeout)
    elif args.cmd == "msg":
        run_msg_throughput(
            args.host,
            args.port,
            args.email,
            args.password,
            args.peer,
            args.messages,
            args.timeout,
        )


if __name__ == "__main__":
    main()
