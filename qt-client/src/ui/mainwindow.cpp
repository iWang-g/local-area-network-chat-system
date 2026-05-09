#include "ui/mainwindow.h"
#include "ui_mainwindow.h"

#include "net/lan_tcp_client.h"
#include "ui/chat_main_widget.h"
#include "ui/network_debug_dialog.h"
#include "ui/profile_dialog.h"
#include "utils/local_profile.h"

#include <QDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace {

constexpr int kAuthWindowWidth = 440;
/// 与参考 LoginWindow::setFixedSize(440, 592) 一致。
constexpr int kAuthLoginHeight = 592;
/// 注册页字段更多，略增高（原 480×620 比例）。
constexpr int kAuthRegisterHeight = 620;

void unlockMainWindowResize(QWidget *w)
{
    w->setMinimumSize(0, 0);
    w->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
}

/// 将窗口外框居中到 `w` 所在屏幕（或主屏）的可用区域。
void centerWindowFrameOnScreen(QWidget *w)
{
    if (!w) {
        return;
    }
    QScreen *scr = w->screen();
    if (!scr) {
        scr = QGuiApplication::primaryScreen();
    }
    if (!scr) {
        return;
    }
    const QRect avail = scr->availableGeometry();
    QRect fg = w->frameGeometry();
    fg.moveCenter(avail.center());
    w->move(fg.topLeft());
}

} // namespace

void MainWindow::attachAuthPageToMainWindow(QDialog *page)
{
    if (!page) {
        return;
    }
    // 作为 QStackedWidget 内页嵌入，避免 QDialog 独立成窗、Esc 关闭等默认行为干扰主窗口。
    page->setWindowFlags(Qt::Widget);
    page->setModal(false);
    page->setSizeGripEnabled(false);
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("局域网聊天"));

    m_client = new LanTcpClient(this);

    auto *central = ui->centralwidget;
    auto *rootLay = new QVBoxLayout(central);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    m_stack = new QStackedWidget(central);
    rootLay->addWidget(m_stack);

    m_login = new LoginWidget(m_stack);
    connect(m_login, &LoginWidget::loginRequested, this, &MainWindow::onLoginRequested);
    connect(m_login, &LoginWidget::registerRequested, this, &MainWindow::onRegisterRequested);
    m_stack->addWidget(m_login);
    attachAuthPageToMainWindow(m_login);

    m_register = new RegisterWidget(m_stack);
    connect(m_register, &RegisterWidget::registerSubmitted, this, &MainWindow::onRegisterSubmitted);
    connect(m_register, &RegisterWidget::requestEmailCode, this, &MainWindow::onRegisterRequestEmailCode);
    connect(m_register, &RegisterWidget::backToLoginRequested, this, &MainWindow::onRegisterBackToLogin);
    m_stack->addWidget(m_register);
    attachAuthPageToMainWindow(m_register);

    connect(m_client, &LanTcpClient::socketConnected, this, &MainWindow::onTcpSocketConnected);
    connect(m_client, &LanTcpClient::jsonReceived, this, &MainWindow::onTcpJsonReceived);
    connect(m_client, &LanTcpClient::protocolError, this, &MainWindow::onTcpProtocolError);

    m_workspace = buildChatWorkspacePage();
    m_stack->addWidget(m_workspace);

    m_stack->setCurrentWidget(m_login);
    menuBar()->hide();
    setFixedSize(kAuthWindowWidth, kAuthLoginHeight);
    centerWindowFrameOnScreen(this);
}

MainWindow::~MainWindow()
{
    delete ui;
}

QWidget *MainWindow::buildChatWorkspacePage()
{
    auto *page = new QWidget();
    page->setObjectName(QStringLiteral("workspaceRoot"));
    page->setStyleSheet(QStringLiteral("QWidget#workspaceRoot { background-color: #e8eef2; }"));

    auto *root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_chat = new ChatMainWidget(page);
    connect(m_chat, &ChatMainWidget::logoutRequested, this, &MainWindow::onBackToLogin);
    connect(m_chat, &ChatMainWidget::tcpDebugRequested, this, &MainWindow::openTcpDebug);
    connect(m_chat, &ChatMainWidget::profileEditRequested, this, &MainWindow::openProfileDialog);
    root->addWidget(m_chat, 1);

    return page;
}

