#include "vsserver/message_parse.hpp"
#include "vsserver/protocol.hpp"
#include "vsserver/base64.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>
#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

namespace vsserver {

namespace {

static std::optional<std::string> matchOne(const std::string &json, const std::regex &re)
{
    std::smatch m;
    if (!std::regex_search(json, m, re) || m.size() < 2) {
        return std::nullopt;
    }
    return m[1].str();
}

static bool isSpace(char c)
{
    return std::isspace(static_cast<unsigned char>(c)) != 0;
}

/// 从 `i` 指向的开引号 `"` 起读取 JSON 字符串内容（支持常见转义），结束后 `i` 停在闭合 `"` 的下一字符。
static std::optional<std::string> readJsonStringToken(const std::string &json, std::size_t &io_i)
{
    if (io_i >= json.size() || json[io_i] != '"') {
        return std::nullopt;
    }
    ++io_i;
    std::string out;
    while (io_i < json.size()) {
        const unsigned char cu = static_cast<unsigned char>(json[io_i]);
        if (cu == '"') {
            ++io_i;
            return out;
        }
        if (cu == '\\' && io_i + 1 < json.size()) {
            const char e = json[io_i + 1];
            switch (e) {
            case '"':
                out.push_back('"');
                io_i += 2;
                continue;
            case '\\':
                out.push_back('\\');
                io_i += 2;
                continue;
            case '/':
                out.push_back('/');
                io_i += 2;
                continue;
            case 'b':
                out.push_back('\b');
                io_i += 2;
                continue;
            case 'f':
                out.push_back('\f');
                io_i += 2;
                continue;
            case 'n':
                out.push_back('\n');
                io_i += 2;
                continue;
            case 'r':
                out.push_back('\r');
                io_i += 2;
                continue;
            case 't':
                out.push_back('\t');
                io_i += 2;
                continue;
            case 'u':
                if (io_i + 6 > json.size()) {
                    return std::nullopt;
                }
                {
                    std::uint32_t cp = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = json[io_i + 2 + k];
                        std::uint32_t v = 0;
                        if (h >= '0' && h <= '9') {
                            v = static_cast<std::uint32_t>(h - '0');
                        } else if (h >= 'a' && h <= 'f') {
                            v = static_cast<std::uint32_t>(h - 'a' + 10);
                        } else if (h >= 'A' && h <= 'F') {
                            v = static_cast<std::uint32_t>(h - 'A' + 10);
                        } else {
                            return std::nullopt;
                        }
                        cp = (cp << 4U) | v;
                    }
                    if (cp <= 0x7FU) {
                        out.push_back(static_cast<char>(cp));
                    } else if (cp <= 0x7FFU) {
                        out.push_back(static_cast<char>(0xC0U | ((cp >> 6U) & 0x1FU)));
                        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                    } else if (cp <= 0xFFFFU) {
                        out.push_back(static_cast<char>(0xE0U | ((cp >> 12U) & 0x0FU)));
                        out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
                        out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
                    } else {
                        return std::nullopt;
                    }
                    io_i += 6;
                    continue;
                }
            default:
                out.push_back(e);
                io_i += 2;
                continue;
            }
        }
        out.push_back(static_cast<char>(cu));
        ++io_i;
    }
    return std::nullopt;
}

} // namespace

