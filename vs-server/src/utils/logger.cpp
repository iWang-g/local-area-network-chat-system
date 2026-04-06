#include "vsserver/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

namespace vsserver {

namespace {

std::mutex g_mx;
std::ofstream g_file;
bool g_open = false;
std::string g_path;

#ifdef _WIN32
void ensureWindowsConsoleUtf8()
{
    static bool done = false;
    if (done) {
        return;
    }
    done = true;
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#endif

} // namespace

std::string Logger::basenameFile(const char *path)
{
    if (path == nullptr) {
        return {};
    }
    std::string p(path);
    const auto posSlash = p.find_last_of("/\\");
    if (posSlash != std::string::npos) {
        return p.substr(posSlash + 1);
    }
    return p;
}

std::string Logger::timestamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

void Logger::init(const std::string &logRoot)
{
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_open) {
        return;
    }

#ifdef _WIN32
    ensureWindowsConsoleUtf8();
#endif

    std::filesystem::path root;
    if (logRoot.empty()) {
        root = std::filesystem::current_path();
    } else {
        root = logRoot;
    }
    const std::filesystem::path logDir = root / "logs";
    std::error_code ec;
    std::filesystem::create_directories(logDir, ec);

    std::ostringstream name;
    {
        const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        name << std::put_time(&tm, "%Y-%m-%d") << ".log";
    }
    g_path = (logDir / name.str()).string();

    bool writeUtf8Bom = true;
    {
        std::error_code ec;
        if (std::filesystem::exists(g_path, ec) && !ec) {
            const auto sz = std::filesystem::file_size(g_path, ec);
            writeUtf8Bom = !ec && sz == 0;
        }
    }

    g_file.open(g_path, std::ios::out | std::ios::app);
    if (!g_file.is_open()) {
        fprintf(stderr, "[Logger] 无法打开日志文件: %s\n", g_path.c_str());
    } else {
        g_open = true;
        if (writeUtf8Bom) {
            static const unsigned char kBom[] = {0xEF, 0xBB, 0xBF};
            g_file.write(reinterpret_cast<const char *>(kBom), sizeof(kBom));
        }
    }

    fprintf(stderr, "[%s] [INFO ] ====== vs-server 启动 ====== (%s)\n", timestamp().c_str(), g_path.c_str());
    fflush(stderr);
    if (g_file.is_open()) {
        g_file << '[' << timestamp() << "] [INFO ] ====== vs-server 启动 ====== (" << g_path << ")\n";
        g_file.flush();
    }
}

void Logger::shutdown()
{
    std::lock_guard<std::mutex> lock(g_mx);
    if (g_file.is_open()) {
        g_file << '[' << timestamp() << "] [INFO ] ====== vs-server 关闭 ======\n";
        g_file.close();
    }
    g_open = false;
}

std::string Logger::logFilePath()
{
    return g_path;
}

void Logger::write(LogLevel level, const char *file, int line, const std::string &msg)
{
#ifdef _WIN32
    ensureWindowsConsoleUtf8();
#endif
    static const char *kLevels[] = {"DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    const int idx = static_cast<int>(level);
    const char *tag = (idx >= 0 && idx < 5) ? kLevels[idx] : "?????";

    std::ostringstream lineOut;
    lineOut << '[' << timestamp() << "] [" << tag << "] " << msg;
    if (file != nullptr && line > 0) {
        lineOut << " (" << basenameFile(file) << ':' << line << ')';
    }
    const std::string out = lineOut.str();

    std::lock_guard<std::mutex> lock(g_mx);
    fprintf(stderr, "%s\n", out.c_str());
    fflush(stderr);
    if (g_file.is_open()) {
        g_file << out << '\n';
        g_file.flush();
    }
}

} // namespace vsserver
