#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QJsonObject>
#include <QMainWindow>

#include "ui/login_widget.h"
#include "ui/register_widget.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class ChatMainWidget;
class LanTcpClient;
class NetworkDebugDialog;
class QStackedWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onLoginRequested(const QString &account, const QString &password, const QString &loginMode);
    void onRegisterRequested();
    void onRegisterSubmitted(const QString &email, const QString &password, const QString &username,
                             const QString &emailCode);
    void onRegisterRequestEmailCode(const QString &email);
    void onRegisterBackToLogin();
    void onBackToLogin();
    void openTcpDebug();
    void openProfileDialog();
    void onTcpSocketConnected();
    void onTcpJsonReceived(const QJsonObject &obj);
    void onTcpProtocolError(const QString &message);

private:
    QWidget *buildChatWorkspacePage();
    void showLoginPage();
    void showChatPage(const QString &email);
    /// 将登录/注册 QDialog 作为中央区页面嵌入（非独立弹窗），与主窗口生命周期一致。
    void attachAuthPageToMainWindow(QDialog *page);

    Ui::MainWindow *ui;
    LanTcpClient *m_client = nullptr;
    QStackedWidget *m_stack = nullptr;
    LoginWidget *m_login = nullptr;
    RegisterWidget *m_register = nullptr;
    QWidget *m_workspace = nullptr;
    ChatMainWidget *m_chat = nullptr;
    NetworkDebugDialog *m_tcpDebug = nullptr;

    /// 0=空闲 1=等 hello_ok 2=等 auth_ok
    int m_authPhase = 0;
    bool m_pendingRegister = false;
    /// 仅请求邮箱验证码（不发 auth_register）。
    bool m_pendingEmailCodeOnly = false;
    QString m_pendingEmail;
    QString m_pendingEmailForCode;
    QString m_pendingPassword;
    QString m_pendingUsername;
    QString m_pendingLoginMode;
    QString m_pendingEmailCode;

    QString m_lastAuthHost = QStringLiteral("127.0.0.1");
    quint16 m_lastAuthPort = 28888;

    /// 登录成功后与 TCP 会话绑定（好友等报文须带 token）。
    QString m_sessionToken;
    QString m_sessionUsername;
    qint64 m_sessionUserId = 0;
};

#endif // MAINWINDOW_H
