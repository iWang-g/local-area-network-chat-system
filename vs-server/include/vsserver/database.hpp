#pragma once

#include <cstdint>
#include <string>

namespace vsserver {

/// 业务层错误码（与 `protocol.hpp` 中 TCP 错误帧 code 对齐）。
enum class AuthErrorCode : int {
    Ok = 0,
    DbUnavailable = 2001,
    EmailTaken = 2002,
    InvalidCredentials = 2003,
    EmailNotFound = 2004,
    InvalidInput = 2005,
};

struct AuthResult {
    bool ok = false;
    AuthErrorCode code = AuthErrorCode::Ok;
    std::string message;
    std::int64_t userId = 0;
    std::string token;
    std::string email;
};

/// 打开 `exe同目录/data/chat.db`，建 `users` 表；多线程通过全局 mutex 串行访问。
class AppDatabase {
public:
    static bool initialize(std::string &errMsg);
    static void shutdown();
    static bool isReady();

    static AuthResult registerUser(const std::string &email, const std::string &passwordPlain,
                                   const std::string &nickname);
    static AuthResult login(const std::string &email, const std::string &passwordPlain);

    /// 后续阶段校验 token 用；当前仅写入映射。
    static bool validateToken(const std::string &token, std::int64_t &outUserId);

private:
    static std::string normalizeEmail(std::string_view raw);
};

} // namespace vsserver
