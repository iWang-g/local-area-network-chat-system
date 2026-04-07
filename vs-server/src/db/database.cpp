#include "vsserver/database.hpp"
#include "vsserver/logger.hpp"
#include "vsserver/mail_helper_http.hpp"
#include "vsserver/password_hash.hpp"
#include "vsserver/sqlite_dynamic.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unordered_map>

namespace vsserver {

namespace {

std::mutex g_dbMutex;
std::mutex g_sessMutex;
sqlite3 *g_db = nullptr;
bool g_ready = false;
std::unordered_map<std::string, std::int64_t> g_tokenToUser;

constexpr int kRegisterCodeTtlSec = 300;
constexpr int kRegisterCodeResendSec = 60;
constexpr const char *kPurposeRegister = "register";

std::unordered_map<std::string, std::chrono::steady_clock::time_point> g_lastRegisterCodeSent;

std::wstring exeDirectoryW()
{
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) {
        return L".";
    }
    std::wstring p(buf, n);
    const auto pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return L".";
    }
    return p.substr(0, pos);
}

std::string wideToUtf8(const std::wstring &w)
{
    if (w.empty()) {
        return {};
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool ensureDataDirectory(const std::wstring &exeDir)
{
    const std::wstring dataDir = exeDir + L"\\data";
    if (::CreateDirectoryW(dataDir.c_str(), nullptr)) {
        return true;
    }
    return ::GetLastError() == ERROR_ALREADY_EXISTS;
}

static AuthResult fail(AuthErrorCode c, const std::string &msg)
{
    AuthResult r;
    r.ok = false;
    r.code = c;
    r.message = msg;
    return r;
}

static AuthResult ok(std::int64_t uid, std::string token, std::string email)
{
    AuthResult r;
    r.ok = true;
    r.code = AuthErrorCode::Ok;
    r.userId = uid;
    r.token = std::move(token);
    r.email = std::move(email);
    r.message.clear();
    return r;
}

static EmailCodeIssueResult emailFail(int code, std::string msg, int retry = 0)
{
    EmailCodeIssueResult r;
    r.ok = false;
    r.errCode = code;
    r.message = std::move(msg);
    r.retryAfterSec = retry;
    return r;
}

static EmailCodeIssueResult emailOk()
{
    EmailCodeIssueResult r;
    r.ok = true;
    r.errCode = 0;
    return r;
}

static std::string trimAscii(std::string_view s)
{
    std::string out(s);
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.front()))) {
        out.erase(out.begin());
    }
    while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) {
        out.pop_back();
    }
    return out;
}

/// 校验并删除已验证的注册验证码；错码累加 attempts，≥5 删除记录。
static bool consumeRegisterEmailCode(SqliteDynamic &api, sqlite3 *db, const std::string &emailNorm,
                                     const std::string &codePlain)
{
    const std::string code = trimAscii(codePlain);
    if (code.empty()) {
        return false;
    }
    const char *sel = "SELECT id, code, expires_at, attempts FROM email_codes WHERE email = ? AND purpose = ? "
                      "LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sel, -1, &st, nullptr) != 0) {
        return false;
    }
    api.bind_text_transient(st, 1, emailNorm.c_str(), static_cast<int>(emailNorm.size()));
    api.bind_text_transient(st, 2, kPurposeRegister, static_cast<int>(std::strlen(kPurposeRegister)));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return false;
    }
    const std::int64_t rowId = api.column_int64(st, 0);
    const unsigned char *codePtr = api.column_text(st, 1);
    const std::string stored = codePtr ? reinterpret_cast<const char *>(codePtr) : "";
    const std::int64_t expiresAt = api.column_int64(st, 2);
    const std::int64_t attempts = api.column_int64(st, 3);
    api.finalize(st);

    const std::time_t now = std::time(nullptr);
    if (now > expiresAt) {
        const char *del = "DELETE FROM email_codes WHERE id = ?;";
        sqlite3_stmt *d = nullptr;
        if (api.prepare(db, del, -1, &d, nullptr) == 0) {
            const std::string idStr = std::to_string(rowId);
            api.bind_text_transient(d, 1, idStr.c_str(), static_cast<int>(idStr.size()));
            api.step(d);
            api.finalize(d);
        }
        return false;
    }

    if (stored != code) {
        const std::string newAttempts = std::to_string(attempts + 1);
        if (attempts + 1 >= 5) {
            const char *del2 = "DELETE FROM email_codes WHERE id = ?;";
            sqlite3_stmt *d2 = nullptr;
            if (api.prepare(db, del2, -1, &d2, nullptr) == 0) {
                const std::string idStr = std::to_string(rowId);
                api.bind_text_transient(d2, 1, idStr.c_str(), static_cast<int>(idStr.size()));
                api.step(d2);
                api.finalize(d2);
            }
        } else {
            const char *up = "UPDATE email_codes SET attempts = ? WHERE id = ?;";
            sqlite3_stmt *u = nullptr;
            if (api.prepare(db, up, -1, &u, nullptr) == 0) {
                api.bind_text_transient(u, 1, newAttempts.c_str(), static_cast<int>(newAttempts.size()));
                const std::string idStr = std::to_string(rowId);
                api.bind_text_transient(u, 2, idStr.c_str(), static_cast<int>(idStr.size()));
                api.step(u);
                api.finalize(u);
            }
        }
        return false;
    }

    const char *delOk = "DELETE FROM email_codes WHERE id = ?;";
    sqlite3_stmt *d3 = nullptr;
    if (api.prepare(db, delOk, -1, &d3, nullptr) != 0) {
        return false;
    }
    const std::string idStr = std::to_string(rowId);
    api.bind_text_transient(d3, 1, idStr.c_str(), static_cast<int>(idStr.size()));
    api.step(d3);
    api.finalize(d3);
    return true;
}

} // namespace

