# -*- coding: utf-8 -*-
"""
安全性相关协议层检查（黑盒探测）。需在论文中说明：
  - 属于「接口与传输层的合规性与滥用场景验证」，非完整渗透测试。
  - 默认端口 28888，协议见仓库 docs/协议与接口草案.md。

用法：
  cd test
  python security_checks.py --host 127.0.0.1
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
from typing import List, Tuple

from lncs_wire import MAX_FRAME_PAYLOAD, encode_frame, read_frame


def _ok(name: str, cond: bool, detail: str = "") -> None:
    tag = "PASS" if cond else "FAIL"
    extra = (" | " + detail) if detail else ""
    print("[%s] %s%s" % (tag, name, extra))


def check_bad_handshake(host: str, port: int, timeout: float) -> bool:
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.settimeout(timeout)
        sock.sendall(
            encode_frame({"type": "hello", "magic": "XXXX", "version": 1})
        )
        obj = read_frame(sock)
        code = obj.get("code")
        return obj.get("type") == "error" and code == 1001
    except (OSError, ValueError, json.JSONDecodeError):
        return False
    finally:
        sock.close()


def check_need_handshake_before_login(host: str, port: int, timeout: float) -> bool:
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.settimeout(timeout)
        sock.sendall(
            encode_frame({"type": "auth_login", "email": "a@b.com", "password": "x"})
        )
        obj = read_frame(sock)
        return obj.get("type") == "error" and obj.get("code") == 2010
    except (OSError, ValueError, json.JSONDecodeError):
        return False
    finally:
        sock.close()


def check_fake_token_rejected(host: str, port: int, timeout: float) -> bool:
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.settimeout(timeout)
        sock.sendall(encode_frame({"type": "hello", "magic": "LNCS", "version": 1}))
        h = read_frame(sock)
        if h.get("type") != "hello_ok":
            return False
        sock.sendall(
            encode_frame({"type": "friend_list", "token": "0000000000000000"})
        )
        obj = read_frame(sock)
        return obj.get("type") == "error" and obj.get("code") == 2011
    except (OSError, ValueError, json.JSONDecodeError):
        return False
    finally:
        sock.close()


def check_invalid_login(host: str, port: int, timeout: float) -> bool:
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.settimeout(timeout)
        sock.sendall(encode_frame({"type": "hello", "magic": "LNCS", "version": 1}))
        if read_frame(sock).get("type") != "hello_ok":
            return False
        sock.sendall(
            encode_frame(
                {
                    "type": "auth_login",
                    "email": "nonexistent-lncs-test@invalid.local",
                    "password": "wrongpassword",
                }
            )
        )
        obj = read_frame(sock)
        # 账号不存在或口令错误均视为「未成功登录」
        return obj.get("type") == "error" and obj.get("code") in (2003, 2004)
    except (OSError, ValueError, json.JSONDecodeError):
        return False
    finally:
        sock.close()


def check_sql_injection_email_login(host: str, port: int, timeout: float) -> bool:
    """常见 SQL 注入负载不应导致服务端崩溃；应答应为业务错误帧。"""
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.settimeout(timeout)
        sock.sendall(encode_frame({"type": "hello", "magic": "LNCS", "version": 1}))
        if read_frame(sock).get("type") != "hello_ok":
            return False
        payload = '''x@y.com\" OR \"1\"=\"1'''
        sock.sendall(
            encode_frame({"type": "auth_login", "email": payload, "password": "p"})
        )
        obj = read_frame(sock)
        return obj.get("type") == "error"
    except (OSError, ValueError, json.JSONDecodeError):
        return False
    finally:
        sock.close()


def check_oversized_frame_length(host: str, port: int, timeout: float) -> Tuple[bool, str]:
    """
    发送声明长度超过 256KiB 的帧头且不提供负载。
    服务端丢弃非法帧后不应回复畸形大包；客户端在限时内应读不到合法 JSON 帧。
    """
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        wait = max(3.0, min(15.0, timeout))
        sock.settimeout(wait)
        bad_len = MAX_FRAME_PAYLOAD + 1024
        sock.sendall(struct.pack(">I", bad_len))
        try:
            read_frame(sock)
            return False, "received a full JSON frame after illegal length (unexpected)"
        except socket.timeout:
            return True, "no server frame within %.1fs (no spurious reply)" % wait
        except (ConnectionError, OSError, ValueError, json.JSONDecodeError) as e:
            return True, "closed or parse error: %s" % e
    finally:
        try:
            sock.close()
        except OSError:
            pass


def main() -> None:
    ap = argparse.ArgumentParser(description="LNCS security-oriented probes")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=28888)
    ap.add_argument("--timeout", type=float, default=10.0)
    args = ap.parse_args()

    print("target %s:%d timeout=%.1fs" % (args.host, args.port, args.timeout))
    print("---")

    results: List[Tuple[str, bool, str]] = []

    b = check_bad_handshake(args.host, args.port, args.timeout)
    results.append(("握手魔数错误返回 1001", b, ""))

    b = check_need_handshake_before_login(args.host, args.port, args.timeout)
    results.append(("未握手即登录返回 2010", b, ""))

    b = check_fake_token_rejected(args.host, args.port, args.timeout)
    results.append(("伪造 token 调用 friend_list 返回 2011", b, ""))

    b = check_invalid_login(args.host, args.port, args.timeout)
    results.append(("错误口令/不存在账号返回 2003/2004", b, ""))

    b = check_sql_injection_email_login(args.host, args.port, args.timeout)
    results.append(("畸形邮箱登录返回 error 而非崩溃", b, ""))

    b, det = check_oversized_frame_length(args.host, args.port, args.timeout)
    results.append(("超大帧长度声明处理 (%s)" % det, b, ""))

    for name, ok, extra in results:
        _ok(name, ok, extra)

    failed = sum(1 for _, ok, _ in results if not ok)
    print("---")
    print("summary: %d failed / %d checks" % (failed, len(results)))
    raise SystemExit(1 if failed else 0)


if __name__ == "__main__":
    main()
