#include "vsserver/password_hash.hpp"

#include "picosha2.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <cstdio>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace vsserver {

namespace {

static void fillCryptoRandom(unsigned char *buf, std::size_t n)
{
#ifdef _WIN32
    const NTSTATUS st =
        ::BCryptGenRandom(nullptr, buf, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        throw std::runtime_error("BCryptGenRandom failed");
    }
#else
    const int fd = ::open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error("open /dev/urandom failed");
    }
    std::size_t off = 0;
    while (off < n) {
        const ssize_t r = ::read(fd, buf + off, n - off);
        if (r <= 0) {
            ::close(fd);
            throw std::runtime_error("read /dev/urandom failed");
        }
        off += static_cast<std::size_t>(r);
    }
    ::close(fd);
#endif
}

} // namespace

static std::string bytesToHex(const unsigned char *data, std::size_t n)
{
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < n; ++i) {
        oss << std::setw(2) << static_cast<int>(data[i]);
    }
    return oss.str();
}

std::string randomSaltHex(std::size_t numBytes)
{
    std::vector<unsigned char> buf(numBytes);
    fillCryptoRandom(buf.data(), numBytes);
    return bytesToHex(buf.data(), buf.size());
}

static std::string sha256HexOfString(const std::string &s)
{
    return picosha2::hash256_hex_string(s.begin(), s.end());
}

std::string derivePasswordHash(const std::string &saltHex, const std::string &passwordPlain)
{
    std::string x = saltHex + '|' + passwordPlain;
    constexpr int kIterations = 10000;
    for (int i = 0; i < kIterations; ++i) {
        x = sha256HexOfString(x);
    }
    return x;
}

bool verifyPassword(const std::string &saltHex, const std::string &passwordPlain, const std::string &storedHashHex)
{
    return derivePasswordHash(saltHex, passwordPlain) == storedHashHex;
}

std::string randomTokenHex(std::size_t numRandomBytes)
{
    return randomSaltHex(numRandomBytes);
}

std::string randomSixDigitCode()
{
    unsigned char b[4]{};
    fillCryptoRandom(b, sizeof(b));
    const unsigned v = (static_cast<unsigned>(b[0]) << 16) | (static_cast<unsigned>(b[1]) << 8) | b[2];
    const unsigned n = v % 1000000U;
    char buf[8]{};
    std::snprintf(buf, sizeof(buf), "%06u", n);
    return std::string(buf);
}

} // namespace vsserver
