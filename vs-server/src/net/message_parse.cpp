#include "vsserver/message_parse.hpp"

#include <regex>
#include <sstream>

namespace vsserver {

static std::optional<std::string> matchOne(const std::string &json, const std::regex &re)
{
    std::smatch m;
    if (!std::regex_search(json, m, re) || m.size() < 2) {
        return std::nullopt;
    }
    return m[1].str();
}

std::optional<std::string> parseMessageType(const std::string &jsonUtf8)
{
    // 使用 R"delim(...)delim"，避免正则里的 )" 被当成原始字符串结束符
    static const std::regex re(R"re("type"\s*:\s*"([^"]*)")re");
    return matchOne(jsonUtf8, re);
}

std::optional<std::string> parseJsonStringField(const std::string &jsonUtf8, const char *asciiFieldName)
{
    const std::string pattern =
        std::string(R"re(")re") + asciiFieldName + R"re("\s*:\s*"([^"]*)")re";
    const std::regex re(pattern);
    return matchOne(jsonUtf8, re);
}

std::string jsonEscapeString(std::string_view utf8)
{
    std::ostringstream oss;
    for (unsigned char c : utf8) {
        switch (c) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (c < 0x20U) {
                static const char *hd = "0123456789abcdef";
                oss << "\\u00" << hd[(c >> 4U) & 0xFU] << hd[c & 0xFU];
            } else {
                oss << static_cast<char>(c);
            }
            break;
        }
    }
    return oss.str();
}

std::string buildErrorJson(int code, const std::string &messageUtf8, int retryAfterSec)
{
    std::string out = std::string(R"({"type":"error","code":)") + std::to_string(code) + R"(,"message":")"
                        + jsonEscapeString(messageUtf8) + "\"";
    if (retryAfterSec >= 0) {
        out += R"(,"retry_after_sec":)" + std::to_string(retryAfterSec);
    }
    out += '}';
    return out;
}

std::string buildAuthOkJson(std::int64_t userId, const std::string &tokenHex, const std::string &emailUtf8)
{
    return std::string(R"({"type":"auth_ok","user_id":)") + std::to_string(userId) + R"(,"token":")"
           + jsonEscapeString(tokenHex) + R"(","email":")" + jsonEscapeString(emailUtf8) + "\"}";
}

std::string buildEmailCodeOkJson()
{
    return R"({"type":"email_code_ok"})";
}

bool validateHello(const std::string &jsonUtf8, std::string &err)
{
    const auto t = parseMessageType(jsonUtf8);
    if (!t || *t != "hello") {
        err = "type must be hello";
        return false;
    }
    static const std::regex magicRe(R"re("magic"\s*:\s*"([^"]*)")re");
    const auto mag = matchOne(jsonUtf8, magicRe);
    if (!mag || *mag != "LNCS") {
        err = "magic must be LNCS";
        return false;
    }
    static const std::regex verRe(R"re("version"\s*:\s*([0-9]+))re");
    const auto ver = matchOne(jsonUtf8, verRe);
    if (!ver || *ver != "1") {
        err = "version must be 1";
        return false;
    }
    err.clear();
    return true;
}

} // namespace vsserver
