#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vsserver {

/// 业务层错误码（与 `protocol.hpp` 中 TCP 错误帧 code 对齐）。
enum class AuthErrorCode : int {
    Ok = 0,
    DbUnavailable = 2001,
    EmailTaken = 2002,
    InvalidCredentials = 2003,
    EmailNotFound = 2004,
    InvalidInput = 2005,
    InvalidEmailCode = 2006,
    EmailCodeRateLimited = 2007,
    EmailSendFailed = 2008,
    UsernameTaken = 2009, // 与 protocol.hpp `kErrUsernameTaken` 一致
};

struct AuthResult {
    bool ok = false;
    AuthErrorCode code = AuthErrorCode::Ok;
    std::string message;
    std::int64_t userId = 0;
    std::string token;
    std::string email;
    /// 登录用户名（`users.username`）；与邮箱不同。
    std::string username;
    /// 展示用昵称（`users.nickname`）；可与登录用户名不同。
    std::string nickname;
    /// 自定义头像版本；每成功上传一次 JPEG 递增，0 表示未设置服务端头像。
    std::int64_t avatarRev = 0;
};

/// 请求注册邮箱验证码的结果（`req_email_code`）。
struct EmailCodeIssueResult {
    bool ok = false;
    int errCode = 0;
    std::string message;
    int retryAfterSec = 0;
};

/// 打开 `exe同目录/data/chat.db`，建表；多线程通过全局 mutex 串行访问。
class AppDatabase {
public:
    static bool initialize(std::string &errMsg);
    static void shutdown();
    static bool isReady();

    /// 生成并落库注册用验证码；同一邮箱 60s 内重复请求返回限流。
    /// 若设置 `LANCS_MAIL_HELPER_URL` 则通过 HTTP 通知 mail-helper 发信；失败则回滚本条验证码并返回 2008。
    /// 未设置该环境变量时仍写入日志（含明文码）便于离线联调。
    static EmailCodeIssueResult issueRegisterEmailCode(const std::string &email);

    /// `usernameUtf8` 必填；`users.nickname` 初始化为与用户名相同（展示名，可后续由资料接口修改）。
    static AuthResult registerUser(const std::string &email, const std::string &passwordPlain,
                                   const std::string &usernameUtf8, const std::string &emailCodePlain);
    static AuthResult loginByEmail(const std::string &email, const std::string &passwordPlain);
    /// 仅匹配已设置 `username` 的账号；历史账号 `username` 为空时只能使用 `loginByEmail`。
    static AuthResult loginByUsername(const std::string &username, const std::string &passwordPlain);

    static bool validateToken(const std::string &token, std::int64_t &outUserId);

    /// --- 好友（阶段 4，协议 §1.0.3）---

    struct FriendOpOutcome {
        bool ok = false;
        int errCode = 0;
        std::string message;
    };

    /// 更新当前用户的展示昵称（UTF-8，与客户端「编辑资料」一致；写入 `users.nickname`）。
    static FriendOpOutcome setNickname(std::int64_t selfUserId, const std::string &nicknameUtf8);
    /// 写入当前用户的 JPEG 头像（解码后二进制）；校验魔数与长度。
    static FriendOpOutcome setUserAvatarJpeg(std::int64_t selfUserId, const std::vector<std::uint8_t> &jpegBytes,
                                             std::int64_t &outNewRev);
    /// 好友读取对方已存储的头像；非好友返回 `kErrFriendNotFriend`。
    static FriendOpOutcome getFriendAvatarJpeg(std::int64_t selfUserId, std::int64_t peerUserId,
                                               std::vector<std::uint8_t> &outJpeg, std::int64_t &outRev);

    struct FriendSearchHit {
        std::int64_t userId = 0;
        std::string email;
        std::string nickname;
    };

    struct FriendListRow {
        std::int64_t userId = 0;
        std::string email;
        std::string nickname;
        /// 与 `users.avatar_rev` 一致；0 表示无自定义头像。
        std::int64_t avatarRev = 0;
        std::int64_t createdAt = 0;
        /// 与好友最近一条消息（无消息时 `lastMessageAt==0` 且 content 空）
        std::string lastMessageContent;
        std::int64_t lastMessageAt = 0;
        std::int64_t lastMessageFromUserId = 0;
    };

