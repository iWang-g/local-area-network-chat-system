#include "vsserver/database.hpp"
#include "vsserver/file_relay.hpp"
#include "vsserver/logger.hpp"
#include "vsserver/message_parse.hpp"
#include "vsserver/net_compat.hpp"
#include "vsserver/protocol.hpp"
#include "vsserver/tcp_server.hpp"
#include "vsserver/wire.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace vsserver {

namespace {

std::mutex g_sendAllMutex;
std::mutex g_onlineMutex;
std::unordered_map<std::int64_t, std::vector<SockHandle>> g_onlineByUser;

bool peerHasOnlineSession(const std::int64_t userId)
{
    std::lock_guard<std::mutex> lk(g_onlineMutex);
    const auto it = g_onlineByUser.find(userId);
    return it != g_onlineByUser.end() && !it->second.empty();
}

void pushFrameToUser(std::int64_t userId, const std::string &frame);

bool sendAllUnsafe(SockHandle s, const std::string &data)
{
    const char *p = data.data();
    size_t left = data.size();
    while (left > 0) {
        const int n = ::send(s, p, static_cast<int>(left), 0);
        if (sendFailed(n)) {
            return false;
        }
        left -= static_cast<size_t>(n);
        p += n;
    }
    return true;
}

bool sendAll(SockHandle s, const std::string &data)
{
    std::lock_guard<std::mutex> lk(g_sendAllMutex);
    return sendAllUnsafe(s, data);
}

void notifyFriendsPresenceChanged(const std::int64_t subjectUserId, bool online);
void notifyFriendsNicknameChanged(const std::int64_t subjectUserId, const std::string &nicknameUtf8);

void onlineRegister(const std::int64_t userId, SockHandle sock)
{
    bool becameOnline = false;
    {
        std::lock_guard<std::mutex> lk(g_onlineMutex);
        auto &vec = g_onlineByUser[userId];
        if (std::find(vec.begin(), vec.end(), sock) != vec.end()) {
            return;
        }
        becameOnline = vec.empty();
        vec.push_back(sock);
    }
    if (becameOnline) {
        notifyFriendsPresenceChanged(userId, true);
    }
}

void onlineUnregister(const std::int64_t userId, SockHandle sock)
{
    bool becameOffline = false;
    {
        std::lock_guard<std::mutex> lk(g_onlineMutex);
        const auto it = g_onlineByUser.find(userId);
        if (it == g_onlineByUser.end()) {
            return;
        }
        auto &vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), sock), vec.end());
        if (vec.empty()) {
            becameOffline = true;
            g_onlineByUser.erase(it);
        }
    }
    if (becameOffline) {
        notifyFriendsPresenceChanged(userId, false);
    }
}

void notifyFriendsPresenceChanged(const std::int64_t subjectUserId, const bool online)
{
    std::vector<std::int64_t> peers;
    const AppDatabase::FriendOpOutcome fo = AppDatabase::friendPeerIds(subjectUserId, peers);
    if (!fo.ok) {
        return;
    }
    const std::string frame = encodeFrame(buildFriendNotifyPresenceJson(subjectUserId, online));
    for (const std::int64_t pid : peers) {
        pushFrameToUser(pid, frame);
    }
}

void notifyFriendsNicknameChanged(const std::int64_t subjectUserId, const std::string &nicknameUtf8)
{
    std::vector<std::int64_t> peers;
    const AppDatabase::FriendOpOutcome fo = AppDatabase::friendPeerIds(subjectUserId, peers);
    if (!fo.ok) {
        return;
    }
    const std::string frame = encodeFrame(buildFriendNotifyNicknameJson(subjectUserId, nicknameUtf8));
    for (const std::int64_t pid : peers) {
        pushFrameToUser(pid, frame);
    }
}

void notifyFriendsAvatarChanged(const std::int64_t subjectUserId, const std::int64_t avatarRev)
{
    std::vector<std::int64_t> peers;
    const AppDatabase::FriendOpOutcome fo = AppDatabase::friendPeerIds(subjectUserId, peers);
    if (!fo.ok) {
        return;
    }
    const std::string frame = encodeFrame(buildFriendNotifyAvatarJson(subjectUserId, avatarRev));
    for (const std::int64_t pid : peers) {
        pushFrameToUser(pid, frame);
    }
}

void pushFrameToUser(const std::int64_t userId, const std::string &frame)
{
    std::vector<SockHandle> copy;
    {
        std::lock_guard<std::mutex> lk(g_onlineMutex);
        const auto it = g_onlineByUser.find(userId);
        if (it != g_onlineByUser.end()) {
            copy = it->second;
        }
    }
    for (SockHandle s : copy) {
        if (!sendAll(s, frame)) {
            VSLOG_WARN("msg_push send failed");
        }
    }
}

enum class AuthRc { IoErr, ContinueLoop, Ok };

/// 好友等业务：`hello` 后须有效 `token`。
AuthRc requireFriendAuth(SockHandle client, bool helloOk, const std::string &json, std::int64_t &outUserId)
{
    if (!helloOk) {
        if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedHandshake, "请先完成握手")))) {
            return AuthRc::IoErr;
        }
        return AuthRc::ContinueLoop;
    }
    if (!AppDatabase::isReady()) {
        if (!sendAll(client, encodeFrame(buildErrorJson(kErrDbUnavailable, "数据库不可用")))) {
            return AuthRc::IoErr;
        }
        return AuthRc::ContinueLoop;
    }
    const auto tok = parseJsonStringField(json, "token");
    if (!tok || tok->empty()) {
        if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedToken, "请先登录")))) {
            return AuthRc::IoErr;
        }
        return AuthRc::ContinueLoop;
    }
    if (!AppDatabase::validateToken(*tok, outUserId)) {
        if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedToken, "登录已失效，请重新登录")))) {
            return AuthRc::IoErr;
        }
        return AuthRc::ContinueLoop;
    }
    return AuthRc::Ok;
}

