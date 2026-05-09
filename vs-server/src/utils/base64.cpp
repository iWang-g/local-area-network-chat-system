#include "vsserver/base64.hpp"

#include <cstring>

namespace vsserver {

namespace {

constexpr char kTable[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

inline int decodeChar(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

} // namespace

bool wireBase64EncodeBytes(const std::uint8_t *data, std::size_t len, std::string &outB64Utf8)
{
    outB64Utf8.clear();
    if (data == nullptr || len == 0) {
        return false;
    }
    outB64Utf8.reserve((len + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        const unsigned b0 = data[i];
        const unsigned b1 = data[i + 1];
        const unsigned b2 = data[i + 2];
        outB64Utf8.push_back(kTable[(b0 >> 2) & 63]);
        outB64Utf8.push_back(kTable[((b0 << 4) | (b1 >> 4)) & 63]);
        outB64Utf8.push_back(kTable[((b1 << 2) | (b2 >> 6)) & 63]);
        outB64Utf8.push_back(kTable[b2 & 63]);
        i += 3;
    }
    const std::size_t rem = len - i;
    if (rem == 1) {
        const unsigned b0 = data[i];
        outB64Utf8.push_back(kTable[(b0 >> 2) & 63]);
        outB64Utf8.push_back(kTable[(b0 << 4) & 63]);
        outB64Utf8.push_back('=');
        outB64Utf8.push_back('=');
    } else if (rem == 2) {
        const unsigned b0 = data[i];
        const unsigned b1 = data[i + 1];
        outB64Utf8.push_back(kTable[(b0 >> 2) & 63]);
        outB64Utf8.push_back(kTable[((b0 << 4) | (b1 >> 4)) & 63]);
        outB64Utf8.push_back(kTable[(b1 << 2) & 63]);
        outB64Utf8.push_back('=');
    }
    return true;
}

bool wireBase64DecodeBytes(const std::string &b64Utf8, std::vector<std::uint8_t> &outBytes)
{
    outBytes.clear();
    const std::size_t n = b64Utf8.size();
    if (n == 0 || (n % 4) != 0) {
        return false;
    }

    std::size_t outLen = n / 4 * 3;
    if (b64Utf8[n - 1] == '=') {
        outLen--;
    }
    if (n >= 2 && b64Utf8[n - 2] == '=') {
        outLen--;
    }
    outBytes.resize(outLen);

    std::size_t outIdx = 0;
    for (std::size_t i = 0; i < n; i += 4) {
        const int a = decodeChar(static_cast<unsigned char>(b64Utf8[i]));
        const int b = decodeChar(static_cast<unsigned char>(b64Utf8[i + 1]));
        if (a < 0 || b < 0) {
            outBytes.clear();
            return false;
        }
        int c = 0;
        int d = 0;
        if (b64Utf8[i + 2] == '=') {
            if (b64Utf8[i + 3] != '=') {
                outBytes.clear();
                return false;
            }
            const unsigned triple = (static_cast<unsigned>(a) << 18U) | (static_cast<unsigned>(b) << 12U);
            if (outIdx >= outLen) {
                outBytes.clear();
                return false;
            }
            outBytes[outIdx++] = static_cast<std::uint8_t>((triple >> 16) & 0xFFU);
            continue;
        }
        c = decodeChar(static_cast<unsigned char>(b64Utf8[i + 2]));
        if (c < 0) {
            outBytes.clear();
            return false;
        }
        if (b64Utf8[i + 3] == '=') {
            const unsigned triple =
                (static_cast<unsigned>(a) << 18U) | (static_cast<unsigned>(b) << 12U) | (static_cast<unsigned>(c) << 6U);
            if (outIdx + 1 >= outLen) {
                outBytes.clear();
                return false;
            }
            outBytes[outIdx++] = static_cast<std::uint8_t>((triple >> 16) & 0xFFU);
            outBytes[outIdx++] = static_cast<std::uint8_t>((triple >> 8) & 0xFFU);
            continue;
        }
        d = decodeChar(static_cast<unsigned char>(b64Utf8[i + 3]));
        if (d < 0) {
            outBytes.clear();
            return false;
        }
        const unsigned triple = (static_cast<unsigned>(a) << 18U) | (static_cast<unsigned>(b) << 12U)
                                | (static_cast<unsigned>(c) << 6U) | static_cast<unsigned>(d);
        if (outIdx + 2 >= outLen) {
            outBytes.clear();
            return false;
        }
        outBytes[outIdx++] = static_cast<std::uint8_t>((triple >> 16) & 0xFFU);
        outBytes[outIdx++] = static_cast<std::uint8_t>((triple >> 8) & 0xFFU);
        outBytes[outIdx++] = static_cast<std::uint8_t>(triple & 0xFFU);
    }

    return outIdx == outLen;
}

bool wireBase64DecodedByteCount(const std::string &b64Utf8, std::size_t &outCount)
{
    std::vector<std::uint8_t> tmp;
    if (!wireBase64DecodeBytes(b64Utf8, tmp)) {
        return false;
    }
    outCount = tmp.size();
    return true;
}

} // namespace vsserver
