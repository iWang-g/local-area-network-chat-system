#include "vsserver/file_relay.hpp"

#include "vsserver/base64.hpp"
#include "vsserver/database.hpp"
#include "vsserver/logger.hpp"
#include "vsserver/message_parse.hpp"
#include "vsserver/platform_paths.hpp"
#include "vsserver/protocol.hpp"

#include "picosha2.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace vsserver {

namespace {

std::mutex g_mu;
struct Rt {
    std::int64_t id = 0;
    std::int64_t fromUserId = 0;
    std::int64_t toUserId = 0;
    std::string fileName;
    std::uint64_t fileSize = 0;
    std::string sha256Hex;
    bool asSticker = false;
    /// 对方离线：分片写入服务端临时文件，完成后校验 SHA 并移到 relay 缓存路径。
    bool serverBufferPeerOffline = false;
    std::filesystem::path serverPartialPath;
    std::unique_ptr<std::ofstream> serverChunkOut;
    enum class Phase { AwaitingAccept, Streaming } phase = Phase::AwaitingAccept;
    std::uint32_t nextSeqExpected = 0;
    std::uint64_t bytesForwarded = 0;
};
std::unordered_map<std::int64_t, Rt> g_byId;

static bool sha256HexOk(const std::string &h)
{
    if (h.empty()) {
        return true;
    }
    if (h.size() != 64) {
        return false;
    }
    for (const char ch : h) {
        if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
            return false;
        }
    }
    return true;
}

static void trimInPlace(std::string &s)
{
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= 32) {
        s.erase(s.begin());
    }
    while (!s.empty() && static_cast<unsigned char>(s.back()) <= 32) {
        s.pop_back();
    }
}

static std::string sanitizeFileName(std::string s)
{
    trimInPlace(s);
    for (char &c : s) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
            c == '|') {
            c = '_';
        }
    }
    trimInPlace(s);
    constexpr std::size_t kMax = 200;
    if (s.size() > kMax) {
        s.resize(kMax);
    }
    return s;
}

static bool base64DecodeSize(const std::string &b64, std::size_t &outDecodedSize)
{
    return wireBase64DecodedByteCount(b64, outDecodedSize);
}

static std::filesystem::path offlinePartialFsPath(const std::int64_t transferId)
{
    const std::filesystem::path root = appExecutableDirectory() / "data" / "offline_partial";
    (void)std::filesystem::create_directories(root);
    return root / (std::to_string(transferId) + std::string(".part"));
}

static std::filesystem::path relayArtifactFsPath(const std::int64_t transferId, const std::string &fileNameUtf8)
{
    const auto dot = fileNameUtf8.find_last_of('.');
    std::string ext = "bin";
    if (dot != std::string::npos && dot + 1 < fileNameUtf8.size()) {
        ext.clear();
        for (std::size_t i = dot + 1; i < fileNameUtf8.size() && ext.size() < 8; ++i) {
            char c = fileNameUtf8[i];
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                ext.push_back(c);
            }
        }
        if (ext.empty()) {
            ext = "bin";
        }
    }
    const std::filesystem::path root = appExecutableDirectory() / "data" / "sticker_cache";
    (void)std::filesystem::create_directories(root);
    return root / (std::to_string(transferId) + std::string(".") + ext);
}

static bool sha256HexEqualsCi(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static void abortTransferDb(const std::int64_t transferId)
{
    const std::time_t now = std::time(nullptr);
    (void)AppDatabase::fileTransferSetStatus(transferId, "aborted", static_cast<std::int64_t>(now));
}

} // namespace