void handleClient(SockHandle client, std::atomic<std::uint64_t> &connId)
{
    const std::uint64_t id = ++connId;
    VSLOG_INFO("[conn " + std::to_string(id) + "] opened");

    FrameAssembler asm_;
    char buf[8192];
    bool helloOk = false;
    std::optional<std::int64_t> authedUser;
    bool connFileChunkBinaryCapable = false;

    for (;;) {
        const int n = ::recv(client, buf, static_cast<int>(sizeof(buf)), 0);
        if (recvWouldStop(n)) {
            break;
        }
        asm_.append(buf, static_cast<size_t>(n));
        std::string json;
        while (asm_.nextFrame(json)) {
            if (json.size() >= 4 && std::memcmp(json.data(), kLnCbMagic, 4) == 0) {
                if (!helloOk) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedHandshake, "请先完成握手")))) {
                        goto end;
                    }
                    continue;
                }
                if (!authedUser.has_value()) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedToken, "请先登录")))) {
                        goto end;
                    }
                    continue;
                }
                LnCbSenderChunkParse p;
                std::string perr;
                if (!parseLnCbSenderChunkPayload(json, p, perr)) {
                    const std::string msg = perr.empty() ? std::string("非法的二进制分片") : perr;
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, msg)))) {
                        goto end;
                    }
                    continue;
                }
                std::int64_t tokUid = 0;
                if (!AppDatabase::validateToken(p.tokenHex64, tokUid) || tokUid != *authedUser) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedToken, "请先登录")))) {
                        goto end;
                    }
                    continue;
                }
                const FileChunkRelayResult cr =
                    fileRelayOnSenderChunkPlain(tokUid, p.transferId, p.seq, p.plain);
                if (!cr.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(cr.errCode, cr.message)))) {
                        goto end;
                    }
                    continue;
                }
                try {
                    if (cr.pushToUserId > 0) {
                        if (!cr.pushBinaryPayload.empty()) {
                            pushFrameToUser(cr.pushToUserId, encodeFrame(cr.pushBinaryPayload));
                        } else if (!cr.pushJsonUtf8.empty()) {
                            pushFrameToUser(cr.pushToUserId, encodeFrame(cr.pushJsonUtf8));
                        }
                    }
                } catch (const std::length_error &) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileChunk, "分片帧过大")))) {
                        goto end;
                    }
                }
                continue;
            }
            const auto t = parseMessageType(json);
            if (!t) {
                VSLOG_WARN("[conn " + std::to_string(id) + "] drop frame (no type)");
                continue;
            }
            if (*t == "hello") {
                std::string err;
                if (validateHello(json, err)) {
                    VSLOG_INFO("[conn " + std::to_string(id) + "] hello ok");
                    helloOk = true;
                    connFileChunkBinaryCapable = json.find("file_chunk_binary_v1") != std::string::npos;
                    if (!sendAll(client, encodeFrame(buildHelloOkJson(connFileChunkBinaryCapable)))) {
                        goto end;
                    }
                } else {
                    VSLOG_WARN("[conn " + std::to_string(id) + "] hello fail: " + err);
                    if (!sendAll(client,
                                  encodeFrame(buildErrorJson(kErrBadHandshake, "bad handshake")))) {
                        goto end;
                    }
                }
            } else if (*t == "heartbeat") {
                if (!helloOk) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedHandshake, "请先完成握手")))) {
                        goto end;
                    }
                } else if (!sendAll(client, encodeFrame(R"({"type":"heartbeat_ack"})"))) {
                    goto end;
                }
            } else if (*t == "auth_login") {
                if (!helloOk) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedHandshake, "请先完成握手")))) {
                        goto end;
                    }
                    continue;
                }
                if (!AppDatabase::isReady()) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrDbUnavailable, "数据库不可用")))) {
                        goto end;
                    }
                    continue;
                }
                const auto password = parseJsonStringField(json, "password");
                const auto usernameLogin = parseJsonStringField(json, "username");
                const auto emailLogin = parseJsonStringField(json, "email");
                if (!password) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 password")))) {
                        goto end;
                    }
                    continue;
                }
                AuthResult r;
                if (usernameLogin && !usernameLogin->empty()) {
                    if (emailLogin && !emailLogin->empty()) {
                        if (!sendAll(client,
                                      encodeFrame(buildErrorJson(kErrInvalidInput, "请勿同时填写 email 与 username")))) {
                            goto end;
                        }
                        continue;
                    }
                    r = AppDatabase::loginByUsername(*usernameLogin, *password);
                } else if (emailLogin && !emailLogin->empty()) {
                    r = AppDatabase::loginByEmail(*emailLogin, *password);
                } else {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 email 或 username")))) {
                        goto end;
                    }
                    continue;
                }
                if (!r.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(static_cast<int>(r.code), r.message)))) {
                        goto end;
                    }
                } else {
                    VSLOG_INFO("[conn " + std::to_string(id) + "] auth_login ok user_id="
                               + std::to_string(r.userId));
                    if (!sendAll(client, encodeFrame(buildAuthOkJson(r.userId, r.token, r.email, r.username,
                                                                     r.nickname, r.avatarRev)))) {
                        goto end;
                    }
                    onlineRegister(r.userId, client);
                    authedUser = r.userId;
                }
            } else if (*t == "req_email_code") {
                if (!helloOk) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedHandshake, "请先完成握手")))) {
                        goto end;
                    }
                    continue;
                }
                if (!AppDatabase::isReady()) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrDbUnavailable, "数据库不可用")))) {
                        goto end;
                    }
                    continue;
                }
                const auto email = parseJsonStringField(json, "email");
                if (!email) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 email")))) {
                        goto end;
                    }
                    continue;
                }
                const auto purposeOpt = parseJsonStringField(json, "purpose");
                const std::string purpose = purposeOpt.value_or("register");
                if (purpose != "register") {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "不支持的 purpose")))) {
                        goto end;
                    }
                    continue;
                }
                const EmailCodeIssueResult er = AppDatabase::issueRegisterEmailCode(*email);
                if (!er.ok) {
                    const int ra = er.retryAfterSec > 0 ? er.retryAfterSec : -1;
                    if (!sendAll(client, encodeFrame(buildErrorJson(er.errCode, er.message, ra)))) {
                        goto end;
                    }
                } else {
                    VSLOG_INFO("[conn " + std::to_string(id) + "] req_email_code ok");
                    if (!sendAll(client, encodeFrame(buildEmailCodeOkJson()))) {
                        goto end;
                    }
                }
            } else if (*t == "auth_register") {
                if (!helloOk) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrNeedHandshake, "请先完成握手")))) {
                        goto end;
                    }
                    continue;
                }
                if (!AppDatabase::isReady()) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrDbUnavailable, "数据库不可用")))) {
                        goto end;
                    }
                    continue;
                }
                const auto email = parseJsonStringField(json, "email");
                const auto password = parseJsonStringField(json, "password");
                const auto username = parseJsonStringField(json, "username");
                const auto emailCode = parseJsonStringField(json, "email_code");
                if (!email || !password) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 email 或 password")))) {
                        goto end;
                    }
                    continue;
                }
                if (!username || username->empty()) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 username")))) {
                        goto end;
                    }
                    continue;
                }
                if (!emailCode) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 email_code")))) {
                        goto end;
                    }
                    continue;
                }
                const AuthResult r = AppDatabase::registerUser(*email, *password, *username, *emailCode);
                if (!r.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(static_cast<int>(r.code), r.message)))) {
                        goto end;
                    }
                } else {
                    VSLOG_INFO("[conn " + std::to_string(id) + "] auth_register ok user_id="
                               + std::to_string(r.userId));
                    if (!sendAll(client, encodeFrame(buildAuthOkJson(r.userId, r.token, r.email, r.username,
                                                                     r.nickname, r.avatarRev)))) {
                        goto end;
                    }
                    onlineRegister(r.userId, client);
                    authedUser = r.userId;
                }
            } else if (*t == "friend_search") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto q = parseJsonStringField(json, "query");
                if (!q || q->empty()) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 query")))) {
                        goto end;
                    }
                    continue;
                }
                int limit = 20;
                if (const auto limOpt = parseJsonInt64Field(json, "limit")) {
                    limit = static_cast<int>(*limOpt);
                }
                std::vector<AppDatabase::FriendSearchHit> hits;
                const AppDatabase::FriendOpOutcome fo = AppDatabase::friendSearch(selfUid, *q, limit, hits);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<FriendPublicUser> pub;
                pub.reserve(hits.size());
                for (const auto &h : hits) {
                    FriendPublicUser u;
                    u.userId = h.userId;
                    u.email = h.email;
                    u.nickname = h.nickname;
                    pub.push_back(std::move(u));
                }
                if (!sendAll(client, encodeFrame(buildFriendSearchResultJson(pub)))) {
                    goto end;
                }
            } else if (*t == "friend_request_send") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const std::int64_t tid = parseJsonInt64Field(json, "target_user_id").value_or(0);
                const auto te = parseJsonStringField(json, "target_email");
                std::int64_t reqId = 0;
                std::int64_t targetResolved = 0;
                const AppDatabase::FriendOpOutcome fo =
                    AppDatabase::friendRequestSend(selfUid, tid, te.value_or(std::string()), reqId, targetResolved);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildFriendRequestSentJson(reqId, targetResolved)))) {
                    goto end;
                }
            } else if (*t == "friend_request_list") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                std::vector<AppDatabase::FriendPendingRow> in;
                std::vector<AppDatabase::FriendPendingRow> out;
                const AppDatabase::FriendOpOutcome fo = AppDatabase::friendRequestList(selfUid, in, out);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<FriendPendingEntry> vi;
                std::vector<FriendPendingEntry> vo;
                vi.reserve(in.size());
                for (const auto &r : in) {
                    FriendPendingEntry e;
                    e.requestId = r.requestId;
                    e.otherUserId = r.otherUserId;
                    e.email = r.email;
                    e.nickname = r.nickname;
                    e.createdAt = r.createdAt;
                    vi.push_back(std::move(e));
                }
                vo.reserve(out.size());
                for (const auto &r : out) {
                    FriendPendingEntry e;
                    e.requestId = r.requestId;
                    e.otherUserId = r.otherUserId;
                    e.email = r.email;
                    e.nickname = r.nickname;
                    e.createdAt = r.createdAt;
                    vo.push_back(std::move(e));
                }
                if (!sendAll(client, encodeFrame(buildFriendRequestListOkJson(vi, vo)))) {
                    goto end;
                }
            } else if (*t == "friend_request_handle") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto ridOpt = parseJsonInt64Field(json, "request_id");
                const auto act = parseJsonStringField(json, "action");
                if (!ridOpt || !act) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 request_id 或 action")))) {
                        goto end;
                    }
                    continue;
                }
                std::int64_t peer = 0;
                const AppDatabase::FriendOpOutcome fo =
                    AppDatabase::friendRequestHandle(selfUid, *ridOpt, *act, peer);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildFriendRequestHandledJson(*ridOpt, *act, peer)))) {
                    goto end;
                }
                if (*act == "accept" && peer > 0) {
                    std::string acEm;
                    std::string acNk;
                    (void)AppDatabase::tryGetUserPublic(selfUid, acEm, acNk);
                    pushFrameToUser(peer,
                                     encodeFrame(buildFriendNotifyRequestAcceptedJson(*ridOpt, selfUid, acEm, acNk)));
                }
            } else if (*t == "friend_list") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                std::vector<AppDatabase::FriendListRow> rows;
                const AppDatabase::FriendOpOutcome fo = AppDatabase::friendList(selfUid, rows);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<FriendListEntry> ents;
                ents.reserve(rows.size());
                for (const auto &r : rows) {
                    FriendListEntry e;
                    e.userId = r.userId;
                    e.email = r.email;
                    e.nickname = r.nickname;
                    e.avatarRev = r.avatarRev;
                    e.createdAt = r.createdAt;
                    e.lastMessagePreview = r.lastMessageContent;
                    e.lastMessageAt = r.lastMessageAt;
                    e.lastMessageFromUserId = r.lastMessageFromUserId;
                    e.online = peerHasOnlineSession(r.userId);
                    ents.push_back(std::move(e));
                }
                if (!sendAll(client, encodeFrame(buildFriendListOkJson(ents)))) {
                    goto end;
                }
            } else if (*t == "friend_delete") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto peerOpt = parseJsonInt64Field(json, "peer_user_id");
                if (!peerOpt) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 peer_user_id")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::FriendOpOutcome fo = AppDatabase::friendDelete(selfUid, *peerOpt);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildFriendDeleteOkJson(*peerOpt)))) {
                    goto end;
                }
            } else if (*t == "profile_set") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const std::int64_t corr = parseJsonInt64Field(json, "corr").value_or(-1);
                if (corr < 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少或无效的 corr", -1, -1)))) {
                        goto end;
                    }
                    continue;
                }
                const auto nickOpt = parseJsonStringField(json, "nickname");
                if (!nickOpt) {
                    if (!sendAll(client,
                                 encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 nickname", -1, corr)))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::FriendOpOutcome fo = AppDatabase::setNickname(selfUid, *nickOpt);
                if (!fo.ok) {
                    if (!sendAll(client,
                                 encodeFrame(buildErrorJson(fo.errCode, fo.message, -1, corr)))) {
                        goto end;
                    }
                    continue;
                }
                std::string pubEm;
                std::string pubNk;
                if (!AppDatabase::tryGetUserPublic(selfUid, pubEm, pubNk)) {
                    if (!sendAll(client,
                                 encodeFrame(buildErrorJson(kErrUserNotFound, "用户不存在", -1, corr)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildProfileSetOkJson(pubNk, corr)))) {
                    goto end;
                }
                notifyFriendsNicknameChanged(selfUid, pubNk);
            } else if (*t == "profile_set_avatar") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const std::int64_t corr = parseJsonInt64Field(json, "corr").value_or(-1);
                if (corr < 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少或无效的 corr", -1, -1)))) {
                        goto end;
                    }
                    continue;
                }
                const auto b64Opt = parseJsonStringField(json, "avatar_b64");
                if (!b64Opt || b64Opt->empty()) {
                    if (!sendAll(client,
                                 encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 avatar_b64", -1, corr)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<std::uint8_t> raw;
                if (!wireBase64Decode(*b64Opt, raw)) {
                    if (!sendAll(client,
                                 encodeFrame(buildErrorJson(kErrInvalidInput, "avatar_b64 解码失败", -1, corr)))) {
                        goto end;
                    }
                    continue;
                }
                std::int64_t newRev = 0;
                const AppDatabase::FriendOpOutcome fo = AppDatabase::setUserAvatarJpeg(selfUid, raw, newRev);
                if (!fo.ok) {
                    if (!sendAll(client,
                                 encodeFrame(buildErrorJson(fo.errCode, fo.message, -1, corr)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildProfileSetAvatarOkJson(newRev, corr)))) {
                    goto end;
                }
                notifyFriendsAvatarChanged(selfUid, newRev);
            } else if (*t == "peer_avatar") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto peerOpt = parseJsonInt64Field(json, "peer_user_id");
                if (!peerOpt || *peerOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 peer_user_id")))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<std::uint8_t> jpg;
                std::int64_t rev = 0;
                const AppDatabase::FriendOpOutcome fo =
                    AppDatabase::getFriendAvatarJpeg(selfUid, *peerOpt, jpg, rev);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::string b64;
                if (!jpg.empty()) {
                    if (!wireBase64Encode(jpg.data(), jpg.size(), b64)) {
                        if (!sendAll(client, encodeFrame(buildErrorJson(kErrDbUnavailable, "头像编码失败")))) {
                            goto end;
                        }
                        continue;
                    }
                }
                const std::string payload = buildPeerAvatarOkJson(*peerOpt, rev, b64);
                if (payload.size() > kMaxFramePayload) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrProfileAvatar, "头像数据过大")))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(payload))) {
                    goto end;
                }
            } else if (*t == "msg_send") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto peerOpt = parseJsonInt64Field(json, "peer_user_id");
                const auto textOpt = parseJsonStringField(json, "text");
                if (!peerOpt || *peerOpt <= 0 || !textOpt) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 peer_user_id 或 text")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::MsgOpOutcome mo =
                    AppDatabase::messageSend(selfUid, *peerOpt, *textOpt);
                if (!mo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(mo.errCode, mo.message)))) {
                        goto end;
                    }
                    continue;
                }
                ChatMessageEntry e;
                e.messageId = mo.messageId;
                e.fromUserId = selfUid;
                e.toUserId = *peerOpt;
                e.content = *textOpt;
                e.createdAt = mo.createdAt;
                VSLOG_INFO("[conn " + std::to_string(id) + "] msg_send id=" + std::to_string(e.messageId));
                if (!sendAll(client, encodeFrame(buildMsgSendOkJson(e)))) {
                    goto end;
                }
                pushFrameToUser(*peerOpt, encodeFrame(buildMsgPushJson(e)));
            } else if (*t == "msg_fetch") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto peerOpt = parseJsonInt64Field(json, "peer_user_id");
                if (!peerOpt || *peerOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 peer_user_id")))) {
                        goto end;
                    }
                    continue;
                }
                const std::int64_t afterId = parseJsonInt64Field(json, "after_id").value_or(0);
                const std::int64_t beforeId = parseJsonInt64Field(json, "before_id").value_or(0);
                int limit = static_cast<int>(parseJsonInt64Field(json, "limit").value_or(50));
                std::vector<AppDatabase::ChatMessageRow> rows;
                const AppDatabase::MsgOpOutcome mo =
                    AppDatabase::messageFetch(selfUid, *peerOpt, afterId, beforeId, limit, rows);
                if (!mo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(mo.errCode, mo.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<ChatMessageEntry> ents;
                ents.reserve(rows.size());
                for (const auto &r : rows) {
                    ChatMessageEntry e;
                    e.messageId = r.messageId;
                    e.fromUserId = r.fromUserId;
                    e.toUserId = r.toUserId;
                    e.content = r.content;
                    e.createdAt = r.createdAt;
                    ents.push_back(std::move(e));
                }
                if (!sendAll(client, encodeFrame(buildMsgFetchOkJson(*peerOpt, ents)))) {
                    goto end;
                }
            } else if (*t == "msg_delete") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto midOpt = parseJsonInt64Field(json, "message_id");
                if (!midOpt || *midOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 message_id")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::MsgOpOutcome mo = AppDatabase::messageHideForUser(selfUid, *midOpt);
                if (!mo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(mo.errCode, mo.message)))) {
                        goto end;
                    }
                    continue;
                }
                VSLOG_INFO("[conn " + std::to_string(id) + "] msg_delete message_id=" + std::to_string(*midOpt));
                if (!sendAll(client, encodeFrame(buildMsgDeleteOkJson(*midOpt)))) {
                    goto end;
                }
            } else if (*t == "msg_clear") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto peerOpt = parseJsonInt64Field(json, "peer_user_id");
                if (!peerOpt || *peerOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 peer_user_id")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::MsgOpOutcome mo = AppDatabase::messageClearConversation(selfUid, *peerOpt);
                if (!mo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(mo.errCode, mo.message)))) {
                        goto end;
                    }
                    continue;
                }
                VSLOG_INFO("[conn " + std::to_string(id) + "] msg_clear peer=" + std::to_string(*peerOpt)
                           + " deleted=" + std::to_string(mo.clearedRows));
                if (!sendAll(client, encodeFrame(buildMsgClearOkJson(*peerOpt, mo.clearedRows)))) {
                    goto end;
                }
                pushFrameToUser(*peerOpt, encodeFrame(buildMsgConvClearedJson(selfUid)));
            } else if (*t == "group_create") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto nameOpt = parseJsonStringField(json, "name");
                const auto memberIdsOpt = parseJsonInt64ArrayField(json, "member_user_ids");
                if (!nameOpt) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 name")))) {
                        goto end;
                    }
                    continue;
                }
                const std::vector<std::int64_t> memberIds = memberIdsOpt.value_or(std::vector<std::int64_t>());
                const AppDatabase::GroupOpOutcome go = AppDatabase::groupCreate(selfUid, *nameOpt, memberIds);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<AppDatabase::GroupMemberRow> members;
                (void)AppDatabase::groupMembers(selfUid, go.groupId, members);
                if (!sendAll(client, encodeFrame(buildGroupCreateOkJson(go.groupId, *nameOpt, selfUid,
                                                                        static_cast<std::int64_t>(members.size()))))) {
                    goto end;
                }
            } else if (*t == "group_list") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                std::vector<AppDatabase::GroupListRow> rows;
                const AppDatabase::GroupOpOutcome go = AppDatabase::groupList(selfUid, rows);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<GroupListEntry> ents;
                ents.reserve(rows.size());
                for (const auto &r : rows) {
                    GroupListEntry e;
                    e.groupId = r.groupId;
                    e.name = r.name;
                    e.ownerUserId = r.ownerUserId;
                    e.memberCount = r.memberCount;
                    e.joinedAt = r.joinedAt;
                    e.lastMessagePreview = r.lastMessageContent;
                    e.lastMessageAt = r.lastMessageAt;
                    e.lastMessageFromUserId = r.lastMessageFromUserId;
                    e.lastMessageFromNickname = r.lastMessageFromNickname;
                    ents.push_back(std::move(e));
                }
                if (!sendAll(client, encodeFrame(buildGroupListOkJson(ents)))) {
                    goto end;
                }
            } else if (*t == "group_members") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto groupOpt = parseJsonInt64Field(json, "group_id");
                if (!groupOpt || *groupOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 group_id")))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<AppDatabase::GroupMemberRow> rows;
                const AppDatabase::GroupOpOutcome go = AppDatabase::groupMembers(selfUid, *groupOpt, rows);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<GroupMemberEntry> ents;
                ents.reserve(rows.size());
                for (const auto &r : rows) {
                    GroupMemberEntry e;
                    e.userId = r.userId;
                    e.email = r.email;
                    e.nickname = r.nickname;
                    e.role = r.role;
                    e.joinedAt = r.joinedAt;
                    ents.push_back(std::move(e));
                }
                if (!sendAll(client, encodeFrame(buildGroupMembersOkJson(*groupOpt, ents)))) {
                    goto end;
                }
            } else if (*t == "group_msg_send") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto groupOpt = parseJsonInt64Field(json, "group_id");
                const auto textOpt = parseJsonStringField(json, "text");
                if (!groupOpt || *groupOpt <= 0 || !textOpt) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 group_id 或 text")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::GroupOpOutcome go = AppDatabase::groupMessageSend(selfUid, *groupOpt, *textOpt);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::string fromEmail;
                std::string fromNickname;
                (void)AppDatabase::tryGetUserPublic(selfUid, fromEmail, fromNickname);
                GroupChatMessageEntry e;
                e.messageId = go.messageId;
                e.groupId = *groupOpt;
                e.fromUserId = selfUid;
                e.fromNickname = fromNickname;
                e.content = *textOpt;
                e.createdAt = go.createdAt;
                if (!sendAll(client, encodeFrame(buildGroupMsgSendOkJson(e)))) {
                    goto end;
                }
                std::vector<std::int64_t> members;
                const AppDatabase::GroupOpOutcome memGo = AppDatabase::groupMemberIds(selfUid, *groupOpt, members);
                if (memGo.ok) {
                    const std::string frame = encodeFrame(buildGroupMsgPushJson(e));
                    for (const std::int64_t uid : members) {
                        if (uid > 0 && uid != selfUid) {
                            pushFrameToUser(uid, frame);
                        }
                    }
                }
            } else if (*t == "group_msg_fetch") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto groupOpt = parseJsonInt64Field(json, "group_id");
                if (!groupOpt || *groupOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 group_id")))) {
                        goto end;
                    }
                    continue;
                }
                const std::int64_t afterId = parseJsonInt64Field(json, "after_id").value_or(0);
                const std::int64_t beforeId = parseJsonInt64Field(json, "before_id").value_or(0);
                int limit = static_cast<int>(parseJsonInt64Field(json, "limit").value_or(50));
                std::vector<AppDatabase::GroupChatMessageRow> rows;
                const AppDatabase::GroupOpOutcome go =
                    AppDatabase::groupMessageFetch(selfUid, *groupOpt, afterId, beforeId, limit, rows);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                std::vector<GroupChatMessageEntry> ents;
                ents.reserve(rows.size());
                for (const auto &r : rows) {
                    GroupChatMessageEntry e;
                    e.messageId = r.messageId;
                    e.groupId = r.groupId;
                    e.fromUserId = r.fromUserId;
                    e.fromNickname = r.fromNickname;
                    e.content = r.content;
                    e.createdAt = r.createdAt;
                    ents.push_back(std::move(e));
                }
                if (!sendAll(client, encodeFrame(buildGroupMsgFetchOkJson(*groupOpt, ents)))) {
                    goto end;
                }
            } else if (*t == "group_msg_delete") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto groupOpt = parseJsonInt64Field(json, "group_id");
                const auto midOpt = parseJsonInt64Field(json, "message_id");
                if (!groupOpt || *groupOpt <= 0 || !midOpt || *midOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 group_id 或 message_id")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::GroupOpOutcome go =
                    AppDatabase::groupMessageHideForUser(selfUid, *groupOpt, *midOpt);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                VSLOG_INFO("[conn " + std::to_string(id) + "] group_msg_delete group=" + std::to_string(*groupOpt)
                           + " message_id=" + std::to_string(*midOpt));
                if (!sendAll(client, encodeFrame(buildGroupMsgDeleteOkJson(*groupOpt, *midOpt)))) {
                    goto end;
                }
            } else if (*t == "group_leave") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto groupOpt = parseJsonInt64Field(json, "group_id");
                if (!groupOpt || *groupOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 group_id")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::GroupOpOutcome go = AppDatabase::groupLeave(selfUid, *groupOpt);
                if (!go.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(go.errCode, go.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildGroupLeaveOkJson(*groupOpt)))) {
                    goto end;
                }
            } else if (*t == "file_offer") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto peerOpt = parseJsonInt64Field(json, "peer_user_id");
                const auto fnOpt = parseJsonStringField(json, "file_name");
                const auto szOpt = parseJsonInt64Field(json, "file_size");
                if (!peerOpt || *peerOpt <= 0 || !fnOpt || fnOpt->empty() || !szOpt || *szOpt <= 0) {
                    if (!sendAll(client,
                                  encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 peer_user_id/file_name/file_size")))) {
                        goto end;
                    }
                    continue;
                }
                std::string sha = parseJsonStringField(json, "sha256_hex").value_or(std::string());
                for (char &c : sha) {
                    if (c >= 'A' && c <= 'Z') {
                        c = static_cast<char>(c - 'A' + 'a');
                    }
                }
                FileVoiceMeta voiceMeta;
                voiceMeta.isVoice = (json.find("\"voice\":true") != std::string::npos);
                if (voiceMeta.isVoice) {
                    voiceMeta.durationMs = parseJsonInt64Field(json, "voice_duration_ms").value_or(0);
                    voiceMeta.mimeType = parseJsonStringField(json, "mime_type").value_or(std::string());
                }
                const std::uint64_t fsz = static_cast<std::uint64_t>(*szOpt);
                const bool asSticker = (json.find("\"as_sticker\":true") != std::string::npos);
                const std::optional<std::uint32_t> offerChunkBinaryMax =
                    connFileChunkBinaryCapable ? std::make_optional(kFileChunkBinaryPlainMax) : std::nullopt;
                if (!peerHasOnlineSession(*peerOpt)) {
                    if (fsz > 0 && fsz <= kFileTransferMaxBytes) {
                        const FileOfferResult fo = fileRelayOfferServerBufferPeerOffline(
                            selfUid, *peerOpt, *fnOpt, fsz, sha, asSticker, voiceMeta, offerChunkBinaryMax);
                        if (!fo.ok) {
                            if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                                goto end;
                            }
                            continue;
                        }
                        if (!sendAll(client, encodeFrame(fo.jsonOfferOkUtf8))) {
                            goto end;
                        }
                        if (!sendAll(client, encodeFrame(buildFileSendReadyJson(fo.transferId)))) {
                            goto end;
                        }
                        pushFrameToUser(selfUid, encodeFrame(buildFileOfferDeliveredJson(fo.transferId)));
                        continue;
                    }
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrPeerOffline, std::string("对方已离线"))))) {
                        goto end;
                    }
                    continue;
                }
                const FileOfferResult fo =
                    fileRelayOffer(selfUid, *peerOpt, *fnOpt, fsz, std::move(sha), asSticker, voiceMeta,
                                   offerChunkBinaryMax);
                if (!fo.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fo.errCode, fo.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(fo.jsonOfferOkUtf8))) {
                    goto end;
                }
                pushFrameToUser(*peerOpt, encodeFrame(fo.jsonIncomingUtf8));
                pushFrameToUser(selfUid, encodeFrame(buildFileOfferDeliveredJson(fo.transferId)));
            } else if (*t == "file_accept") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto tidOpt = parseJsonInt64Field(json, "transfer_id");
                if (!tidOpt || *tidOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 transfer_id")))) {
                        goto end;
                    }
                    continue;
                }
                const FileNotifyResult na = fileRelayAccept(selfUid, *tidOpt);
                if (!na.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(na.errCode, na.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(R"({"type":"file_accept_ok"})"))) {
                    goto end;
                }
                pushFrameToUser(na.notifyUserId, encodeFrame(na.notifyJsonUtf8));
            } else if (*t == "file_reject") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto tidOpt = parseJsonInt64Field(json, "transfer_id");
                if (!tidOpt || *tidOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 transfer_id")))) {
                        goto end;
                    }
                    continue;
                }
                const FileNotifyResult nr = fileRelayReject(selfUid, *tidOpt);
                if (!nr.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(nr.errCode, nr.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildFileRejectOkJson(*tidOpt)))) {
                    goto end;
                }
                pushFrameToUser(nr.notifyUserId, encodeFrame(nr.notifyJsonUtf8));
                if (nr.hasChatMsg) {
                    pushFrameToUser(nr.chatMsg.fromUserId, encodeFrame(buildMsgPushJson(nr.chatMsg)));
                    pushFrameToUser(nr.chatMsg.toUserId, encodeFrame(buildMsgPushJson(nr.chatMsg)));
                }
            } else if (*t == "file_chunk") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto tidOpt = parseJsonInt64Field(json, "transfer_id");
                const auto seqOpt = parseJsonInt64Field(json, "seq");
                const auto dataOpt = parseJsonStringField(json, "data_b64");
                if (!tidOpt || *tidOpt <= 0 || !seqOpt || *seqOpt < 0 || !dataOpt) {
                    if (!sendAll(client,
                                  encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 transfer_id/seq/data_b64")))) {
                        goto end;
                    }
                    continue;
                }
                if (dataOpt->size() > 120000) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileChunk, "分片过大")))) {
                        goto end;
                    }
                    continue;
                }
                const std::uint32_t seq32 = static_cast<std::uint32_t>(*seqOpt);
                const FileChunkRelayResult cr =
                    fileRelayOnSenderChunk(selfUid, *tidOpt, seq32, *dataOpt);
                if (!cr.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(cr.errCode, cr.message)))) {
                        goto end;
                    }
                    continue;
                }
                try {
                    if (cr.pushToUserId > 0) {
                        if (!cr.pushBinaryPayload.empty()) {
                            pushFrameToUser(cr.pushToUserId, encodeFrame(cr.pushBinaryPayload));
                        } else if (!cr.pushJsonUtf8.empty()) {
                            pushFrameToUser(cr.pushToUserId, encodeFrame(cr.pushJsonUtf8));
                        }
                    }
                } catch (const std::length_error &) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileChunk, "分片帧过大")))) {
                        goto end;
                    }
                    continue;
                }
            } else if (*t == "file_sender_done") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto tidOpt = parseJsonInt64Field(json, "transfer_id");
                if (!tidOpt || *tidOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 transfer_id")))) {
                        goto end;
                    }
                    continue;
                }
                const FileDoneRelayResult fd = fileRelayOnSenderDone(selfUid, *tidOpt);
                if (!fd.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(fd.errCode, fd.message)))) {
                        goto end;
                    }
                    continue;
                }
                if (!sendAll(client, encodeFrame(buildFileSenderDoneOkJson(*tidOpt)))) {
                    goto end;
                }
                try {
                    pushFrameToUser(fd.pushToUserId, encodeFrame(fd.pushDoneJsonUtf8));
                    if (fd.hasChatMsg) {
                        pushFrameToUser(fd.chatMsg.fromUserId, encodeFrame(buildMsgPushJson(fd.chatMsg)));
                        pushFrameToUser(fd.chatMsg.toUserId, encodeFrame(buildMsgPushJson(fd.chatMsg)));
                    }
                } catch (const std::length_error &) {
                    VSLOG_WARN("file done push frame too large");
                }
            } else if (*t == "file_sticker_pull") {
                std::int64_t selfUid = 0;
                const AuthRc ar = requireFriendAuth(client, helloOk, json, selfUid);
                if (ar == AuthRc::IoErr) {
                    goto end;
                }
                if (ar == AuthRc::ContinueLoop) {
                    continue;
                }
                const auto tidOpt = parseJsonInt64Field(json, "transfer_id");
                if (!tidOpt || *tidOpt <= 0) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 transfer_id")))) {
                        goto end;
                    }
                    continue;
                }
                const AppDatabase::FileTransferLookupRow row =
                    AppDatabase::fileTransferLookupParticipant(*tidOpt, selfUid);
                if (!row.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileNotFound, "传输不存在或无权访问")))) {
                        goto end;
                    }
                    continue;
                }
                /// 优先按磁盘判断：`sticker_cache` 或写满的 `offline_partial/*.part`；大小须与 DB 声明一致。
                const std::string artPath =
                    fileRelayServerBufferArtifactPathIfExists(*tidOpt, row.fileNameUtf8, row.fileSizeBytes);
                std::ifstream in;
                std::uint64_t onDisk = 0;
                if (!artPath.empty()) {
                    in.open(artPath, std::ios::binary | std::ios::ate);
                    if (in) {
                        const auto endPos = in.tellg();
                        if (endPos > 0) {
                            onDisk = static_cast<std::uint64_t>(endPos);
                        }
                    }
                }
                const bool diskMatchesDeclared =
                    in.is_open() && onDisk > 0 && row.fileSizeBytes > 0 &&
                    onDisk == static_cast<std::uint64_t>(row.fileSizeBytes);
                if (!diskMatchesDeclared) {
                    if (row.statusUtf8 != "completed") {
                        if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileNotFound, "表情尚未就绪")))) {
                            goto end;
                        }
                    } else if (artPath.empty()) {
                        if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileNotFound, "无服务端文件缓存")))) {
                            goto end;
                        }
                    } else {
                        if (!sendAll(client, encodeFrame(buildErrorJson(kErrFileSizeMismatch, "表情大小不一致")))) {
                            goto end;
                        }
                    }
                    continue;
                }
                in.seekg(0, std::ios::beg);
                if (!in) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrDbUnavailable, "无法读取表情文件")))) {
                        goto end;
                    }
                    continue;
                }
                constexpr std::uint32_t kPullPlain = 49152;
                if (!sendAll(client, encodeFrame(buildFileStickerPullOkJson(*tidOpt, onDisk, kPullPlain)))) {
                    goto end;
                }
                std::vector<char> buf(static_cast<std::size_t>(kPullPlain));
                std::uint32_t seq = 0;
                bool stickerStreamOk = true;
                while (in && stickerStreamOk) {
                    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                    const std::streamsize got = in.gcount();
                    if (got <= 0) {
                        break;
                    }
                    std::string b64;
                    if (!wireBase64Encode(reinterpret_cast<const std::uint8_t *>(buf.data()),
                                          static_cast<std::size_t>(got), b64)) {
                        (void)sendAll(client, encodeFrame(buildErrorJson(kErrFileChunk, "编码失败")));
                        stickerStreamOk = false;
                        break;
                    }
                    try {
                        if (!sendAll(client,
                                      encodeFrame(buildFileStickerPullChunkJson(*tidOpt, seq, std::move(b64))))) {
                            goto end;
                        }
                    } catch (const std::length_error &) {
                        (void)sendAll(client, encodeFrame(buildErrorJson(kErrFileChunk, "分片帧过大")));
                        stickerStreamOk = false;
                        break;
                    }
                    ++seq;
                }
                if (stickerStreamOk) {
                    if (!sendAll(client, encodeFrame(buildFileStickerPullDoneJson(*tidOpt, row.sha256HexLower)))) {
                        goto end;
                    }
                }
            } else {
                VSLOG_INFO("[conn " + std::to_string(id) + "] unknown type: " + *t);
            }
        }
    }
