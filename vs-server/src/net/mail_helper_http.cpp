#include "vsserver/mail_helper_http.hpp"
#include "vsserver/message_parse.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <winhttp.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace vsserver {

namespace {

std::optional<std::string> dupEnv(const char *name)
{
    char *buf = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&buf, &n, name) != 0 || buf == nullptr) {
        return std::nullopt;
    }
    std::string s(buf);
    std::free(buf);
    if (s.empty()) {
        return std::nullopt;
    }
    return s;
}

} // namespace

bool mailHelperConfigured()
{
    const auto u = dupEnv("LANCS_MAIL_HELPER_URL");
    return u.has_value();
}

bool mailHelperNotifyRegisterCode(const std::string &email, const std::string &code, std::string &errOut)
{
    const auto urlOpt = dupEnv("LANCS_MAIL_HELPER_URL");
    if (!urlOpt.has_value()) {
        return true;
    }
    const std::string &urlUtf8 = *urlOpt;
    const auto secretOpt = dupEnv("LANCS_MAIL_HELPER_SECRET");

    const std::string body = std::string(R"({"email":")") + jsonEscapeString(email) + R"(","code":")"
                             + jsonEscapeString(code) + R"(","purpose":"register"})";

    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, urlUtf8.c_str(), -1, nullptr, 0);
    if (wlen <= 0) {
        errOut = "LANCS_MAIL_HELPER_URL 不是合法 UTF-8";
        return false;
    }
    std::vector<wchar_t> urlW(static_cast<std::size_t>(wlen));
    if (::MultiByteToWideChar(CP_UTF8, 0, urlUtf8.c_str(), -1, urlW.data(), wlen) <= 0) {
        errOut = "URL 转宽字符失败";
        return false;
    }

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256]{};
    wchar_t pathBuf[2048]{};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = static_cast<DWORD>((sizeof(hostBuf) / sizeof(hostBuf[0])) - 1U);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = static_cast<DWORD>((sizeof(pathBuf) / sizeof(pathBuf[0])) - 1U);

    if (!::WinHttpCrackUrl(urlW.data(), 0, 0, &uc)) {
        errOut = "无法解析 LANCS_MAIL_HELPER_URL";
        return false;
    }

    INTERNET_PORT port = uc.nPort;
    if (port == 0) {
        port = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT
                                                     : INTERNET_DEFAULT_HTTP_PORT;
    }

    const HINTERNET hSession =
        ::WinHttpOpen(L"lancs-vs-server/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        errOut = "WinHttpOpen 失败";
        return false;
    }
    (void)::WinHttpSetTimeouts(hSession, 10000, 10000, 10000, 10000);

    const HINTERNET hConnect = ::WinHttpConnect(hSession, uc.lpszHostName, port, 0);
    if (!hConnect) {
        ::WinHttpCloseHandle(hSession);
        errOut = "WinHttpConnect 失败";
        return false;
    }

    const wchar_t *pathPtr = uc.lpszUrlPath;
    const std::wstring pathFallback = L"/";
    if (pathPtr == nullptr || pathPtr[0] == L'\0') {
        pathPtr = pathFallback.c_str();
    }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    const HINTERNET hRequest =
        ::WinHttpOpenRequest(hConnect, L"POST", pathPtr, nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        errOut = "WinHttpOpenRequest 失败";
        return false;
    }

    std::wstring hdr = L"Content-Type: application/json; charset=utf-8\r\n";
    if (secretOpt.has_value()) {
        const std::string bearer = "Bearer " + *secretOpt;
        const int blen =
            ::MultiByteToWideChar(CP_UTF8, 0, bearer.c_str(), static_cast<int>(bearer.size()), nullptr, 0);
        if (blen <= 0) {
            ::WinHttpCloseHandle(hRequest);
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            errOut = "LANCS_MAIL_HELPER_SECRET 转宽字符失败";
            return false;
        }
        std::vector<wchar_t> bearerW(static_cast<std::size_t>(blen));
        if (::MultiByteToWideChar(CP_UTF8, 0, bearer.c_str(), static_cast<int>(bearer.size()),
                                  bearerW.data(), blen) <= 0) {
            ::WinHttpCloseHandle(hRequest);
            ::WinHttpCloseHandle(hConnect);
            ::WinHttpCloseHandle(hSession);
            errOut = "Authorization 头构造失败";
            return false;
        }
        hdr += L"Authorization: ";
        hdr.append(bearerW.data(), static_cast<std::size_t>(blen));
        hdr += L"\r\n";
    }

    if (!::WinHttpAddRequestHeaders(hRequest, hdr.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        errOut = "WinHttpAddRequestHeaders 失败";
        return false;
    }

    const BOOL sent = ::WinHttpSendRequest(
        hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, reinterpret_cast<LPVOID>(const_cast<char *>(body.data())),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        errOut = "WinHttpSendRequest 失败";
        return false;
    }

    if (!::WinHttpReceiveResponse(hRequest, nullptr)) {
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        errOut = "WinHttpReceiveResponse 失败";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!::WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        errOut = "无法读取 HTTP 状态码";
        return false;
    }

    ::WinHttpCloseHandle(hRequest);
    ::WinHttpCloseHandle(hConnect);
    ::WinHttpCloseHandle(hSession);

    if (status < 200 || status > 299) {
        errOut = "mail-helper 返回 HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

} // namespace vsserver