FileOfferResult fileRelayOffer(const std::int64_t fromUserId, const std::int64_t peerUserId,
                               std::string rawFileNameUtf8, const std::uint64_t fileSizeBytes,
                               std::string sha256HexLower, const bool asSticker,
                               const std::optional<std::uint32_t> chunkPlainMaxBinary)
{
    FileOfferResult r;
    if (fromUserId <= 0 || peerUserId <= 0 || fromUserId == peerUserId) {
        r.errCode = kErrInvalidInput;
        r.message = "无效的收发方";
        return r;
    }
    if (fileSizeBytes == 0 || fileSizeBytes > kFileTransferMaxBytes) {
        r.errCode = kErrFileTooLarge;
        r.message = "文件过大或大小无效";
        return r;
    }
    if (!AppDatabase::areUsersFriends(fromUserId, peerUserId)) {
        r.errCode = kErrFileNotFriend;
        r.message = "非好友关系";
        return r;
    }
    const std::string name = sanitizeFileName(std::move(rawFileNameUtf8));
    if (name.empty()) {
        r.errCode = kErrFileBadName;
        r.message = "无效的文件名";
        return r;
    }
    if (!sha256HexOk(sha256HexLower)) {
        r.errCode = kErrInvalidInput;
        r.message = "sha256_hex 须为 64 位十六进制或留空";
        return r;
    }

    const AppDatabase::FileOpOutcome ins =
        AppDatabase::fileTransferInsertOffer(fromUserId, peerUserId, name, static_cast<std::int64_t>(fileSizeBytes),
                                           sha256HexLower);
    if (!ins.ok) {
        r.errCode = ins.errCode;
        r.message = ins.message;
        return r;
    }

    Rt rt;
    rt.id = ins.transferId;
    rt.fromUserId = fromUserId;
    rt.toUserId = peerUserId;
    rt.fileName = name;
    rt.fileSize = fileSizeBytes;
    rt.sha256Hex = sha256HexLower;
    rt.phase = Rt::Phase::AwaitingAccept;
    rt.nextSeqExpected = 0;
    rt.bytesForwarded = 0;
    rt.asSticker = asSticker;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_byId[ins.transferId] = std::move(rt);
    }

    r.ok = true;
    r.transferId = ins.transferId;
    r.jsonOfferOkUtf8 = buildFileOfferOkJson(ins.transferId, kFileChunkPlainMax, chunkPlainMaxBinary);
    r.jsonIncomingUtf8 = buildFileIncomingJson(ins.transferId, fromUserId, name, fileSizeBytes, sha256HexLower);
    return r;
}

FileOfferResult fileRelayOfferServerBufferPeerOffline(const std::int64_t fromUserId, const std::int64_t peerUserId,
                                                      std::string rawFileNameUtf8, const std::uint64_t fileSizeBytes,
                                                      std::string sha256HexLower, const bool asSticker,
                                                      const std::optional<std::uint32_t> chunkPlainMaxBinary)
{
    FileOfferResult r;
    if (fromUserId <= 0 || peerUserId <= 0 || fromUserId == peerUserId) {
        r.errCode = kErrInvalidInput;
        r.message = "无效的收发方";
        return r;
    }
    if (fileSizeBytes == 0 || fileSizeBytes > kFileTransferMaxBytes) {
        r.errCode = kErrFileTooLarge;
        r.message = "文件过大或大小无效";
        return r;
    }
    if (!AppDatabase::areUsersFriends(fromUserId, peerUserId)) {
        r.errCode = kErrFileNotFriend;
        r.message = "非好友关系";
        return r;
    }
    const std::string name = sanitizeFileName(std::move(rawFileNameUtf8));
    if (name.empty()) {
        r.errCode = kErrFileBadName;
        r.message = "无效的文件名";
        return r;
    }
    if (!sha256HexOk(sha256HexLower)) {
        r.errCode = kErrInvalidInput;
        r.message = "sha256_hex 须为 64 位十六进制或留空";
        return r;
    }

    const AppDatabase::FileOpOutcome ins =
        AppDatabase::fileTransferInsertOffer(fromUserId, peerUserId, name, static_cast<std::int64_t>(fileSizeBytes),
                                           sha256HexLower);
    if (!ins.ok) {
        r.errCode = ins.errCode;
        r.message = ins.message;
        return r;
    }
    const AppDatabase::FileOpOutcome up =
        AppDatabase::fileTransferSetStatus(ins.transferId, "transferring", 0);
    if (!up.ok) {
        r.errCode = up.errCode;
        r.message = up.message;
        return r;
    }

    Rt rt;
    rt.id = ins.transferId;
    rt.fromUserId = fromUserId;
    rt.toUserId = peerUserId;
    rt.fileName = name;
    rt.fileSize = fileSizeBytes;
    rt.sha256Hex = std::move(sha256HexLower);
    rt.asSticker = asSticker;
    rt.serverBufferPeerOffline = true;
    rt.serverPartialPath = offlinePartialFsPath(ins.transferId);
    rt.phase = Rt::Phase::Streaming;
    rt.nextSeqExpected = 0;
    rt.bytesForwarded = 0;

    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_byId[ins.transferId] = std::move(rt);
    }

    r.ok = true;
    r.transferId = ins.transferId;
    r.jsonOfferOkUtf8 = buildFileOfferOkJson(ins.transferId, kFileChunkPlainMax, chunkPlainMaxBinary);
    r.jsonIncomingUtf8.clear();
    return r;
}