end:
    if (authedUser) {
        const std::int64_t uidOff = *authedUser;
        onlineUnregister(uidOff, client);
        for (const auto &p : fileRelayOnUserDisconnect(uidOff)) {
            if (p.notifyUserId > 0 && !p.fileAbortedJson.empty()) {
                pushFrameToUser(p.notifyUserId, encodeFrame(p.fileAbortedJson));
            }
            if (p.hasChatMsg) {
                pushFrameToUser(p.chatMsg.fromUserId, encodeFrame(buildMsgPushJson(p.chatMsg)));
                pushFrameToUser(p.chatMsg.toUserId, encodeFrame(buildMsgPushJson(p.chatMsg)));
            }
        }
    }
    sockClose(client);
    VSLOG_INFO("[conn " + std::to_string(id) + "] closed");
}

} // namespace

int runTcpServer(std::uint16_t port)
{
    if (!sockStartup()) {
#ifdef _WIN32
        VSLOG_ERROR("WSAStartup failed");
#else
        VSLOG_ERROR("socket subsystem init failed");
#endif
        return 1;
    }

    SockHandle listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockIsInvalid(listenSock)) {
        VSLOG_ERROR("socket failed");
        sockCleanup();
        return 1;
    }

    int opt = 1;
    ::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        VSLOG_ERROR("bind failed (port in use?)");
        sockClose(listenSock);
        sockCleanup();
        return 1;
    }
    if (::listen(listenSock, SOMAXCONN) != 0) {
        VSLOG_ERROR("listen failed");
        sockClose(listenSock);
        sockCleanup();
        return 1;
    }

    VSLOG_INFO("lan-chat-server listening on 0.0.0.0:" + std::to_string(port)
               + " (Ctrl+C to stop — close window to exit)");

    std::atomic<std::uint64_t> connCounter{0};

    for (;;) {
        SockHandle client = ::accept(listenSock, nullptr, nullptr);
        if (sockIsInvalid(client)) {
            VSLOG_ERROR("accept failed");
            break;
        }
        std::thread(handleClient, client, std::ref(connCounter)).detach();
    }

    sockClose(listenSock);
    sockCleanup();
    return 0;
}

} // namespace vsserver
