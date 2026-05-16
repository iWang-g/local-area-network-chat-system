#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vsserver {

struct FriendPublicUser {
    std::int64_t userId = 0;
    std::string email;
    std::string nickname;
};

struct FriendListEntry {
    std::int64_t userId = 0;
    std::string email;
    std::string nickname;
    std::int64_t avatarRev = 0;
    std::int64_t createdAt = 0;
    std::string lastMessagePreview;
    std::int64_t lastMessageAt = 0;
    std::int64_t lastMessageFromUserId = 0;
    bool online = false;
};

struct FriendPendingEntry {
    std::int64_t requestId = 0;
    std::int64_t otherUserId = 0;
    std::string email;
    std::string nickname;
    std::int64_t createdAt = 0;
};

struct ChatMessageEntry {
    std::int64_t messageId = 0;
    std::int64_t fromUserId = 0;
    std::int64_t toUserId = 0;
    std::string content;
    std::int64_t createdAt = 0;
};

struct FileVoiceMeta {
    bool isVoice = false;
    std::int64_t durationMs = 0;
    std::string mimeType;
};

struct GroupListEntry {
    std::int64_t groupId = 0;
    std::string name;
    std::int64_t ownerUserId = 0;
    std::int64_t memberCount = 0;
    std::int64_t joinedAt = 0;
    std::string lastMessagePreview;
    std::int64_t lastMessageAt = 0;
    std::int64_t lastMessageFromUserId = 0;
    std::string lastMessageFromNickname;
};

struct GroupMemberEntry {
    std::int64_t userId = 0;
    std::string email;
    std::string nickname;
    std::string role;
    std::int64_t joinedAt = 0;
};

struct GroupChatMessageEntry {
    std::int64_t messageId = 0;
    std::int64_t groupId = 0;
    std::int64_t fromUserId = 0;
    std::string fromNickname;
    std::string content;
    std::int64_t createdAt = 0;
};

/// 从 JSON 文本中解析 "type" 字段（字符串值）；失败返回 nullopt。
std::optional<std::string> parseMessageType(const std::string &jsonUtf8);

/// 解析 `"field": "value"` 中的字符串值（不含转义序列扩展，阶段 2 够用）。
std::optional<std::string> parseJsonStringField(const std::string &jsonUtf8, const char *asciiFieldName);

/// 解析 `"field": <integer>`（无引号数字）。
std::optional<std::int64_t> parseJsonInt64Field(const std::string &jsonUtf8, const char *asciiFieldName);
/// 解析 `"field": [1,2,3]` 形式的整型数组；支持空数组。
std::optional<std::vector<std::int64_t>> parseJsonInt64ArrayField(const std::string &jsonUtf8,
                                                                  const char *asciiFieldName);

std::string jsonEscapeString(std::string_view utf8);

bool wireBase64Encode(const std::uint8_t *data, std::size_t len, std::string &outB64Utf8);
bool wireBase64Decode(const std::string &b64Utf8, std::vector<std::uint8_t> &outBytes);

/// retryAfterSec >= 0 时附加 `retry_after_sec` 字段（客户端重发倒计时）。
/// `corr >= 0` 时在 JSON 中附加 `corr`，供客户端关联请求与错误/应答。
std::string buildErrorJson(int code, const std::string &messageUtf8, int retryAfterSec = -1,
                           std::int64_t corr = -1);
std::string buildAuthOkJson(std::int64_t userId, const std::string &tokenHex, const std::string &emailUtf8,
                            const std::string &usernameUtf8, const std::string &nicknameUtf8, std::int64_t avatarRev);
std::string buildEmailCodeOkJson();

std::string buildFriendSearchResultJson(const std::vector<FriendPublicUser> &users);
std::string buildFriendRequestSentJson(std::int64_t requestId, std::int64_t targetUserId);
std::string buildFriendRequestListOkJson(const std::vector<FriendPendingEntry> &incoming,
                                         const std::vector<FriendPendingEntry> &outgoing);
std::string buildFriendRequestHandledJson(std::int64_t requestId, const std::string &action,
                                          std::int64_t peerUserId);
/// 同意申请后下发给**发起人**（原申请方）：便于客户端刷新好友/会话列表。
std::string buildFriendNotifyRequestAcceptedJson(std::int64_t requestId, std::int64_t peerUserId,
                                                 const std::string &emailUtf8, const std::string &nicknameUtf8);
std::string buildFriendNotifyPresenceJson(std::int64_t peerUserId, bool online);
/// 好友修改展示昵称后向在线好友下推；`peer_user_id` 为修改方。
std::string buildFriendNotifyNicknameJson(std::int64_t peerUserId, const std::string &nicknameUtf8);
std::string buildProfileSetOkJson(const std::string &nicknameUtf8, std::int64_t corr);
std::string buildProfileSetAvatarOkJson(std::int64_t avatarRev, std::int64_t corr);
std::string buildPeerAvatarOkJson(std::int64_t peerUserId, std::int64_t avatarRev, const std::string &avatarB64Utf8);
std::string buildFriendNotifyAvatarJson(std::int64_t peerUserId, std::int64_t avatarRev);
std::string buildFriendListOkJson(const std::vector<FriendListEntry> &friends);
std::string buildFriendDeleteOkJson(std::int64_t peerUserId);

