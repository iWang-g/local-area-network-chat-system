#include "vsserver/wire.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace vsserver {

static void writeBe32(std::string &out, std::uint32_t v)
{
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

std::string encodeFrame(std::string_view jsonUtf8)
{
    if (jsonUtf8.size() > kMaxFramePayload) {
        throw std::length_error("encodeFrame: payload too large");
    }
    const auto len = static_cast<std::uint32_t>(jsonUtf8.size());
    std::string out;
    out.reserve(4 + jsonUtf8.size());
    writeBe32(out, len);
    out.append(jsonUtf8.data(), jsonUtf8.size());
    return out;
}

void FrameAssembler::append(const char *data, std::size_t n)
{
    if (n == 0) {
        return;
    }
    const auto *p = reinterpret_cast<const std::uint8_t *>(data);
    buf_.insert(buf_.end(), p, p + n);
}

bool FrameAssembler::nextFrame(std::string &outJson)
{
    if (buf_.size() < 4) {
        return false;
    }
    const std::uint32_t len =
        (static_cast<std::uint32_t>(buf_[0]) << 24) | (static_cast<std::uint32_t>(buf_[1]) << 16) |
        (static_cast<std::uint32_t>(buf_[2]) << 8) | static_cast<std::uint32_t>(buf_[3]);
    if (len > kMaxFramePayload) {
        reset();
        return false;
    }
    if (buf_.size() < 4 + len) {
        return false;
    }
    outJson.assign(reinterpret_cast<const char *>(buf_.data() + 4), len);
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(4 + len));
    return true;
}

void FrameAssembler::reset()
{
    buf_.clear();
}

} // namespace vsserver
