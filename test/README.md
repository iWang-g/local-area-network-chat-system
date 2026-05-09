# LNCS 测试脚本（论文「性能 / 安全性」验证）

面向 **vs-server**（TCP JSON，默认端口 **28888**）。脚本使用 Python **3.8+** 标准库，无需安装第三方包。

## 前置条件

1. 在本机（或与 `--host` 可达的机器）启动 **`vs-server.exe`**，且 **`sqlite3.dll`**、数据库可用（参见 `docs/协议与接口草案.md`）。
2. 在项目目录下进入 `test/`：

```bat
cd test
```

## 性能测试：`perf_tcp_load.py`

单连接 **heartbeat** 往返（无需账号）：

```bat
python perf_tcp_load.py heartbeat --host 127.0.0.1 --count 500
```

并发 **握手**（连接风暴粗测）：

```bat
python perf_tcp_load.py concurrent --host 127.0.0.1 --connections 80
```

**msg_send** 往返（需数据库中已有用户，且与 `--peer` 已为好友）：

```bat
python perf_tcp_load.py msg --host 127.0.0.1 --email you@example.com --password 你的密码 --peer 2 --messages 200
```

论文中建议写明：测试机硬件、是否与本机 loopback、服务端版本与负载参数（`--count` / `--connections` / `--messages`）。

## 文件传输性能：`perf_file_transfer.py`

服务端对单文件体积上限为 **512 MiB**（`protocol.hpp` → `kFileTransferMaxBytes`）；明文分片默认不超过 **65536** 字节/片（`file_offer_ok.chunk_plain_max`）。

本脚本使用 **两条 TCP 连接**（发送方、接收方各登录），接收方线程先阻塞等待 **`file_incoming`**，发送方再 **`file_offer`** → 接收方 **`file_accept`** → 发送方循环 **`file_chunk`** → **`file_sender_done`**，统计「从发起邀请到 **`file_sender_done_ok`**」的墙钟时间与 **MiB/s**。

```bat
python perf_file_transfer.py --host 127.0.0.1 ^
  --sender-email a@x.com --sender-password p1 ^
  --receiver-email b@x.com --receiver-password p2 ^
  --peer <接收方user_id> --size 10485760
```

- **`--peer`**：接收方账号的 **`user_id`**（好友关系中的对端），须与发送方互为好友且接收进程在线。
- **`--size`**：字节数；默认 **10 MiB**；测上限附近时可设为 **`536870912`**（512 MiB）。
- 负载为确定性字节序列并计算 **SHA256**，与服务端校验一致。

## 安全性探测：`security_checks.py`

```bat
python security_checks.py --host 127.0.0.1 --port 28888
```

退出码 **0** 表示所列检查全部通过；**1** 表示存在失败项（需对照控制台输出）。

检查项包括：错误握手魔数、未握手登录、伪造 token、非法登录、畸形邮箱字符串、超大帧长度声明等。**不属于**完整渗透测试，论文表述请与此一致。

## 截图建议

将控制台完整输出复制或截图；若使用 Windows 终端，可选用「全选复制」保留时间与命令行参数，便于答辩时说明可复现性。