std::string buildMsgSendOkJson(const ChatMessageEntry &e);
std::string buildMsgFetchOkJson(std::int64_t peerUserId, const std::vector<ChatMessageEntry> &messages);
std::string buildMsgPushJson(const ChatMessageEntry &e);
std::string buildMsgClearOkJson(std::int64_t peerUserId, std::int64_t deletedRows);
/// 对端在线时下推：发起清空的一方 `by_user_id`。
std::string buildMsgConvClearedJson(std::int64_t byUserId);
std::string buildMsgDeleteOkJson(std::int64_t messageId);

std::string buildGroupCreateOkJson(std::int64_t groupId, const std::string &nameUtf8, std::int64_t ownerUserId,
                                   std::int64_t memberCount);
std::string buildGroupListOkJson(const std::vector<GroupListEntry> &groups);
std::string buildGroupMembersOkJson(std::int64_t groupId, const std::vector<GroupMemberEntry> &members);
std::string buildGroupMsgSendOkJson(const GroupChatMessageEntry &e);
std::string buildGroupMsgFetchOkJson(std::int64_t groupId, const std::vector<GroupChatMessageEntry> &messages);
std::string buildGroupMsgPushJson(const GroupChatMessageEntry &e);
std::string buildGroupMsgDeleteOkJson(std::int64_t groupId, std::int64_t messageId);
std::string buildGroupLeaveOkJson(std::int64_t groupId);

/// 阶段 6：文件分片（JSON + Base64）；若给定 `chunkPlainMaxBinary` 则附带二进制分片能力字段。
std::string buildFileOfferOkJson(std::int64_t transferId, std::uint32_t chunkPlainMax,
                                 std::optional<std::uint32_t> chunkPlainMaxBinary = std::nullopt);

/// `hello_ok`：可选宣告二进制文件分片（客户端需在 hello 中带 `capabilities` 含 `file_chunk_binary_v1`）。
std::string buildHelloOkJson(bool fileChunkBinary);

/// C→S：二进制 `file_chunk` 负载（不含 4 字节长度前缀）。
std::string buildLnCbSenderChunkPayload(std::int64_t transferId, std::uint32_t seq, const std::string &tokenHex64,
                                        const std::uint8_t *plain, std::size_t plainLen);
/// S→C：二进制 `file_chunk_push` 负载（不含长度前缀）。
std::string buildLnCbChunkPushPayload(std::int64_t transferId, std::uint32_t seq, const std::uint8_t *plain,
                                      std::size_t plainLen);

struct LnCbSenderChunkParse {
    std::int64_t transferId = 0;
    std::uint32_t seq = 0;
    std::string tokenHex64;
    std::vector<std::uint8_t> plain;
};
/// 解析 C→S LNCB 分片；成功返回 true。
bool parseLnCbSenderChunkPayload(const std::string &payload, LnCbSenderChunkParse &out, std::string &err);
/// 已向接收方投递 `file_incoming` 后发给发送方，用于将气泡状态更新为「已发送」。
std::string buildFileOfferDeliveredJson(std::int64_t transferId);
std::string buildFileIncomingJson(std::int64_t transferId, std::int64_t fromUserId, const std::string &fileNameUtf8,
                                  std::uint64_t fileSize, const std::string &sha256HexLower,
                                  const FileVoiceMeta &voiceMeta = {});
std::string buildFileSendReadyJson(std::int64_t transferId);
std::string buildFileChunkPushJson(std::int64_t transferId, std::uint32_t seq, const std::string &dataB64Utf8);
std::string buildFileTransferDonePushJson(std::int64_t transferId, const std::string &sha256HexLower);
std::string buildFileAbortedJson(std::int64_t transferId, const std::string &reasonUtf8);
std::string buildFileRejectOkJson(std::int64_t transferId);
std::string buildFileSenderDoneOkJson(std::int64_t transferId);

/// 客户端拉取「服务端缓存的表情」二进制（对方离线时经缓冲落盘的传输）。
std::string buildFileStickerPullOkJson(std::int64_t transferId, std::uint64_t fileSizeBytes,
                                       std::uint32_t chunkPlainMax);
std::string buildFileStickerPullChunkJson(std::int64_t transferId, std::uint32_t seq,
                                          const std::string &dataB64Utf8);
std::string buildFileStickerPullDoneJson(std::int64_t transferId, const std::string &sha256HexLower);

/// 写入 `messages.content` 的文件/图片类消息 JSON（`kind=file` 或 `kind=image`，由文件名扩展名判定）。
/// `state`：`ok` 时填 `sha256HexLower`；`failed` 时填 `reasonUtf8`，`sha256` 忽略。
std::string buildFileChatMessageContentJson(std::int64_t transferId, const std::string &fileNameUtf8,
                                            std::int64_t fileSizeBytes, const std::string &sha256HexLower,
                                            const char *stateAscii, const std::string &reasonUtf8, bool asSticker = false,
                                            const FileVoiceMeta &voiceMeta = {});

/// 校验握手：type=hello、magic=LNCS、version=1。
bool validateHello(const std::string &jsonUtf8, std::string &err);

} // namespace vsserver