std::string fileRelayServerBufferArtifactPathIfExists(const std::int64_t transferId, const std::string &fileNameUtf8,
                                                      const std::int64_t expectedFileSizeBytes)
{
    if (transferId <= 0) {
        return {};
    }
    try {
        const std::filesystem::path cache = relayArtifactFsPath(transferId, fileNameUtf8);
        if (std::filesystem::exists(cache) && std::filesystem::is_regular_file(cache)) {
            if (expectedFileSizeBytes > 0) {
                if (std::filesystem::file_size(cache) == static_cast<std::uintmax_t>(expectedFileSizeBytes)) {
                    return cache.u8string();
                }
            } else {
                return cache.u8string();
            }
        }
        // 发送方已写完但尚未 rename / 或 DB 未标 completed 时，完整数据可能仍在 `.part`。
        if (expectedFileSizeBytes > 0) {
            const std::filesystem::path part = offlinePartialFsPath(transferId);
            if (std::filesystem::exists(part) && std::filesystem::is_regular_file(part)) {
                if (std::filesystem::file_size(part) == static_cast<std::uintmax_t>(expectedFileSizeBytes)) {
                    return part.u8string();
                }
            }
        }
    } catch (const std::exception &) {
    }
    return {};
}

FileNotifyResult fileRelayAccept(const std::int64_t selfUserId, const std::int64_t transferId)
{
    FileNotifyResult r;
    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_byId.find(transferId);
    if (it == g_byId.end()) {
        r.errCode = kErrFileNotFound;
        r.message = "传输不存在或已结束";
        return r;
    }
    Rt &t = it->second;
    if (t.serverBufferPeerOffline) {
        r.errCode = kErrFileWrongRole;
        r.message = "该传输无需接受";
        return r;
    }
    if (t.toUserId != selfUserId) {
        r.errCode = kErrFileWrongRole;
        r.message = "无权接受该传输";
        return r;
    }
    if (t.phase != Rt::Phase::AwaitingAccept) {
        r.errCode = kErrFileNotFound;
        r.message = "传输状态无效";
        return r;
    }
    const AppDatabase::FileOpOutcome up = AppDatabase::fileTransferSetStatus(transferId, "transferring", 0);
    if (!up.ok) {
        r.errCode = up.errCode;
        r.message = up.message;
        return r;
    }
    t.phase = Rt::Phase::Streaming;
    r.ok = true;
    r.notifyUserId = t.fromUserId;
    r.notifyJsonUtf8 = buildFileSendReadyJson(transferId);
    return r;
}

