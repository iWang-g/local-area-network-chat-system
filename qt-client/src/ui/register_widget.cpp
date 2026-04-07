#include "ui/register_widget.h"

#include "style/app_style.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QIntValidator>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

RegisterWidget::RegisterWidget(QWidget *parent)
    : QWidget(parent)
    , m_cooldownTimer(new QTimer(this))
{
    setObjectName(QStringLiteral("LanRegisterWidget"));
    m_cooldownTimer->setInterval(1000);
    connect(m_cooldownTimer, &QTimer::timeout, this, &RegisterWidget::onCooldownTick);
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

void RegisterWidget::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addStretch(1);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedWidth(380);
    card->setMinimumHeight(560);

    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(36, 32, 36, 32);
    cardLay->setSpacing(12);

    auto *avatarRow = new QHBoxLayout();
    avatarRow->addStretch(1);
    auto *avatar = new QLabel(card);
    avatar->setFixedSize(64, 64);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setStyleSheet(QStringLiteral(
        "QLabel { background: #e6f7ff; border-radius: 32px; font-size: 28px; color: #12b7f5; }"));
    avatar->setText(QStringLiteral("✎"));
    avatarRow->addWidget(avatar);
    avatarRow->addStretch(1);
    cardLay->addLayout(avatarRow);

    auto *title = new QLabel(QStringLiteral("创建账号"), card);
    title->setObjectName(QStringLiteral("loginTitle"));
    title->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(title);

    auto *sub = new QLabel(QStringLiteral("使用邮箱注册局域网聊天"), card);
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

    auto *codeRow = new QHBoxLayout();
    m_emailCode = new QLineEdit(card);
    m_emailCode->setObjectName(QStringLiteral("loginInput"));
    m_emailCode->setPlaceholderText(QStringLiteral("邮箱验证码"));
    m_emailCode->setClearButtonEnabled(true);
    codeRow->addWidget(m_emailCode, 1);
    m_sendCodeBtn = new QPushButton(QStringLiteral("获取验证码"), card);
    m_sendCodeBtn->setObjectName(QStringLiteral("loginSecondaryButton"));
    m_sendCodeBtn->setCursor(Qt::PointingHandCursor);
    m_sendCodeBtn->setMinimumHeight(38);
    m_sendCodeBtn->setFixedWidth(112);
    codeRow->addWidget(m_sendCodeBtn);
    cardLay->addLayout(codeRow);

    m_codeHintLabel = new QLabel(card);
    m_codeHintLabel->setObjectName(QStringLiteral("loginSubtitle"));
    m_codeHintLabel->setWordWrap(true);
    m_codeHintLabel->setAlignment(Qt::AlignCenter);
    m_codeHintLabel->setVisible(false);
    m_codeHintLabel->setStyleSheet(QStringLiteral("color: #52c41a; font-size: 12px;"));
    cardLay->addWidget(m_codeHintLabel);

    m_password = new QLineEdit(card);
    m_password->setObjectName(QStringLiteral("loginInput"));
    m_password->setPlaceholderText(QStringLiteral("密码"));
    m_password->setEchoMode(QLineEdit::Password);
    cardLay->addWidget(m_password);

    m_password2 = new QLineEdit(card);
    m_password2->setObjectName(QStringLiteral("loginInput"));
    m_password2->setPlaceholderText(QStringLiteral("确认密码"));
    m_password2->setEchoMode(QLineEdit::Password);
    cardLay->addWidget(m_password2);

    m_nickname = new QLineEdit(card);
    m_nickname->setObjectName(QStringLiteral("loginInput"));
    m_nickname->setPlaceholderText(QStringLiteral("昵称（可选）"));
    m_nickname->setClearButtonEnabled(true);
    cardLay->addWidget(m_nickname);

    auto *regBtn = new QPushButton(QStringLiteral("注 册"), card);
    regBtn->setObjectName(QStringLiteral("loginPrimaryButton"));
    regBtn->setCursor(Qt::PointingHandCursor);
    regBtn->setMinimumHeight(44);
    cardLay->addWidget(regBtn);

    auto *backBtn = new QPushButton(QStringLiteral("已有账号？返回登录"), card);
    backBtn->setObjectName(QStringLiteral("loginTextButton"));
    backBtn->setCursor(Qt::PointingHandCursor);
    cardLay->addWidget(backBtn, 0, Qt::AlignCenter);

    cardLay->addStretch(1);

    outer->addWidget(card, 0, Qt::AlignHCenter);
    outer->addStretch(1);

    connect(regBtn, &QPushButton::clicked, this, &RegisterWidget::onSubmitClicked);
    connect(backBtn, &QPushButton::clicked, this, &RegisterWidget::onBackClicked);
    connect(m_sendCodeBtn, &QPushButton::clicked, this, &RegisterWidget::onSendCodeClicked);
    connect(m_email, &QLineEdit::returnPressed, this, &RegisterWidget::onSubmitClicked);
    connect(m_password, &QLineEdit::returnPressed, this, &RegisterWidget::onSubmitClicked);
    connect(m_password2, &QLineEdit::returnPressed, this, &RegisterWidget::onSubmitClicked);
}