/// 不对整帧跑 std::regex（大帧如 avatar_b64 会在 libstdc++ 下栈溢出/崩溃）；线性扫描。
std::optional<std::string> parseMessageType(const std::string &jsonUtf8)
{
    constexpr const char kKey[] = "\"type\"";
    std::size_t pos = 0;
    const std::size_t n = jsonUtf8.size();
    const std::size_t keyLen = sizeof(kKey) - 1;
    while (pos < n) {
        pos = jsonUtf8.find(kKey, pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        std::size_t i = pos + keyLen;
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        if (i >= n || jsonUtf8[i] != ':') {
            ++pos;
            continue;
        }
        ++i;
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        std::optional<std::string> val = readJsonStringToken(jsonUtf8, i);
        if (val) {
            return val;
        }
        ++pos;
    }
    return std::nullopt;
}

std::optional<std::string> parseJsonStringField(const std::string &jsonUtf8, const char *asciiFieldName)
{
    const std::string key = std::string("\"") + asciiFieldName + "\"";
    std::size_t pos = 0;
    const std::size_t n = jsonUtf8.size();
    while (pos < n) {
        pos = jsonUtf8.find(key, pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        std::size_t i = pos + key.size();
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        if (i >= n || jsonUtf8[i] != ':') {
            ++pos;
            continue;
        }
        ++i;
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        std::optional<std::string> val = readJsonStringToken(jsonUtf8, i);
        if (val) {
            return val;
        }
        ++pos;
    }
    return std::nullopt;
}

std::optional<std::int64_t> parseJsonInt64Field(const std::string &jsonUtf8, const char *asciiFieldName)
{
    const std::string key = std::string("\"") + asciiFieldName + "\"";
    std::size_t pos = 0;
    const std::size_t n = jsonUtf8.size();
    while (pos < n) {
        pos = jsonUtf8.find(key, pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        std::size_t i = pos + key.size();
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        if (i >= n || jsonUtf8[i] != ':') {
            ++pos;
            continue;
        }
        ++i;
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        bool neg = false;
        if (i < n && jsonUtf8[i] == '-') {
            neg = true;
            ++i;
        }
        if (i >= n || jsonUtf8[i] < '0' || jsonUtf8[i] > '9') {
            ++pos;
            continue;
        }
        const std::size_t start = i;
        while (i < n && jsonUtf8[i] >= '0' && jsonUtf8[i] <= '9') {
            ++i;
        }
        try {
            long long v = std::stoll(jsonUtf8.substr(start, i - start));
            if (neg) {
                v = -v;
            }
            return static_cast<std::int64_t>(v);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<std::vector<std::int64_t>> parseJsonInt64ArrayField(const std::string &jsonUtf8, const char *asciiFieldName)
{
    const std::string key = std::string("\"") + asciiFieldName + "\"";
    std::size_t pos = 0;
    const std::size_t n = jsonUtf8.size();
    while (pos < n) {
        pos = jsonUtf8.find(key, pos);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        std::size_t i = pos + key.size();
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        if (i >= n || jsonUtf8[i] != ':') {
            ++pos;
            continue;
        }
        ++i;
        while (i < n && isSpace(jsonUtf8[i])) {
            ++i;
        }
        if (i >= n || jsonUtf8[i] != '[') {
            ++pos;
            continue;
        }
        ++i;
        std::vector<std::int64_t> out;
        for (;;) {
            while (i < n && isSpace(jsonUtf8[i])) {
                ++i;
            }
            if (i >= n) {
                return std::nullopt;
            }
            if (jsonUtf8[i] == ']') {
                ++i;
                return out;
            }
            bool neg = false;
            if (jsonUtf8[i] == '-') {
                neg = true;
                ++i;
            }
            if (i >= n || jsonUtf8[i] < '0' || jsonUtf8[i] > '9') {
                return std::nullopt;
            }
            const std::size_t start = i;
            while (i < n && jsonUtf8[i] >= '0' && jsonUtf8[i] <= '9') {
                ++i;
            }
            try {
                long long v = std::stoll(jsonUtf8.substr(start, i - start));
                out.push_back(static_cast<std::int64_t>(neg ? -v : v));
            } catch (...) {
                return std::nullopt;
            }
            while (i < n && isSpace(jsonUtf8[i])) {
                ++i;
            }
            if (i >= n) {
                return std::nullopt;
            }
            if (jsonUtf8[i] == ',') {
                ++i;
                continue;
            }
            if (jsonUtf8[i] == ']') {
                ++i;
                return out;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
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

std::string buildErrorJson(int code, const std::string &messageUtf8, int retryAfterSec, std::int64_t corr)
{
    std::string out = std::string(R"({"type":"error","code":)") + std::to_string(code) + R"(,"message":")"
                        + jsonEscapeString(messageUtf8) + "\"";
    if (retryAfterSec >= 0) {
        out += R"(,"retry_after_sec":)" + std::to_string(retryAfterSec);
    }
    if (corr >= 0) {
        out += R"(,"corr":)" + std::to_string(corr);
    }
    out += '}';
    return out;
}

bool wireBase64Encode(const std::uint8_t *data, const std::size_t len, std::string &outB64Utf8)
{
    return wireBase64EncodeBytes(data, len, outB64Utf8);
}

bool wireBase64Decode(const std::string &b64Utf8, std::vector<std::uint8_t> &outBytes)
{
    return wireBase64DecodeBytes(b64Utf8, outBytes);
}

std::string buildAuthOkJson(const std::int64_t userId, const std::string &tokenHex, const std::string &emailUtf8,
                            const std::string &usernameUtf8, const std::string &nicknameUtf8,
                            const std::int64_t avatarRev)
{
    return std::string(R"({"type":"auth_ok","user_id":)") + std::to_string(userId) + R"(,"token":")"
           + jsonEscapeString(tokenHex) + R"(","email":")" + jsonEscapeString(emailUtf8) + R"(","username":")"
           + jsonEscapeString(usernameUtf8) + R"(","nickname":")" + jsonEscapeString(nicknameUtf8)
           + R"(","avatar_rev":)" + std::to_string(avatarRev) + "}";
}

std::string buildEmailCodeOkJson()
{
    return R"({"type":"email_code_ok"})";
}

std::string buildFriendSearchResultJson(const std::vector<FriendPublicUser> &users)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_search_result","users":[)";
    for (std::size_t i = 0; i < users.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << R"({"user_id":)" << users[i].userId << R"(,"email":")" << jsonEscapeString(users[i].email)
            << R"(","nickname":")" << jsonEscapeString(users[i].nickname) << R"("})";
    }
    oss << "]}";
    return oss.str();
}

std::string buildFriendRequestSentJson(const std::int64_t requestId, const std::int64_t targetUserId)
{
    return std::string(R"({"type":"friend_request_sent","request_id":)") + std::to_string(requestId)
           + R"(,"target_user_id":)" + std::to_string(targetUserId) + "}";
}

std::string buildFriendRequestListOkJson(const std::vector<FriendPendingEntry> &incoming,
                                         const std::vector<FriendPendingEntry> &outgoing)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_request_list_ok","incoming":[)";
    for (std::size_t i = 0; i < incoming.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << R"({"request_id":)" << incoming[i].requestId << R"(,"from_user_id":)"
            << incoming[i].otherUserId << R"(,"email":")" << jsonEscapeString(incoming[i].email)
            << R"(","nickname":")" << jsonEscapeString(incoming[i].nickname) << R"(","created_at":)"
            << incoming[i].createdAt << '}';
    }
    oss << R"(],"outgoing":[)";
    for (std::size_t i = 0; i < outgoing.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << R"({"request_id":)" << outgoing[i].requestId << R"(,"to_user_id":)"
            << outgoing[i].otherUserId << R"(,"email":")" << jsonEscapeString(outgoing[i].email)
            << R"(","nickname":")" << jsonEscapeString(outgoing[i].nickname) << R"(","created_at":)"
            << outgoing[i].createdAt << '}';
    }
    oss << "]}";
    return oss.str();
}