FileNotifyResult fileRelayReject(const std::int64_t selfUserId, const std::int64_t transferId)
{
    FileNotifyResult r;
    std::int64_t senderId = 0;
    Rt snap{};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_byId.find(transferId);
        if (it == g_byId.end()) {
            r.errCode = kErrFileNotFound;
            r.message = "传输不存在或已结束";
            return r;
        }
        Rt &t = it->second;
        if (t.serverBufferPeerOffline) {
            r.errCode = kErrFileNotFound;
            r.message = "传输状态无效";
            return r;
        }
        if (t.toUserId != selfUserId) {
            r.errCode = kErrFileWrongRole;
            r.message = "无权拒绝该传输";
            return r;
        }
        if (t.phase != Rt::Phase::AwaitingAccept) {
            r.errCode = kErrFileNotFound;
            r.message = "传输状态无效";
            return r;
        }
        senderId = t.fromUserId;
        snap = std::move(it->second);
        g_byId.erase(it);
    }
    const std::time_t now = std::time(nullptr);
    (void)AppDatabase::fileTransferSetStatus(transferId, "rejected", static_cast<std::int64_t>(now));
    const std::string chatBody = buildFileChatMessageContentJson(
        transferId, snap.fileName, static_cast<std::int64_t>(snap.fileSize), "", "failed", "对方已拒绝接收", snap.asSticker);
    const AppDatabase::MsgOpOutcome moChat =
        AppDatabase::messageInsertChatRecord(snap.fromUserId, snap.toUserId, chatBody);
    r.ok = true;
    r.notifyUserId = senderId;
    r.notifyJsonUtf8 = buildFileAbortedJson(transferId, "对方已拒绝接收");
    if (moChat.ok) {
        r.hasChatMsg = true;
        r.chatMsg.messageId = moChat.messageId;
        r.chatMsg.fromUserId = snap.fromUserId;
        r.chatMsg.toUserId = snap.toUserId;
        r.chatMsg.content = chatBody;
        r.chatMsg.createdAt = moChat.createdAt;
    }
    return r;
}

FileChunkRelayResult fileRelayOnSenderChunk(const std::int64_t selfUserId, const std::int64_t transferId,
                                            const std::uint32_t seq, const std::string &dataB64Utf8)
{
    FileChunkRelayResult r;
    std::size_t decSz = 0;
    if (!base64DecodeSize(dataB64Utf8, decSz)) {
        r.errCode = kErrFileChunk;
        r.message = "无效的 Base64 数据";
        return r;
    }
    if (decSz == 0 || decSz > kFileChunkPlainMax) {
        r.errCode = kErrFileChunk;
        r.message = "分片大小无效";
        return r;
    }

    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_byId.find(transferId);
    if (it == g_byId.end()) {
        r.errCode = kErrFileNotFound;
        r.message = "传输不存在或已结束";
        return r;
    }
    Rt &t = it->second;
    if (t.fromUserId != selfUserId) {
        r.errCode = kErrFileWrongRole;
        r.message = "仅发送方可上传分片";
        return r;
    }
    if (t.phase != Rt::Phase::Streaming) {
        r.errCode = kErrFileNotFound;
        r.message = "传输未就绪或已结束";
        return r;
    }
    if (seq != t.nextSeqExpected) {
        r.errCode = kErrFileSeq;
        r.message = "分片序号错误";
        return r;
    }
    if (t.bytesForwarded + decSz > t.fileSize) {
        r.errCode = kErrFileChunk;
        r.message = "分片总长度超过文件大小";
        return r;
    }

    if (t.serverBufferPeerOffline) {
        if (!t.serverChunkOut) {
            try {
                t.serverChunkOut = std::make_unique<std::ofstream>(t.serverPartialPath.u8string(),
                                                                 std::ios::binary | std::ios::trunc);
            } catch (const std::exception &) {
                r.errCode = kErrDbUnavailable;
                r.message = "无法创建临时文件";
                return r;
            }
            if (!t.serverChunkOut->good()) {
                t.serverChunkOut.reset();
                r.errCode = kErrDbUnavailable;
                r.message = "无法创建临时文件";
                return r;
            }
        }
        std::vector<std::uint8_t> plainBin;
        if (!wireBase64DecodeBytes(dataB64Utf8, plainBin)) {
            r.errCode = kErrFileChunk;
            r.message = "分片解码失败";
            return r;
        }
        if (plainBin.size() != decSz) {
            r.errCode = kErrFileChunk;
            r.message = "解码长度不一致";
            return r;
        }
        std::string plain(reinterpret_cast<const char *>(plainBin.data()), plainBin.size());
        t.serverChunkOut->write(plain.data(), static_cast<std::streamsize>(decSz));
        if (!t.serverChunkOut->good()) {
            r.errCode = kErrFileChunk;
            r.message = "写入临时文件失败";
            return r;
        }
        t.nextSeqExpected = seq + 1;
        t.bytesForwarded += decSz;
        r.ok = true;
        r.pushToUserId = 0;
        r.pushJsonUtf8.clear();
        return r;
    }

    t.nextSeqExpected = seq + 1;
    t.bytesForwarded += decSz;
    r.ok = true;
    r.pushToUserId = t.toUserId;
    r.pushJsonUtf8 = buildFileChunkPushJson(transferId, seq, dataB64Utf8);
    return r;
}

