#pragma once

#include <cstdint>

namespace vsserver {

/// 与客户端约定的协议版本、魔数；后续在此扩展帧类型、错误码等。
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr char kMagic[4] = {'L', 'N', 'C', 'S'};
/// 默认监听端口（与 qt-client 默认一致）。
inline constexpr std::uint16_t kDefaultTcpPort = 28888;

/// TCP JSON `error` 帧中的 `code`（与 `AppDatabase` / 握手逻辑对齐）。
inline constexpr int kErrBadHandshake = 1001;
inline constexpr int kErrNeedHandshake = 2010;
inline constexpr int kErrDbUnavailable = 2001;
inline constexpr int kErrEmailTaken = 2002;
inline constexpr int kErrInvalidCredentials = 2003;
inline constexpr int kErrUserNotFound = 2004;
inline constexpr int kErrInvalidInput = 2005;
inline constexpr int kErrInvalidEmailCode = 2006;
inline constexpr int kErrEmailCodeRateLimited = 2007;
/// 已配置 `LANCS_MAIL_HELPER_URL` 但 HTTP 通知或 SMTP 侧失败（验证码未下发给用户）。
inline constexpr int kErrEmailSendFailed = 2008;

} // namespace vsserver