    struct FriendPendingRow {
        std::int64_t requestId = 0;
        std::int64_t otherUserId = 0;
        std::string email;
        std::string nickname;
        std::int64_t createdAt = 0;
    };

    static FriendOpOutcome friendSearch(std::int64_t selfUserId, const std::string &query, int limit,
                                          std::vector<FriendSearchHit> &outUsers);

    /// `targetUserId` 与 `targetEmail` 二选一：优先 `targetUserId > 0`。成功时 `outTargetUserId` 为实际对象 id。
    static FriendOpOutcome friendRequestSend(std::int64_t selfUserId, std::int64_t targetUserId,
                                             const std::string &targetEmail, std::int64_t &outRequestId,
                                             std::int64_t &outTargetUserId);

    static FriendOpOutcome friendRequestList(std::int64_t selfUserId, std::vector<FriendPendingRow> &incoming,
                                             std::vector<FriendPendingRow> &outgoing);

    /// `action`：`accept` / `reject`。成功时 `outPeerId` 为对方 user_id（发起人）。
    static FriendOpOutcome friendRequestHandle(std::int64_t selfUserId, std::int64_t requestId,
                                               const std::string &action, std::int64_t &outPeerId);

    /// 按 `user_id` 读取展示用邮箱与昵称；用户不存在返回 `false`。
    static bool tryGetUserPublic(std::int64_t userId, std::string &outEmail, std::string &outNickname);

    static FriendOpOutcome friendList(std::int64_t selfUserId, std::vector<FriendListRow> &out);

    /// 仅好友的 `peer_user_id` 列表（用于上下线向好友广播）。
    static FriendOpOutcome friendPeerIds(std::int64_t selfUserId, std::vector<std::int64_t> &out);

    static FriendOpOutcome friendDelete(std::int64_t selfUserId, std::int64_t peerUserId);

    /// 是否已为好友（供文件中继等模块使用）。
    static bool areUsersFriends(std::int64_t userId, std::int64_t peerUserId);

    /// --- 即时消息（阶段 5）---

    struct ChatMessageRow {
        std::int64_t messageId = 0;
        std::int64_t fromUserId = 0;
        std::int64_t toUserId = 0;
        std::string content;
        std::int64_t createdAt = 0;
    };

    struct MsgOpOutcome {
        bool ok = false;
        int errCode = 0;
        std::string message;
        std::int64_t messageId = 0;
        std::int64_t createdAt = 0;
        /// `messageClearConversation` 成功时 SQLite `DELETE` 影响行数。
        std::int64_t clearedRows = 0;
    };

    /// 好友间发送文本；`content` UTF-8，长度由服务端限制。
    static MsgOpOutcome messageSend(std::int64_t fromUserId, std::int64_t toUserId, const std::string &contentUtf8);

    /// 插入一条会话消息（与 `messageSend` 相同校验；供文件记录等结构化 `content` 使用）。
    static MsgOpOutcome messageInsertChatRecord(std::int64_t fromUserId, std::int64_t toUserId,
                                                const std::string &contentUtf8);

    /// `before_exclusive>0`：取 `id < before_exclusive` 的最近 `limit` 条（按 id 正序返回，用于向上翻更早）。
    /// 否则：`after_id==0` 取最近 `limit` 条（正序）；`after_id>0` 取 id 更大者（增量拉取）。
    static MsgOpOutcome messageFetch(std::int64_t selfUserId, std::int64_t peerUserId, std::int64_t afterId,
                                     std::int64_t beforeExclusive, int limit, std::vector<ChatMessageRow> &out);

    /// 删除双方在该会话中的全部 `messages` 行（双向）；须为好友。
    static MsgOpOutcome messageClearConversation(std::int64_t selfUserId, std::int64_t peerUserId);