void MainWindow::showLoginPage()
{
    if (m_tcpDebug) {
        m_tcpDebug->hide();
    }
    m_pendingLoginMode.clear();
    m_sessionToken.clear();
    m_sessionUsername.clear();
    m_sessionUserId = 0;
    if (m_chat) {
        m_chat->clearSession();
    }
    m_authPhase = 0;
    m_pendingEmailCodeOnly = false;
    m_client->disconnectFromServer();
    m_stack->setCurrentWidget(m_login);
    m_login->refreshAvatarFromLocalProfile(m_login->emailInput());
    m_login->clearError();
    if (m_register) {
        m_register->clearError();
        m_register->clearCodeSentHint();
    }
    menuBar()->clear();
    menuBar()->hide();
    setWindowTitle(QStringLiteral("局域网聊天"));
    setFixedSize(kAuthWindowWidth, kAuthLoginHeight);
    centerWindowFrameOnScreen(this);
}

void MainWindow::showChatPage(const QString &email)
{
    {
        QSettings s;
        s.setValue(QStringLiteral("server/host"), m_lastAuthHost);
        s.setValue(QStringLiteral("server/port"), QString::number(m_lastAuthPort));
    }
    if (m_login) {
        m_login->setServerFields(m_lastAuthHost, QString::number(m_lastAuthPort));
    }
    if (m_register) {
        m_register->setServerFields(m_lastAuthHost, QString::number(m_lastAuthPort));
    }
    m_authPhase = 0;
    m_stack->setCurrentWidget(m_workspace);
    menuBar()->clear();
    menuBar()->hide();

    setWindowTitle(QStringLiteral("局域网聊天"));
    unlockMainWindowResize(this);
    setMinimumSize(880, 560);
    resize(1040, 760);
    centerWindowFrameOnScreen(this);
    if (m_chat) {
        m_chat->setUserEmail(email);
    }
}

void MainWindow::openTcpDebug()
{
    if (m_tcpDebug == nullptr) {
        m_tcpDebug = new NetworkDebugDialog(m_client, this);
    }
    m_tcpDebug->show();
    m_tcpDebug->raise();
    m_tcpDebug->activateWindow();
}

void MainWindow::openProfileDialog()
{
    if (!m_chat) {
        return;
    }
    const QString em = m_chat->userEmail();
    if (em.isEmpty()) {
        return;
    }
    ProfileDialog dlg(em, m_sessionUsername, m_client, m_sessionToken, this);
    if (dlg.exec() == QDialog::Accepted && m_login) {
        m_login->refreshAvatarFromLocalProfile(em);
    }
}

void MainWindow::onLoginRequested(const QString &account, const QString &password, const QString &loginMode)
{
    m_login->clearError();
    if (account.trimmed().isEmpty()) {
        m_login->showError(loginMode == QStringLiteral("username") ? QStringLiteral("请输入用户名")
                                                                     : QStringLiteral("请输入邮箱"));
        return;
    }
    if (password.isEmpty()) {
        m_login->showError(QStringLiteral("请输入密码"));
        return;
    }
    QString host = m_login->serverHost();
    if (host.isEmpty()) {
        host = QStringLiteral("127.0.0.1");
    }
    m_lastAuthHost = host;
    m_lastAuthPort = m_login->serverPort();
    m_pendingEmail = account.trimmed();
    m_pendingPassword = password;
    m_pendingLoginMode = loginMode;
    m_pendingRegister = false;
    m_authPhase = 1;
    m_client->connectToServer(host, m_lastAuthPort);
}