FileChunkRelayResult fileRelayOnSenderChunkPlain(const std::int64_t selfUserId, const std::int64_t transferId,
                                                 const std::uint32_t seq, const std::vector<std::uint8_t> &plain)
{
    FileChunkRelayResult r;
    const std::size_t decSz = plain.size();
    if (decSz == 0 || decSz > kFileChunkBinaryPlainMax) {
        r.errCode = kErrFileChunk;
        r.message = "分片大小无效";
        return r;
    }

    std::lock_guard<std::mutex> lk(g_mu);
    const auto it = g_byId.find(transferId);
    if (it == g_byId.end()) {
        r.errCode = kErrFileNotFound;
        r.message = "传输不存在或已结束";
        return r;
    }
    Rt &t = it->second;
    if (t.fromUserId != selfUserId) {
        r.errCode = kErrFileWrongRole;
        r.message = "仅发送方可上传分片";
        return r;
    }
    if (t.phase != Rt::Phase::Streaming) {
        r.errCode = kErrFileNotFound;
        r.message = "传输未就绪或已结束";
        return r;
    }
    if (seq != t.nextSeqExpected) {
        r.errCode = kErrFileSeq;
        r.message = "分片序号错误";
        return r;
    }
    if (t.bytesForwarded + decSz > t.fileSize) {
        r.errCode = kErrFileChunk;
        r.message = "分片总长度超过文件大小";
        return r;
    }

    if (t.serverBufferPeerOffline) {
        if (!t.serverChunkOut) {
            try {
                t.serverChunkOut = std::make_unique<std::ofstream>(t.serverPartialPath.u8string(),
                                                                 std::ios::binary | std::ios::trunc);
            } catch (const std::exception &) {
                r.errCode = kErrDbUnavailable;
                r.message = "无法创建临时文件";
                return r;
            }
            if (!t.serverChunkOut->good()) {
                t.serverChunkOut.reset();
                r.errCode = kErrDbUnavailable;
                r.message = "无法创建临时文件";
                return r;
            }
        }
        t.serverChunkOut->write(reinterpret_cast<const char *>(plain.data()), static_cast<std::streamsize>(decSz));
        if (!t.serverChunkOut->good()) {
            r.errCode = kErrFileChunk;
            r.message = "写入临时文件失败";
            return r;
        }
        t.nextSeqExpected = seq + 1;
        t.bytesForwarded += decSz;
        r.ok = true;
        r.pushToUserId = 0;
        r.pushJsonUtf8.clear();
        r.pushBinaryPayload.clear();
        return r;
    }

    t.nextSeqExpected = seq + 1;
    t.bytesForwarded += decSz;
    r.ok = true;
    r.pushToUserId = t.toUserId;
    r.pushBinaryPayload =
        buildLnCbChunkPushPayload(transferId, seq, plain.empty() ? nullptr : plain.data(), plain.size());
    return r;
}

