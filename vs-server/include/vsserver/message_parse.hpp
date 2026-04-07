#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vsserver {

/// 从 JSON 文本中解析 "type" 字段（字符串值）；失败返回 nullopt。
std::optional<std::string> parseMessageType(const std::string &jsonUtf8);

/// 解析 `"field": "value"` 中的字符串值（不含转义序列扩展，阶段 2 够用）。
std::optional<std::string> parseJsonStringField(const std::string &jsonUtf8, const char *asciiFieldName);

std::string jsonEscapeString(std::string_view utf8);

/// retryAfterSec >= 0 时附加 `retry_after_sec` 字段（客户端重发倒计时）。
std::string buildErrorJson(int code, const std::string &messageUtf8, int retryAfterSec = -1);
std::string buildAuthOkJson(std::int64_t userId, const std::string &tokenHex, const std::string &emailUtf8);
std::string buildEmailCodeOkJson();

/// 校验握手：type=hello、magic=LNCS、version=1。
bool validateHello(const std::string &jsonUtf8, std::string &err);

} // namespace vsserver
