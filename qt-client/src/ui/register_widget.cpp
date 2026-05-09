#include "ui/register_widget.h"

#include "style/app_style.h"

#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QPushButton>
#include <QIntValidator>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

namespace {

QString timeGreetingWord()
{
    const int hour = QDateTime::currentDateTime().time().hour();
    if (hour >= 6 && hour < 12) {
        return QStringLiteral("上午好");
    }
    if (hour >= 12 && hour < 18) {
        return QStringLiteral("下午好");
    }
    return QStringLiteral("晚上好");
}

} // namespace

RegisterWidget::RegisterWidget(QWidget *parent)
    : QDialog(parent)
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
    updateTitle();
}

void RegisterWidget::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addStretch(1);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedWidth(350);
    card->setMinimumHeight(560);

    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(32, 32, 32, 32);
    cardLay->setSpacing(14);

    auto *avatarRow = new QHBoxLayout();
    avatarRow->addStretch(1);
    auto *avatar = new QLabel(card);
    avatar->setObjectName(QStringLiteral("loginAvatar"));
    avatar->setFixedSize(72, 72);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setText(QStringLiteral("✎"));
    avatar->setStyleSheet(QStringLiteral(
        "QLabel#loginAvatar { background: #e6f7ff; border-radius: 36px; font-size: 32px; color: #12b7f5; }"));
    avatarRow->addWidget(avatar);
    avatarRow->addStretch(1);
    cardLay->addLayout(avatarRow);

    m_titleLabel = new QLabel(QStringLiteral("创建账号"), card);
    m_titleLabel->setObjectName(QStringLiteral("loginTitle"));
    m_titleLabel->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(m_titleLabel);

    auto *sub = new QLabel(QStringLiteral("使用邮箱注册局域网聊天"), card);
    sub->setObjectName(QStringLiteral("loginSubtitle"));
    sub->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(sub);

    m_errorLabel = new QLabel(card);
    m_errorLabel->setObjectName(QStringLiteral("loginErrorLabel"));
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setVisible(false);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(m_errorLabel);

    auto *srvRow = new QHBoxLayout();
    m_serverHost = new QLineEdit(card);
    m_serverHost->setObjectName(QStringLiteral("loginInput"));
    m_serverHost->setPlaceholderText(QStringLiteral("服务器地址"));
    m_serverHost->setClearButtonEnabled(true);
    m_serverHost->setMinimumHeight(40);
    srvRow->addWidget(m_serverHost, 1);
    m_serverPort = new QLineEdit(card);
    m_serverPort->setObjectName(QStringLiteral("loginInput"));
    m_serverPort->setPlaceholderText(QStringLiteral("端口"));
    m_serverPort->setFixedWidth(88);
    m_serverPort->setMinimumHeight(40);
    m_serverPort->setValidator(new QIntValidator(1, 65535, m_serverPort));
    srvRow->addWidget(m_serverPort);
    cardLay->addLayout(srvRow);

    m_email = new QLineEdit(card);
    m_email->setObjectName(QStringLiteral("loginInput"));
    m_email->setPlaceholderText(QStringLiteral("邮箱"));
    m_email->setClearButtonEnabled(true);
    m_email->setMinimumHeight(40);
    cardLay->addWidget(m_email);

    auto *codeRow = new QHBoxLayout();
    m_emailCode = new QLineEdit(card);
    m_emailCode->setObjectName(QStringLiteral("loginInput"));
    m_emailCode->setPlaceholderText(QStringLiteral("邮箱验证码"));
    m_emailCode->setClearButtonEnabled(true);
    m_emailCode->setMinimumHeight(40);
    codeRow->addWidget(m_emailCode, 1);
    m_sendCodeBtn = new QPushButton(QStringLiteral("获取验证码"), card);
    m_sendCodeBtn->setObjectName(QStringLiteral("loginSecondaryButton"));
    m_sendCodeBtn->setCursor(Qt::PointingHandCursor);
    m_sendCodeBtn->setMinimumHeight(40);
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

    auto *pwdRow = new QHBoxLayout();
    pwdRow->setSpacing(8);
    m_password = new QLineEdit(card);
    m_password->setObjectName(QStringLiteral("loginInput"));
    m_password->setPlaceholderText(QStringLiteral("密码"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setMinimumHeight(40);
    pwdRow->addWidget(m_password, 1);
    m_pwdVisibleCheck = new QCheckBox(QStringLiteral("显示"), card);
    m_pwdVisibleCheck->setObjectName(QStringLiteral("passwordVisibleCheck"));
    m_pwdVisibleCheck->setFocusPolicy(Qt::ClickFocus);
    pwdRow->addWidget(m_pwdVisibleCheck, 0, Qt::AlignVCenter);
    cardLay->addLayout(pwdRow);

    auto *pwd2Row = new QHBoxLayout();
    pwd2Row->setSpacing(8);
    m_password2 = new QLineEdit(card);
    m_password2->setObjectName(QStringLiteral("loginInput"));
    m_password2->setPlaceholderText(QStringLiteral("确认密码"));
    m_password2->setEchoMode(QLineEdit::Password);
    m_password2->setMinimumHeight(40);
    pwd2Row->addWidget(m_password2, 1);
    m_pwd2VisibleCheck = new QCheckBox(QStringLiteral("显示"), card);
    m_pwd2VisibleCheck->setObjectName(QStringLiteral("passwordVisibleCheck"));
    m_pwd2VisibleCheck->setFocusPolicy(Qt::ClickFocus);
    pwd2Row->addWidget(m_pwd2VisibleCheck, 0, Qt::AlignVCenter);
    cardLay->addLayout(pwd2Row);

    m_username = new QLineEdit(card);
    m_username->setObjectName(QStringLiteral("loginInput"));
    m_username->setPlaceholderText(QStringLiteral("用户名（中英文或数字，2～32 位）"));
    m_username->setClearButtonEnabled(true);
    m_username->setMinimumHeight(40);
    cardLay->addWidget(m_username);

    auto *regBtn = new QPushButton(QStringLiteral("注册"), card);
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
    connect(m_email, &QLineEdit::textChanged, this, &RegisterWidget::onEmailTextChanged);
    connect(m_pwdVisibleCheck, &QCheckBox::toggled, this, &RegisterWidget::onPwdVisibleToggled);
    connect(m_pwd2VisibleCheck, &QCheckBox::toggled, this, &RegisterWidget::onPwd2VisibleToggled);

    for (QLineEdit *ed :
         {m_serverHost, m_serverPort, m_email, m_emailCode, m_password, m_password2, m_username}) {
        ed->installEventFilter(this);
    }
}

void RegisterWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    AppStyle::paintAuthPageBackground(&p, rect());
}

void RegisterWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        event->ignore();
        return;
    }
    QDialog::keyPressEvent(event);
}

void RegisterWidget::updateTitle()
{
    if (!m_titleLabel) {
        return;
    }
    const QString email = m_email ? m_email->text().trimmed() : QString();
    const QString greet = timeGreetingWord();
    if (email.isEmpty()) {
        m_titleLabel->setText(QStringLiteral("创建账号"));
        return;
    }
    QString local = email;
    if (const int at = email.indexOf(QLatin1Char('@')); at > 0) {
        local = email.left(at);
    }
    m_titleLabel->setText(QStringLiteral("%1，%2，注册新账号").arg(local, greet));
}

void RegisterWidget::onEmailTextChanged(const QString &)
{
    updateTitle();
}

void RegisterWidget::onPwdVisibleToggled(bool checked)
{
    if (m_password) {
        m_password->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    }
}

void RegisterWidget::onPwd2VisibleToggled(bool checked)
{
    if (m_password2) {
        m_password2->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
    }
}

bool RegisterWidget::cycleFieldFocusWithArrow(QLineEdit *current, int key)
{
    QLineEdit *const chain[] = {m_serverHost, m_serverPort, m_email, m_emailCode,
                                m_password, m_password2, m_username};
    constexpr int n = 7;
    int idx = -1;
    for (int i = 0; i < n; ++i) {
        if (chain[i] == current) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return false;
    }
    const int delta = (key == Qt::Key_Down) ? 1 : -1;
    const int next = (idx + delta + n) % n;
    chain[next]->setFocus(Qt::ShortcutFocusReason);
    return true;
}

void RegisterWidget::applyCtrlHToFocusedPassword(QLineEdit *field)
{
    if (field == m_password && m_pwdVisibleCheck) {
        const bool toPlain = (m_password->echoMode() == QLineEdit::Password);
        m_password->setEchoMode(toPlain ? QLineEdit::Normal : QLineEdit::Password);
        QSignalBlocker b(m_pwdVisibleCheck);
        m_pwdVisibleCheck->setChecked(m_password->echoMode() == QLineEdit::Normal);
        return;
    }
    if (field == m_password2 && m_pwd2VisibleCheck) {
        const bool toPlain = (m_password2->echoMode() == QLineEdit::Password);
        m_password2->setEchoMode(toPlain ? QLineEdit::Normal : QLineEdit::Password);
        QSignalBlocker b(m_pwd2VisibleCheck);
        m_pwd2VisibleCheck->setChecked(m_password2->echoMode() == QLineEdit::Normal);
    }
}

bool RegisterWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        auto *le = qobject_cast<QLineEdit *>(watched);
        if (le && ke->modifiers() == Qt::NoModifier) {
            if (ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down) {
                if (cycleFieldFocusWithArrow(le, ke->key())) {
                    return true;
                }
            }
        }
        if (le && ke->modifiers() == Qt::ControlModifier && ke->key() == Qt::Key_H) {
            if (le == m_password || le == m_password2) {
                applyCtrlHToFocusedPassword(le);
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void RegisterWidget::clearError()
{
    if (m_errorLabel) {
        m_errorLabel->clear();
        m_errorLabel->setStyleSheet(QString());
        m_errorLabel->setVisible(false);
    }
}

void RegisterWidget::showError(const QString &message)
{
    clearCodeSentHint();
    if (m_errorLabel) {
        m_errorLabel->setStyleSheet(QString());
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
    const QString uname = m_username ? m_username->text().trimmed() : QString();
    if (uname.isEmpty()) {
        showError(QStringLiteral("请填写用户名"));
        return;
    }
    if (!isValidUsername(uname)) {
        showError(QStringLiteral("用户名为 2～32 位，仅允许中英文或数字"));
        return;
    }
    emit registerSubmitted(email, pwd, uname, code);
}

void RegisterWidget::onBackClicked()
{
    clearError();
    emit backToLoginRequested();
}

bool RegisterWidget::isValidUsername(const QString &text)
{
    const QString t = text.trimmed();
    static const QRegularExpression re(QStringLiteral(R"(^[\p{L}\p{N}]{2,32}$)"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    return re.match(t).hasMatch();
}