FileDoneRelayResult fileRelayOnSenderDone(const std::int64_t selfUserId, const std::int64_t transferId)
{
    FileDoneRelayResult r;
    Rt copy;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        const auto it = g_byId.find(transferId);
        if (it == g_byId.end()) {
            r.errCode = kErrFileNotFound;
            r.message = "传输不存在或已结束";
            return r;
        }
        Rt &t = it->second;
        if (t.fromUserId != selfUserId) {
            r.errCode = kErrFileWrongRole;
            r.message = "仅发送方可结束传输";
            return r;
        }
        if (t.phase != Rt::Phase::Streaming) {
            r.errCode = kErrFileNotFound;
            r.message = "传输未就绪或已结束";
            return r;
        }
        if (t.bytesForwarded != t.fileSize) {
            r.errCode = kErrFileSizeMismatch;
            r.message = "已发送字节数与声明大小不一致";
            return r;
        }
        copy = std::move(t);
        g_byId.erase(it);
    }

    if (copy.serverBufferPeerOffline) {
        if (copy.serverChunkOut) {
            copy.serverChunkOut->flush();
            copy.serverChunkOut->close();
            copy.serverChunkOut.reset();
        }
        std::uint64_t szOnDisk = 0;
        try {
            szOnDisk = std::filesystem::file_size(copy.serverPartialPath);
        } catch (const std::exception &) {
            r.errCode = kErrFileSizeMismatch;
            r.message = "临时文件大小读取失败";
            abortTransferDb(transferId);
            try {
                std::filesystem::remove(copy.serverPartialPath);
            } catch (const std::exception &) {
            }
            return r;
        }
        if (szOnDisk != copy.fileSize) {
            r.errCode = kErrFileSizeMismatch;
            r.message = "缓冲大小与声明不一致";
            abortTransferDb(transferId);
            try {
                std::filesystem::remove(copy.serverPartialPath);
            } catch (const std::exception &) {
            }
            return r;
        }
        std::string shaActual;
        try {
            std::ifstream inHash(copy.serverPartialPath.u8string(), std::ios::binary);
            shaActual = picosha2::hash256_hex_string(std::istreambuf_iterator<char>(inHash),
                                                     std::istreambuf_iterator<char>());
        } catch (const std::exception &) {
            r.errCode = kErrDbUnavailable;
            r.message = "SHA256 计算失败";
            abortTransferDb(transferId);
            try {
                std::filesystem::remove(copy.serverPartialPath);
            } catch (const std::exception &) {
            }
            return r;
        }
        std::string shaForMsg = copy.sha256Hex;
        if (!copy.sha256Hex.empty() && !sha256HexEqualsCi(shaActual, copy.sha256Hex)) {
            r.errCode = kErrFileChunk;
            r.message = "SHA256 与声明不一致";
            abortTransferDb(transferId);
            try {
                std::filesystem::remove(copy.serverPartialPath);
            } catch (const std::exception &) {
            }
            return r;
        }
        if (copy.sha256Hex.empty()) {
            shaForMsg = shaActual;
        }
        const std::filesystem::path finalPath = relayArtifactFsPath(copy.id, copy.fileName);
        try {
            (void)std::filesystem::create_directories(finalPath.parent_path());
            if (std::filesystem::exists(finalPath)) {
                std::filesystem::remove(finalPath);
            }
            std::filesystem::rename(copy.serverPartialPath, finalPath);
        } catch (const std::exception &) {
            r.errCode = kErrDbUnavailable;
            r.message = "文件落盘失败";
            abortTransferDb(transferId);
            try {
                std::filesystem::remove(copy.serverPartialPath);
            } catch (const std::exception &) {
            }
            return r;
        }

        const std::time_t nowSb = std::time(nullptr);
        const AppDatabase::FileOpOutcome upSb =
            AppDatabase::fileTransferSetStatus(transferId, "completed", static_cast<std::int64_t>(nowSb));
        if (!upSb.ok) {
            VSLOG_WARN("file_transfer_set_status failed id=" + std::to_string(transferId));
            try {
                std::filesystem::remove(finalPath);
            } catch (const std::exception &) {
            }
            abortTransferDb(transferId);
            r.errCode = upSb.errCode;
            r.message = upSb.message.empty() ? "传输状态更新失败" : upSb.message;
            return r;
        }
        const std::string chatBody =
            buildFileChatMessageContentJson(copy.id, copy.fileName, static_cast<std::int64_t>(copy.fileSize),
                                            shaForMsg, "ok", {}, copy.asSticker);
        const AppDatabase::MsgOpOutcome moChat =
            AppDatabase::messageInsertChatRecord(copy.fromUserId, copy.toUserId, chatBody);
        if (moChat.ok) {
            r.hasChatMsg = true;
            r.chatMsg.messageId = moChat.messageId;
            r.chatMsg.fromUserId = copy.fromUserId;
            r.chatMsg.toUserId = copy.toUserId;
            r.chatMsg.content = chatBody;
            r.chatMsg.createdAt = moChat.createdAt;
        }
        r.ok = true;
        r.pushToUserId = copy.toUserId;
        r.pushDoneJsonUtf8 = buildFileTransferDonePushJson(transferId, shaForMsg);
        return r;
    }

    const std::time_t now = std::time(nullptr);
    const AppDatabase::FileOpOutcome up =
        AppDatabase::fileTransferSetStatus(transferId, "completed", static_cast<std::int64_t>(now));
    if (!up.ok) {
        VSLOG_WARN("file_transfer_set_status failed id=" + std::to_string(transferId));
        r.errCode = up.errCode;
        r.message = up.message.empty() ? "传输状态更新失败" : up.message;
        return r;
    }
    const std::string chatBody =
        buildFileChatMessageContentJson(copy.id, copy.fileName, static_cast<std::int64_t>(copy.fileSize),
                                        copy.sha256Hex, "ok", {}, copy.asSticker);
    const AppDatabase::MsgOpOutcome moChat =
        AppDatabase::messageInsertChatRecord(copy.fromUserId, copy.toUserId, chatBody);
    if (moChat.ok) {
        r.hasChatMsg = true;
        r.chatMsg.messageId = moChat.messageId;
        r.chatMsg.fromUserId = copy.fromUserId;
        r.chatMsg.toUserId = copy.toUserId;
        r.chatMsg.content = chatBody;
        r.chatMsg.createdAt = moChat.createdAt;
    }
    r.ok = true;
    r.pushToUserId = copy.toUserId;
    r.pushDoneJsonUtf8 = buildFileTransferDonePushJson(transferId, copy.sha256Hex);
    return r;
}

