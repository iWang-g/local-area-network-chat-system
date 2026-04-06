#ifndef LAN_TCP_CLIENT_H
#define LAN_TCP_CLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QTcpSocket>

/// 与聊天主服务端之间的 TCP 连接：长度前缀（大端 4 字节）+ UTF-8 JSON 负载。
class LanTcpClient : public QObject
{
    Q_OBJECT

public:
    explicit LanTcpClient(QObject *parent = nullptr);

    void connectToServer(const QString &host, quint16 port);
    void disconnectFromServer();
    bool isConnected() const;

    void sendJsonObject(const QJsonObject &obj);

signals:
    void socketConnected();
    void socketDisconnected();
    void jsonReceived(const QJsonObject &obj);
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
