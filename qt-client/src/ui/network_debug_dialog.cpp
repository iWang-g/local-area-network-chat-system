#include "ui/network_debug_dialog.h"

#include "net/lan_tcp_client.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>

NetworkDebugDialog::NetworkDebugDialog(LanTcpClient *client, QWidget *parent)
    : QDialog(parent)
    , m_client(client)
{
    setWindowTitle(QStringLiteral("TCP 联调"));
    setMinimumSize(520, 420);
    resize(640, 480);

    auto *root = new QVBoxLayout(this);

    auto *grp = new QGroupBox(QStringLiteral("连接（阶段1：握手 / 心跳）"), this);
    auto *gh = new QHBoxLayout(grp);
    gh->addWidget(new QLabel(QStringLiteral("主机"), grp));
    m_hostEdit = new QLineEdit(QStringLiteral("127.0.0.1"), grp);
    gh->addWidget(m_hostEdit, 1);
    gh->addWidget(new QLabel(QStringLiteral("端口"), grp));
    m_portSpin = new QSpinBox(grp);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(28888);
    gh->addWidget(m_portSpin);
    m_btnConnect = new QPushButton(QStringLiteral("连接"), grp);
    m_btnDisconnect = new QPushButton(QStringLiteral("断开"), grp);
    m_btnDisconnect->setEnabled(false);
    gh->addWidget(m_btnConnect);
    gh->addWidget(m_btnDisconnect);
    root->addWidget(grp);

    auto *row2 = new QHBoxLayout();
    m_btnHello = new QPushButton(QStringLiteral("发送握手 hello"), this);
    m_btnHb = new QPushButton(QStringLiteral("发送心跳 heartbeat"), this);
    m_btnHello->setEnabled(false);
    m_btnHb->setEnabled(false);
    row2->addWidget(m_btnHello);
    row2->addWidget(m_btnHb);
    row2->addStretch(1);
    root->addLayout(row2);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    root->addWidget(m_log, 1);

    connect(m_btnConnect, &QPushButton::clicked, this, &NetworkDebugDialog::onConnectClicked);
    connect(m_btnDisconnect, &QPushButton::clicked, this, &NetworkDebugDialog::onDisconnectClicked);
    connect(m_btnHello, &QPushButton::clicked, this, &NetworkDebugDialog::onSendHelloClicked);
    connect(m_btnHb, &QPushButton::clicked, this, &NetworkDebugDialog::onSendHeartbeatClicked);

    if (m_client) {
        connect(m_client, &LanTcpClient::socketConnected, this, &NetworkDebugDialog::onClientConnected);
        connect(m_client, &LanTcpClient::socketDisconnected, this, &NetworkDebugDialog::onClientDisconnected);
        connect(m_client, &LanTcpClient::jsonReceived, this, &NetworkDebugDialog::onJsonReceived);
        connect(m_client, &LanTcpClient::protocolError, this, &NetworkDebugDialog::onProtocolError);
    }

    appendLog(QStringLiteral("请先启动 vs-server，再点「连接」。"));
}

void NetworkDebugDialog::appendLog(const QString &line)
{
    if (m_log) {
        m_log->appendPlainText(line);
    }
}

void NetworkDebugDialog::onConnectClicked()
{
    if (!m_client || !m_hostEdit || !m_portSpin) {
        return;
    }
    const QString host = m_hostEdit->text().trimmed();
    if (host.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请填写主机地址"));
        return;
    }
    m_client->connectToServer(host, static_cast<quint16>(m_portSpin->value()));
    appendLog(QStringLiteral("正在连接 %1:%2 …").arg(host).arg(m_portSpin->value()));
}

void NetworkDebugDialog::onDisconnectClicked()
{
    if (m_client) {
        m_client->disconnectFromServer();
    }
}

void NetworkDebugDialog::onSendHelloClicked()
{
    if (!m_client) {
        return;
    }
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("hello");
    o[QStringLiteral("magic")] = QStringLiteral("LNCS");
    o[QStringLiteral("version")] = 1;
    m_client->sendJsonObject(o);
    appendLog(QStringLiteral("→ 发送 hello"));
}

void NetworkDebugDialog::onSendHeartbeatClicked()
{
    if (!m_client) {
        return;
    }
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("heartbeat");
    m_client->sendJsonObject(o);
    appendLog(QStringLiteral("→ 发送 heartbeat"));
}

void NetworkDebugDialog::onClientConnected()
{
    appendLog(QStringLiteral("已连接。"));
    if (m_btnConnect) {
        m_btnConnect->setEnabled(false);
    }
    if (m_btnDisconnect) {
        m_btnDisconnect->setEnabled(true);
    }
    if (m_btnHello) {
        m_btnHello->setEnabled(true);
    }
    if (m_btnHb) {
        m_btnHb->setEnabled(true);
    }
}

void NetworkDebugDialog::onClientDisconnected()
{
    appendLog(QStringLiteral("已断开。"));
    if (m_btnConnect) {
        m_btnConnect->setEnabled(true);
    }
    if (m_btnDisconnect) {
        m_btnDisconnect->setEnabled(false);
    }
    if (m_btnHello) {
        m_btnHello->setEnabled(false);
    }
    if (m_btnHb) {
        m_btnHb->setEnabled(false);
    }
}

void NetworkDebugDialog::onJsonReceived(const QJsonObject &obj)
{
    const QByteArray pretty = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    appendLog(QStringLiteral("← 收到 %1").arg(QString::fromUtf8(pretty)));
}

void NetworkDebugDialog::onProtocolError(const QString &msg)
{
    appendLog(QStringLiteral("! %1").arg(msg));
}