std::vector<FileDisconnectItem> fileRelayOnUserDisconnect(const std::int64_t userId)
{
    std::vector<FileDisconnectItem> out;
    std::vector<std::pair<std::int64_t, Rt>> snapshots;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto it = g_byId.begin(); it != g_byId.end();) {
            if (it->second.fromUserId != userId && it->second.toUserId != userId) {
                ++it;
                continue;
            }
            snapshots.emplace_back(it->first, std::move(it->second));
            it = g_byId.erase(it);
        }
    }
    for (auto &pr : snapshots) {
        Rt t = std::move(pr.second);
        const std::int64_t tid = pr.first;
        if (t.serverChunkOut) {
            t.serverChunkOut->flush();
            t.serverChunkOut->close();
            t.serverChunkOut.reset();
        }
        if (t.serverBufferPeerOffline && !t.serverPartialPath.empty()) {
            try {
                std::filesystem::remove(t.serverPartialPath);
            } catch (const std::exception &) {
            }
        }
        abortTransferDb(tid);
        if (t.serverBufferPeerOffline && t.bytesForwarded < t.fileSize) {
            FileDisconnectItem it;
            it.notifyUserId = 0;
            out.push_back(std::move(it));
            continue;
        }
        const std::int64_t other = (t.fromUserId == userId) ? t.toUserId : t.fromUserId;
        FileDisconnectItem it;
        it.notifyUserId = other;
        it.fileAbortedJson = buildFileAbortedJson(tid, "对方已离线");
        const std::string chatBody =
            buildFileChatMessageContentJson(tid, t.fileName, static_cast<std::int64_t>(t.fileSize), "", "failed",
                                            "对方已离线", t.asSticker);
        const AppDatabase::MsgOpOutcome moChat =
            AppDatabase::messageInsertChatRecord(t.fromUserId, t.toUserId, chatBody);
        if (moChat.ok) {
            it.hasChatMsg = true;
            it.chatMsg.messageId = moChat.messageId;
            it.chatMsg.fromUserId = t.fromUserId;
            it.chatMsg.toUserId = t.toUserId;
            it.chatMsg.content = chatBody;
            it.chatMsg.createdAt = moChat.createdAt;
        }
        out.push_back(std::move(it));
    }
    return out;
}

} // namespace vsserver
