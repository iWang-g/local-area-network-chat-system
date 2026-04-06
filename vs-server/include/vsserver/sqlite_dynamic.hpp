#pragma once

#include <cstdint>
#include <memory>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace vsserver {

/// 运行时加载同目录下 sqlite3.dll（官方预编译 x64 包），失败则数据库不可用。
class SqliteDynamic {
public:
    SqliteDynamic();
    ~SqliteDynamic();

    bool isReady() const { return m_ok; }
    const std::string &lastError() const { return m_err; }

    int open(const char *path, sqlite3 **db);
    int close(sqlite3 *db);
    int exec(sqlite3 *db, const char *sql, char **errmsgOut);
    int prepare(sqlite3 *db, const char *sql, int nByte, sqlite3_stmt **stmt, const char **tail);
    int step(sqlite3_stmt *stmt);
    int finalize(sqlite3_stmt *stmt);
    int reset(sqlite3_stmt *stmt);

    int bind_text(sqlite3_stmt *stmt, int idx, const char *val, int n, void (*destructor)(void *));
    /// 等价于 SQLITE_TRANSIENT：由 SQLite 复制字符串内容。
    int bind_text_transient(sqlite3_stmt *stmt, int idx, const char *val, int n);

    const unsigned char *column_text(sqlite3_stmt *stmt, int idx);
    std::int64_t column_int64(sqlite3_stmt *stmt, int idx);

    void free(void *p);
    const char *errmsg(sqlite3 *db);
    std::int64_t last_insert_rowid(sqlite3 *db);

private:
    bool loadDll();
    void unload();

    void *m_dll = nullptr;
    bool m_ok = false;
    std::string m_err;

    int (*p_open)(const char *, sqlite3 **) = nullptr;
    int (*p_close)(sqlite3 *) = nullptr;
    int (*p_exec)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **) = nullptr;
    int (*p_prepare_v2)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **) = nullptr;
    int (*p_step)(sqlite3_stmt *) = nullptr;
    int (*p_finalize)(sqlite3_stmt *) = nullptr;
    int (*p_reset)(sqlite3_stmt *) = nullptr;
    int (*p_bind_text)(sqlite3_stmt *, int, const char *, int, void (*)(void *)) = nullptr;
    const unsigned char *(*p_column_text)(sqlite3_stmt *, int) = nullptr;
    long long (*p_column_int64)(sqlite3_stmt *, int) = nullptr;
    void (*p_free)(void *) = nullptr;
    const char *(*p_errmsg)(sqlite3 *) = nullptr;
    long long (*p_last_insert_rowid)(sqlite3 *) = nullptr;
};

/// 进程内单例，供各连接线程通过 mutex 串行访问（阶段 2 简化模型）。
SqliteDynamic &sqliteApi();

} // namespace vsserver