std::string buildFriendRequestHandledJson(const std::int64_t requestId, const std::string &action,
                                          const std::int64_t peerUserId)
{
    return std::string(R"({"type":"friend_request_handled","request_id":)") + std::to_string(requestId)
           + R"(,"action":")" + jsonEscapeString(action) + R"(","peer_user_id":)" + std::to_string(peerUserId)
           + "}";
}

std::string buildFriendNotifyRequestAcceptedJson(const std::int64_t requestId, const std::int64_t peerUserId,
                                                 const std::string &emailUtf8, const std::string &nicknameUtf8)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_notify","event":"request_accepted","request_id":)" << requestId
        << R"(,"peer_user_id":)" << peerUserId << R"(,"email":")" << jsonEscapeString(emailUtf8)
        << R"(","nickname":")" << jsonEscapeString(nicknameUtf8) << "\"}";
    return oss.str();
}

std::string buildFriendNotifyPresenceJson(const std::int64_t peerUserId, const bool online)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_notify","event":"presence","peer_user_id":)" << peerUserId << R"(,"online":)"
        << (online ? "true" : "false") << '}';
    return oss.str();
}

std::string buildFriendNotifyNicknameJson(const std::int64_t peerUserId, const std::string &nicknameUtf8)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_notify","event":"nickname","peer_user_id":)" << peerUserId << R"(,"nickname":")"
        << jsonEscapeString(nicknameUtf8) << "\"}";
    return oss.str();
}

