#include "vsserver/database.hpp"
#include "vsserver/logger.hpp"
#include "vsserver/mail_helper_http.hpp"
#include "vsserver/password_hash.hpp"
#include "vsserver/platform_paths.hpp"
#include "vsserver/protocol.hpp"
#include "vsserver/sqlite_dynamic.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

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

static bool ensureDataDirectoryFs(const std::filesystem::path &exeDir)
{
    const std::filesystem::path dataDir = exeDir / "data";
    std::error_code ec;
    std::filesystem::create_directories(dataDir, ec);
    return !ec && std::filesystem::is_directory(dataDir);
}

static AuthResult fail(AuthErrorCode c, const std::string &msg)
{
    AuthResult r;
    r.ok = false;
    r.code = c;
    r.message = msg;
    return r;
}

static AuthResult ok(std::int64_t uid, std::string token, std::string email, std::string nickname = {},
                     const std::int64_t avatarRev = 0, std::string usernameUtf8 = {})
{
    AuthResult r;
    r.ok = true;
    r.code = AuthErrorCode::Ok;
    r.userId = uid;
    r.token = std::move(token);
    r.email = std::move(email);
    r.nickname = std::move(nickname);
    r.avatarRev = avatarRev;
    r.username = std::move(usernameUtf8);
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

static bool utf8NextCodepoint(const std::string &s, std::size_t &i, char32_t &cp)
{
    if (i >= s.size()) {
        return false;
    }
    const unsigned char c0 = static_cast<unsigned char>(s[i]);
    if (c0 < 0x80u) {
        cp = c0;
        i += 1;
        return true;
    }
    if ((c0 & 0xE0u) == 0xC0u) {
        if (i + 1 >= s.size()) {
            return false;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        if ((c1 & 0xC0u) != 0x80u) {
            return false;
        }
        cp = (char32_t(c0 & 0x1Fu) << 6) | (c1 & 0x3Fu);
        if (cp < 0x80u) {
            return false;
        }
        i += 2;
        return true;
    }
    if ((c0 & 0xF0u) == 0xE0u) {
        if (i + 2 >= s.size()) {
            return false;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
            return false;
        }
        cp = (char32_t(c0 & 0x0Fu) << 12) | (char32_t(c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
        if (cp < 0x800u) {
            return false;
        }
        i += 3;
        return true;
    }
    if ((c0 & 0xF8u) == 0xF0u) {
        if (i + 3 >= s.size()) {
            return false;
        }
        const unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
        const unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
        const unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
            return false;
        }
        cp = (char32_t(c0 & 0x07u) << 18) | (char32_t(c1 & 0x3Fu) << 12) | (char32_t(c2 & 0x3Fu) << 6)
            | (c3 & 0x3Fu);
        if (cp < 0x10000u || cp > 0x10FFFFu) {
            return false;
        }
        i += 4;
        return true;
    }
    return false;
}

/// 用户名：2～32 个 Unicode 码位；仅字母（各文种）、数字；禁止 `@` 与空白。
static bool isValidUsernameUtf8(const std::string &raw)
{
    const std::string u = trimAscii(raw);
    if (u.empty()) {
        return false;
    }
    constexpr std::size_t kMin = 2;
    constexpr std::size_t kMax = 32;
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < u.size()) {
        char32_t cp = 0;
        if (!utf8NextCodepoint(u, i, cp)) {
            return false;
        }
        if (cp <= 32 || cp == static_cast<char32_t>('@') || cp == 127) {
            return false;
        }
        bool ok = false;
        if (cp >= U'0' && cp <= U'9') {
            ok = true;
        } else if (cp >= U'a' && cp <= U'z') {
            ok = true;
        } else if (cp >= U'A' && cp <= U'Z') {
            ok = true;
        } else if (cp >= 0x4E00u && cp <= 0x9FFFu) {
            ok = true;
        } else if (cp >= 0x3400u && cp <= 0x4DBFu) {
            ok = true;
        } else if (cp >= 0x3040u && cp <= 0x30FFu) {
            ok = true;
        } else if (cp >= 0xAC00u && cp <= 0xD7AFu) {
            ok = true;
        }
        if (!ok) {
            return false;
        }
        ++n;
        if (n > kMax) {
            return false;
        }
    }
    return n >= kMin && n <= kMax;
}

/// 展示昵称：0～36 个 Unicode 码位；合法 UTF-8；禁止 ASCII 控制字符与 DEL；总字节数上限 400。
static bool isValidDisplayNicknameUtf8(const std::string &s)
{
    const std::string t = trimAscii(s);
    if (t.size() > 400) {
        return false;
    }
    std::size_t i = 0;
    std::size_t cps = 0;
    while (i < t.size()) {
        char32_t cp = 0;
        if (!utf8NextCodepoint(t, i, cp)) {
            return false;
        }
        if (cp < 0x20u || cp == 0x7Fu) {
            return false;
        }
        ++cps;
        if (cps > 36) {
            return false;
        }
    }
    return true;
}

/// 群名：1～40 个 Unicode 码位；合法 UTF-8；禁止 ASCII 控制字符与 DEL；总字节数上限 200。
static bool isValidGroupNameUtf8(const std::string &s)
{
    const std::string t = trimAscii(s);
    if (t.empty() || t.size() > 200) {
        return false;
    }
    std::size_t i = 0;
    std::size_t cps = 0;
    while (i < t.size()) {
        char32_t cp = 0;
        if (!utf8NextCodepoint(t, i, cp)) {
            return false;
        }
        if (cp < 0x20u || cp == 0x7Fu) {
            return false;
        }
        ++cps;
        if (cps > 40) {
            return false;
        }
    }
    return cps >= 1 && cps <= 40;
}

static void migrateAddUsernameColumn(SqliteDynamic &api, sqlite3 *db)
{
    char *err = nullptr;
    const int rc = api.exec(db, "ALTER TABLE users ADD COLUMN username TEXT;", &err);
    if (rc != 0) {
        if (err != nullptr) {
            const std::string msg = err;
            api.free(err);
            err = nullptr;
            if (msg.find("duplicate column") == std::string::npos) {
                VSLOG_WARN(std::string("[db] ALTER users.username: ") + msg);
            }
        }
    }
}

static void migrateAddUserAvatarColumns(SqliteDynamic &api, sqlite3 *db)
{
    const char *sqls[] = {"ALTER TABLE users ADD COLUMN avatar_jpeg BLOB;",
                           "ALTER TABLE users ADD COLUMN avatar_rev INTEGER NOT NULL DEFAULT 0;"};
    for (const char *sql : sqls) {
        char *err = nullptr;
        const int rc = api.exec(db, sql, &err);
        if (rc != 0) {
            if (err != nullptr) {
                const std::string msg = err;
                api.free(err);
                err = nullptr;
                if (msg.find("duplicate column") == std::string::npos) {
                    VSLOG_WARN(std::string("[db] ALTER users avatar: ") + msg);
                }
            }
        } else if (err != nullptr) {
            api.free(err);
        }
    }
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

    const std::filesystem::path exe = appExecutableDirectory();
    if (!ensureDataDirectoryFs(exe)) {
        errMsg = "无法创建 data 目录";
        return false;
    }
    const std::string path = (exe / "data" / "chat.db").u8string();

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
                         "  avatar_jpeg BLOB,"
                         "  avatar_rev INTEGER NOT NULL DEFAULT 0,"
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
                         "CREATE INDEX IF NOT EXISTS idx_email_codes_email_purpose ON email_codes(email, purpose);"
                         "CREATE TABLE IF NOT EXISTS friend_requests ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  from_user_id INTEGER NOT NULL,"
                         "  to_user_id INTEGER NOT NULL,"
                         "  status TEXT NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  FOREIGN KEY(from_user_id) REFERENCES users(user_id),"
                         "  FOREIGN KEY(to_user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_friend_requests_to_st ON friend_requests(to_user_id, status);"
                         "CREATE INDEX IF NOT EXISTS idx_friend_requests_from ON friend_requests(from_user_id);"
                         "CREATE TABLE IF NOT EXISTS friends ("
                         "  user_id INTEGER NOT NULL,"
                         "  peer_user_id INTEGER NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  PRIMARY KEY (user_id, peer_user_id),"
                         "  FOREIGN KEY(user_id) REFERENCES users(user_id),"
                         "  FOREIGN KEY(peer_user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE TABLE IF NOT EXISTS messages ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  from_user_id INTEGER NOT NULL,"
                         "  to_user_id INTEGER NOT NULL,"
                         "  content TEXT NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  FOREIGN KEY(from_user_id) REFERENCES users(user_id),"
                         "  FOREIGN KEY(to_user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_messages_conv ON messages(from_user_id, to_user_id, id);"
                         "CREATE TABLE IF NOT EXISTS chat_groups ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  name TEXT NOT NULL,"
                         "  owner_user_id INTEGER NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  FOREIGN KEY(owner_user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE TABLE IF NOT EXISTS group_members ("
                         "  group_id INTEGER NOT NULL,"
                         "  user_id INTEGER NOT NULL,"
                         "  role TEXT NOT NULL,"
                         "  joined_at INTEGER NOT NULL,"
                         "  PRIMARY KEY (group_id, user_id),"
                         "  FOREIGN KEY(group_id) REFERENCES chat_groups(id) ON DELETE CASCADE,"
                         "  FOREIGN KEY(user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_group_members_user ON group_members(user_id, joined_at);"
                         "CREATE TABLE IF NOT EXISTS group_messages ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  group_id INTEGER NOT NULL,"
                         "  from_user_id INTEGER NOT NULL,"
                         "  content TEXT NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  FOREIGN KEY(group_id) REFERENCES chat_groups(id) ON DELETE CASCADE,"
                         "  FOREIGN KEY(from_user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_group_messages_group ON group_messages(group_id, id);"
                         "CREATE TABLE IF NOT EXISTS file_transfers ("
                         "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
                         "  from_user_id INTEGER NOT NULL,"
                         "  to_user_id INTEGER NOT NULL,"
                         "  file_name TEXT NOT NULL,"
                         "  file_size INTEGER NOT NULL,"
                         "  sha256_hex TEXT,"
                         "  status TEXT NOT NULL,"
                         "  created_at INTEGER NOT NULL,"
                         "  completed_at INTEGER,"
                         "  FOREIGN KEY(from_user_id) REFERENCES users(user_id),"
                         "  FOREIGN KEY(to_user_id) REFERENCES users(user_id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_file_transfers_from ON file_transfers(from_user_id, status);"
                         "CREATE INDEX IF NOT EXISTS idx_file_transfers_to ON file_transfers(to_user_id, status);"
                         "CREATE TABLE IF NOT EXISTS message_deletions ("
                         "  user_id INTEGER NOT NULL,"
                         "  message_id INTEGER NOT NULL,"
                         "  deleted_at INTEGER NOT NULL,"
                         "  PRIMARY KEY (user_id, message_id),"
                         "  FOREIGN KEY(user_id) REFERENCES users(user_id),"
                         "  FOREIGN KEY(message_id) REFERENCES messages(id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_message_deletions_user ON message_deletions(user_id, message_id);"
                         "CREATE TABLE IF NOT EXISTS group_message_deletions ("
                         "  user_id INTEGER NOT NULL,"
                         "  group_id INTEGER NOT NULL,"
                         "  message_id INTEGER NOT NULL,"
                         "  deleted_at INTEGER NOT NULL,"
                         "  PRIMARY KEY (user_id, message_id),"
                         "  FOREIGN KEY(user_id) REFERENCES users(user_id),"
                         "  FOREIGN KEY(group_id) REFERENCES chat_groups(id),"
                         "  FOREIGN KEY(message_id) REFERENCES group_messages(id)"
                         ");"
                         "CREATE INDEX IF NOT EXISTS idx_group_message_deletions_user ON group_message_deletions(user_id, group_id, message_id);";

    if (api.exec(g_db, schema, &err) != 0) {
        errMsg = err ? err : api.errmsg(g_db);
        api.free(err);
        api.close(g_db);
        g_db = nullptr;
        return false;
    }
    api.free(err);

    migrateAddUsernameColumn(api, g_db);
    migrateAddUserAvatarColumns(api, g_db);

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
                                     const std::string &usernameUtf8, const std::string &emailCodePlain)
{
    const std::string em = normalizeEmail(email);
    if (em.empty() || passwordPlain.empty()) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码不能为空");
    }
    if (em.size() > 128 || passwordPlain.size() > 256) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码过长");
    }
    const std::string userNorm = trimAscii(usernameUtf8);
    if (!isValidUsernameUtf8(userNorm)) {
        return fail(AuthErrorCode::InvalidInput, "用户名须为 2～32 位中英文或数字，且不能含空格与@");
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

    const char *checkUser = "SELECT 1 FROM users WHERE username = ? LIMIT 1;";
    if (api.prepare(g_db, checkUser, -1, &st, nullptr) != 0) {
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, userNorm.c_str(), static_cast<int>(userNorm.size()));
    const int stepUser = api.step(st);
    api.finalize(st);
    if (stepUser == 100) {
        return fail(AuthErrorCode::UsernameTaken, "用户名已被占用");
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
    const std::string ins = "INSERT INTO users (email, password_hash, salt, username, nickname, created_at) "
                            "VALUES (?,?,?,?,?,?);";
    sqlite3_stmt *insSt = nullptr;
    if (api.prepare(g_db, ins.c_str(), -1, &insSt, nullptr) != 0) {
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(insSt, 1, em.c_str(), static_cast<int>(em.size()));
    api.bind_text_transient(insSt, 2, hash.c_str(), static_cast<int>(hash.size()));
    api.bind_text_transient(insSt, 3, salt.c_str(), static_cast<int>(salt.size()));
    api.bind_text_transient(insSt, 4, userNorm.c_str(), static_cast<int>(userNorm.size()));
    api.bind_text_transient(insSt, 5, userNorm.c_str(), static_cast<int>(userNorm.size()));
    api.bind_text_transient(insSt, 6, nowStr.c_str(), static_cast<int>(nowStr.size()));

    if (api.step(insSt) != 101) { // SQLITE_DONE = 101
        api.finalize(insSt);
        const std::string dberr = api.errmsg(g_db);
        if (dberr.find("UNIQUE") != std::string::npos) {
            return fail(AuthErrorCode::UsernameTaken, "用户名已被占用");
        }
        return fail(AuthErrorCode::DbUnavailable, dberr);
    }
    api.finalize(insSt);

    const std::int64_t uid = api.last_insert_rowid(g_db);
    std::string token = randomTokenHex(32);
    {
        std::lock_guard<std::mutex> slock(g_sessMutex);
        g_tokenToUser[token] = uid;
    }
    return ok(uid, std::move(token), em, userNorm, 0, userNorm);
}

AuthResult AppDatabase::loginByEmail(const std::string &email, const std::string &passwordPlain)
{
    const std::string em = normalizeEmail(email);
    if (em.empty() || passwordPlain.empty()) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码不能为空");
    }
    if (em.find('@') == std::string::npos) {
        return fail(AuthErrorCode::InvalidInput, "邮箱格式不正确");
    }
    if (em.size() > 128 || passwordPlain.size() > 256) {
        return fail(AuthErrorCode::InvalidInput, "邮箱或密码过长");
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return fail(AuthErrorCode::DbUnavailable, "数据库不可用");
    }

    auto &api = sqliteApi();
    const char *sql =
        "SELECT user_id, salt, password_hash, IFNULL(nickname,''), IFNULL(avatar_rev,0), IFNULL(username,'') "
        "FROM users WHERE email = ? LIMIT 1;";
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
    const unsigned char *nickPtr = api.column_text(st, 3);
    const std::int64_t avRev = api.column_int64(st, 4);
    const unsigned char *userPtr = api.column_text(st, 5);
    const std::string salt = saltPtr ? reinterpret_cast<const char *>(saltPtr) : "";
    const std::string stored = hashPtr ? reinterpret_cast<const char *>(hashPtr) : "";
    const std::string nick = nickPtr ? reinterpret_cast<const char *>(nickPtr) : "";
    const std::string userName = userPtr ? reinterpret_cast<const char *>(userPtr) : "";
    api.finalize(st);

    if (!verifyPassword(salt, passwordPlain, stored)) {
        return fail(AuthErrorCode::InvalidCredentials, "密码错误");
    }

    std::string token = randomTokenHex(32);
    {
        std::lock_guard<std::mutex> slock(g_sessMutex);
        g_tokenToUser[token] = uid;
    }
    return ok(uid, std::move(token), em, nick, avRev, userName);
}

AuthResult AppDatabase::loginByUsername(const std::string &username, const std::string &passwordPlain)
{
    const std::string userNorm = trimAscii(username);
    if (userNorm.empty() || passwordPlain.empty()) {
        return fail(AuthErrorCode::InvalidInput, "用户名或密码不能为空");
    }
    if (passwordPlain.size() > 256) {
        return fail(AuthErrorCode::InvalidInput, "密码过长");
    }
    if (!isValidUsernameUtf8(userNorm)) {
        return fail(AuthErrorCode::InvalidInput, "用户名格式不正确");
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return fail(AuthErrorCode::DbUnavailable, "数据库不可用");
    }

    auto &api = sqliteApi();
    const char *sql =
        "SELECT user_id, salt, password_hash, email, IFNULL(nickname,''), IFNULL(avatar_rev,0) FROM users WHERE "
        "username = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return fail(AuthErrorCode::DbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, userNorm.c_str(), static_cast<int>(userNorm.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return fail(AuthErrorCode::EmailNotFound, "用户不存在");
    }

    const std::int64_t uid = api.column_int64(st, 0);
    const unsigned char *saltPtr = api.column_text(st, 1);
    const unsigned char *hashPtr = api.column_text(st, 2);
    const unsigned char *emailPtr = api.column_text(st, 3);
    const unsigned char *nickPtr = api.column_text(st, 4);
    const std::int64_t avRev = api.column_int64(st, 5);
    const std::string salt = saltPtr ? reinterpret_cast<const char *>(saltPtr) : "";
    const std::string stored = hashPtr ? reinterpret_cast<const char *>(hashPtr) : "";
    const std::string em = emailPtr ? reinterpret_cast<const char *>(emailPtr) : "";
    const std::string nick = nickPtr ? reinterpret_cast<const char *>(nickPtr) : "";
    api.finalize(st);

    if (!verifyPassword(salt, passwordPlain, stored)) {
        return fail(AuthErrorCode::InvalidCredentials, "密码错误");
    }

    std::string token = randomTokenHex(32);
    {
        std::lock_guard<std::mutex> slock(g_sessMutex);
        g_tokenToUser[token] = uid;
    }
    return ok(uid, std::move(token), normalizeEmail(em), nick, avRev, userNorm);
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

namespace {

constexpr const char *kFrPending = "pending";
constexpr const char *kFrAccepted = "accepted";
constexpr const char *kFrRejected = "rejected";

AppDatabase::FriendOpOutcome foFail(const int code, const char *msg)
{
    AppDatabase::FriendOpOutcome o;
    o.ok = false;
    o.errCode = code;
    o.message = msg;
    return o;
}

AppDatabase::FriendOpOutcome foOk()
{
    AppDatabase::FriendOpOutcome o;
    o.ok = true;
    return o;
}

bool lookupUserIdByEmailNorm(SqliteDynamic &api, sqlite3 *db, const std::string &em, std::int64_t &outId)
{
    const char *sql = "SELECT user_id FROM users WHERE email = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    api.bind_text_transient(st, 1, em.c_str(), static_cast<int>(em.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return false;
    }
    outId = api.column_int64(st, 0);
    api.finalize(st);
    return true;
}

bool userExists(SqliteDynamic &api, sqlite3 *db, std::int64_t uid)
{
    const char *sql = "SELECT 1 FROM users WHERE user_id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    const std::string idStr = std::to_string(uid);
    api.bind_text_transient(st, 1, idStr.c_str(), static_cast<int>(idStr.size()));
    const int sr = api.step(st);
    api.finalize(st);
    return sr == 100;
}

bool areFriends(SqliteDynamic &api, sqlite3 *db, std::int64_t a, std::int64_t b)
{
    const char *sql = "SELECT 1 FROM friends WHERE user_id = ? AND peer_user_id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    const std::string sa = std::to_string(a);
    const std::string sb = std::to_string(b);
    api.bind_text_transient(st, 1, sa.c_str(), static_cast<int>(sa.size()));
    api.bind_text_transient(st, 2, sb.c_str(), static_cast<int>(sb.size()));
    const int sr = api.step(st);
    api.finalize(st);
    return sr == 100;
}

bool hasPendingRequest(SqliteDynamic &api, sqlite3 *db, std::int64_t fromUid, std::int64_t toUid)
{
    const char *sql =
        "SELECT 1 FROM friend_requests WHERE from_user_id = ? AND to_user_id = ? AND status = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    const std::string f = std::to_string(fromUid);
    const std::string t = std::to_string(toUid);
    api.bind_text_transient(st, 1, f.c_str(), static_cast<int>(f.size()));
    api.bind_text_transient(st, 2, t.c_str(), static_cast<int>(t.size()));
    api.bind_text_transient(st, 3, kFrPending, static_cast<int>(std::strlen(kFrPending)));
    const int sr = api.step(st);
    api.finalize(st);
    return sr == 100;
}

bool groupExists(SqliteDynamic &api, sqlite3 *db, std::int64_t groupId)
{
    const char *sql = "SELECT 1 FROM chat_groups WHERE id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    const std::string gid = std::to_string(groupId);
    api.bind_text_transient(st, 1, gid.c_str(), static_cast<int>(gid.size()));
    const int sr = api.step(st);
    api.finalize(st);
    return sr == 100;
}

bool groupLookupMembership(SqliteDynamic &api, sqlite3 *db, std::int64_t groupId, std::int64_t userId,
                           std::string *outRole = nullptr)
{
    const char *sql = "SELECT role FROM group_members WHERE group_id = ? AND user_id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    const std::string gid = std::to_string(groupId);
    const std::string uid = std::to_string(userId);
    api.bind_text_transient(st, 1, gid.c_str(), static_cast<int>(gid.size()));
    api.bind_text_transient(st, 2, uid.c_str(), static_cast<int>(uid.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return false;
    }
    if (outRole != nullptr) {
        const unsigned char *rp = api.column_text(st, 0);
        *outRole = rp ? reinterpret_cast<const char *>(rp) : "";
    }
    api.finalize(st);
    return true;
}

} // namespace (friend helpers)

AppDatabase::FriendOpOutcome AppDatabase::friendSearch(const std::int64_t selfUserId, const std::string &query,
                                                       int limit, std::vector<AppDatabase::FriendSearchHit> &outUsers)
{
    outUsers.clear();
    const std::string q = trimAscii(query);
    if (q.empty()) {
        return foFail(kErrInvalidInput, "搜索内容不能为空");
    }
    if (limit < 1) {
        limit = 1;
    }
    if (limit > 50) {
        limit = 50;
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();

    const char *sql =
        "SELECT u.user_id, u.email, IFNULL(u.nickname,'') FROM users u "
        "WHERE u.user_id != ? AND u.email LIKE '%' || ? || '%' COLLATE NOCASE "
        "AND NOT EXISTS (SELECT 1 FROM friends f WHERE f.user_id = ? AND f.peer_user_id = u.user_id) "
        "AND NOT EXISTS (SELECT 1 FROM friend_requests fr WHERE fr.from_user_id = ? AND fr.to_user_id = u.user_id "
        "AND fr.status = ?) "
        "AND NOT EXISTS (SELECT 1 FROM friend_requests fr2 WHERE fr2.from_user_id = u.user_id AND fr2.to_user_id = ? "
        "AND fr2.status = ?) "
        "ORDER BY u.email ASC LIMIT ?;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    const std::string selfStr = std::to_string(selfUserId);
    const std::string limStr = std::to_string(limit);
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 2, q.c_str(), static_cast<int>(q.size()));
    api.bind_text_transient(st, 3, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 4, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 5, kFrPending, static_cast<int>(std::strlen(kFrPending)));
    api.bind_text_transient(st, 6, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 7, kFrPending, static_cast<int>(std::strlen(kFrPending)));
    api.bind_text_transient(st, 8, limStr.c_str(), static_cast<int>(limStr.size()));

    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        AppDatabase::FriendSearchHit h;
        h.userId = api.column_int64(st, 0);
        const unsigned char *ep = api.column_text(st, 1);
        const unsigned char *np = api.column_text(st, 2);
        h.email = ep ? reinterpret_cast<const char *>(ep) : "";
        h.nickname = np ? reinterpret_cast<const char *>(np) : "";
        outUsers.push_back(std::move(h));
    }
    api.finalize(st);
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::friendRequestSend(const std::int64_t selfUserId,
                                                            std::int64_t targetUserId,
                                                            const std::string &targetEmail,
                                                            std::int64_t &outRequestId,
                                                            std::int64_t &outTargetUserId)
{
    outRequestId = 0;
    outTargetUserId = 0;
    std::int64_t tid = targetUserId;
    if (tid <= 0) {
        const std::string em = normalizeEmail(targetEmail);
        if (em.empty()) {
            return foFail(kErrInvalidInput, "缺少 target_user_id 或 target_email");
        }
        std::lock_guard<std::mutex> lock(g_dbMutex);
        if (!g_ready || g_db == nullptr) {
            return foFail(kErrDbUnavailable, "数据库不可用");
        }
        if (!lookupUserIdByEmailNorm(sqliteApi(), g_db, em, tid)) {
            return foFail(kErrFriendUserNotFound, "用户不存在");
        }
        if (tid == selfUserId) {
            return foFail(kErrFriendCannotSelf, "不能添加自己");
        }
        auto &api = sqliteApi();
        if (areFriends(api, g_db, selfUserId, tid)) {
            return foFail(kErrFriendAlready, "已是好友");
        }
        if (hasPendingRequest(api, g_db, selfUserId, tid)) {
            return foFail(kErrFriendPendingExists, "已发送待处理申请");
        }
        if (hasPendingRequest(api, g_db, tid, selfUserId)) {
            return foFail(kErrFriendTheyPending, "对方已向你发起申请，请在申请列表中处理");
        }
        const std::time_t now = std::time(nullptr);
        const std::string nowStr = std::to_string(static_cast<long long>(now));
        const char *ins =
            "INSERT INTO friend_requests (from_user_id, to_user_id, status, created_at) VALUES (?,?,?,?);";
        sqlite3_stmt *ist = nullptr;
        if (api.prepare(g_db, ins, -1, &ist, nullptr) != 0) {
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        const std::string fs = std::to_string(selfUserId);
        const std::string ts = std::to_string(tid);
        api.bind_text_transient(ist, 1, fs.c_str(), static_cast<int>(fs.size()));
        api.bind_text_transient(ist, 2, ts.c_str(), static_cast<int>(ts.size()));
        api.bind_text_transient(ist, 3, kFrPending, static_cast<int>(std::strlen(kFrPending)));
        api.bind_text_transient(ist, 4, nowStr.c_str(), static_cast<int>(nowStr.size()));
        if (api.step(ist) != 101) {
            api.finalize(ist);
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.finalize(ist);
        outRequestId = api.last_insert_rowid(g_db);
        outTargetUserId = tid;
        return foOk();
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    if (tid == selfUserId) {
        return foFail(kErrFriendCannotSelf, "不能添加自己");
    }
    if (!userExists(api, g_db, tid)) {
        return foFail(kErrFriendUserNotFound, "用户不存在");
    }
    if (areFriends(api, g_db, selfUserId, tid)) {
        return foFail(kErrFriendAlready, "已是好友");
    }
    if (hasPendingRequest(api, g_db, selfUserId, tid)) {
        return foFail(kErrFriendPendingExists, "已发送待处理申请");
    }
    if (hasPendingRequest(api, g_db, tid, selfUserId)) {
        return foFail(kErrFriendTheyPending, "对方已向你发起申请，请在申请列表中处理");
    }
    const std::time_t now = std::time(nullptr);
    const std::string nowStr = std::to_string(static_cast<long long>(now));
    const char *ins =
        "INSERT INTO friend_requests (from_user_id, to_user_id, status, created_at) VALUES (?,?,?,?);";
    sqlite3_stmt *ist = nullptr;
    if (api.prepare(g_db, ins, -1, &ist, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    const std::string fs = std::to_string(selfUserId);
    const std::string ts = std::to_string(tid);
    api.bind_text_transient(ist, 1, fs.c_str(), static_cast<int>(fs.size()));
    api.bind_text_transient(ist, 2, ts.c_str(), static_cast<int>(ts.size()));
    api.bind_text_transient(ist, 3, kFrPending, static_cast<int>(std::strlen(kFrPending)));
    api.bind_text_transient(ist, 4, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(ist) != 101) {
        api.finalize(ist);
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(ist);
    outRequestId = api.last_insert_rowid(g_db);
    outTargetUserId = tid;
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::friendRequestList(const std::int64_t selfUserId,
                                                            std::vector<AppDatabase::FriendPendingRow> &incoming,
                                                            std::vector<AppDatabase::FriendPendingRow> &outgoing)
{
    incoming.clear();
    outgoing.clear();
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    const std::string selfStr = std::to_string(selfUserId);

    const char *sqlIn =
        "SELECT fr.id, fr.from_user_id, u.email, IFNULL(u.nickname,''), fr.created_at "
        "FROM friend_requests fr JOIN users u ON u.user_id = fr.from_user_id "
        "WHERE fr.to_user_id = ? AND fr.status = ? ORDER BY fr.created_at DESC;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sqlIn, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 2, kFrPending, static_cast<int>(std::strlen(kFrPending)));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        AppDatabase::FriendPendingRow r;
        r.requestId = api.column_int64(st, 0);
        r.otherUserId = api.column_int64(st, 1);
        const unsigned char *ep = api.column_text(st, 2);
        const unsigned char *np = api.column_text(st, 3);
        r.email = ep ? reinterpret_cast<const char *>(ep) : "";
        r.nickname = np ? reinterpret_cast<const char *>(np) : "";
        r.createdAt = api.column_int64(st, 4);
        incoming.push_back(std::move(r));
    }
    api.finalize(st);

    const char *sqlOut =
        "SELECT fr.id, fr.to_user_id, u.email, IFNULL(u.nickname,''), fr.created_at "
        "FROM friend_requests fr JOIN users u ON u.user_id = fr.to_user_id "
        "WHERE fr.from_user_id = ? AND fr.status = ? ORDER BY fr.created_at DESC;";
    if (api.prepare(g_db, sqlOut, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 2, kFrPending, static_cast<int>(std::strlen(kFrPending)));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        AppDatabase::FriendPendingRow r;
        r.requestId = api.column_int64(st, 0);
        r.otherUserId = api.column_int64(st, 1);
        const unsigned char *ep = api.column_text(st, 2);
        const unsigned char *np = api.column_text(st, 3);
        r.email = ep ? reinterpret_cast<const char *>(ep) : "";
        r.nickname = np ? reinterpret_cast<const char *>(np) : "";
        r.createdAt = api.column_int64(st, 4);
        outgoing.push_back(std::move(r));
    }
    api.finalize(st);
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::friendRequestHandle(const std::int64_t selfUserId,
                                                              const std::int64_t requestId,
                                                              const std::string &action,
                                                              std::int64_t &outPeerId)
{
    outPeerId = 0;
    const std::string act = trimAscii(action);
    if (act != "accept" && act != "reject") {
        return foFail(kErrInvalidInput, "action 须为 accept 或 reject");
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    const char *sel = "SELECT from_user_id, to_user_id, status FROM friend_requests WHERE id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sel, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    const std::string rid = std::to_string(requestId);
    api.bind_text_transient(st, 1, rid.c_str(), static_cast<int>(rid.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return foFail(kErrFriendRequestGone, "申请不存在或已处理");
    }
    const std::int64_t fromUid = api.column_int64(st, 0);
    const std::int64_t toUid = api.column_int64(st, 1);
    const unsigned char *sp = api.column_text(st, 2);
    const std::string stt = sp ? reinterpret_cast<const char *>(sp) : "";
    api.finalize(st);

    if (toUid != selfUserId) {
        return foFail(kErrFriendRequestNotYours, "无权处理该申请");
    }
    if (stt != kFrPending) {
        return foFail(kErrFriendRequestGone, "申请不存在或已处理");
    }

    outPeerId = fromUid;

    if (act == "reject") {
        const char *up = "UPDATE friend_requests SET status = ? WHERE id = ?;";
        sqlite3_stmt *u = nullptr;
        if (api.prepare(g_db, up, -1, &u, nullptr) != 0) {
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.bind_text_transient(u, 1, kFrRejected, static_cast<int>(std::strlen(kFrRejected)));
        api.bind_text_transient(u, 2, rid.c_str(), static_cast<int>(rid.size()));
        if (api.step(u) != 101) {
            api.finalize(u);
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.finalize(u);
        return foOk();
    }

    const char *up = "UPDATE friend_requests SET status = ? WHERE id = ?;";
    sqlite3_stmt *u = nullptr;
    if (api.prepare(g_db, up, -1, &u, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(u, 1, kFrAccepted, static_cast<int>(std::strlen(kFrAccepted)));
    api.bind_text_transient(u, 2, rid.c_str(), static_cast<int>(rid.size()));
    if (api.step(u) != 101) {
        api.finalize(u);
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(u);

    const std::time_t now = std::time(nullptr);
    const std::string nowStr = std::to_string(static_cast<long long>(now));
    const std::string a = std::to_string(fromUid);
    const std::string b = std::to_string(toUid);
    const char *ins = "INSERT OR IGNORE INTO friends (user_id, peer_user_id, created_at) VALUES (?,?,?);";
    for (int k = 0; k < 2; ++k) {
        sqlite3_stmt *ist = nullptr;
        if (api.prepare(g_db, ins, -1, &ist, nullptr) != 0) {
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        if (k == 0) {
            api.bind_text_transient(ist, 1, a.c_str(), static_cast<int>(a.size()));
            api.bind_text_transient(ist, 2, b.c_str(), static_cast<int>(b.size()));
        } else {
            api.bind_text_transient(ist, 1, b.c_str(), static_cast<int>(b.size()));
            api.bind_text_transient(ist, 2, a.c_str(), static_cast<int>(a.size()));
        }
        api.bind_text_transient(ist, 3, nowStr.c_str(), static_cast<int>(nowStr.size()));
        if (api.step(ist) != 101) {
            api.finalize(ist);
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.finalize(ist);
    }
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::friendList(const std::int64_t selfUserId,
                                                       std::vector<AppDatabase::FriendListRow> &out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    const char *sql =
        "SELECT t.peer_user_id, t.email, t.nn, t.avrev, t.created_at, t.last_content, t.last_at, t.last_from "
        "FROM ("
        "  SELECT f.peer_user_id, u.email AS email, IFNULL(u.nickname,'') AS nn, IFNULL(u.avatar_rev,0) AS avrev, "
        "f.created_at,"
        "    (SELECT m.content FROM messages m"
        "     WHERE ((m.from_user_id = f.user_id AND m.to_user_id = f.peer_user_id)"
        "        OR (m.from_user_id = f.peer_user_id AND m.to_user_id = f.user_id))"
        "       AND NOT EXISTS (SELECT 1 FROM message_deletions d WHERE d.user_id = f.user_id AND d.message_id = m.id)"
        "     ORDER BY m.id DESC LIMIT 1) AS last_content,"
        "    (SELECT m.created_at FROM messages m"
        "     WHERE ((m.from_user_id = f.user_id AND m.to_user_id = f.peer_user_id)"
        "        OR (m.from_user_id = f.peer_user_id AND m.to_user_id = f.user_id))"
        "       AND NOT EXISTS (SELECT 1 FROM message_deletions d WHERE d.user_id = f.user_id AND d.message_id = m.id)"
        "     ORDER BY m.id DESC LIMIT 1) AS last_at,"
        "    (SELECT m.from_user_id FROM messages m"
        "     WHERE ((m.from_user_id = f.user_id AND m.to_user_id = f.peer_user_id)"
        "        OR (m.from_user_id = f.peer_user_id AND m.to_user_id = f.user_id))"
        "       AND NOT EXISTS (SELECT 1 FROM message_deletions d WHERE d.user_id = f.user_id AND d.message_id = m.id)"
        "     ORDER BY m.id DESC LIMIT 1) AS last_from"
        "  FROM friends f"
        "  JOIN users u ON u.user_id = f.peer_user_id"
        "  WHERE f.user_id = ?"
        ") AS t "
        "ORDER BY IFNULL(t.last_at, 0) DESC, t.email COLLATE NOCASE ASC;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    const std::string selfStr = std::to_string(selfUserId);
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        AppDatabase::FriendListRow r;
        r.userId = api.column_int64(st, 0);
        const unsigned char *ep = api.column_text(st, 1);
        const unsigned char *np = api.column_text(st, 2);
        r.email = ep ? reinterpret_cast<const char *>(ep) : "";
        r.nickname = np ? reinterpret_cast<const char *>(np) : "";
        r.avatarRev = api.column_int64(st, 3);
        r.createdAt = api.column_int64(st, 4);
        const unsigned char *lp = api.column_text(st, 5);
        if (lp) {
            r.lastMessageContent.assign(reinterpret_cast<const char *>(lp));
            /// 与单条消息 content 上限一致；文件/表情类消息为 JSON，原先 200 字节会截断导致客户端无法解析。
            constexpr std::size_t kPreviewMax = 8192;
            if (r.lastMessageContent.size() > kPreviewMax) {
                r.lastMessageContent.resize(kPreviewMax);
            }
        }
        r.lastMessageAt = api.column_int64(st, 6);
        r.lastMessageFromUserId = api.column_int64(st, 7);
        out.push_back(std::move(r));
    }
    api.finalize(st);
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::friendPeerIds(const std::int64_t selfUserId, std::vector<std::int64_t> &out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    const char *sql = "SELECT peer_user_id FROM friends WHERE user_id = ? ORDER BY peer_user_id;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    const std::string selfStr = std::to_string(selfUserId);
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        out.push_back(api.column_int64(st, 0));
    }
    api.finalize(st);
    return foOk();
}

bool AppDatabase::areUsersFriends(const std::int64_t userId, const std::int64_t peerUserId)
{
    if (userId <= 0 || peerUserId <= 0 || userId == peerUserId) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return false;
    }
    return areFriends(sqliteApi(), g_db, userId, peerUserId);
}

AppDatabase::FriendOpOutcome AppDatabase::setNickname(const std::int64_t selfUserId, const std::string &nicknameUtf8)
{
    const std::string n = trimAscii(nicknameUtf8);
    if (!isValidDisplayNicknameUtf8(n)) {
        return foFail(kErrProfileNickname, "昵称长度或内容不符合要求");
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    const char *up = "UPDATE users SET nickname = ? WHERE user_id = ?;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, up, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, n.c_str(), static_cast<int>(n.size()));
    const std::string uidStr = std::to_string(selfUserId);
    api.bind_text_transient(st, 2, uidStr.c_str(), static_cast<int>(uidStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(st);
    if (api.changes(g_db) == 0) {
        return foFail(kErrUserNotFound, "用户不存在");
    }
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::setUserAvatarJpeg(const std::int64_t selfUserId,
                                                            const std::vector<std::uint8_t> &jpegBytes,
                                                            std::int64_t &outNewRev)
{
    constexpr std::size_t kMaxAvatarJpegBytes = 180000;
    const auto jpegOk = [](const std::vector<std::uint8_t> &b) -> bool {
        return b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
    };
    outNewRev = 0;
    if (jpegBytes.empty() || jpegBytes.size() > kMaxAvatarJpegBytes) {
        return foFail(kErrProfileAvatar, "头像须为 JPEG 且不超过大小限制");
    }
    if (!jpegOk(jpegBytes)) {
        return foFail(kErrProfileAvatar, "头像须为 JPEG 格式");
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    const char *up =
        "UPDATE users SET avatar_jpeg = ?, avatar_rev = IFNULL(avatar_rev, 0) + 1 WHERE user_id = ?;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, up, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_blob_transient(st, 1, jpegBytes.data(), static_cast<int>(jpegBytes.size()));
    const std::string uidStr = std::to_string(selfUserId);
    api.bind_text_transient(st, 2, uidStr.c_str(), static_cast<int>(uidStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(st);
    if (api.changes(g_db) == 0) {
        return foFail(kErrUserNotFound, "用户不存在");
    }
    const char *sel = "SELECT IFNULL(avatar_rev,0) FROM users WHERE user_id = ? LIMIT 1;";
    sqlite3_stmt *rd = nullptr;
    if (api.prepare(g_db, sel, -1, &rd, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(rd, 1, uidStr.c_str(), static_cast<int>(uidStr.size()));
    if (api.step(rd) != 100) {
        api.finalize(rd);
        return foFail(kErrDbUnavailable, "读取头像版本失败");
    }
    outNewRev = api.column_int64(rd, 0);
    api.finalize(rd);
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::getFriendAvatarJpeg(const std::int64_t selfUserId,
                                                              const std::int64_t peerUserId,
                                                              std::vector<std::uint8_t> &outJpeg,
                                                              std::int64_t &outRev)
{
    outJpeg.clear();
    outRev = 0;
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    if (!areFriends(api, g_db, selfUserId, peerUserId)) {
        return foFail(kErrFriendNotFriend, "非好友关系");
    }
    const char *sql = "SELECT IFNULL(avatar_rev,0), avatar_jpeg FROM users WHERE user_id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return foFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    const std::string peerStr = std::to_string(peerUserId);
    api.bind_text_transient(st, 1, peerStr.c_str(), static_cast<int>(peerStr.size()));
    if (api.step(st) != 100) {
        api.finalize(st);
        return foFail(kErrUserNotFound, "用户不存在");
    }
    outRev = api.column_int64(st, 0);
    const void *blob = api.column_blob(st, 1);
    const int nb = api.column_bytes(st, 1);
    if (blob != nullptr && nb > 0) {
        outJpeg.assign(static_cast<const std::uint8_t *>(blob), static_cast<const std::uint8_t *>(blob) + nb);
    }
    api.finalize(st);
    return foOk();
}

AppDatabase::FriendOpOutcome AppDatabase::friendDelete(const std::int64_t selfUserId,
                                                       const std::int64_t peerUserId)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return foFail(kErrDbUnavailable, "数据库不可用");
    }
    auto &api = sqliteApi();
    if (!areFriends(api, g_db, selfUserId, peerUserId)) {
        return foFail(kErrFriendNotFriend, "非好友关系");
    }
    const char *del = "DELETE FROM friends WHERE user_id = ? AND peer_user_id = ?;";
    for (int k = 0; k < 2; ++k) {
        sqlite3_stmt *d = nullptr;
        if (api.prepare(g_db, del, -1, &d, nullptr) != 0) {
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        const std::string a = std::to_string(k == 0 ? selfUserId : peerUserId);
        const std::string b = std::to_string(k == 0 ? peerUserId : selfUserId);
        api.bind_text_transient(d, 1, a.c_str(), static_cast<int>(a.size()));
        api.bind_text_transient(d, 2, b.c_str(), static_cast<int>(b.size()));
        if (api.step(d) != 101) {
            api.finalize(d);
            return foFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.finalize(d);
    }
    return foOk();
}

AppDatabase::FileOpOutcome AppDatabase::fileTransferInsertOffer(const std::int64_t fromUserId,
                                                                const std::int64_t toUserId,
                                                                const std::string &fileNameUtf8,
                                                                const std::int64_t fileSizeBytes,
                                                                const std::string &sha256HexLower)
{
    FileOpOutcome o;
    o.ok = false;
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    const std::time_t now = std::time(nullptr);
    const std::string nowStr = std::to_string(static_cast<long long>(now));
    const std::string fs = std::to_string(fileSizeBytes);
    const char *ins = "INSERT INTO file_transfers (from_user_id, to_user_id, file_name, file_size, sha256_hex, "
                    "status, created_at) VALUES (?,?,?,?,?,?,?);";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, ins, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    const std::string fuid = std::to_string(fromUserId);
    const std::string tuid = std::to_string(toUserId);
    api.bind_text_transient(st, 1, fuid.c_str(), static_cast<int>(fuid.size()));
    api.bind_text_transient(st, 2, tuid.c_str(), static_cast<int>(tuid.size()));
    api.bind_text_transient(st, 3, fileNameUtf8.c_str(), static_cast<int>(fileNameUtf8.size()));
    api.bind_text_transient(st, 4, fs.c_str(), static_cast<int>(fs.size()));
    api.bind_text_transient(st, 5, sha256HexLower.c_str(), static_cast<int>(sha256HexLower.size()));
    const char *stAwait = "awaiting_accept";
    api.bind_text_transient(st, 6, stAwait, static_cast<int>(std::strlen(stAwait)));
    api.bind_text_transient(st, 7, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.finalize(st);
    o.ok = true;
    o.transferId = api.last_insert_rowid(g_db);
    return o;
}

AppDatabase::FileOpOutcome AppDatabase::fileTransferSetStatus(const std::int64_t transferId,
                                                              const std::string &statusUtf8,
                                                              const std::int64_t completedAtUnixOrZero)
{
    FileOpOutcome o;
    o.ok = false;
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    const std::string tid = std::to_string(transferId);
    if (completedAtUnixOrZero > 0) {
        const std::string ct = std::to_string(static_cast<long long>(completedAtUnixOrZero));
        const char *sql =
            "UPDATE file_transfers SET status = ?, completed_at = ? WHERE id = ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.bind_text_transient(st, 1, statusUtf8.c_str(), static_cast<int>(statusUtf8.size()));
        api.bind_text_transient(st, 2, ct.c_str(), static_cast<int>(ct.size()));
        api.bind_text_transient(st, 3, tid.c_str(), static_cast<int>(tid.size()));
        if (api.step(st) != 101) {
            api.finalize(st);
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.finalize(st);
    } else {
        const char *sql = "UPDATE file_transfers SET status = ? WHERE id = ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.bind_text_transient(st, 1, statusUtf8.c_str(), static_cast<int>(statusUtf8.size()));
        api.bind_text_transient(st, 2, tid.c_str(), static_cast<int>(tid.size()));
        if (api.step(st) != 101) {
            api.finalize(st);
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.finalize(st);
    }
    /// `sqlite3_changes` 在「值未变」的 UPDATE 上可能为 0；此时校验当前行是否已是目标状态（幂等）。
    if (api.changes(g_db) != 1) {
        const char *q = "SELECT status FROM file_transfers WHERE id = ? LIMIT 1;";
        sqlite3_stmt *st2 = nullptr;
        if (api.prepare(g_db, q, -1, &st2, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.bind_text_transient(st2, 1, tid.c_str(), static_cast<int>(tid.size()));
        const int sr = api.step(st2);
        if (sr != 100) {
            api.finalize(st2);
            o.errCode = kErrFileNotFound;
            o.message = "传输记录不存在或未更新";
            return o;
        }
        const unsigned char *stt = api.column_text(st2, 0);
        const std::string cur = stt ? reinterpret_cast<const char *>(stt) : std::string();
        api.finalize(st2);
        if (cur != statusUtf8) {
            o.errCode = kErrFileNotFound;
            o.message = "传输记录不存在或未更新";
            return o;
        }
    }
    o.ok = true;
    return o;
}

AppDatabase::FileTransferLookupRow AppDatabase::fileTransferLookupParticipant(const std::int64_t transferId,
                                                                              const std::int64_t userId)
{
    FileTransferLookupRow r;
    if (transferId <= 0 || userId <= 0) {
        return r;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return r;
    }
    auto &api = sqliteApi();
    const char *sql =
        "SELECT from_user_id, to_user_id, file_size, file_name, status, IFNULL(sha256_hex,'') FROM file_transfers "
        "WHERE id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return r;
    }
    const std::string tid = std::to_string(transferId);
    api.bind_text_transient(st, 1, tid.c_str(), static_cast<int>(tid.size()));
    if (api.step(st) != 100) {
        api.finalize(st);
        return r;
    }
    const std::int64_t fromU = api.column_int64(st, 0);
    const std::int64_t toU = api.column_int64(st, 1);
    const std::int64_t fsz = api.column_int64(st, 2);
    const unsigned char *fn = api.column_text(st, 3);
    const unsigned char *stt = api.column_text(st, 4);
    const unsigned char *sha = api.column_text(st, 5);
    api.finalize(st);
    if (fromU != userId && toU != userId) {
        return r;
    }
    r.ok = true;
    r.fromUserId = fromU;
    r.toUserId = toU;
    r.fileSizeBytes = fsz;
    r.fileNameUtf8 = fn ? reinterpret_cast<const char *>(fn) : "";
    r.statusUtf8 = stt ? reinterpret_cast<const char *>(stt) : "";
    r.sha256HexLower = sha ? reinterpret_cast<const char *>(sha) : "";
    return r;
}

bool AppDatabase::tryGetUserPublic(const std::int64_t userId, std::string &outEmail, std::string &outNickname)
{
    outEmail.clear();
    outNickname.clear();
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return false;
    }
    auto &api = sqliteApi();
    const char *sql = "SELECT email, IFNULL(nickname,'') FROM users WHERE user_id = ? LIMIT 1;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        return false;
    }
    const std::string idStr = std::to_string(userId);
    api.bind_text_transient(st, 1, idStr.c_str(), static_cast<int>(idStr.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return false;
    }
    const unsigned char *ep = api.column_text(st, 0);
    const unsigned char *np = api.column_text(st, 1);
    outEmail = ep ? reinterpret_cast<const char *>(ep) : "";
    outNickname = np ? reinterpret_cast<const char *>(np) : "";
    api.finalize(st);
    return true;
}

static constexpr std::size_t kMsgMaxContentUtf8Bytes = 8192;

static AppDatabase::MsgOpOutcome msgOpFail(const int code, std::string msg)
{
    AppDatabase::MsgOpOutcome o;
    o.ok = false;
    o.errCode = code;
    o.message = std::move(msg);
    return o;
}

AppDatabase::MsgOpOutcome AppDatabase::messageSend(const std::int64_t fromUserId, const std::int64_t toUserId,
                                                   const std::string &contentUtf8)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return msgOpFail(kErrDbUnavailable, "数据库不可用");
    }
    if (fromUserId == toUserId) {
        return msgOpFail(kErrInvalidInput, "不能给自己发消息");
    }
    if (contentUtf8.empty()) {
        return msgOpFail(kErrMsgTooLong, "消息内容不能为空");
    }
    if (contentUtf8.size() > kMsgMaxContentUtf8Bytes) {
        return msgOpFail(kErrMsgTooLong, "消息过长");
    }
    auto &api = sqliteApi();
    if (!areFriends(api, g_db, fromUserId, toUserId)) {
        return msgOpFail(kErrMsgNotFriend, "非好友关系");
    }

    const std::time_t now = std::time(nullptr);
    const std::int64_t nowSec = static_cast<std::int64_t>(now);
    const std::string nowStr = std::to_string(static_cast<long long>(nowSec));
    const std::string fs = std::to_string(fromUserId);
    const std::string ts = std::to_string(toUserId);
    const char *ins =
        "INSERT INTO messages (from_user_id, to_user_id, content, created_at) VALUES (?,?,?,?);";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, ins, -1, &st, nullptr) != 0) {
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, fs.c_str(), static_cast<int>(fs.size()));
    api.bind_text_transient(st, 2, ts.c_str(), static_cast<int>(ts.size()));
    api.bind_text_transient(st, 3, contentUtf8.c_str(), static_cast<int>(contentUtf8.size()));
    api.bind_text_transient(st, 4, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(st);

    MsgOpOutcome o;
    o.ok = true;
    o.messageId = api.last_insert_rowid(g_db);
    o.createdAt = nowSec;
    return o;
}

AppDatabase::MsgOpOutcome AppDatabase::messageInsertChatRecord(const std::int64_t fromUserId, const std::int64_t toUserId,
                                                               const std::string &contentUtf8)
{
    return messageSend(fromUserId, toUserId, contentUtf8);
}

AppDatabase::MsgOpOutcome AppDatabase::messageFetch(const std::int64_t selfUserId, const std::int64_t peerUserId,
                                                    const std::int64_t afterId, const std::int64_t beforeExclusive,
                                                    int limit, std::vector<ChatMessageRow> &out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return msgOpFail(kErrDbUnavailable, "数据库不可用");
    }
    if (selfUserId == peerUserId) {
        return msgOpFail(kErrInvalidInput, "无效的会话");
    }
    auto &api = sqliteApi();
    if (!areFriends(api, g_db, selfUserId, peerUserId)) {
        return msgOpFail(kErrMsgNotFriend, "非好友关系");
    }
    if (limit <= 0) {
        limit = 50;
    }
    if (limit > 200) {
        limit = 200;
    }

    const std::string selfs = std::to_string(selfUserId);
    const std::string peers = std::to_string(peerUserId);
    const std::string limStr = std::to_string(limit);

    if (beforeExclusive > 0) {
        const std::string beforeStr = std::to_string(beforeExclusive);
        const char *sql =
            "SELECT m.id, m.from_user_id, m.to_user_id, m.content, m.created_at FROM messages m "
            "WHERE ((m.from_user_id = ? AND m.to_user_id = ?) OR (m.from_user_id = ? AND m.to_user_id = ?)) "
            "AND m.id < ? "
            "AND NOT EXISTS (SELECT 1 FROM message_deletions d WHERE d.user_id = ? AND d.message_id = m.id) "
            "ORDER BY m.id DESC LIMIT ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
            return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.bind_text_transient(st, 1, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 2, peers.c_str(), static_cast<int>(peers.size()));
        api.bind_text_transient(st, 3, peers.c_str(), static_cast<int>(peers.size()));
        api.bind_text_transient(st, 4, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 5, beforeStr.c_str(), static_cast<int>(beforeStr.size()));
        api.bind_text_transient(st, 6, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 7, limStr.c_str(), static_cast<int>(limStr.size()));
        for (;;) {
            const int sr = api.step(st);
            if (sr != 100) {
                break;
            }
            ChatMessageRow r;
            r.messageId = api.column_int64(st, 0);
            r.fromUserId = api.column_int64(st, 1);
            r.toUserId = api.column_int64(st, 2);
            const unsigned char *cp = api.column_text(st, 3);
            r.content = cp ? reinterpret_cast<const char *>(cp) : "";
            r.createdAt = api.column_int64(st, 4);
            out.push_back(std::move(r));
        }
        api.finalize(st);
        std::reverse(out.begin(), out.end());
    } else if (afterId <= 0) {
        const char *sql =
            "SELECT m.id, m.from_user_id, m.to_user_id, m.content, m.created_at FROM messages m "
            "WHERE ((m.from_user_id = ? AND m.to_user_id = ?) OR (m.from_user_id = ? AND m.to_user_id = ?)) "
            "AND NOT EXISTS (SELECT 1 FROM message_deletions d WHERE d.user_id = ? AND d.message_id = m.id) "
            "ORDER BY m.id DESC LIMIT ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
            return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.bind_text_transient(st, 1, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 2, peers.c_str(), static_cast<int>(peers.size()));
        api.bind_text_transient(st, 3, peers.c_str(), static_cast<int>(peers.size()));
        api.bind_text_transient(st, 4, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 5, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 6, limStr.c_str(), static_cast<int>(limStr.size()));
        for (;;) {
            const int sr = api.step(st);
            if (sr != 100) {
                break;
            }
            ChatMessageRow r;
            r.messageId = api.column_int64(st, 0);
            r.fromUserId = api.column_int64(st, 1);
            r.toUserId = api.column_int64(st, 2);
            const unsigned char *cp = api.column_text(st, 3);
            r.content = cp ? reinterpret_cast<const char *>(cp) : "";
            r.createdAt = api.column_int64(st, 4);
            out.push_back(std::move(r));
        }
        api.finalize(st);
        std::reverse(out.begin(), out.end());
    } else if (afterId > 0) {
        const std::string afterStr = std::to_string(afterId);
        const char *sql =
            "SELECT m.id, m.from_user_id, m.to_user_id, m.content, m.created_at FROM messages m "
            "WHERE ((m.from_user_id = ? AND m.to_user_id = ?) OR (m.from_user_id = ? AND m.to_user_id = ?)) "
            "AND m.id > ? "
            "AND NOT EXISTS (SELECT 1 FROM message_deletions d WHERE d.user_id = ? AND d.message_id = m.id) "
            "ORDER BY m.id ASC LIMIT ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
            return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
        }
        api.bind_text_transient(st, 1, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 2, peers.c_str(), static_cast<int>(peers.size()));
        api.bind_text_transient(st, 3, peers.c_str(), static_cast<int>(peers.size()));
        api.bind_text_transient(st, 4, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 5, afterStr.c_str(), static_cast<int>(afterStr.size()));
        api.bind_text_transient(st, 6, selfs.c_str(), static_cast<int>(selfs.size()));
        api.bind_text_transient(st, 7, limStr.c_str(), static_cast<int>(limStr.size()));
        for (;;) {
            const int sr = api.step(st);
            if (sr != 100) {
                break;
            }
            ChatMessageRow r;
            r.messageId = api.column_int64(st, 0);
            r.fromUserId = api.column_int64(st, 1);
            r.toUserId = api.column_int64(st, 2);
            const unsigned char *cp = api.column_text(st, 3);
            r.content = cp ? reinterpret_cast<const char *>(cp) : "";
            r.createdAt = api.column_int64(st, 4);
            out.push_back(std::move(r));
        }
        api.finalize(st);
    }

    MsgOpOutcome o;
    o.ok = true;
    return o;
}

AppDatabase::MsgOpOutcome AppDatabase::messageClearConversation(const std::int64_t selfUserId,
                                                                const std::int64_t peerUserId)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return msgOpFail(kErrDbUnavailable, "数据库不可用");
    }
    if (selfUserId == peerUserId || peerUserId <= 0) {
        return msgOpFail(kErrInvalidInput, "无效的会话");
    }
    auto &api = sqliteApi();
    if (!areFriends(api, g_db, selfUserId, peerUserId)) {
        return msgOpFail(kErrMsgNotFriend, "非好友关系");
    }

    const std::string selfs = std::to_string(selfUserId);
    const std::string peers = std::to_string(peerUserId);
    const char *delDeps =
        "DELETE FROM message_deletions WHERE message_id IN ("
        "SELECT id FROM messages WHERE (from_user_id = ? AND to_user_id = ?) OR (from_user_id = ? AND to_user_id = ?));";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, delDeps, -1, &st, nullptr) != 0) {
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, selfs.c_str(), static_cast<int>(selfs.size()));
    api.bind_text_transient(st, 2, peers.c_str(), static_cast<int>(peers.size()));
    api.bind_text_transient(st, 3, peers.c_str(), static_cast<int>(peers.size()));
    api.bind_text_transient(st, 4, selfs.c_str(), static_cast<int>(selfs.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(st);

    const char *del =
        "DELETE FROM messages WHERE (from_user_id = ? AND to_user_id = ?) OR (from_user_id = ? AND to_user_id = ?);";
    if (api.prepare(g_db, del, -1, &st, nullptr) != 0) {
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, selfs.c_str(), static_cast<int>(selfs.size()));
    api.bind_text_transient(st, 2, peers.c_str(), static_cast<int>(peers.size()));
    api.bind_text_transient(st, 3, peers.c_str(), static_cast<int>(peers.size()));
    api.bind_text_transient(st, 4, selfs.c_str(), static_cast<int>(selfs.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(st);

    const std::int64_t n = static_cast<std::int64_t>(api.changes(g_db));
    MsgOpOutcome o;
    o.ok = true;
    o.clearedRows = n;
    return o;
}

AppDatabase::MsgOpOutcome AppDatabase::messageHideForUser(const std::int64_t selfUserId, const std::int64_t messageId)
{
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        return msgOpFail(kErrDbUnavailable, "数据库不可用");
    }
    if (messageId <= 0) {
        return msgOpFail(kErrInvalidInput, "缺少 message_id");
    }
    auto &api = sqliteApi();
    const std::string midStr = std::to_string(messageId);
    const char *sel = "SELECT from_user_id, to_user_id FROM messages WHERE id = ?;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sel, -1, &st, nullptr) != 0) {
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, midStr.c_str(), static_cast<int>(midStr.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        return msgOpFail(kErrMsgNotFound, "消息不存在");
    }
    const std::int64_t fromUserId = api.column_int64(st, 0);
    const std::int64_t toUserId = api.column_int64(st, 1);
    api.finalize(st);
    if (fromUserId != selfUserId && toUserId != selfUserId) {
        return msgOpFail(kErrMsgNotFound, "消息不存在或无权删除");
    }
    const std::int64_t peerUserId = (fromUserId == selfUserId) ? toUserId : fromUserId;
    if (!areFriends(api, g_db, selfUserId, peerUserId)) {
        return msgOpFail(kErrMsgNotFriend, "非好友关系");
    }

    const std::time_t now = std::time(nullptr);
    const std::int64_t nowSec = static_cast<std::int64_t>(now);
    const std::string selfStr = std::to_string(selfUserId);
    const std::string nowStr = std::to_string(static_cast<long long>(nowSec));
    const char *ins =
        "INSERT OR IGNORE INTO message_deletions (user_id, message_id, deleted_at) VALUES (?,?,?);";
    if (api.prepare(g_db, ins, -1, &st, nullptr) != 0) {
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 2, midStr.c_str(), static_cast<int>(midStr.size()));
    api.bind_text_transient(st, 3, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        return msgOpFail(kErrDbUnavailable, api.errmsg(g_db));
    }
    api.finalize(st);

    MsgOpOutcome o;
    o.ok = true;
    o.messageId = messageId;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupCreate(const std::int64_t ownerUserId, const std::string &nameUtf8,
                                                     const std::vector<std::int64_t> &memberUserIds)
{
    GroupOpOutcome o;
    const std::string groupName = trimAscii(nameUtf8);
    if (!isValidGroupNameUtf8(groupName)) {
        o.errCode = kErrGroupBadName;
        o.message = "群名称格式不正确";
        return o;
    }

    std::vector<std::int64_t> uniqueMembers;
    uniqueMembers.reserve(memberUserIds.size());
    for (const std::int64_t uid : memberUserIds) {
        if (uid > 0 && uid != ownerUserId) {
            uniqueMembers.push_back(uid);
        }
    }
    std::sort(uniqueMembers.begin(), uniqueMembers.end());
    uniqueMembers.erase(std::unique(uniqueMembers.begin(), uniqueMembers.end()), uniqueMembers.end());
    if (uniqueMembers.empty()) {
        o.errCode = kErrGroupBadMembers;
        o.message = "请至少选择一位好友";
        return o;
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    if (!userExists(api, g_db, ownerUserId)) {
        o.errCode = kErrUserNotFound;
        o.message = "用户不存在";
        return o;
    }
    for (const std::int64_t uid : uniqueMembers) {
        if (!userExists(api, g_db, uid) || !areFriends(api, g_db, ownerUserId, uid)) {
            o.errCode = kErrGroupBadMembers;
            o.message = "只能邀请已添加的好友入群";
            return o;
        }
    }

    const std::int64_t nowSec = static_cast<std::int64_t>(std::time(nullptr));
    const std::string nowStr = std::to_string(static_cast<long long>(nowSec));
    const std::string ownerStr = std::to_string(ownerUserId);

    const char *insGroup = "INSERT INTO chat_groups (name, owner_user_id, created_at) VALUES (?,?,?);";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, insGroup, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.bind_text_transient(st, 1, groupName.c_str(), static_cast<int>(groupName.size()));
    api.bind_text_transient(st, 2, ownerStr.c_str(), static_cast<int>(ownerStr.size()));
    api.bind_text_transient(st, 3, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.finalize(st);
    const std::int64_t groupId = api.last_insert_rowid(g_db);
    const std::string gidStr = std::to_string(groupId);

    const char *insMember =
        "INSERT INTO group_members (group_id, user_id, role, joined_at) VALUES (?,?,?,?);";
    const char *roleOwner = "owner";
    const char *roleMember = "member";
    const auto insertMember = [&](std::int64_t uid, const char *roleAscii) -> bool {
        sqlite3_stmt *mst = nullptr;
        if (api.prepare(g_db, insMember, -1, &mst, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return false;
        }
        const std::string uidStr = std::to_string(uid);
        api.bind_text_transient(mst, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
        api.bind_text_transient(mst, 2, uidStr.c_str(), static_cast<int>(uidStr.size()));
        api.bind_text_transient(mst, 3, roleAscii, static_cast<int>(std::strlen(roleAscii)));
        api.bind_text_transient(mst, 4, nowStr.c_str(), static_cast<int>(nowStr.size()));
        const bool okStep = (api.step(mst) == 101);
        if (!okStep) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
        }
        api.finalize(mst);
        return okStep;
    };

    if (!insertMember(ownerUserId, roleOwner)) {
        return o;
    }
    for (const std::int64_t uid : uniqueMembers) {
        if (!insertMember(uid, roleMember)) {
            return o;
        }
    }

    o.ok = true;
    o.groupId = groupId;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupList(const std::int64_t selfUserId,
                                                   std::vector<AppDatabase::GroupListRow> &out)
{
    out.clear();
    GroupOpOutcome o;
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    const char *sql =
        "SELECT t.group_id, t.name, t.owner_user_id, t.joined_at, t.member_count, "
        "t.last_content, t.last_at, t.last_from, t.last_from_nickname "
        "FROM ("
        "  SELECT g.id AS group_id, g.name, g.owner_user_id, gm.joined_at,"
        "    (SELECT COUNT(1) FROM group_members gm2 WHERE gm2.group_id = g.id) AS member_count,"
        "    (SELECT m.content FROM group_messages m"
        "      WHERE m.group_id = g.id"
        "        AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = gm.user_id AND d.message_id = m.id)"
        "      ORDER BY m.id DESC LIMIT 1) AS last_content,"
        "    (SELECT m.created_at FROM group_messages m"
        "      WHERE m.group_id = g.id"
        "        AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = gm.user_id AND d.message_id = m.id)"
        "      ORDER BY m.id DESC LIMIT 1) AS last_at,"
        "    (SELECT m.from_user_id FROM group_messages m"
        "      WHERE m.group_id = g.id"
        "        AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = gm.user_id AND d.message_id = m.id)"
        "      ORDER BY m.id DESC LIMIT 1) AS last_from,"
        "    (SELECT IFNULL(u.nickname,'') FROM group_messages m JOIN users u ON u.user_id = m.from_user_id "
        "      WHERE m.group_id = g.id"
        "        AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = gm.user_id AND d.message_id = m.id)"
        "      ORDER BY m.id DESC LIMIT 1) AS last_from_nickname "
        "  FROM group_members gm "
        "  JOIN chat_groups g ON g.id = gm.group_id "
        "  WHERE gm.user_id = ?"
        ") AS t "
        "ORDER BY IFNULL(t.last_at, 0) DESC, t.name COLLATE NOCASE ASC;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    const std::string selfStr = std::to_string(selfUserId);
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        GroupListRow row;
        row.groupId = api.column_int64(st, 0);
        const unsigned char *np = api.column_text(st, 1);
        row.name = np ? reinterpret_cast<const char *>(np) : "";
        row.ownerUserId = api.column_int64(st, 2);
        row.joinedAt = api.column_int64(st, 3);
        row.memberCount = api.column_int64(st, 4);
        const unsigned char *cp = api.column_text(st, 5);
        row.lastMessageContent = cp ? reinterpret_cast<const char *>(cp) : "";
        if (row.lastMessageContent.size() > kMsgMaxContentUtf8Bytes) {
            row.lastMessageContent.resize(kMsgMaxContentUtf8Bytes);
        }
        row.lastMessageAt = api.column_int64(st, 6);
        row.lastMessageFromUserId = api.column_int64(st, 7);
        const unsigned char *ln = api.column_text(st, 8);
        row.lastMessageFromNickname = ln ? reinterpret_cast<const char *>(ln) : "";
        out.push_back(std::move(row));
    }
    api.finalize(st);
    o.ok = true;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupMembers(const std::int64_t selfUserId, const std::int64_t groupId,
                                                      std::vector<AppDatabase::GroupMemberRow> &out)
{
    out.clear();
    GroupOpOutcome o;
    if (groupId <= 0) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    if (!groupExists(api, g_db, groupId)) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (!groupLookupMembership(api, g_db, groupId, selfUserId)) {
        o.errCode = kErrGroupNotMember;
        o.message = "你还不是该群成员";
        return o;
    }
    const char *sql =
        "SELECT gm.user_id, u.email, IFNULL(u.nickname,''), gm.role, gm.joined_at "
        "FROM group_members gm JOIN users u ON u.user_id = gm.user_id "
        "WHERE gm.group_id = ? ORDER BY CASE gm.role WHEN 'owner' THEN 0 ELSE 1 END, gm.joined_at ASC, gm.user_id ASC;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    const std::string gidStr = std::to_string(groupId);
    api.bind_text_transient(st, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        GroupMemberRow row;
        row.userId = api.column_int64(st, 0);
        const unsigned char *ep = api.column_text(st, 1);
        const unsigned char *np = api.column_text(st, 2);
        const unsigned char *rp = api.column_text(st, 3);
        row.email = ep ? reinterpret_cast<const char *>(ep) : "";
        row.nickname = np ? reinterpret_cast<const char *>(np) : "";
        row.role = rp ? reinterpret_cast<const char *>(rp) : "";
        row.joinedAt = api.column_int64(st, 4);
        out.push_back(std::move(row));
    }
    api.finalize(st);
    o.ok = true;
    o.groupId = groupId;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupMessageSend(const std::int64_t fromUserId, const std::int64_t groupId,
                                                          const std::string &contentUtf8)
{
    GroupOpOutcome o;
    if (groupId <= 0) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (contentUtf8.empty()) {
        o.errCode = kErrMsgTooLong;
        o.message = "消息内容不能为空";
        return o;
    }
    if (contentUtf8.size() > kMsgMaxContentUtf8Bytes) {
        o.errCode = kErrMsgTooLong;
        o.message = "消息过长";
        return o;
    }

    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    if (!groupExists(api, g_db, groupId)) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (!groupLookupMembership(api, g_db, groupId, fromUserId)) {
        o.errCode = kErrGroupNotMember;
        o.message = "你还不是该群成员";
        return o;
    }

    const std::int64_t nowSec = static_cast<std::int64_t>(std::time(nullptr));
    const std::string gidStr = std::to_string(groupId);
    const std::string fromStr = std::to_string(fromUserId);
    const std::string nowStr = std::to_string(static_cast<long long>(nowSec));
    const char *ins =
        "INSERT INTO group_messages (group_id, from_user_id, content, created_at) VALUES (?,?,?,?);";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, ins, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.bind_text_transient(st, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
    api.bind_text_transient(st, 2, fromStr.c_str(), static_cast<int>(fromStr.size()));
    api.bind_text_transient(st, 3, contentUtf8.c_str(), static_cast<int>(contentUtf8.size()));
    api.bind_text_transient(st, 4, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.finalize(st);
    o.ok = true;
    o.groupId = groupId;
    o.messageId = api.last_insert_rowid(g_db);
    o.createdAt = nowSec;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupMessageFetch(const std::int64_t selfUserId, const std::int64_t groupId,
                                                           const std::int64_t afterId, const std::int64_t beforeExclusive,
                                                           int limit, std::vector<AppDatabase::GroupChatMessageRow> &out)
{
    out.clear();
    GroupOpOutcome o;
    if (groupId <= 0) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (limit <= 0) {
        limit = 50;
    }
    if (limit > 200) {
        limit = 200;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    if (!groupExists(api, g_db, groupId)) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (!groupLookupMembership(api, g_db, groupId, selfUserId)) {
        o.errCode = kErrGroupNotMember;
        o.message = "你还不是该群成员";
        return o;
    }

    const std::string gidStr = std::to_string(groupId);
    const std::string limStr = std::to_string(limit);
    const std::string selfStr = std::to_string(selfUserId);
    const char *sqlRecent =
        "SELECT m.id, m.group_id, m.from_user_id, IFNULL(u.nickname,''), m.content, m.created_at "
        "FROM group_messages m JOIN users u ON u.user_id = m.from_user_id "
        "WHERE m.group_id = ? "
        "AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = ? AND d.message_id = m.id) "
        "ORDER BY m.id DESC LIMIT ?;";
    const char *sqlBefore =
        "SELECT m.id, m.group_id, m.from_user_id, IFNULL(u.nickname,''), m.content, m.created_at "
        "FROM group_messages m JOIN users u ON u.user_id = m.from_user_id "
        "WHERE m.group_id = ? AND m.id < ? "
        "AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = ? AND d.message_id = m.id) "
        "ORDER BY m.id DESC LIMIT ?;";
    const char *sqlAfter =
        "SELECT m.id, m.group_id, m.from_user_id, IFNULL(u.nickname,''), m.content, m.created_at "
        "FROM group_messages m JOIN users u ON u.user_id = m.from_user_id "
        "WHERE m.group_id = ? AND m.id > ? "
        "AND NOT EXISTS (SELECT 1 FROM group_message_deletions d WHERE d.user_id = ? AND d.message_id = m.id) "
        "ORDER BY m.id ASC LIMIT ?;";
    sqlite3_stmt *st = nullptr;
    const char *chosenSql = sqlRecent;
    if (beforeExclusive > 0) {
        chosenSql = sqlBefore;
    } else if (afterId > 0) {
        chosenSql = sqlAfter;
    }
    if (api.prepare(g_db, chosenSql, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.bind_text_transient(st, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
    if (beforeExclusive > 0) {
        const std::string beforeStr = std::to_string(beforeExclusive);
        api.bind_text_transient(st, 2, beforeStr.c_str(), static_cast<int>(beforeStr.size()));
        api.bind_text_transient(st, 3, selfStr.c_str(), static_cast<int>(selfStr.size()));
        api.bind_text_transient(st, 4, limStr.c_str(), static_cast<int>(limStr.size()));
    } else if (afterId <= 0) {
        api.bind_text_transient(st, 2, selfStr.c_str(), static_cast<int>(selfStr.size()));
        api.bind_text_transient(st, 3, limStr.c_str(), static_cast<int>(limStr.size()));
    } else {
        const std::string afterStr = std::to_string(afterId);
        api.bind_text_transient(st, 2, afterStr.c_str(), static_cast<int>(afterStr.size()));
        api.bind_text_transient(st, 3, selfStr.c_str(), static_cast<int>(selfStr.size()));
        api.bind_text_transient(st, 4, limStr.c_str(), static_cast<int>(limStr.size()));
    }
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        GroupChatMessageRow row;
        row.messageId = api.column_int64(st, 0);
        row.groupId = api.column_int64(st, 1);
        row.fromUserId = api.column_int64(st, 2);
        const unsigned char *np = api.column_text(st, 3);
        const unsigned char *cp = api.column_text(st, 4);
        row.fromNickname = np ? reinterpret_cast<const char *>(np) : "";
        row.content = cp ? reinterpret_cast<const char *>(cp) : "";
        row.createdAt = api.column_int64(st, 5);
        out.push_back(std::move(row));
    }
    api.finalize(st);
    if (beforeExclusive > 0 || afterId <= 0) {
        std::reverse(out.begin(), out.end());
    }
    o.ok = true;
    o.groupId = groupId;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupMessageHideForUser(const std::int64_t selfUserId, const std::int64_t groupId,
                                                                const std::int64_t messageId)
{
    GroupOpOutcome o;
    if (groupId <= 0 || messageId <= 0) {
        o.errCode = kErrInvalidInput;
        o.message = "缺少 group_id 或 message_id";
        return o;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    if (!groupExists(api, g_db, groupId)) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (!groupLookupMembership(api, g_db, groupId, selfUserId)) {
        o.errCode = kErrGroupNotMember;
        o.message = "你还不是该群成员";
        return o;
    }

    const std::string midStr = std::to_string(messageId);
    const char *sel = "SELECT group_id FROM group_messages WHERE id = ?;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sel, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.bind_text_transient(st, 1, midStr.c_str(), static_cast<int>(midStr.size()));
    const int sr = api.step(st);
    if (sr != 100) {
        api.finalize(st);
        o.errCode = kErrMsgNotFound;
        o.message = "消息不存在";
        return o;
    }
    const std::int64_t msgGroupId = api.column_int64(st, 0);
    api.finalize(st);
    if (msgGroupId != groupId) {
        o.errCode = kErrMsgNotFound;
        o.message = "消息不存在";
        return o;
    }

    const std::time_t now = std::time(nullptr);
    const std::int64_t nowSec = static_cast<std::int64_t>(now);
    const std::string selfStr = std::to_string(selfUserId);
    const std::string gidStr = std::to_string(groupId);
    const std::string nowStr = std::to_string(static_cast<long long>(nowSec));
    const char *ins =
        "INSERT OR IGNORE INTO group_message_deletions (user_id, group_id, message_id, deleted_at) VALUES (?,?,?,?);";
    if (api.prepare(g_db, ins, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.bind_text_transient(st, 1, selfStr.c_str(), static_cast<int>(selfStr.size()));
    api.bind_text_transient(st, 2, gidStr.c_str(), static_cast<int>(gidStr.size()));
    api.bind_text_transient(st, 3, midStr.c_str(), static_cast<int>(midStr.size()));
    api.bind_text_transient(st, 4, nowStr.c_str(), static_cast<int>(nowStr.size()));
    if (api.step(st) != 101) {
        api.finalize(st);
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    api.finalize(st);
    o.ok = true;
    o.groupId = groupId;
    o.messageId = messageId;
    o.createdAt = nowSec;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupLeave(const std::int64_t selfUserId, const std::int64_t groupId)
{
    GroupOpOutcome o;
    if (groupId <= 0) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    std::string role;
    if (!groupExists(api, g_db, groupId)) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (!groupLookupMembership(api, g_db, groupId, selfUserId, &role)) {
        o.errCode = kErrGroupNotMember;
        o.message = "你还不是该群成员";
        return o;
    }
    if (role == "owner") {
        const char *cntSql = "SELECT COUNT(1) FROM group_members WHERE group_id = ?;";
        sqlite3_stmt *cntSt = nullptr;
        if (api.prepare(g_db, cntSql, -1, &cntSt, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        const std::string gidStr = std::to_string(groupId);
        api.bind_text_transient(cntSt, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
        std::int64_t memberCount = 0;
        if (api.step(cntSt) == 100) {
            memberCount = api.column_int64(cntSt, 0);
        }
        api.finalize(cntSt);
        if (memberCount > 1) {
            o.errCode = kErrGroupOwnerLeave;
            o.message = "群主请先解散或转交群聊";
            return o;
        }
        const char *delGroup = "DELETE FROM chat_groups WHERE id = ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, delGroup, -1, &st, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.bind_text_transient(st, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
        if (api.step(st) != 101) {
            api.finalize(st);
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.finalize(st);
    } else {
        const char *delMember = "DELETE FROM group_members WHERE group_id = ? AND user_id = ?;";
        sqlite3_stmt *st = nullptr;
        if (api.prepare(g_db, delMember, -1, &st, nullptr) != 0) {
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        const std::string gidStr = std::to_string(groupId);
        const std::string uidStr = std::to_string(selfUserId);
        api.bind_text_transient(st, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
        api.bind_text_transient(st, 2, uidStr.c_str(), static_cast<int>(uidStr.size()));
        if (api.step(st) != 101) {
            api.finalize(st);
            o.errCode = kErrDbUnavailable;
            o.message = api.errmsg(g_db);
            return o;
        }
        api.finalize(st);
    }
    o.ok = true;
    o.groupId = groupId;
    return o;
}

AppDatabase::GroupOpOutcome AppDatabase::groupMemberIds(const std::int64_t selfUserId, const std::int64_t groupId,
                                                        std::vector<std::int64_t> &outMemberUserIds)
{
    outMemberUserIds.clear();
    GroupOpOutcome o;
    if (groupId <= 0) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    std::lock_guard<std::mutex> lock(g_dbMutex);
    if (!g_ready || g_db == nullptr) {
        o.errCode = kErrDbUnavailable;
        o.message = "数据库不可用";
        return o;
    }
    auto &api = sqliteApi();
    if (!groupExists(api, g_db, groupId)) {
        o.errCode = kErrGroupNotFound;
        o.message = "群聊不存在";
        return o;
    }
    if (!groupLookupMembership(api, g_db, groupId, selfUserId)) {
        o.errCode = kErrGroupNotMember;
        o.message = "你还不是该群成员";
        return o;
    }
    const char *sql = "SELECT user_id FROM group_members WHERE group_id = ? ORDER BY user_id ASC;";
    sqlite3_stmt *st = nullptr;
    if (api.prepare(g_db, sql, -1, &st, nullptr) != 0) {
        o.errCode = kErrDbUnavailable;
        o.message = api.errmsg(g_db);
        return o;
    }
    const std::string gidStr = std::to_string(groupId);
    api.bind_text_transient(st, 1, gidStr.c_str(), static_cast<int>(gidStr.size()));
    for (;;) {
        const int sr = api.step(st);
        if (sr != 100) {
            break;
        }
        outMemberUserIds.push_back(api.column_int64(st, 0));
    }
    api.finalize(st);
    o.ok = true;
    o.groupId = groupId;
    return o;
}

} // namespace vsserver
