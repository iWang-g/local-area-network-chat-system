#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "vsserver/message_parse.hpp"

namespace vsserver {

struct FileOfferResult {
    bool ok = false;
    int errCode = 0;
    std::string message;
    std::int64_t transferId = 0;
    std::string jsonOfferOkUtf8;
    std::string jsonIncomingUtf8;
};

struct FileNotifyResult {
    bool ok = false;
    int errCode = 0;
    std::string message;
    std::int64_t notifyUserId = 0;
    std::string notifyJsonUtf8;
    /// 拒绝/失败时写入会话表后，向双方推送 `msg_push`。
    bool hasChatMsg = false;
    ChatMessageEntry chatMsg;
};

struct FileChunkRelayResult {
    bool ok = false;
    int errCode = 0;
    std::string message;
    std::int64_t pushToUserId = 0;
    /// JSON Base64 路径（与 `pushBinaryPayload` 二选一）。
    std::string pushJsonUtf8;
    /// 二进制 LNCB 下行负载（与 `pushJsonUtf8` 二选一）。
    std::string pushBinaryPayload;
};

struct FileDoneRelayResult {
    bool ok = false;
    int errCode = 0;
    std::string message;
    std::int64_t pushToUserId = 0;
    std::string pushDoneJsonUtf8;
    bool hasChatMsg = false;
    ChatMessageEntry chatMsg;
};

FileOfferResult fileRelayOffer(std::int64_t fromUserId, std::int64_t peerUserId, std::string rawFileNameUtf8,
                               std::uint64_t fileSizeBytes, std::string sha256HexLower, bool asSticker = false,
                               std::optional<std::uint32_t> chunkPlainMaxBinary = std::nullopt);

/// 对方离线：在服务端磁盘缓冲全部分片，完成后写入会话（无需对端 `file_accept`）。`asSticker` 写入消息 JSON。
FileOfferResult fileRelayOfferServerBufferPeerOffline(std::int64_t fromUserId, std::int64_t peerUserId,
                                                      std::string rawFileNameUtf8, std::uint64_t fileSizeBytes,
                                                      std::string sha256HexLower, bool asSticker,
                                                      std::optional<std::uint32_t> chunkPlainMaxBinary = std::nullopt);

/// 若存在可读的离线缓冲文件，返回其 UTF-8 路径；否则返回空。
/// `expectedFileSizeBytes > 0` 时要求文件大小与声明一致；先查 `sticker_cache`，再查未 rename 的 `offline_partial/*.part`。
std::string fileRelayServerBufferArtifactPathIfExists(std::int64_t transferId, const std::string &fileNameUtf8,
                                                      std::int64_t expectedFileSizeBytes = 0);

FileNotifyResult fileRelayAccept(std::int64_t selfUserId, std::int64_t transferId);
FileNotifyResult fileRelayReject(std::int64_t selfUserId, std::int64_t transferId);

FileChunkRelayResult fileRelayOnSenderChunk(std::int64_t selfUserId, std::int64_t transferId, std::uint32_t seq,
                                            const std::string &dataB64Utf8);

/// 二进制明文分片（与 `fileRelayOnSenderChunk` 互斥使用场景）。
FileChunkRelayResult fileRelayOnSenderChunkPlain(std::int64_t selfUserId, std::int64_t transferId, std::uint32_t seq,
                                                 const std::vector<std::uint8_t> &plain);

FileDoneRelayResult fileRelayOnSenderDone(std::int64_t selfUserId, std::int64_t transferId);

struct FileDisconnectItem {
    std::int64_t notifyUserId = 0;
    std::string fileAbortedJson;
    bool hasChatMsg = false;
    ChatMessageEntry chatMsg;
};

/// 断线时取消该用户参与的未完成传输；并可为会话插入「失败」文件消息。
std::vector<FileDisconnectItem> fileRelayOnUserDisconnect(std::int64_t userId);

} // namespace vsserver