std::string buildProfileSetOkJson(const std::string &nicknameUtf8, const std::int64_t corr)
{
    std::ostringstream oss;
    oss << R"({"type":"profile_set_ok","nickname":")" << jsonEscapeString(nicknameUtf8) << R"(","corr":)"
        << corr << '}';
    return oss.str();
}

std::string buildProfileSetAvatarOkJson(const std::int64_t avatarRev, const std::int64_t corr)
{
    std::ostringstream oss;
    oss << R"({"type":"profile_set_avatar_ok","avatar_rev":)" << avatarRev << R"(,"corr":)" << corr << '}';
    return oss.str();
}

std::string buildPeerAvatarOkJson(const std::int64_t peerUserId, const std::int64_t avatarRev,
                                    const std::string &avatarB64Utf8)
{
    std::ostringstream oss;
    oss << R"({"type":"peer_avatar_ok","peer_user_id":)" << peerUserId << R"(,"avatar_rev":)" << avatarRev
        << R"(,"avatar_b64":")" << jsonEscapeString(avatarB64Utf8) << "\"}";
    return oss.str();
}

std::string buildFriendNotifyAvatarJson(const std::int64_t peerUserId, const std::int64_t avatarRev)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_notify","event":"avatar","peer_user_id":)" << peerUserId << R"(,"avatar_rev":)"
        << avatarRev << '}';
    return oss.str();
}

std::string buildFriendListOkJson(const std::vector<FriendListEntry> &friends)
{
    std::ostringstream oss;
    oss << R"({"type":"friend_list_ok","friends":[)";
    for (std::size_t i = 0; i < friends.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << R"({"user_id":)" << friends[i].userId << R"(,"email":")" << jsonEscapeString(friends[i].email)
            << R"(","nickname":")" << jsonEscapeString(friends[i].nickname) << R"(","avatar_rev":)"
            << friends[i].avatarRev << R"(,"created_at":)" << friends[i].createdAt << R"(,"online":)"
            << (friends[i].online ? "true" : "false");
        if (friends[i].lastMessageAt > 0) {
            oss << R"(,"last_message_preview":")" << jsonEscapeString(friends[i].lastMessagePreview)
                << R"(","last_message_at":)" << friends[i].lastMessageAt
                << R"(,"last_message_from_user_id":)" << friends[i].lastMessageFromUserId;
        }
        oss << '}';
    }
    oss << "]}";
    return oss.str();
}

std::string buildFriendDeleteOkJson(const std::int64_t peerUserId)
{
    return std::string(R"({"type":"friend_delete_ok","peer_user_id":)") + std::to_string(peerUserId) + "}";
}

std::string buildMsgSendOkJson(const ChatMessageEntry &e)
{
    std::ostringstream oss;
    oss << R"({"type":"msg_send_ok","message_id":)" << e.messageId << R"(,"from_user_id":)" << e.fromUserId
        << R"(,"to_user_id":)" << e.toUserId << R"(,"content":")" << jsonEscapeString(e.content)
        << R"(","created_at":)" << e.createdAt << '}';
    return oss.str();
}

std::string buildMsgFetchOkJson(const std::int64_t peerUserId, const std::vector<ChatMessageEntry> &messages)
{
    std::ostringstream oss;
    oss << R"({"type":"msg_fetch_ok","peer_user_id":)" << peerUserId << R"(,"messages":[)";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        const auto &e = messages[i];
        oss << R"({"message_id":)" << e.messageId << R"(,"from_user_id":)" << e.fromUserId
            << R"(,"to_user_id":)" << e.toUserId << R"(,"content":")" << jsonEscapeString(e.content)
            << R"(","created_at":)" << e.createdAt << '}';
    }
    oss << "]}";
    return oss.str();
}

