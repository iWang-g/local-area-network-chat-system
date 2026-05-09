#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vsserver {

/// RFC 4648 Base64；编码无换行，解码采用严格字符集（与原先 Crypt32 STRICT 行为接近）。
bool wireBase64EncodeBytes(const std::uint8_t *data, std::size_t len, std::string &outB64Utf8);
bool wireBase64DecodeBytes(const std::string &b64Utf8, std::vector<std::uint8_t> &outBytes);
/// 仅校验并返回解码后字节数（不写入缓冲区）。
bool wireBase64DecodedByteCount(const std::string &b64Utf8, std::size_t &outCount);

} // namespace vsserver
