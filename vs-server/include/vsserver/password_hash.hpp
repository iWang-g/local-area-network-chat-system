#pragma once

#include <string>

namespace vsserver {

/// 口令存储：随机 salt + 多次 SHA256（演示级强度，论文可再述 PBKDF2/bcrypt 升级路径）。
std::string randomSaltHex(std::size_t numBytes = 16);
std::string derivePasswordHash(const std::string &saltHex, const std::string &passwordPlain);

bool verifyPassword(const std::string &saltHex, const std::string &passwordPlain, const std::string &storedHashHex);

/// 会话 token：随机字节转 hex（与 salt 生成同源）。
std::string randomTokenHex(std::size_t numRandomBytes = 32);

} // namespace vsserver
