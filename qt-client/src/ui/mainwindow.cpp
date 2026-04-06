#include "ui/mainwindow.h"
#include "ui_mainwindow.h"

#include "net/lan_tcp_client.h"
#include "ui/chat_main_widget.h"
#include "ui/login_widget.h"
#include "ui/register_widget.h"
#include "ui/network_debug_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QVBoxLayout>

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

    m_register = new RegisterWidget(m_stack);
    connect(m_register, &RegisterWidget::registerSubmitted, this, &MainWindow::onRegisterSubmitted);
    connect(m_register, &RegisterWidget::backToLoginRequested, this, &MainWindow::onRegisterBackToLogin);
    m_stack->addWidget(m_register);

    connect(m_client, &LanTcpClient::socketConnected, this, &MainWindow::onTcpSocketConnected);
    connect(m_client, &LanTcpClient::jsonReceived, this, &MainWindow::onTcpJsonReceived);
    connect(m_client, &LanTcpClient::protocolError, this, &MainWindow::onTcpProtocolError);

    m_workspace = buildChatWorkspacePage();
    m_stack->addWidget(m_workspace);

    m_stack->setCurrentWidget(m_login);
    menuBar()->hide();
    resize(480, 560);
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

    auto *topBar = new QWidget(page);
    topBar->setStyleSheet(QStringLiteral("background-color: #eef3f7; border-bottom: 1px solid #d5dee6;"));
    topBar->setFixedHeight(40);
    auto *topLay = new QHBoxLayout(topBar);
    topLay->setContentsMargins(10, 0, 12, 0);

    auto *backBtn = new QPushButton(QStringLiteral("← 返回登录"), topBar);
    backBtn->setCursor(Qt::PointingHandCursor);
    backBtn->setStyleSheet(QStringLiteral(
        "QPushButton { color: #12b7f5; border: none; background: transparent; font-size: 13px; }"
        "QPushButton:hover { text-decoration: underline; }"));
    connect(backBtn, &QPushButton::clicked, this, &MainWindow::onBackToLogin);
    topLay->addWidget(backBtn);

    m_userBadge = new QLabel(topBar);
    m_userBadge->setStyleSheet(QStringLiteral("color: #595959; font-size: 12px;"));
    topLay->addStretch(1);
    topLay->addWidget(m_userBadge);

    root->addWidget(topBar);

    m_chat = new ChatMainWidget(page);
    connect(m_chat, &ChatMainWidget::logoutRequested, this, &MainWindow::onBackToLogin);
    root->addWidget(m_chat, 1);

    return page;
}

void MainWindow::showLoginPage()
{
    if (m_tcpDebug) {
        m_tcpDebug->hide();
    }
    m_authPhase = 0;
    m_client->disconnectFromServer();
    m_stack->setCurrentWidget(m_login);
    m_login->clearError();
    if (m_register) {
        m_register->clearError();
    }
    menuBar()->clear();
    menuBar()->hide();
    setWindowTitle(QStringLiteral("局域网聊天"));
    resize(480, 560);
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
    auto *tools = menuBar()->addMenu(QStringLiteral("工具"));
    tools->addAction(QStringLiteral("TCP 联调…"), this, &MainWindow::openTcpDebug);
    menuBar()->show();

    setWindowTitle(QStringLiteral("局域网聊天"));
    resize(1024, 680);
    if (m_userBadge) {
        m_userBadge->setText(QStringLiteral("当前：%1").arg(email));
    }
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

void MainWindow::onLoginRequested(const QString &email, const QString &password)
{
    m_login->clearError();
    if (email.trimmed().isEmpty()) {
        m_login->showError(QStringLiteral("请输入邮箱"));
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
    m_pendingEmail = email.trimmed();
    m_pendingPassword = password;
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
    resize(480, 620);
}

void MainWindow::onRegisterSubmitted(const QString &email, const QString &password, const QString &nickname)
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
    m_pendingNickname = nickname;
    m_pendingRegister = true;
    m_authPhase = 1;
    m_client->connectToServer(host, m_lastAuthPort);
}

void MainWindow::onRegisterBackToLogin()
{
    if (m_authPhase != 0) {
        m_client->disconnectFromServer();
        m_authPhase = 0;
        m_pendingRegister = false;
    }
    if (m_register && m_login) {
        m_register->clearError();
        m_login->setServerFields(m_register->serverHost().isEmpty() ? QStringLiteral("127.0.0.1")
                                                                     : m_register->serverHost(),
                                 QString::number(m_register->serverPort()));
    }
    m_stack->setCurrentWidget(m_login);
    resize(480, 560);
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
        if (m_pendingRegister) {
            QJsonObject reg;
            reg.insert(QStringLiteral("type"), QStringLiteral("auth_register"));
            reg.insert(QStringLiteral("email"), m_pendingEmail);
            reg.insert(QStringLiteral("password"), m_pendingPassword);
            if (!m_pendingNickname.isEmpty()) {
                reg.insert(QStringLiteral("nickname"), m_pendingNickname);
            }
            m_client->sendJsonObject(reg);
        } else {
            QJsonObject login;
            login.insert(QStringLiteral("type"), QStringLiteral("auth_login"));
            login.insert(QStringLiteral("email"), m_pendingEmail);
            login.insert(QStringLiteral("password"), m_pendingPassword);
            m_client->sendJsonObject(login);
        }
        return;
    }
    if (t == QStringLiteral("auth_ok")) {
        if (m_authPhase != 2) {
            return;
        }
        const QString em = obj.value(QStringLiteral("email")).toString();
        showChatPage(em.isEmpty() ? m_pendingEmail : em);
        return;
    }
    if (t == QStringLiteral("error")) {
        if (m_authPhase == 0) {
            return;
        }
        const bool fromRegister = m_pendingRegister;
        m_authPhase = 0;
        m_pendingRegister = false;
        const QString msg = obj.value(QStringLiteral("message")).toString();
        m_client->disconnectFromServer();
        if (fromRegister && m_register) {
            m_register->showError(msg);
            m_stack->setCurrentWidget(m_register);
        } else {
            m_login->showError(msg);
            m_stack->setCurrentWidget(m_login);
        }
    }
}

void MainWindow::onTcpProtocolError(const QString &message)
{
    if (m_authPhase != 0) {
        const bool fromRegister = m_pendingRegister;
        m_authPhase = 0;
        m_pendingRegister = false;
        if (fromRegister && m_register) {
            m_register->showError(message);
            m_stack->setCurrentWidget(m_register);
        } else {
            m_login->showError(message);
            m_stack->setCurrentWidget(m_login);
        }
    }
}
