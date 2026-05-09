#include "ui/login_widget.h"

#include "style/app_style.h"
#include "utils/local_profile.h"

#include <QCheckBox>
#include <QDateTime>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPaintEvent>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QIntValidator>
#include <QVBoxLayout>

namespace {

constexpr int kAvatarSide = 96;

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

LoginWidget::LoginWidget(QWidget *parent)
    : QDialog(parent)
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
        m_loginByEmail = s.value(QStringLiteral("auth/prefer_email_login"), true).toBool();
    }
    setStyleSheet(AppStyle::loginWidgetStyle());
    applyLoginModeUi();
    updateWelcomeTitle();
}

void LoginWidget::buildUi()
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    outer->addStretch(1);

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("loginCard"));
    card->setFixedWidth(350);
    card->setMinimumHeight(420);

    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(32, 40, 32, 40);
    cardLay->setSpacing(16);

    m_avatarLabel = new QLabel(card);
    m_avatarLabel->setObjectName(QStringLiteral("loginAvatar"));
    m_avatarLabel->setFixedSize(kAvatarSide, kAvatarSide);
    m_avatarLabel->setAlignment(Qt::AlignCenter);
    m_avatarLabel->setText(QStringLiteral("💬"));
    m_avatarLabel->setStyleSheet(QStringLiteral(
        "QLabel#loginAvatar { background: #e6f7ff; border-radius: %1px; font-size: 44px; color: #12b7f5; }")
                                       .arg(kAvatarSide / 2));
    cardLay->addWidget(m_avatarLabel, 0, Qt::AlignHCenter);
    cardLay->addSpacing(4);

    m_titleLabel = new QLabel(QStringLiteral("欢迎登录"), card);
    m_titleLabel->setObjectName(QStringLiteral("loginTitle"));
    m_titleLabel->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(m_titleLabel);

    m_subtitleLabel = new QLabel(card);
    m_subtitleLabel->setObjectName(QStringLiteral("loginSubtitle"));
    m_subtitleLabel->setAlignment(Qt::AlignCenter);
    cardLay->addWidget(m_subtitleLabel);

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

    auto *pwdRow = new QHBoxLayout();
    pwdRow->setSpacing(8);
    m_password = new QLineEdit(card);
    m_password->setObjectName(QStringLiteral("loginInput"));
    m_password->setPlaceholderText(QStringLiteral("密码"));
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setMinimumHeight(40);
    pwdRow->addWidget(m_password, 1);
    m_passwordVisibleCheck = new QCheckBox(QStringLiteral("显示"), card);
    m_passwordVisibleCheck->setObjectName(QStringLiteral("passwordVisibleCheck"));
    m_passwordVisibleCheck->setFocusPolicy(Qt::ClickFocus);
    pwdRow->addWidget(m_passwordVisibleCheck, 0, Qt::AlignVCenter);
    cardLay->addLayout(pwdRow);

    m_remember = new QCheckBox(QStringLiteral("记住密码"), card);
    m_remember->setObjectName(QStringLiteral("loginRemember"));
    auto *rememberRow = new QHBoxLayout();
    rememberRow->setContentsMargins(0, 0, 0, 0);
    rememberRow->addWidget(m_remember, 0, Qt::AlignLeft);
    rememberRow->addStretch(1);
    m_toggleLoginModeBtn = new QPushButton(card);
    m_toggleLoginModeBtn->setObjectName(QStringLiteral("loginTextButton"));
    m_toggleLoginModeBtn->setFlat(true);
    m_toggleLoginModeBtn->setCursor(Qt::PointingHandCursor);
    rememberRow->addWidget(m_toggleLoginModeBtn, 0, Qt::AlignRight);
    cardLay->addLayout(rememberRow);
    connect(m_toggleLoginModeBtn, &QPushButton::clicked, this, &LoginWidget::onToggleLoginMode);

    auto *loginBtn = new QPushButton(QStringLiteral("登录"), card);
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
    connect(m_email, &QLineEdit::textChanged, this, &LoginWidget::onEmailTextChanged);

    refreshAvatarFromLocalProfile(m_email ? m_email->text().trimmed() : QString());
    connect(m_passwordVisibleCheck, &QCheckBox::toggled, this, &LoginWidget::onPasswordVisibleToggled);

    for (QLineEdit *ed : {m_serverHost, m_serverPort, m_email, m_password}) {
        ed->installEventFilter(this);
    }
}

void LoginWidget::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    AppStyle::paintAuthPageBackground(&p, rect());
}

void LoginWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        event->ignore();
        return;
    }
    QDialog::keyPressEvent(event);
}

void LoginWidget::updateWelcomeTitle()
{
    if (!m_titleLabel) {
        return;
    }
    const QString account = m_email ? m_email->text().trimmed() : QString();
    const QString greet = timeGreetingWord();
    if (account.isEmpty()) {
        m_titleLabel->setText(QStringLiteral("欢迎登录"));
        return;
    }
    QString local = account;
    if (m_loginByEmail) {
        if (const int at = account.indexOf(QLatin1Char('@')); at > 0) {
            local = account.left(at);
        }
    }
    m_titleLabel->setText(QStringLiteral("%1，%2，欢迎登录").arg(local, greet));
}

void LoginWidget::onEmailTextChanged(const QString &)
{
    updateWelcomeTitle();
    const QString key = m_loginByEmail ? (m_email ? m_email->text().trimmed() : QString()) : QString();
    refreshAvatarFromLocalProfile(key);
}