    /// 对当前用户单向隐藏一条私聊消息（写入 `message_deletions`，不删 `messages` 行）。
    static MsgOpOutcome messageHideForUser(std::int64_t selfUserId, std::int64_t messageId);

    /// --- 群聊（阶段 7，MVP）---

    struct GroupListRow {
        std::int64_t groupId = 0;
        std::string name;
        std::int64_t ownerUserId = 0;
        std::int64_t memberCount = 0;
        std::int64_t joinedAt = 0;
        std::string lastMessageContent;
        std::int64_t lastMessageAt = 0;
        std::int64_t lastMessageFromUserId = 0;
        std::string lastMessageFromNickname;
    };

    struct GroupMemberRow {
        std::int64_t userId = 0;
        std::string email;
        std::string nickname;
        std::string role;
        std::int64_t joinedAt = 0;
    };

    struct GroupChatMessageRow {
        std::int64_t messageId = 0;
        std::int64_t groupId = 0;
        std::int64_t fromUserId = 0;
        std::string fromNickname;
        std::string content;
        std::int64_t createdAt = 0;
    };

    struct GroupOpOutcome {
        bool ok = false;
        int errCode = 0;
        std::string message;
        std::int64_t groupId = 0;
        std::int64_t messageId = 0;
        std::int64_t createdAt = 0;
    };

    static GroupOpOutcome groupCreate(std::int64_t ownerUserId, const std::string &nameUtf8,
                                      const std::vector<std::int64_t> &memberUserIds);
    static GroupOpOutcome groupList(std::int64_t selfUserId, std::vector<GroupListRow> &out);
    static GroupOpOutcome groupMembers(std::int64_t selfUserId, std::int64_t groupId,
                                       std::vector<GroupMemberRow> &out);
    static GroupOpOutcome groupMessageSend(std::int64_t fromUserId, std::int64_t groupId, const std::string &contentUtf8);
    static GroupOpOutcome groupMessageFetch(std::int64_t selfUserId, std::int64_t groupId, std::int64_t afterId,
                                            std::int64_t beforeExclusive, int limit,
                                            std::vector<GroupChatMessageRow> &out);
    /// 对当前用户单向隐藏一条群消息（写入 `group_message_deletions`）。
    static GroupOpOutcome groupMessageHideForUser(std::int64_t selfUserId, std::int64_t groupId,
                                                  std::int64_t messageId);
    static GroupOpOutcome groupLeave(std::int64_t selfUserId, std::int64_t groupId);
    static GroupOpOutcome groupMemberIds(std::int64_t selfUserId, std::int64_t groupId,
                                         std::vector<std::int64_t> &outMemberUserIds);

    /// --- 文件传输记录（阶段 6）---

    struct FileOpOutcome {
        bool ok = false;
        int errCode = 0;
        std::string message;
        std::int64_t transferId = 0;
    };

    /// 插入一条 `awaiting_accept` 记录；`sha256HexLower` 可为空（接收方跳过校验）。
    static FileOpOutcome fileTransferInsertOffer(std::int64_t fromUserId, std::int64_t toUserId,
                                                 const std::string &fileNameUtf8, std::int64_t fileSizeBytes,
                                                 const std::string &sha256HexLower);

    /// `completedAtUnixOrZero` > 0 时同时写入 `completed_at`。
    static FileOpOutcome fileTransferSetStatus(std::int64_t transferId, const std::string &statusUtf8,
                                               std::int64_t completedAtUnixOrZero);

    /// 查询 `file_transfers` 一行；`userId` 须为 from 或 to 之一。
    struct FileTransferLookupRow {
        bool ok = false;
        std::int64_t fromUserId = 0;
        std::int64_t toUserId = 0;
        std::int64_t fileSizeBytes = 0;
        std::string fileNameUtf8;
        std::string statusUtf8;
        std::string sha256HexLower;
    };
    static FileTransferLookupRow fileTransferLookupParticipant(std::int64_t transferId, std::int64_t userId);

private:
    static std::string normalizeEmail(std::string_view raw);
};

} // namespace vsserver