std::string buildMsgPushJson(const ChatMessageEntry &e)
{
    std::ostringstream oss;
    oss << R"({"type":"msg_push","message_id":)" << e.messageId << R"(,"from_user_id":)" << e.fromUserId
        << R"(,"to_user_id":)" << e.toUserId << R"(,"content":")" << jsonEscapeString(e.content)
        << R"(","created_at":)" << e.createdAt << '}';
    return oss.str();
}

std::string buildMsgClearOkJson(const std::int64_t peerUserId, const std::int64_t deletedRows)
{
    std::ostringstream oss;
    oss << R"({"type":"msg_clear_ok","peer_user_id":)" << peerUserId << R"(,"deleted_rows":)" << deletedRows << '}';
    return oss.str();
}

std::string buildMsgConvClearedJson(const std::int64_t byUserId)
{
    return std::string(R"({"type":"msg_conv_cleared","by_user_id":)") + std::to_string(byUserId) + '}';
}

std::string buildGroupCreateOkJson(const std::int64_t groupId, const std::string &nameUtf8, const std::int64_t ownerUserId,
                                   const std::int64_t memberCount)
{
    std::ostringstream oss;
    oss << R"({"type":"group_create_ok","group_id":)" << groupId << R"(,"name":")" << jsonEscapeString(nameUtf8)
        << R"(","owner_user_id":)" << ownerUserId << R"(,"member_count":)" << memberCount << '}';
    return oss.str();
}

std::string buildGroupListOkJson(const std::vector<GroupListEntry> &groups)
{
    std::ostringstream oss;
    oss << R"({"type":"group_list_ok","groups":[)";
    for (std::size_t i = 0; i < groups.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        const auto &g = groups[i];
        oss << R"({"group_id":)" << g.groupId << R"(,"name":")" << jsonEscapeString(g.name)
            << R"(","owner_user_id":)" << g.ownerUserId << R"(,"member_count":)" << g.memberCount
            << R"(,"joined_at":)" << g.joinedAt;
        if (g.lastMessageAt > 0) {
            oss << R"(,"last_message_preview":")" << jsonEscapeString(g.lastMessagePreview)
                << R"(","last_message_at":)" << g.lastMessageAt
                << R"(,"last_message_from_user_id":)" << g.lastMessageFromUserId;
            if (!g.lastMessageFromNickname.empty()) {
                oss << R"(,"last_message_from_nickname":")" << jsonEscapeString(g.lastMessageFromNickname) << '"';
            }
        }
        oss << '}';
    }
    oss << "]}";
    return oss.str();
}

std::string buildGroupMembersOkJson(const std::int64_t groupId, const std::vector<GroupMemberEntry> &members)
{
    std::ostringstream oss;
    oss << R"({"type":"group_members_ok","group_id":)" << groupId << R"(,"members":[)";
    for (std::size_t i = 0; i < members.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        const auto &m = members[i];
        oss << R"({"user_id":)" << m.userId << R"(,"email":")" << jsonEscapeString(m.email)
            << R"(","nickname":")" << jsonEscapeString(m.nickname) << R"(","role":")"
            << jsonEscapeString(m.role) << R"(","joined_at":)" << m.joinedAt << '}';
    }
    oss << "]}";
    return oss.str();
}

std::string buildGroupMsgSendOkJson(const GroupChatMessageEntry &e)
{
    std::ostringstream oss;
    oss << R"({"type":"group_msg_send_ok","message_id":)" << e.messageId << R"(,"group_id":)" << e.groupId
        << R"(,"from_user_id":)" << e.fromUserId << R"(,"from_nickname":")" << jsonEscapeString(e.fromNickname)
        << R"(","content":")" << jsonEscapeString(e.content) << R"(","created_at":)" << e.createdAt << '}';
    return oss.str();
}