void RegisterWidget::clearError()
{
    if (m_errorLabel) {
        m_errorLabel->clear();
        m_errorLabel->setVisible(false);
    }
}

void RegisterWidget::showError(const QString &message)
{
    clearCodeSentHint();
    if (m_errorLabel) {
        m_errorLabel->setText(message);
        m_errorLabel->setVisible(true);
    }
}

void RegisterWidget::showCodeSentHint()
{
    if (m_codeHintLabel) {
        m_codeHintLabel->setText(
            QStringLiteral("验证码已生成。当前为离线演示：请查看服务端控制台或日志文件中的验证码。"));
        m_codeHintLabel->setVisible(true);
    }
}

void RegisterWidget::clearCodeSentHint()
{
    if (m_codeHintLabel) {
        m_codeHintLabel->clear();
        m_codeHintLabel->setVisible(false);
    }
}

void RegisterWidget::setServerFields(const QString &host, const QString &portText)
{
    if (m_serverHost) {
        m_serverHost->setText(host);
    }
    if (m_serverPort) {
        m_serverPort->setText(portText);
    }
}

QString RegisterWidget::serverHost() const
{
    return m_serverHost ? m_serverHost->text().trimmed() : QString();
}

quint16 RegisterWidget::serverPort() const
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

QString RegisterWidget::registerEmailInput() const
{
    return m_email ? m_email->text().trimmed() : QString();
}

void RegisterWidget::startResendCooldown(int seconds)
{
    if (!m_sendCodeBtn || !m_cooldownTimer) {
        return;
    }
    m_cooldownLeft = qMax(1, seconds);
    m_sendCodeBtn->setEnabled(false);
    m_sendCodeBtn->setText(QStringLiteral("%1s 后重发").arg(m_cooldownLeft));
    m_cooldownTimer->start();
}

void RegisterWidget::onCooldownTick()
{
    if (!m_sendCodeBtn) {
        return;
    }
    m_cooldownLeft--;
    if (m_cooldownLeft <= 0) {
        m_cooldownTimer->stop();
        m_sendCodeBtn->setEnabled(true);
        m_sendCodeBtn->setText(QStringLiteral("获取验证码"));
        return;
    }
    m_sendCodeBtn->setText(QStringLiteral("%1s 后重发").arg(m_cooldownLeft));
}

void RegisterWidget::onSendCodeClicked()
{
    clearError();
    const QString email = registerEmailInput();
    if (email.isEmpty()) {
        showError(QStringLiteral("请先填写邮箱"));
        return;
    }
    if (!email.contains(QLatin1Char('@'))) {
        showError(QStringLiteral("邮箱格式不正确"));
        return;
    }
    emit requestEmailCode(email);
}

void RegisterWidget::onSubmitClicked()
{
    clearError();
    const QString email = registerEmailInput();
    const QString pwd = m_password->text();
    const QString pwd2 = m_password2->text();
    const QString code = m_emailCode ? m_emailCode->text().trimmed() : QString();
    if (email.isEmpty()) {
        showError(QStringLiteral("请输入邮箱"));
        return;
    }
    if (code.isEmpty()) {
        showError(QStringLiteral("请输入邮箱验证码"));
        return;
    }
    if (pwd.isEmpty()) {
        showError(QStringLiteral("请输入密码"));
        return;
    }
    if (pwd != pwd2) {
        showError(QStringLiteral("两次输入的密码不一致"));
        return;
    }
    emit registerSubmitted(email, pwd, m_nickname ? m_nickname->text().trimmed() : QString(), code);
}

void RegisterWidget::onBackClicked()
{
    clearError();
    emit backToLoginRequested();
}
