#include "vsserver/database.hpp"
#include "vsserver/password_hash.hpp"
#include "vsserver/sqlite_dynamic.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
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
                         ");";

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

AuthResult AppDatabase::registerUser(const std::string &email, const std::string &passwordPlain,
                                     const std::string &nickname)
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