std::string AppDatabase::normalizeEmail(std::string_view raw)
{
    std::string s(raw);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    for (char &c : s) {
        if (static_cast<unsigned char>(c) <= 127 && c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

bool AppDatabase::initialize(std::string &errMsg)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    errMsg.clear();
    if (g_ready && g_db != nullptr) {
        return true;
    }

    auto &api = sqliteApi();
    if (!api.isReady()) {
        errMsg = api.lastError();
        return false;
    }

    const std::wstring exe = exeDirectoryW();
    if (!ensureDataDirectory(exe)) {
        errMsg = "无法创建 data 目录";
        return false;
    }
    const std::string path = wideToUtf8(exe + L"\\data\\chat.db");

    char *err = nullptr;
    const int rc = api.open(path.c_str(), &g_db);
    if (rc != 0 || g_db == nullptr) {
        errMsg = "无法打开数据库";
        if (g_db != nullptr) {
            errMsg += std::string(": ") + api.errmsg(g_db);
            api.close(g_db);
            g_db = nullptr;
        }
        return false;
    }

    const char *schema = "PRAGMA foreign_keys = ON;"
                         "CREATE TABLE IF NOT EXISTS users ("
                         "  user_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  email TEXT NOT NULL UNIQUE COLLATE NOCASE,"
                         "  password_hash TEXT NOT NULL,"
                         "  salt TEXT,"
                         "  nickname TEXT,"
                         "  avatar_path TEXT,"
                         "  created_at INTEGER NOT NULL"
                         ");"
                         "CREATE TABLE IF NOT EXISTS email_codes ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  email TEXT NOT NULL,"
                         "  code TEXT NOT NULL,"
                         "  purpose TEXT NOT NULL,"
                         "  expires_at INTEGER NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  attempts INTEGER NOT NULL DEFAULT 0"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_email_codes_email_purpose ON email_codes(email, purpose);";

    if (api.exec(g_db, schema, &err) != 0) {
        errMsg = err ? err : api.errmsg(g_db);
        api.free(err);
        api.close(g_db);
        g_db = nullptr;
        return false;
    }
    api.free(err);

    g_ready = true;
    return true;
}

void AppDatabase::shutdown()
{
    std::lock_guard<std::mutex> slock(g_sessMutex);
    g_tokenToUser.clear();

    std::lock_guard<std::mutex> lock(g_dbMutex);
    g_lastRegisterCodeSent.clear();
    g_ready = false;
    if (g_db != nullptr) {
        sqliteApi().close(g_db);
        g_db = nullptr;
    }
}

bool AppDatabase::isReady()
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    return g_ready && g_db != nullptr;
}

EmailCodeIssueResult AppDatabase::issueRegisterEmailCode(const std::string &email)
{
    const std::string em = normalizeEmail(email);
    if (em.empty() || em.find('@') == std::string::npos) {
        return emailFail(static_cast<int>(AuthErrorCode::InvalidInput), "邮箱格式不正确", 0);
    }
    if (em.size() > 128) {
        return emailFail(static_cast<int>(AuthErrorCode::InvalidInput), "邮箱过长", 0);
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return emailFail(static_cast<int>(AuthErrorCode::DbUnavailable), "数据库不可用", 0);
    }

    const auto nowSteady = std::chrono::steady_clock::now();
    const auto it = g_lastRegisterCodeSent.find(em);
    if (it != g_lastRegisterCodeSent.end()) {
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::seconds>(nowSteady - it->second).count();
        if (elapsed < kRegisterCodeResendSec) {
            const int wait = static_cast<int>(kRegisterCodeResendSec - elapsed);
            return emailFail(static_cast<int>(AuthErrorCode::EmailCodeRateLimited), "发送过于频繁，请稍后再试",
                             wait);
        }
    }

    std::string code;
    try {
        code = randomSixDigitCode();
    } catch (const std::exception &) {
        return emailFail(static_cast<int>(AuthErrorCode::DbUnavailable), "生成验证码失败", 0);
    }

    auto &api = sqliteApi();
    const char *del = "DELETE FROM email_codes WHERE email = ? AND purpose = ?;";
    sqlite3_stmt *dst = nullptr;
    if (api.prepare(g_db, del, -1, &dst, nullptr) != 0) {
        return emailFail(static_cast<int>(AuthErrorCode::DbUnavailable), api.errmsg(g_db), 0);
    }
    api.bind_text_transient(dst, 1, em.c_str(), static_cast<int>(em.size()));
    api.bind_text_transient(dst, 2, kPurposeRegister, static_cast<int>(std::strlen(kPurposeRegister)));
    api.step(dst);
    api.finalize(dst);

    const std::time_t now = std::time(nullptr);
    const std::time_t exp = now + kRegisterCodeTtlSec;
    const std::string nowStr = std::to_string(static_cast<long long>(now));
    const std::string expStr = std::to_string(static_cast<long long>(exp));
    const char *ins = "INSERT INTO email_codes (email, code, purpose, expires_at, created_at, attempts) "
                      "VALUES (?,?,?,?,?,0);";
    sqlite3_stmt *ist = nullptr;
    if (api.prepare(g_db, ins, -1, &ist, nullptr) != 0) {
        return emailFail(static_cast<int>(AuthErrorCode::DbUnavailable), api.errmsg(g_db), 0);
    }
    api.bind_text_transient(ist, 1, em.c_str(), static_cast<int>(em.size()));
    api.bind_text_transient(ist, 2, code.c_str(), static_cast<int>(code.size()));
    api.bind_text_transient(ist, 3, kPurposeRegister, static_cast<int>(std::strlen(kPurposeRegister)));
    api.bind_text_transient(ist, 4, expStr.c_str(), static_cast<int>(expStr.size()));
    api.bind_text_transient(ist, 5, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(ist) != 101) {
        api.finalize(ist);
        return emailFail(static_cast<int>(AuthErrorCode::DbUnavailable), api.errmsg(g_db), 0);
    }
    api.finalize(ist);

    std::string mailErr;
    if (mailHelperConfigured()) {
        if (!mailHelperNotifyRegisterCode(em, code, mailErr)) {
            sqlite3_stmt *rb = nullptr;
            if (api.prepare(g_db, del, -1, &rb, nullptr) != 0) {
                VSLOG_ERROR(std::string("[email_code] mail-helper 失败且回滚准备失败: ") + mailErr + " | "
                            + api.errmsg(g_db));
                return emailFail(static_cast<int>(AuthErrorCode::EmailSendFailed), "邮件发送失败，请稍后重试", 0);
            }
            api.bind_text_transient(rb, 1, em.c_str(), static_cast<int>(em.size()));
            api.bind_text_transient(rb, 2, kPurposeRegister, static_cast<int>(std::strlen(kPurposeRegister)));
            api.step(rb);
            api.finalize(rb);
            VSLOG_ERROR(std::string("[email_code] mail-helper 调用失败，已回滚验证码: ") + mailErr);
            return emailFail(static_cast<int>(AuthErrorCode::EmailSendFailed), "邮件发送失败，请稍后重试", 0);
        }
        VSLOG_INFO(std::string("[email_code] 已通过 mail-helper 通知发信 邮箱=") + em);
    } else {
        VSLOG_INFO(std::string("[email_code] 注册验证码已生成 邮箱=") + em + " 验证码=" + code
                   + "（未设置 LANCS_MAIL_HELPER_URL：离线演示，请查看本日志）");
    }

    g_lastRegisterCodeSent[em] = nowSteady;
    return emailOk();
}

AuthResult AppDatabase::registerUser(const std::string &email, const std::string &passwordPlain,
                                     const std::string &nickname, const std::string &emailCodePlain)
{
    const std::string em = normalizeEmail(email);
    if (em.empty() || passwordPlain.empty()) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码不能为空");
    }
    if (em.size() > 128 || passwordPlain.size() > 256) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码过长");
    }
    if (nickname.size() > 64) {
        return fail(AuthErrorCode::InvalidInput, "昵称过长");
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return fail(AuthErrorCode::DbUnavailable, "数据库不可用");
    }

    auto &api = sqliteApi();
    const char *checkSql = "SELECT 1 FROM users WHERE email = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, checkSql, -1, &st, nullptr) != 0) {
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, em.c_str(), static_cast<int>(em.size()));
    const int stepRc = api.step(st);
    api.finalize(st);
    if (stepRc == 100) { // SQLITE_ROW
        return fail(AuthErrorCode::EmailTaken, "该邮箱已注册");
    }

    if (emailCodePlain.empty()) {
        return fail(AuthErrorCode::InvalidInput, "请填写邮箱验证码");
    }
    if (!consumeRegisterEmailCode(api, g_db, em, emailCodePlain)) {
        return fail(AuthErrorCode::InvalidEmailCode, "验证码错误或已过期");
    }

    std::string salt;
    std::string hash;
    try {
        salt = randomSaltHex(16);
        hash = derivePasswordHash(salt, passwordPlain);
    } catch (const std::exception &e) {
        return fail(AuthErrorCode::DbUnavailable, e.what());
    }

    const std::time_t now = std::time(nullptr);
    const std::string nowStr = std::to_string(static_cast<long long>(now));
    const std::string ins =
        "INSERT INTO users (email, password_hash, salt, nickname, created_at) VALUES (?,?,?,?,?);";
    sqlite3_stmt *insSt = nullptr;
    if (api.prepare(g_db, ins.c_str(), -1, &insSt, nullptr) != 0) {
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(insSt, 1, em.c_str(), static_cast<int>(em.size()));
    api.bind_text_transient(insSt, 2, hash.c_str(), static_cast<int>(hash.size()));
    api.bind_text_transient(insSt, 3, salt.c_str(), static_cast<int>(salt.size()));
    const std::string nick = nickname.empty() ? std::string() : nickname;
    api.bind_text_transient(insSt, 4, nick.c_str(), static_cast<int>(nick.size()));
    api.bind_text_transient(insSt, 5, nowStr.c_str(), static_cast<int>(nowStr.size()));

    if (api.step(insSt) != 101) { // SQLITE_DONE = 101
        api.finalize(insSt);
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.finalize(insSt);

    const std::int64_t uid = api.last_insert_rowid(g_db);
    std::string token = randomTokenHex(32);
    {
        std::lock_guard<std::mutex> slock(g_sessMutex);
        g_tokenToUser[token] = uid;
    }
    return ok(uid, std::move(token), em);
}

AuthResult AppDatabase::login(const std::string &email, const std::string &passwordPlain)
{
    const std::string em = normalizeEmail(email);
    if (em.empty() || passwordPlain.empty()) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码不能为空");
    }
    if (em.size() > 128 || passwordPlain.size() > 256) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码过长");
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return fail(AuthErrorCode::DbUnavailable, "数据库不可用");
    }

    auto &api = sqliteApi();
    const char *sql = "SELECT user_id, salt, password_hash FROM users WHERE email = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, em.c_str(), static_cast<int>(em.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return fail(AuthErrorCode::EmailNotFound, "用户不存在");
    }

    const std::int64_t uid = api.column_int64(st, 0);
    const unsigned char *saltPtr = api.column_text(st, 1);
    const unsigned char *hashPtr = api.column_text(st, 2);
    const std::string salt = saltPtr ? reinterpret_cast<const char *>(saltPtr) : "";
    const std::string stored = hashPtr ? reinterpret_cast<const char *>(hashPtr) : "";
    api.finalize(st);

    if (!verifyPassword(salt, passwordPlain, stored)) {
        return fail(AuthErrorCode::InvalidCredentials, "密码错误");
    }

    std::string token = randomTokenHex(32);
    {
        std::lock_guard<std::mutex> slock(g_sessMutex);
        g_tokenToUser[token] = uid;
    }
    return ok(uid, std::move(token), em);
}

bool AppDatabase::validateToken(const std::string &token, std::int64_t &outUserId)
{
    std::lock_guard<std::mutex> slock(g_sessMutex);
    const auto it = g_tokenToUser.find(token);
    if (it == g_tokenToUser.end()) {
        return false;
    }
    outUserId = it->second;
    return true;
}

} // namespace vsserver
