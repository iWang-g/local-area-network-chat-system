#ifndef LAN_TCP_CLIENT_H
#define LAN_TCP_CLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QTcpSocket>

/// 与聊天主服务端之间的 TCP 连接：长度前缀（大端 4 字节）+ UTF-8 JSON **或** LNCB 二进制负载。
class LanTcpClient : public QObject
{
    Q_OBJECT

public:
    explicit LanTcpClient(QObject *parent = nullptr);

    void connectToServer(const QString &host, quint16 port);
    void disconnectFromServer();
    bool isConnected() const;

    void sendJsonObject(const QJsonObject &obj);
    /// 发送已含 LNCB 等协议体的完整负载（再经 4 字节大端长度封装）。
    void sendRawPayload(const QByteArray &payload);

signals:
    void socketConnected();
    void socketDisconnected();
    void jsonReceived(const QJsonObject &obj);
    /// 与 JSON 相对；负载首 4 字节为 `LNCB`。
    void binaryPayloadReceived(const QByteArray &payload);
    void protocolError(const QString &message);

private slots:
    void onReadyRead();
    void onSocketError(QAbstractSocket::SocketError err);
    void onConnected();
    void onDisconnected();

private:
    void tryExtractFrames();
    static QByteArray encodeFrame(const QByteArray &jsonUtf8);

    QTcpSocket socket_;
    QByteArray rxBuf_;
};

#endif // LAN_TCP_CLIENT_H
