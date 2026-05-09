#include "vsserver/platform_paths.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <limits.h>
#include <unistd.h>

#include <vector>
#endif

namespace vsserver {

std::filesystem::path appExecutableDirectory()
{
#ifdef _WIN32
    wchar_t buf[MAX_PATH]{};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0) {
        return std::filesystem::current_path();
    }
    std::wstring p(buf, n);
    const auto pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(p.substr(0, pos));
#else
    std::vector<char> buf(static_cast<std::size_t>(PATH_MAX > 0 ? PATH_MAX : 4096));
    for (;;) {
        const ssize_t r = ::readlink("/proc/self/exe", buf.data(), buf.size());
        if (r < 0) {
            return std::filesystem::current_path();
        }
        if (static_cast<std::size_t>(r) < buf.size()) {
            buf[static_cast<std::size_t>(r)] = '\0';
            std::filesystem::path exe(buf.data());
            const std::filesystem::path parent = exe.parent_path();
            return parent.empty() ? std::filesystem::current_path() : parent;
        }
        buf.resize(buf.size() * 2);
    }
#endif
}

} // namespace vsserver
