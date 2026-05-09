#include "vsserver/message_parse.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Wincrypt.h>

#pragma comment(lib, "crypt32.lib")

#include <regex>
#include <sstream>
#include <string_view>
#include <vector>

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

std::optional<std::int64_t> parseJsonInt64Field(const std::string &jsonUtf8, const char *asciiFieldName)
{
    const std::string pattern =
        std::string(R"re(")re") + asciiFieldName + R"re("\s*:\s*(-?[0-9]+))re";
    const std::regex re(pattern);
    const auto s = matchOne(jsonUtf8, re);
    if (!s) {
        return std::nullopt;
    }
    try {
        return static_cast<std::int64_t>(std::stoll(*s));
    } catch (...) {
        return std::nullopt;
    }
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
    outB64Utf8.clear();
    if (data == nullptr || len == 0) {
        return false;
    }
    DWORD cch = 0;
    if (!::CryptBinaryToStringA(reinterpret_cast<const BYTE *>(data), static_cast<DWORD>(len),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &cch)) {
        return false;
    }
    outB64Utf8.assign(static_cast<std::size_t>(cch), '\0');
    if (!::CryptBinaryToStringA(reinterpret_cast<const BYTE *>(data), static_cast<DWORD>(len),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, reinterpret_cast<LPSTR>(outB64Utf8.data()),
                                &cch)) {
        outB64Utf8.clear();
        return false;
    }
    while (!outB64Utf8.empty() && (outB64Utf8.back() == '\0' || outB64Utf8.back() == '\r' || outB64Utf8.back() == '\n')) {
        outB64Utf8.pop_back();
    }
    return !outB64Utf8.empty();
}

bool wireBase64Decode(const std::string &b64Utf8, std::vector<std::uint8_t> &outBytes)
{
    outBytes.clear();
    if (b64Utf8.empty()) {
        return false;
    }
    DWORD nbytes = 0;
    if (!::CryptStringToBinaryA(b64Utf8.c_str(), static_cast<DWORD>(b64Utf8.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT, nullptr, &nbytes, nullptr, nullptr)) {
        return false;
    }
    outBytes.resize(static_cast<std::size_t>(nbytes));
    if (!::CryptStringToBinaryA(b64Utf8.c_str(), static_cast<DWORD>(b64Utf8.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_STRICT,
                                reinterpret_cast<BYTE *>(outBytes.data()), &nbytes, nullptr, nullptr)) {
        outBytes.clear();
        return false;
    }
    outBytes.resize(static_cast<std::size_t>(nbytes));
    return true;
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

std::string buildFileOfferOkJson(const std::int64_t transferId, const std::uint32_t chunkPlainMax)
{
    std::ostringstream oss;
    oss << R"({"type":"file_offer_ok","transfer_id":)" << transferId << R"(,"chunk_plain_max":)" << chunkPlainMax
        << '}';
    return oss.str();
}

std::string buildFileOfferDeliveredJson(const std::int64_t transferId)
{
    return std::string(R"({"type":"file_offer_delivered","transfer_id":)") + std::to_string(transferId) + '}';
}

std::string buildFileIncomingJson(const std::int64_t transferId, const std::int64_t fromUserId,
                                  const std::string &fileNameUtf8, const std::uint64_t fileSize,
                                  const std::string &sha256HexLower)
{
    std::ostringstream oss;
    oss << R"({"type":"file_incoming","transfer_id":)" << transferId << R"(,"from_user_id":)" << fromUserId
        << R"(,"file_name":")" << jsonEscapeString(fileNameUtf8) << R"(","file_size":)" << fileSize
        << R"(,"sha256_hex":")" << jsonEscapeString(sha256HexLower) << "\"}";
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
                                            const char *stateAscii, const std::string &reasonUtf8, const bool asSticker)
{
    const char *const kindAscii = isRasterImageFileName(fileNameUtf8) ? "image" : "file";
    std::ostringstream oss;
    oss << "{\"kind\":\"" << kindAscii << R"(","transfer_id":)" << transferId << R"(,"name":")"
        << jsonEscapeString(fileNameUtf8) << R"(","size":)" << fileSizeBytes;
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