void LoginWidget::onPasswordVisibleToggled(bool checked)
{
    if (!m_password) {
        return;
    }
    m_password->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password);
}

bool LoginWidget::cycleFieldFocusWithArrow(QLineEdit *current, int key)
{
    QLineEdit *const chain[] = {m_serverHost, m_serverPort, m_email, m_password};
    constexpr int n = 4;
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

void LoginWidget::applyCtrlHToPasswordField()
{
    if (!m_password || !m_passwordVisibleCheck) {
        return;
    }
    const bool toPlain = (m_password->echoMode() == QLineEdit::Password);
    m_password->setEchoMode(toPlain ? QLineEdit::Normal : QLineEdit::Password);
    QSignalBlocker b(m_passwordVisibleCheck);
    m_passwordVisibleCheck->setChecked(m_password->echoMode() == QLineEdit::Normal);
}

bool LoginWidget::eventFilter(QObject *watched, QEvent *event)
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
        if (le == m_password && ke->modifiers() == Qt::ControlModifier && ke->key() == Qt::Key_H) {
            applyCtrlHToPasswordField();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void LoginWidget::clearError()
{
    if (m_errorLabel) {
        m_errorLabel->clear();
        m_errorLabel->setStyleSheet(QString());
        m_errorLabel->setVisible(false);
    }
}

void LoginWidget::showError(const QString &message)
{
    if (m_errorLabel) {
        m_errorLabel->setStyleSheet(QString());
        m_errorLabel->setText(message);
        m_errorLabel->setVisible(true);
    }
}

void LoginWidget::onLoginClicked()
{
    clearError();
    const QString account = m_email->text().trimmed();
    const QString pwd = m_password->text();
    if (account.isEmpty()) {
        showError(m_loginByEmail ? QStringLiteral("请输入邮箱") : QStringLiteral("请输入用户名"));
        return;
    }
    if (pwd.isEmpty()) {
        showError(QStringLiteral("请输入密码"));
        return;
    }
    if (m_loginByEmail) {
        if (!account.contains(QLatin1Char('@'))) {
            showError(QStringLiteral("邮箱格式不正确"));
            return;
        }
    } else {
        if (!isValidUsername(account)) {
            showError(QStringLiteral("用户名为 2～32 位，仅允许中英文或数字"));
            return;
        }
    }
    emit loginRequested(account, pwd, m_loginByEmail ? QStringLiteral("email") : QStringLiteral("username"));
}

void LoginWidget::onToggleLoginMode()
{
    m_loginByEmail = !m_loginByEmail;
    {
        QSettings s;
        s.setValue(QStringLiteral("auth/prefer_email_login"), m_loginByEmail);
    }
    applyLoginModeUi();
    updateWelcomeTitle();
    onEmailTextChanged({});
}

void LoginWidget::applyLoginModeUi()
{
    if (!m_email || !m_subtitleLabel || !m_toggleLoginModeBtn) {
        return;
    }
    if (m_loginByEmail) {
        m_email->setPlaceholderText(QStringLiteral("邮箱"));
        m_subtitleLabel->setText(QStringLiteral("使用邮箱登录局域网聊天"));
        m_toggleLoginModeBtn->setText(QStringLiteral("切换到用户名登录"));
    } else {
        m_email->setPlaceholderText(QStringLiteral("用户名"));
        m_subtitleLabel->setText(QStringLiteral("使用注册时的用户名登录（老账号请用邮箱）"));
        m_toggleLoginModeBtn->setText(QStringLiteral("切换到邮箱登录"));
    }
}

bool LoginWidget::isValidUsername(const QString &text)
{
    const QString t = text.trimmed();
    static const QRegularExpression re(QStringLiteral(R"(^[\p{L}\p{N}]{2,32}$)"),
                                       QRegularExpression::UseUnicodePropertiesOption);
    return re.match(t).hasMatch();
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

void LoginWidget::refreshAvatarFromLocalProfile(const QString &emailForKey)
{
    if (!m_avatarLabel) {
        return;
    }
    const QString e = emailForKey.trimmed();
    if (e.isEmpty()) {
        m_avatarLabel->clear();
        m_avatarLabel->setPixmap(QPixmap());
        m_avatarLabel->setText(QStringLiteral("💬"));
        m_avatarLabel->setStyleSheet(QStringLiteral(
            "QLabel#loginAvatar { background: #e6f7ff; border-radius: %1px; font-size: 44px; color: #12b7f5; }")
                                           .arg(kAvatarSide / 2));
        return;
    }
    QPixmap circ;
    if (LocalProfile::loadAvatarPixmap(e, kAvatarSide, &circ) && !circ.isNull()) {
        m_avatarLabel->clear();
        m_avatarLabel->setText(QString());
        m_avatarLabel->setStyleSheet(QStringLiteral(
            "QLabel#loginAvatar { background: transparent; border-radius: %1px; }").arg(kAvatarSide / 2));
        m_avatarLabel->setPixmap(circ);
        return;
    }
    m_avatarLabel->clear();
    m_avatarLabel->setPixmap(QPixmap());
    m_avatarLabel->setText(QStringLiteral("💬"));
    m_avatarLabel->setStyleSheet(QStringLiteral(
        "QLabel#loginAvatar { background: #e6f7ff; border-radius: %1px; font-size: 44px; color: #12b7f5; }")
                                       .arg(kAvatarSide / 2));
}
