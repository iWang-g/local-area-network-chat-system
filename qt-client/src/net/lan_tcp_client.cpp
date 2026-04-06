#include "net/lan_tcp_client.h"

#include <cstring>

#include <QJsonDocument>
#include <QJsonParseError>

namespace {
constexpr quint32 kMaxPayload = 256 * 1024;
}

LanTcpClient::LanTcpClient(QObject *parent)
    : QObject(parent)
{
    connect(&socket_, &QTcpSocket::connected, this, &LanTcpClient::onConnected);
    connect(&socket_, &QTcpSocket::disconnected, this, &LanTcpClient::onDisconnected);
    connect(&socket_, &QTcpSocket::readyRead, this, &LanTcpClient::onReadyRead);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(&socket_, &QTcpSocket::errorOccurred, this, &LanTcpClient::onSocketError);
#else
    connect(&socket_, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
            &LanTcpClient::onSocketError);
#endif
}

void LanTcpClient::connectToServer(const QString &host, quint16 port)
{
    rxBuf_.clear();
    socket_.abort();
    socket_.connectToHost(host, port);
}

void LanTcpClient::disconnectFromServer()
{
    socket_.disconnectFromHost();
}

bool LanTcpClient::isConnected() const
{
    return socket_.state() == QAbstractSocket::ConnectedState;
}

QByteArray LanTcpClient::encodeFrame(const QByteArray &jsonUtf8)
{
    if (jsonUtf8.size() > static_cast<int>(kMaxPayload)) {
        return {};
    }
    const quint32 len = static_cast<quint32>(jsonUtf8.size());
    QByteArray out;
    out.resize(4 + jsonUtf8.size());
    out[0] = static_cast<char>((len >> 24) & 0xFF);
    out[1] = static_cast<char>((len >> 16) & 0xFF);
    out[2] = static_cast<char>((len >> 8) & 0xFF);
    out[3] = static_cast<char>(len & 0xFF);
    std::memcpy(out.data() + 4, jsonUtf8.constData(), static_cast<size_t>(jsonUtf8.size()));
    return out;
}

void LanTcpClient::sendJsonObject(const QJsonObject &obj)
{
    if (!isConnected()) {
        emit protocolError(QStringLiteral("未连接"));
        return;
    }
    const QByteArray json = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const QByteArray frame = encodeFrame(json);
    if (frame.isEmpty()) {
        emit protocolError(QStringLiteral("负载过大"));
        return;
    }
    socket_.write(frame);
}

void LanTcpClient::onReadyRead()
{
    rxBuf_.append(socket_.readAll());
    tryExtractFrames();
}

void LanTcpClient::tryExtractFrames()
{
    while (rxBuf_.size() >= 4) {
        const auto b0 = static_cast<quint32>(static_cast<unsigned char>(rxBuf_[0]));
        const auto b1 = static_cast<quint32>(static_cast<unsigned char>(rxBuf_[1]));
        const auto b2 = static_cast<quint32>(static_cast<unsigned char>(rxBuf_[2]));
        const auto b3 = static_cast<quint32>(static_cast<unsigned char>(rxBuf_[3]));
        const quint32 len = (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
        if (len > kMaxPayload) {
            rxBuf_.clear();
            emit protocolError(QStringLiteral("非法帧长度"));
            disconnectFromServer();
            return;
        }
        if (rxBuf_.size() < 4 + static_cast<int>(len)) {
            return;
        }
        const QByteArray payload = rxBuf_.mid(4, static_cast<int>(len));
        rxBuf_.remove(0, 4 + static_cast<int>(len));

        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(payload, &pe);
        if (!doc.isObject()) {
            emit protocolError(QStringLiteral("JSON 解析失败: %1").arg(pe.errorString()));
            continue;
        }
        emit jsonReceived(doc.object());
    }
}

void LanTcpClient::onSocketError(QAbstractSocket::SocketError err)
{
    Q_UNUSED(err);
    emit protocolError(socket_.errorString());
}

void LanTcpClient::onConnected()
{
    emit socketConnected();
}

void LanTcpClient::onDisconnected()
{
    rxBuf_.clear();
    emit socketDisconnected();
}
