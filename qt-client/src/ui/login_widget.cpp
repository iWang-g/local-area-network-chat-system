#include "ui/login_widget.h"

#include "style/app_style.h"

#include <QCheckBox>
#include <QSettings>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QIntValidator>
#include <QVBoxLayout>

LoginWidget::LoginWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("LanLoginWidget"));
    buildUi();
    {
        QSettings s;
        if (m_serverHost) {
            m_serverHost->setText(s.value(QStringLiteral("server/host"), QStringLiteral("127.0.0.1")).toString());
        }
        if (m_serverPort) {
            m_serverPort->setText(s.value(QStringLiteral("server/port"), QStringLiteral("28888")).toString());
        }
    }
    setStyleSheet(AppStyle::loginWidgetStyle());
}

void LoginWidget::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addStretch(1);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedWidth(380);
    card->setMinimumHeight(400);

    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(36, 40, 36, 36);
    cardLay->setSpacing(14);

    auto *avatarRow = new QHBoxLayout();
    avatarRow->addStretch(1);
    auto *avatar = new QLabel(card);
    avatar->setFixedSize(72, 72);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setStyleSheet(QStringLiteral(
        "QLabel { background: #e6f7ff; border-radius: 36px; font-size: 32px; color: #12b7f5; }"));
    avatar->setText(QStringLiteral("💬"));
    avatarRow->addWidget(avatar);
    avatarRow->addStretch(1);
    cardLay->addLayout(avatarRow);

    auto *title = new QLabel(QStringLiteral("欢迎登录"), card);
    title->setObjectName(QStringLiteral("loginTitle"));
    title->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(title);

    auto *sub = new QLabel(QStringLiteral("使用邮箱登录局域网聊天"), card);
    sub->setObjectName(QStringLiteral("loginSubtitle"));
    sub->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(sub);

    auto *srvRow = new QHBoxLayout();
    m_serverHost = new QLineEdit(card);
    m_serverHost->setObjectName(QStringLiteral("loginInput"));
    m_serverHost->setPlaceholderText(QStringLiteral("服务器地址"));
    m_serverHost->setClearButtonEnabled(true);
    srvRow->addWidget(m_serverHost, 1);
    m_serverPort = new QLineEdit(card);
    m_serverPort->setObjectName(QStringLiteral("loginInput"));
    m_serverPort->setPlaceholderText(QStringLiteral("端口"));
    m_serverPort->setFixedWidth(88);
    m_serverPort->setValidator(new QIntValidator(1, 65535, m_serverPort));
    srvRow->addWidget(m_serverPort);
    cardLay->addLayout(srvRow);

    m_errorLabel = new QLabel(card);
    m_errorLabel->setObjectName(QStringLiteral("loginErrorLabel"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(m_errorLabel);

    m_email = new QLineEdit(card);
    m_email->setObjectName(QStringLiteral("loginInput"));
    m_email->setPlaceholderText(QStringLiteral("邮箱"));
    m_email->setClearButtonEnabled(true);
    cardLay->addWidget(m_email);

    m_password = new QLineEdit(card);
    m_password->setObjectName(QStringLiteral("loginInput"));
    m_password->setPlaceholderText(QStringLiteral("密码"));
    m_password->setEchoMode(QLineEdit::Password);
    cardLay->addWidget(m_password);

    m_remember = new QCheckBox(QStringLiteral("记住密码"), card);
    m_remember->setObjectName(QStringLiteral("loginRemember"));
    cardLay->addWidget(m_remember);

    auto *loginBtn = new QPushButton(QStringLiteral("登 录"), card);
    loginBtn->setObjectName(QStringLiteral("loginPrimaryButton"));
    loginBtn->setCursor(Qt::PointingHandCursor);
    loginBtn->setMinimumHeight(44);
    cardLay->addWidget(loginBtn);

    auto *regBtn = new QPushButton(QStringLiteral("注册账号"), card);
    regBtn->setObjectName(QStringLiteral("loginTextButton"));
    regBtn->setCursor(Qt::PointingHandCursor);
    cardLay->addWidget(regBtn, 0, Qt::AlignCenter);

    cardLay->addStretch(1);

    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch(1);

    connect(loginBtn, &QPushButton::clicked, this, &LoginWidget::onLoginClicked);
    connect(regBtn, &QPushButton::clicked, this, &LoginWidget::onRegisterClicked);
    connect(m_email, &QLineEdit::returnPressed, this, &LoginWidget::onLoginClicked);
    connect(m_password, &QLineEdit::returnPressed, this, &LoginWidget::onLoginClicked);
}

void LoginWidget::clearError()
{
    if (m_errorLabel) {
        m_errorLabel->clear();
        m_errorLabel->setVisible(false);
    }
}

void LoginWidget::showError(const QString &message)
{
    if (m_errorLabel) {
        m_errorLabel->setText(message);
        m_errorLabel->setVisible(true);
    }
}

void LoginWidget::onLoginClicked()
{
    clearError();
    const QString email = m_email->text().trimmed();
    const QString pwd = m_password->text();
    if (email.isEmpty()) {
        showError(QStringLiteral("请输入邮箱"));
        return;
    }
    if (pwd.isEmpty()) {
        showError(QStringLiteral("请输入密码"));
        return;
    }
    emit loginRequested(email, pwd);
}

void LoginWidget::onRegisterClicked()
{
    clearError();
    emit registerRequested();
}

QString LoginWidget::serverHost() const
{
    return m_serverHost ? m_serverHost->text().trimmed() : QString();
}

quint16 LoginWidget::serverPort() const
{
    if (!m_serverPort) {
        return 28888;
    }
    bool ok = false;
    const int p = m_serverPort->text().trimmed().toInt(&ok);
    if (!ok || p <= 0 || p > 65535) {
        return 28888;
    }
    return static_cast<quint16>(p);
}

QString LoginWidget::emailInput() const
{
    return m_email ? m_email->text().trimmed() : QString();
}

QString LoginWidget::passwordInput() const
{
    return m_password ? m_password->text() : QString();
}

void LoginWidget::setServerFields(const QString &host, const QString &portText)
{
    if (m_serverHost) {
        m_serverHost->setText(host);
    }
    if (m_serverPort) {
        m_serverPort->setText(portText);
    }
}
