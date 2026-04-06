#pragma once

#include <string>

namespace vsserver {

enum class LogLevel { Debug, Info, Warning, Error, Fatal };

/// 控制台 + 按日落盘到指定目录（默认 ./logs）。
class Logger {
public:
    /// logRoot 为空时使用当前工作目录下的 logs；否则为 logRoot/logs。
    static void init(const std::string &logRoot = {});
    static void shutdown();
    static std::string logFilePath();

    static void write(LogLevel level, const char *file, int line, const std::string &msg);

private:
    static std::string basenameFile(const char *path);
    static std::string timestamp();
};

} // namespace vsserver

#define VSLOG_DEBUG(msg) ::vsserver::Logger::write(::vsserver::LogLevel::Debug, __FILE__, __LINE__, (msg))
#define VSLOG_INFO(msg) ::vsserver::Logger::write(::vsserver::LogLevel::Info, __FILE__, __LINE__, (msg))
#define VSLOG_WARN(msg) ::vsserver::Logger::write(::vsserver::LogLevel::Warning, __FILE__, __LINE__, (msg))
#define VSLOG_ERROR(msg) ::vsserver::Logger::write(::vsserver::LogLevel::Error, __FILE__, __LINE__, (msg))
