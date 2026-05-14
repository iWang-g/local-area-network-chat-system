#pragma once

#include <cstdint>

namespace vsserver {

/// 与客户端约定的协议版本、魔数；后续在此扩展帧类型、错误码等。
inline constexpr std::uint16_t kProtocolVersion = 1;
inline constexpr char kMagic[4] = {'L', 'N', 'C', 'S'};
/// 默认监听端口（与 qt-client 默认一致）。
inline constexpr std::uint16_t kDefaultTcpPort = 28888;

/// TCP JSON `error` 帧中的 `code`（与 `AppDatabase` / 握手逻辑对齐）。
inline constexpr int kErrBadHandshake = 1001;
inline constexpr int kErrNeedHandshake = 2010;
/// 未登录或 token 无效（好友等业务报文须携带 `auth_ok` 下发的 token）。
inline constexpr int kErrNeedToken = 2011;
/// 昵称格式或长度不符合要求（`profile_set`）。
inline constexpr int kErrProfileNickname = 2012;
/// 头像不是合法 JPEG 或超过单帧/服务端大小上限（`profile_set_avatar`）。
inline constexpr int kErrProfileAvatar = 2013;
inline constexpr int kErrDbUnavailable = 2001;
inline constexpr int kErrEmailTaken = 2002;
inline constexpr int kErrInvalidCredentials = 2003;
inline constexpr int kErrUserNotFound = 2004;
inline constexpr int kErrInvalidInput = 2005;
inline constexpr int kErrInvalidEmailCode = 2006;
inline constexpr int kErrEmailCodeRateLimited = 2007;
/// 已配置 `LANCS_MAIL_HELPER_URL` 但 HTTP 通知或 SMTP 侧失败（验证码未下发给用户）。
inline constexpr int kErrEmailSendFailed = 2008;
/// 注册用户名已被占用。
inline constexpr int kErrUsernameTaken = 2009;

/// 好友模块（与协议草案 §1.0.3 对齐）
inline constexpr int kErrFriendUserNotFound = 3001;
inline constexpr int kErrFriendCannotSelf = 3002;
inline constexpr int kErrFriendAlready = 3003;
inline constexpr int kErrFriendPendingExists = 3004;
inline constexpr int kErrFriendTheyPending = 3005;
inline constexpr int kErrFriendRequestGone = 3006;
inline constexpr int kErrFriendRequestNotYours = 3007;
inline constexpr int kErrFriendNotFriend = 3008;

/// 即时消息（阶段 5）
inline constexpr int kErrMsgNotFriend = 4001;
inline constexpr int kErrMsgTooLong = 4002;

/// 群聊（阶段 7，MVP）
inline constexpr int kErrGroupNotFound = 6001;
inline constexpr int kErrGroupNotMember = 6002;
inline constexpr int kErrGroupBadName = 6003;
inline constexpr int kErrGroupBadMembers = 6004;
inline constexpr int kErrGroupOwnerLeave = 6005;

/// 文件中继（阶段 6，JSON + Base64 分片，单帧 ≤256KiB）
inline constexpr int kErrFileNotFriend = 5001;
inline constexpr int kErrFileTooLarge = 5002;
inline constexpr int kErrFileBadName = 5003;
inline constexpr int kErrFileNotFound = 5004;
inline constexpr int kErrFileWrongRole = 5005;
inline constexpr int kErrFileSeq = 5006;
inline constexpr int kErrFileChunk = 5007;
inline constexpr int kErrFileSizeMismatch = 5008;
/// 好友在线但当前无 TCP 会话（文件邀约无法投递 `file_incoming`）。
inline constexpr int kErrPeerOffline = 5009;

/// 明文分片上限（与 `wire::kMaxFramePayload` 内 Base64 预留一致）。
inline constexpr std::uint32_t kFileChunkPlainMax = 65536;

/// 二进制文件分片帧（LNCB）魔数，负载首 4 字节；与 JSON 对象首字节 `{` 区分。
inline constexpr char kLnCbMagic[4] = {'L', 'N', 'C', 'B'};
inline constexpr std::uint16_t kLnCbVersion = 1;
/// 下行 `file_chunk_push` 二进制子类型。
inline constexpr std::uint16_t kLnCbKindChunkPush = 1;
/// C→S 二进制分片头：magic(4)+ver(2)+transfer_id(8)+seq(4)+token(64)+plain_len(4)=86。
inline constexpr std::size_t kLnCbSenderChunkHeaderSize = 86;
/// S→C 二进制分片头：magic(4)+ver(2)+kind(2)+transfer_id(8)+seq(4)+plain_len(4)=24。
inline constexpr std::size_t kLnCbChunkPushHeaderSize = 24;
/// 单帧内明文最大长度（256KiB 负载 − 发送头）；与 `wire::kMaxFramePayload` 对齐。
inline constexpr std::uint32_t kFileChunkBinaryPlainMax =
    static_cast<std::uint32_t>((256U * 1024U) - kLnCbSenderChunkHeaderSize);
/// 单文件传输大小上限（字节）。
inline constexpr std::uint64_t kFileTransferMaxBytes = 512ULL * 1024 * 1024;
/// 对方离线时，仅「表情」可走服务端内存缓冲 + 落盘，单条大小上限（字节）。
inline constexpr std::uint64_t kOfflineStickerServerBufferMaxBytes = 8ULL * 1024 * 1024;

} // namespace vsserver
