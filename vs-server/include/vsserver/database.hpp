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
    InvalidEmailCode = 2006,
    EmailCodeRateLimited = 2007,
    EmailSendFailed = 2008,
};

struct AuthResult {
    bool ok = false;
    AuthErrorCode code = AuthErrorCode::Ok;
    std::string message;
    std::int64_t userId = 0;
    std::string token;
    std::string email;
};

/// 请求注册邮箱验证码的结果（`req_email_code`）。
struct EmailCodeIssueResult {
    bool ok = false;
    int errCode = 0;
    std::string message;
    int retryAfterSec = 0;
};

/// 打开 `exe同目录/data/chat.db`，建表；多线程通过全局 mutex 串行访问。
class AppDatabase {
public:
    static bool initialize(std::string &errMsg);
    static void shutdown();
    static bool isReady();

    /// 生成并落库注册用验证码；同一邮箱 60s 内重复请求返回限流。
    /// 若设置 `LANCS_MAIL_HELPER_URL` 则通过 HTTP 通知 mail-helper 发信；失败则回滚本条验证码并返回 2008。
    /// 未设置该环境变量时仍写入日志（含明文码）便于离线联调。
    static EmailCodeIssueResult issueRegisterEmailCode(const std::string &email);

    static AuthResult registerUser(const std::string &email, const std::string &passwordPlain,
                                   const std::string &nickname, const std::string &emailCodePlain);
    static AuthResult login(const std::string &email, const std::string &passwordPlain);

    static bool validateToken(const std::string &token, std::int64_t &outUserId);

private:
    static std::string normalizeEmail(std::string_view raw);
};

} // namespace vsserver
