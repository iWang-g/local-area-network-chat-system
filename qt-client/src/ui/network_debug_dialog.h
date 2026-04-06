#ifndef NETWORK_DEBUG_DIALOG_H
#define NETWORK_DEBUG_DIALOG_H

#include <QDialog>
#include <QJsonObject>

class LanTcpClient;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

/// 阶段 1 TCP 联调：连接、握手、心跳与原始 JSON 日志。
class NetworkDebugDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NetworkDebugDialog(LanTcpClient *client, QWidget *parent = nullptr);

private slots:
    void onConnectClicked();
    void onDisconnectClicked();
    void onSendHelloClicked();
    void onSendHeartbeatClicked();
    void onClientConnected();
    void onClientDisconnected();
    void onJsonReceived(const QJsonObject &obj);
    void onProtocolError(const QString &msg);

private:
    void appendLog(const QString &line);

    LanTcpClient *m_client = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QPushButton *m_btnConnect = nullptr;
    QPushButton *m_btnDisconnect = nullptr;
    QPushButton *m_btnHello = nullptr;
    QPushButton *m_btnHb = nullptr;
    QPlainTextEdit *m_log = nullptr;
};

#endif // NETWORK_DEBUG_DIALOG_H