void MainWindow::onRegisterRequested()
{
    m_login->clearError();
    if (m_register) {
        m_register->clearError();
        const QString h = m_login->serverHost();
        m_register->setServerFields(h.isEmpty() ? QStringLiteral("127.0.0.1") : h,
                                    QString::number(m_login->serverPort()));
        m_stack->setCurrentWidget(m_register);
    }
    setFixedSize(kAuthWindowWidth, kAuthRegisterHeight);
    centerWindowFrameOnScreen(this);
}

void MainWindow::onRegisterSubmitted(const QString &email, const QString &password, const QString &username,
                                     const QString &emailCode)
{
    m_register->clearError();
    QString host = m_register->serverHost();
    if (host.isEmpty()) {
        host = QStringLiteral("127.0.0.1");
    }
    m_lastAuthHost = host;
    m_lastAuthPort = m_register->serverPort();
    m_pendingEmail = email;
    m_pendingPassword = password;
    m_pendingUsername = username;
    m_pendingEmailCode = emailCode;
    m_pendingEmailCodeOnly = false;
    m_pendingRegister = true;
    m_authPhase = 1;
    m_client->connectToServer(host, m_lastAuthPort);
}

void MainWindow::onRegisterRequestEmailCode(const QString &email)
{
    m_register->clearError();
    QString host = m_register->serverHost();
    if (host.isEmpty()) {
        host = QStringLiteral("127.0.0.1");
    }
    m_lastAuthHost = host;
    m_lastAuthPort = m_register->serverPort();
    m_pendingEmailForCode = email;
    m_pendingEmailCodeOnly = true;
    m_pendingRegister = false;
    m_authPhase = 1;
    m_client->connectToServer(host, m_lastAuthPort);
}

void MainWindow::onRegisterBackToLogin()
{
    if (m_authPhase != 0) {
        m_client->disconnectFromServer();
        m_authPhase = 0;
        m_pendingRegister = false;
        m_pendingEmailCodeOnly = false;
    }
    if (m_register && m_login) {
        m_register->clearError();
        m_login->setServerFields(m_register->serverHost().isEmpty() ? QStringLiteral("127.0.0.1")
                                                                     : m_register->serverHost(),
                                 QString::number(m_register->serverPort()));
    }
    m_stack->setCurrentWidget(m_login);
    setFixedSize(kAuthWindowWidth, kAuthLoginHeight);
    centerWindowFrameOnScreen(this);
}

void MainWindow::onBackToLogin()
{
    showLoginPage();
}

void MainWindow::onTcpSocketConnected()
{
    if (m_authPhase != 1) {
        return;
    }
    QJsonObject hello;
    hello.insert(QStringLiteral("type"), QStringLiteral("hello"));
    hello.insert(QStringLiteral("magic"), QStringLiteral("LNCS"));
    hello.insert(QStringLiteral("version"), 1);
    m_client->sendJsonObject(hello);
}

