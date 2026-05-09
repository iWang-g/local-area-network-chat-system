#pragma once

#include <filesystem>

namespace vsserver {

/// 可执行文件所在目录（UTF-8 路径）；失败时返回当前工作目录。
std::filesystem::path appExecutableDirectory();

} // namespace vsserver