std::string buildGroupMsgFetchOkJson(const std::int64_t groupId, const std::vector<GroupChatMessageEntry> &messages)
{
    std::ostringstream oss;
    oss << R"({"type":"group_msg_fetch_ok","group_id":)" << groupId << R"(,"messages":[)";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        const auto &e = messages[i];
        oss << R"({"message_id":)" << e.messageId << R"(,"group_id":)" << e.groupId << R"(,"from_user_id":)"
            << e.fromUserId << R"(,"from_nickname":")" << jsonEscapeString(e.fromNickname) << R"(","content":")"
            << jsonEscapeString(e.content) << R"(","created_at":)" << e.createdAt << '}';
    }
    oss << "]}";
    return oss.str();
}

std::string buildGroupMsgPushJson(const GroupChatMessageEntry &e)
{
    std::ostringstream oss;
    oss << R"({"type":"group_msg_push","message_id":)" << e.messageId << R"(,"group_id":)" << e.groupId
        << R"(,"from_user_id":)" << e.fromUserId << R"(,"from_nickname":")" << jsonEscapeString(e.fromNickname)
        << R"(","content":")" << jsonEscapeString(e.content) << R"(","created_at":)" << e.createdAt << '}';
    return oss.str();
}

std::string buildGroupLeaveOkJson(const std::int64_t groupId)
{
    return std::string(R"({"type":"group_leave_ok","group_id":)") + std::to_string(groupId) + '}';
}

std::string buildHelloOkJson(bool fileChunkBinary)
{
    if (!fileChunkBinary) {
        return R"({"type":"hello_ok","version":1})";
    }
    std::ostringstream oss;
    oss << R"({"type":"hello_ok","version":1,"file_chunk_binary":true,"chunk_plain_max_binary":)"
        << kFileChunkBinaryPlainMax << "}";
    return oss.str();
}

std::string buildFileOfferOkJson(const std::int64_t transferId, const std::uint32_t chunkPlainMax,
                                 const std::optional<std::uint32_t> chunkPlainMaxBinary)
{
    std::ostringstream oss;
    oss << R"({"type":"file_offer_ok","transfer_id":)" << transferId << R"(,"chunk_plain_max":)" << chunkPlainMax;
    if (chunkPlainMaxBinary.has_value()) {
        oss << R"(,"file_chunk_binary":true,"chunk_plain_max_binary":)" << *chunkPlainMaxBinary;
    }
    oss << '}';
    return oss.str();
}