void MainWindow::onTcpJsonReceived(const QJsonObject &obj)
{
    const QString t = obj.value(QStringLiteral("type")).toString();
    if (t == QStringLiteral("hello_ok")) {
        if (m_authPhase != 1) {
            return;
        }
        m_authPhase = 2;
        if (m_pendingEmailCodeOnly) {
            QJsonObject req;
            req.insert(QStringLiteral("type"), QStringLiteral("req_email_code"));
            req.insert(QStringLiteral("email"), m_pendingEmailForCode);
            req.insert(QStringLiteral("purpose"), QStringLiteral("register"));
            m_client->sendJsonObject(req);
        } else if (m_pendingRegister) {
            QJsonObject reg;
            reg.insert(QStringLiteral("type"), QStringLiteral("auth_register"));
            reg.insert(QStringLiteral("email"), m_pendingEmail);
            reg.insert(QStringLiteral("password"), m_pendingPassword);
            reg.insert(QStringLiteral("email_code"), m_pendingEmailCode);
            reg.insert(QStringLiteral("username"), m_pendingUsername);
            m_client->sendJsonObject(reg);
        } else {
            QJsonObject login;
            login.insert(QStringLiteral("type"), QStringLiteral("auth_login"));
            login.insert(QStringLiteral("password"), m_pendingPassword);
            if (m_pendingLoginMode == QStringLiteral("username")) {
                login.insert(QStringLiteral("username"), m_pendingEmail);
            } else {
                login.insert(QStringLiteral("email"), m_pendingEmail);
            }
            m_client->sendJsonObject(login);
        }
        return;
    }
    if (t == QStringLiteral("email_code_ok")) {
        if (m_authPhase != 2 || !m_pendingEmailCodeOnly) {
            return;
        }
        m_authPhase = 0;
        m_pendingEmailCodeOnly = false;
        m_client->disconnectFromServer();
        m_register->showCodeSentHint();
        m_register->startResendCooldown(60);
        return;
    }
    if (t == QStringLiteral("auth_ok")) {
        if (m_authPhase != 2) {
            return;
        }
        m_authPhase = 0;
        const QString em = obj.value(QStringLiteral("email")).toString();
        m_sessionToken = obj.value(QStringLiteral("token")).toString();
        m_sessionUsername = obj.value(QStringLiteral("username")).toString();
        m_sessionUserId = obj.value(QStringLiteral("user_id")).toVariant().toLongLong();
        const QString accountEmail = em.isEmpty() ? m_pendingEmail : em;
        if (obj.contains(QStringLiteral("nickname"))) {
            LocalProfile::Data prof;
            LocalProfile::load(accountEmail, &prof);
            LocalProfile::saveMeta(accountEmail, obj.value(QStringLiteral("nickname")).toString(), prof.bio);
        }
        showChatPage(accountEmail);
        if (m_chat) {
            m_chat->setSession(m_client, m_sessionToken, m_sessionUserId);
        }
        return;
    }
    if (t == QStringLiteral("error")) {
        if (m_authPhase != 0) {
            const bool codeOnly = m_pendingEmailCodeOnly;
            const bool fromRegister = m_pendingRegister;
            m_authPhase = 0;
            m_pendingRegister = false;
            m_pendingEmailCodeOnly = false;
            const QString msg = obj.value(QStringLiteral("message")).toString();
            const int retryAfter = obj.value(QStringLiteral("retry_after_sec")).toInt(0);
            m_client->disconnectFromServer();
            if (codeOnly && m_register) {
                m_register->showError(msg);
                if (retryAfter > 0) {
                    m_register->startResendCooldown(retryAfter);
                }
                m_stack->setCurrentWidget(m_register);
            } else if (fromRegister && m_register) {
                m_register->showError(msg);
                m_stack->setCurrentWidget(m_register);
            } else {
                m_login->showError(msg);
                m_stack->setCurrentWidget(m_login);
            }
            return;
        }
        if (m_stack->currentWidget() == m_workspace && !m_sessionToken.isEmpty() && m_chat) {
            m_chat->handleServerJson(obj);
        }
        return;
    }

    if (m_stack->currentWidget() == m_workspace && !m_sessionToken.isEmpty() && m_chat) {
        m_chat->handleServerJson(obj);
    }
}

void MainWindow::onTcpProtocolError(const QString &message)
{
    if (m_authPhase != 0) {
        const bool codeOnly = m_pendingEmailCodeOnly;
        const bool fromRegister = m_pendingRegister;
        m_authPhase = 0;
        m_pendingRegister = false;
        m_pendingEmailCodeOnly = false;
        if (codeOnly && m_register) {
            m_register->showError(message);
            m_stack->setCurrentWidget(m_register);
        } else if (fromRegister && m_register) {
            m_register->showError(message);
            m_stack->setCurrentWidget(m_register);
        } else {
            m_login->showError(message);
            m_stack->setCurrentWidget(m_login);
        }
        return;
    }
    if (m_stack->currentWidget() == m_workspace && !m_sessionToken.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("连接异常"), message);
    }
}
