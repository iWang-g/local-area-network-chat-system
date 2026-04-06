#pragma once

#include <cstdint>

namespace vsserver {

/// 阻塞运行 TCP 服务（每连接一线程）。返回 0 表示正常结束监听循环。
int runTcpServer(std::uint16_t port);

} // namespace vsserver