namespace {

void appendBe16(std::string &s, std::uint16_t v)
{
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

void appendBe32(std::string &s, std::uint32_t v)
{
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

void appendBe64(std::string &s, std::int64_t v)
{
    const auto u = static_cast<std::uint64_t>(v);
    for (int i = 7; i >= 0; --i) {
        s.push_back(static_cast<char>((u >> (i * 8)) & 0xFF));
    }
}

std::uint16_t readBe16(const unsigned char *p)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | static_cast<std::uint16_t>(p[1]));
}

std::uint32_t readBe32(const unsigned char *p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

std::int64_t readBe64(const unsigned char *p)
{
    const auto u = (static_cast<std::uint64_t>(p[0]) << 56) | (static_cast<std::uint64_t>(p[1]) << 48) |
                     (static_cast<std::uint64_t>(p[2]) << 40) | (static_cast<std::uint64_t>(p[3]) << 32) |
                     (static_cast<std::uint64_t>(p[4]) << 24) | (static_cast<std::uint64_t>(p[5]) << 16) |
                     (static_cast<std::uint64_t>(p[6]) << 8) | static_cast<std::uint64_t>(p[7]);
    return static_cast<std::int64_t>(u);
}

} // namespace

std::string buildLnCbSenderChunkPayload(const std::int64_t transferId, const std::uint32_t seq,
                                        const std::string &tokenHex64, const std::uint8_t *plain,
                                        const std::size_t plainLen)
{
    std::string out;
    out.reserve(kLnCbSenderChunkHeaderSize + plainLen);
    out.append(kLnCbMagic, kLnCbMagic + 4);
    appendBe16(out, kLnCbVersion);
    appendBe64(out, transferId);
    appendBe32(out, seq);
    if (tokenHex64.size() != 64) {
        return {};
    }
    out.append(tokenHex64);
    appendBe32(out, static_cast<std::uint32_t>(plainLen));
    if (plainLen > 0 && plain != nullptr) {
        out.append(reinterpret_cast<const char *>(plain), plainLen);
    }
    return out;
}

std::string buildLnCbChunkPushPayload(const std::int64_t transferId, const std::uint32_t seq, const std::uint8_t *plain,
                                      const std::size_t plainLen)
{
    std::string out;
    out.reserve(kLnCbChunkPushHeaderSize + plainLen);
    out.append(kLnCbMagic, kLnCbMagic + 4);
    appendBe16(out, kLnCbVersion);
    appendBe16(out, kLnCbKindChunkPush);
    appendBe64(out, transferId);
    appendBe32(out, seq);
    appendBe32(out, static_cast<std::uint32_t>(plainLen));
    if (plainLen > 0 && plain != nullptr) {
        out.append(reinterpret_cast<const char *>(plain), plainLen);
    }
    return out;
}

bool parseLnCbSenderChunkPayload(const std::string &payload, LnCbSenderChunkParse &out, std::string &err)
{
    err.clear();
    if (payload.size() < kLnCbSenderChunkHeaderSize) {
        err = "LNCB payload too short";
        return false;
    }
    const auto *p = reinterpret_cast<const unsigned char *>(payload.data());
    if (std::memcmp(p, kLnCbMagic, 4) != 0) {
        err = "LNCB magic mismatch";
        return false;
    }
    if (readBe16(p + 4) != kLnCbVersion) {
        err = "LNCB version unsupported";
        return false;
    }
    out.transferId = readBe64(p + 6);
    out.seq = readBe32(p + 14);
    out.tokenHex64.assign(reinterpret_cast<const char *>(p + 18), 64);
    const std::uint32_t plainLen = readBe32(p + 82);
    if (plainLen == 0 || plainLen > kFileChunkBinaryPlainMax) {
        err = "LNCB plain length invalid";
        return false;
    }
    if (payload.size() != kLnCbSenderChunkHeaderSize + plainLen) {
        err = "LNCB frame size mismatch";
        return false;
    }
    out.plain.resize(plainLen);
    if (plainLen > 0) {
        std::memcpy(out.plain.data(), p + kLnCbSenderChunkHeaderSize, plainLen);
    }
    return true;
}

std::string buildFileOfferDeliveredJson(const std::int64_t transferId)
{
    return std::string(R"({"type":"file_offer_delivered","transfer_id":)") + std::to_string(transferId) + '}';
}

std::string buildFileIncomingJson(const std::int64_t transferId, const std::int64_t fromUserId,
                                  const std::string &fileNameUtf8, const std::uint64_t fileSize,
                                  const std::string &sha256HexLower, const FileVoiceMeta &voiceMeta)
{
    std::ostringstream oss;
    oss << R"({"type":"file_incoming","transfer_id":)" << transferId << R"(,"from_user_id":)" << fromUserId
        << R"(,"file_name":")" << jsonEscapeString(fileNameUtf8) << R"(","file_size":)" << fileSize
        << R"(,"sha256_hex":")" << jsonEscapeString(sha256HexLower) << '"';
    if (voiceMeta.isVoice) {
        oss << R"(,"voice":true,"voice_duration_ms":)" << voiceMeta.durationMs;
        if (!voiceMeta.mimeType.empty()) {
            oss << R"(,"mime_type":")" << jsonEscapeString(voiceMeta.mimeType) << '"';
        }
    }
    oss << '}';
    return oss.str();
}

std::string buildFileSendReadyJson(const std::int64_t transferId)
{
    return std::string(R"({"type":"file_send_ready","transfer_id":)") + std::to_string(transferId) + "}";
}

std::string buildFileChunkPushJson(const std::int64_t transferId, const std::uint32_t seq,
                                   const std::string &dataB64Utf8)
{
    std::ostringstream oss;
    oss << R"({"type":"file_chunk_push","transfer_id":)" << transferId << R"(,"seq":)" << seq << R"(,"data_b64":")"
        << jsonEscapeString(dataB64Utf8) << "\"}";
    return oss.str();
}

