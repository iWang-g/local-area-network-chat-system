#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vsserver {

/// 单帧负载上限（不含 4 字节长度头），防止恶意大包占满内存。
constexpr std::size_t kMaxFramePayload = 256 * 1024;

/// 将 UTF-8 JSON 文本封装为：4 字节大端长度 + 负载。
std::string encodeFrame(std::string_view jsonUtf8);

/// 从字节流中切出完整帧（处理粘包 / 半包）。
class FrameAssembler {
public:
    void append(const char *data, std::size_t n);
    /// 若解析到一帧，写入 outJson 并返回 true；否则返回 false。
    bool nextFrame(std::string &outJson);
    void reset();

private:
    std::vector<std::uint8_t> buf_;
};

} // namespace vsserver
