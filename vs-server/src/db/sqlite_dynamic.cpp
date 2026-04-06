#include "vsserver/sqlite_dynamic.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <mutex>

namespace vsserver {

namespace {

std::mutex g_sqliteOnce;
std::unique_ptr<SqliteDynamic> g_inst;

std::wstring exeDirectory()
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

} // namespace

SqliteDynamic &sqliteApi()
{
    std::lock_guard<std::mutex> lock(g_sqliteOnce);
    if (!g_inst) {
        g_inst = std::make_unique<SqliteDynamic>();
    }
    return *g_inst;
}

SqliteDynamic::SqliteDynamic()
{
    loadDll();
}

SqliteDynamic::~SqliteDynamic()
{
    unload();
}

bool SqliteDynamic::loadDll()
{
    const std::wstring nearExe = exeDirectory() + L"\\sqlite3.dll";
    m_dll = ::LoadLibraryW(nearExe.c_str());
    if (m_dll == nullptr) {
        m_dll = ::LoadLibraryW(L"sqlite3.dll");
    }
    if (m_dll == nullptr) {
        m_err = "无法加载 sqlite3.dll（请将官方 sqlite-dll-win-x64 中的 sqlite3.dll 放在 exe 同目录或 PATH）";
        return false;
    }

#define LOAD_SYM(name, var, type)                                                                                    \
    var = reinterpret_cast<type>(::GetProcAddress(static_cast<HMODULE>(m_dll), name));                                 \
    if (var == nullptr) {                                                                                              \
        m_err = std::string("sqlite3.dll 缺少符号: ") + name;                                                          \
        unload();                                                                                                      \
        return false;                                                                                                  \
    }

    LOAD_SYM("sqlite3_open", p_open, int (*)(const char *, sqlite3 **));
    LOAD_SYM("sqlite3_close", p_close, int (*)(sqlite3 *));
    LOAD_SYM("sqlite3_exec", p_exec, int (*)(sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *,
                                             char **));
    LOAD_SYM("sqlite3_prepare_v2", p_prepare_v2,
             int (*)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **));
    LOAD_SYM("sqlite3_step", p_step, int (*)(sqlite3_stmt *));
    LOAD_SYM("sqlite3_finalize", p_finalize, int (*)(sqlite3_stmt *));
    LOAD_SYM("sqlite3_reset", p_reset, int (*)(sqlite3_stmt *));
    LOAD_SYM("sqlite3_bind_text", p_bind_text,
             int (*)(sqlite3_stmt *, int, const char *, int, void (*)(void *)));
    LOAD_SYM("sqlite3_column_text", p_column_text, const unsigned char *(*)(sqlite3_stmt *, int));
    LOAD_SYM("sqlite3_column_int64", p_column_int64, long long (*)(sqlite3_stmt *, int));
    LOAD_SYM("sqlite3_free", p_free, void (*)(void *));
    LOAD_SYM("sqlite3_errmsg", p_errmsg, const char *(*)(sqlite3 *));
    LOAD_SYM("sqlite3_last_insert_rowid", p_last_insert_rowid, long long (*)(sqlite3 *));

#undef LOAD_SYM

    m_ok = true;
    return true;
}

void SqliteDynamic::unload()
{
    if (m_dll != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(m_dll));
        m_dll = nullptr;
    }
    m_ok = false;
    p_open = nullptr;
    p_close = nullptr;
    p_exec = nullptr;
    p_prepare_v2 = nullptr;
    p_step = nullptr;
    p_finalize = nullptr;
    p_reset = nullptr;
    p_bind_text = nullptr;
    p_column_text = nullptr;
    p_column_int64 = nullptr;
    p_free = nullptr;
    p_errmsg = nullptr;
    p_last_insert_rowid = nullptr;
}

int SqliteDynamic::open(const char *path, sqlite3 **db)
{
    if (!m_ok) {
        return 14;
    } // SQLITE_CANTOPEN
    return p_open(path, db);
}

int SqliteDynamic::close(sqlite3 *db)
{
    if (!m_ok || db == nullptr) {
        return 0;
    }
    return p_close(db);
}

int SqliteDynamic::exec(sqlite3 *db, const char *sql, char **errmsgOut)
{
    return p_exec(db, sql, nullptr, nullptr, errmsgOut);
}

int SqliteDynamic::prepare(sqlite3 *db, const char *sql, int nByte, sqlite3_stmt **stmt, const char **tail)
{
    return p_prepare_v2(db, sql, nByte, stmt, tail);
}

int SqliteDynamic::step(sqlite3_stmt *stmt)
{
    return p_step(stmt);
}

int SqliteDynamic::finalize(sqlite3_stmt *stmt)
{
    return p_finalize(stmt);
}

int SqliteDynamic::reset(sqlite3_stmt *stmt)
{
    return p_reset(stmt);
}

int SqliteDynamic::bind_text(sqlite3_stmt *stmt, int idx, const char *val, int n, void (*destructor)(void *))
{
    return p_bind_text(stmt, idx, val, n, destructor);
}

int SqliteDynamic::bind_text_transient(sqlite3_stmt *stmt, int idx, const char *val, int n)
{
    using Dtor = void (*)(void *);
    return p_bind_text(stmt, idx, val, n, reinterpret_cast<Dtor>(static_cast<std::uintptr_t>(-1)));
}

const unsigned char *SqliteDynamic::column_text(sqlite3_stmt *stmt, int idx)
{
    return p_column_text(stmt, idx);
}

std::int64_t SqliteDynamic::column_int64(sqlite3_stmt *stmt, int idx)
{
    return static_cast<std::int64_t>(p_column_int64(stmt, idx));
}

void SqliteDynamic::free(void *p)
{
    if (p_free != nullptr) {
        p_free(p);
    }
}

const char *SqliteDynamic::errmsg(sqlite3 *db)
{
    if (!m_ok || db == nullptr) {
        return "sqlite not loaded";
    }
    return p_errmsg(db);
}

std::int64_t SqliteDynamic::last_insert_rowid(sqlite3 *db)
{
    if (!m_ok || db == nullptr || p_last_insert_rowid == nullptr) {
        return 0;
    }
    return static_cast<std::int64_t>(p_last_insert_rowid(db));
}

} // namespace vsserver