std::string buildFileTransferDonePushJson(const std::int64_t transferId, const std::string &sha256HexLower)
{
    std::ostringstream oss;
    oss << R"({"type":"file_transfer_done","transfer_id":)" << transferId << R"(,"sha256_hex":")"
        << jsonEscapeString(sha256HexLower) << "\"}";
    return oss.str();
}

std::string buildFileAbortedJson(const std::int64_t transferId, const std::string &reasonUtf8)
{
    std::ostringstream oss;
    oss << R"({"type":"file_aborted","transfer_id":)" << transferId << R"(,"reason":")"
        << jsonEscapeString(reasonUtf8) << "\"}";
    return oss.str();
}

std::string buildFileRejectOkJson(const std::int64_t transferId)
{
    return std::string(R"({"type":"file_reject_ok","transfer_id":)") + std::to_string(transferId) + "}";
}

std::string buildFileSenderDoneOkJson(const std::int64_t transferId)
{
    return std::string(R"({"type":"file_sender_done_ok","transfer_id":)") + std::to_string(transferId) + "}";
}

std::string buildFileStickerPullOkJson(const std::int64_t transferId, const std::uint64_t fileSizeBytes,
                                       const std::uint32_t chunkPlainMax)
{
    std::ostringstream oss;
    oss << R"({"type":"file_sticker_pull_ok","transfer_id":)" << transferId << R"(,"file_size":)" << fileSizeBytes
        << R"(,"chunk_plain_max":)" << chunkPlainMax << '}';
    return oss.str();
}

std::string buildFileStickerPullChunkJson(const std::int64_t transferId, const std::uint32_t seq,
                                          const std::string &dataB64Utf8)
{
    std::ostringstream oss;
    oss << R"({"type":"file_sticker_pull_chunk","transfer_id":)" << transferId << R"(,"seq":)" << seq
        << R"(,"data_b64":")" << jsonEscapeString(dataB64Utf8) << "\"}";
    return oss.str();
}

std::string buildFileStickerPullDoneJson(const std::int64_t transferId, const std::string &sha256HexLower)
{
    std::ostringstream oss;
    oss << R"({"type":"file_sticker_pull_done","transfer_id":)" << transferId << R"(,"sha256_hex":")"
        << jsonEscapeString(sha256HexLower) << "\"}";
    return oss.str();
}

static bool isRasterImageFileName(const std::string &name)
{
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "bmp" || ext == "webp"
           || ext == "ico" || ext == "heic" || ext == "heif";
}

std::string buildFileChatMessageContentJson(const std::int64_t transferId, const std::string &fileNameUtf8,
                                            const std::int64_t fileSizeBytes, const std::string &sha256HexLower,
                                            const char *stateAscii, const std::string &reasonUtf8, const bool asSticker,
                                            const FileVoiceMeta &voiceMeta)
{
    const char *const kindAscii = isRasterImageFileName(fileNameUtf8) ? "image" : "file";
    std::ostringstream oss;
    oss << "{\"kind\":\"" << kindAscii << R"(","transfer_id":)" << transferId << R"(,"name":")"
        << jsonEscapeString(fileNameUtf8) << R"(","size":)" << fileSizeBytes;
    if (voiceMeta.isVoice) {
        oss << R"(,"voice":true,"voice_duration_ms":)" << voiceMeta.durationMs;
        if (!voiceMeta.mimeType.empty()) {
            oss << R"(,"mime_type":")" << jsonEscapeString(voiceMeta.mimeType) << '"';
        }
    }
    if (std::string_view(stateAscii) == "ok") {
        oss << R"(,"state":"ok","sha256":")" << jsonEscapeString(sha256HexLower) << "\"";
        if (asSticker) {
            oss << R"(,"as_sticker":true)";
        }
        oss << '}';
    } else {
        oss << R"(,"state":"failed","reason":")" << jsonEscapeString(reasonUtf8) << "\"}";
    }
    return oss.str();
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
