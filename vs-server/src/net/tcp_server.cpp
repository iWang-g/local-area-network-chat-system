#include "vsserver/database.hpp"
#include "vsserver/logger.hpp"
#include "vsserver/message_parse.hpp"
#include "vsserver/protocol.hpp"
#include "vsserver/tcp_server.hpp"
#include "vsserver/wire.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>

#pragma comment(lib, "Ws2_32.lib")

namespace vsserver {

namespace {

bool sendAll(SOCKET s, const std::string &data)
{
    const char *p = data.data();
    size_t left = data.size();
    while (left > 0) {
        const int n = ::send(s, p, static_cast<int>(left), 0);
        if (n == SOCKET_ERROR || n == 0) {
            return false;
        }
        left -= static_cast<size_t>(n);
        p += n;
    }
    return true;
}

void handleClient(SOCKET client, std::atomic<std::uint64_t> &connId)
{
    const std::uint64_t id = ++connId;
    VSLOG_INFO("[conn " + std::to_string(id) + "] opened");

    FrameAssembler asm_;
    char buf[8192];
    bool helloOk = false;

    for (;;) {
        const int n = ::recv(client, buf, static_cast<int>(sizeof(buf)), 0);
        if (n == SOCKET_ERROR || n == 0) {
            break;
        }
        asm_.append(buf, static_cast<size_t>(n));
        std::string json;
        while (asm_.nextFrame(json)) {
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
                    if (!sendAll(client, encodeFrame(R"({"type":"hello_ok","version":1})"))) {
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
                const auto email = parseJsonStringField(json, "email");
                const auto password = parseJsonStringField(json, "password");
                if (!email || !password) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 email 或 password")))) {
                        goto end;
                    }
                    continue;
                }
                const AuthResult r = AppDatabase::login(*email, *password);
                if (!r.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(static_cast<int>(r.code), r.message)))) {
                        goto end;
                    }
                } else {
                    VSLOG_INFO("[conn " + std::to_string(id) + "] auth_login ok user_id="
                               + std::to_string(r.userId));
                    if (!sendAll(client, encodeFrame(buildAuthOkJson(r.userId, r.token, r.email)))) {
                        goto end;
                    }
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
                const auto nickname = parseJsonStringField(json, "nickname");
                const auto emailCode = parseJsonStringField(json, "email_code");
                if (!email || !password) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(kErrInvalidInput, "缺少 email 或 password")))) {
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
                const AuthResult r =
                    AppDatabase::registerUser(*email, *password, nickname.value_or(std::string()), *emailCode);
                if (!r.ok) {
                    if (!sendAll(client, encodeFrame(buildErrorJson(static_cast<int>(r.code), r.message)))) {
                        goto end;
                    }
                } else {
                    VSLOG_INFO("[conn " + std::to_string(id) + "] auth_register ok user_id="
                               + std::to_string(r.userId));
                    if (!sendAll(client, encodeFrame(buildAuthOkJson(r.userId, r.token, r.email)))) {
                        goto end;
                    }
                }
            } else {
                VSLOG_INFO("[conn " + std::to_string(id) + "] unknown type: " + *t);
            }
        }
    }
end:
    ::closesocket(client);
    VSLOG_INFO("[conn " + std::to_string(id) + "] closed");
}

} // namespace

int runTcpServer(std::uint16_t port)
{
    SOCKET listenSock = INVALID_SOCKET;
    WSADATA wsaData{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        VSLOG_ERROR("WSAStartup failed");
        return 1;
    }

    listenSock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) {
        VSLOG_ERROR("socket failed");
        ::WSACleanup();
        return 1;
    }

    BOOL opt = TRUE;
    ::setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(listenSock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        VSLOG_ERROR("bind failed (port in use?)");
        ::closesocket(listenSock);
        ::WSACleanup();
        return 1;
    }
    if (::listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
        VSLOG_ERROR("listen failed");
        ::closesocket(listenSock);
        ::WSACleanup();
        return 1;
    }

    VSLOG_INFO("lan-chat-server listening on 0.0.0.0:" + std::to_string(port)
               + " (Ctrl+C to stop — close window to exit)");

    std::atomic<std::uint64_t> connCounter{0};

    for (;;) {
        SOCKET client = ::accept(listenSock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            VSLOG_ERROR("accept failed");
            break;
        }
        std::thread(handleClient, client, std::ref(connCounter)).detach();
    }

    ::closesocket(listenSock);
    ::WSACleanup();
    return 0;
}

} // namespace vsserver
