#include "ui/profile_dialog.h"

#include "net/lan_tcp_client.h"
#include "style/app_style.h"
#include "utils/avatar_utils.h"
#include "utils/local_profile.h"

#include <QByteArray>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QJsonObject>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QTextCursor>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int kNickMax = 36;
constexpr int kBioMax = 80;
constexpr int kAvatarPreview = 96;

} // namespace

ProfileDialog::ProfileDialog(const QString &accountEmail, const QString &username, LanTcpClient *tcp,
                             const QString &sessionToken, QWidget *parent)
    : QDialog(parent)
    , m_email(accountEmail.trimmed())
    , m_username(username.trimmed())
    , m_tcp(tcp)
    , m_sessionToken(sessionToken)
{
    setObjectName(QStringLiteral("ProfileDialog"));
    setWindowTitle(QStringLiteral("编辑资料"));
    setModal(true);
    setMinimumWidth(420);
    setStyleSheet(AppStyle::profileDialogStyle());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(24, 20, 24, 20);
    root->setSpacing(0);

    auto *title = new QLabel(QStringLiteral("编辑资料"), this);
    title->setObjectName(QStringLiteral("profileDlgTitle"));
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);
    root->addSpacing(20);

    m_avatarLabel = new QLabel(this);
    m_avatarLabel->setObjectName(QStringLiteral("profileAvatar"));
    m_avatarLabel->setFixedSize(kAvatarPreview, kAvatarPreview);
    m_avatarLabel->setAlignment(Qt::AlignCenter);

    auto *avatarRow = new QHBoxLayout();
    avatarRow->addStretch(1);
    avatarRow->addWidget(m_avatarLabel, 0, Qt::AlignCenter);
    avatarRow->addStretch(1);
    root->addLayout(avatarRow);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto *pickBtn = new QPushButton(QStringLiteral("更换头像"), this);
    pickBtn->setObjectName(QStringLiteral("profileSecondaryBtn"));
    pickBtn->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(pickBtn);
    btnRow->addStretch(1);
    root->addLayout(btnRow);
    root->addSpacing(16);

    const auto addFieldRow = [this](const QString &labelText, QWidget *field, QLabel **counterOut) -> QWidget * {
        auto *wrap = new QWidget(this);
        auto *h = new QHBoxLayout(wrap);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(12);
        auto *lab = new QLabel(labelText, wrap);
        lab->setObjectName(QStringLiteral("profileFieldLabel"));
        lab->setFixedWidth(52);
        h->addWidget(lab, 0, Qt::AlignTop);
        h->addWidget(field, 1);
        if (counterOut) {
            *counterOut = new QLabel(wrap);
            (*counterOut)->setObjectName(QStringLiteral("profileCounter"));
            (*counterOut)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            h->addWidget(*counterOut, 0, Qt::AlignTop);
        }
        return wrap;
    };

    m_accountEdit = new QLineEdit(this);
    m_accountEdit->setObjectName(QStringLiteral("profileReadOnlyField"));
    m_accountEdit->setReadOnly(true);
    m_accountEdit->setText(m_email);
    m_accountEdit->setToolTip(m_email);
    root->addWidget(addFieldRow(QStringLiteral("账号"), m_accountEdit, nullptr));
    root->addSpacing(10);

    m_usernameEdit = new QLineEdit(this);
    m_usernameEdit->setObjectName(QStringLiteral("profileReadOnlyField"));
    m_usernameEdit->setReadOnly(true);
    m_usernameEdit->setText(m_username.isEmpty() ? QStringLiteral("—") : m_username);
    m_usernameEdit->setToolTip(m_username.isEmpty() ? QString() : m_username);
    root->addWidget(addFieldRow(QStringLiteral("用户名"), m_usernameEdit, nullptr));
    root->addSpacing(10);

    m_nicknameEdit = new QLineEdit(this);
    m_nicknameEdit->setObjectName(QStringLiteral("profileEditField"));
    m_nicknameEdit->setMaxLength(kNickMax);
    m_nicknameEdit->setPlaceholderText(QStringLiteral("昵称"));
    root->addWidget(addFieldRow(QStringLiteral("昵称"), m_nicknameEdit, &m_nickCountLabel));
    root->addSpacing(10);

    m_bioEdit = new QPlainTextEdit(this);
    m_bioEdit->setObjectName(QStringLiteral("profileBioEdit"));
    m_bioEdit->setPlaceholderText(QStringLiteral("一句话介绍自己"));
    m_bioEdit->setFixedHeight(88);
    m_bioEdit->setTabChangesFocus(true);
    root->addWidget(addFieldRow(QStringLiteral("个签"), m_bioEdit, &m_bioCountLabel));

    connect(m_nicknameEdit, &QLineEdit::textChanged, this, &ProfileDialog::onNicknameChanged);
    connect(m_bioEdit, &QPlainTextEdit::textChanged, this, &ProfileDialog::onBioChanged);
    connect(pickBtn, &QPushButton::clicked, this, &ProfileDialog::onPickAvatar);

    root->addSpacing(20);

    auto *bbox = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    bbox->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
    bbox->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    bbox->button(QDialogButtonBox::Save)->setObjectName(QStringLiteral("profilePrimaryBtn"));
    bbox->button(QDialogButtonBox::Cancel)->setObjectName(QStringLiteral("profileSecondaryBtn"));
    connect(bbox, &QDialogButtonBox::accepted, this, &ProfileDialog::onSaveClicked);
    connect(bbox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bbox, 0, Qt::AlignRight);

    loadFromLocal();
    refreshAvatarPreview();
    refreshCounters();
}

bool ProfileDialog::pushNicknameToServer(const QString &nickname, QString *errOut)
{
    if (!m_tcp || !m_tcp->isConnected() || m_sessionToken.isEmpty()) {
        return true;
    }
    const qint64 corr = QRandomGenerator::global()->bounded(1, 0x7ffffffe);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setParent(&loop);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    bool ok = false;
    QString err;
    const QMetaObject::Connection con = QObject::connect(
        m_tcp, &LanTcpClient::jsonReceived, &loop,
        [corr, &ok, &err, &loop, &timer](const QJsonObject &o) {
            const QString ty = o.value(QStringLiteral("type")).toString();
            if (ty == QStringLiteral("profile_set_ok")) {
                if (o.value(QStringLiteral("corr")).toVariant().toLongLong() != corr) {
                    return;
                }
                ok = true;
                timer.stop();
                loop.quit();
                return;
            }
            if (ty == QStringLiteral("error")) {
                if (o.value(QStringLiteral("corr")).toVariant().toLongLong() != corr) {
                    return;
                }
                err = o.value(QStringLiteral("message")).toString();
                timer.stop();
                loop.quit();
            }
        });

    QJsonObject req;
    req.insert(QStringLiteral("type"), QStringLiteral("profile_set"));
    req.insert(QStringLiteral("token"), m_sessionToken);
    req.insert(QStringLiteral("nickname"), nickname);
    req.insert(QStringLiteral("corr"), corr);
    m_tcp->sendJsonObject(req);

    timer.start(12000);
    loop.exec();
    QObject::disconnect(con);
    if (ok) {
        return true;
    }
    if (errOut) {
        *errOut = err.isEmpty() ? QStringLiteral("同步昵称超时，请检查网络后重试") : err;
    }
    return false;
}

bool ProfileDialog::pushAvatarJpegFileToServer(QString *errOut)
{
    if (!m_tcp || !m_tcp->isConnected() || m_sessionToken.isEmpty()) {
        return true;
    }
    const QString path = LocalProfile::avatarFilePath(m_email);
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        return true;
    }
    const QByteArray jpeg = f.readAll();
    if (jpeg.isEmpty()) {
        return true;
    }
    const qint64 corr = QRandomGenerator::global()->bounded(1, 0x7ffffffe);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    timer.setParent(&loop);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    bool ok = false;
    QString err;
    const QMetaObject::Connection con = QObject::connect(
        m_tcp, &LanTcpClient::jsonReceived, &loop,
        [corr, &ok, &err, &loop, &timer](const QJsonObject &o) {
            const QString ty = o.value(QStringLiteral("type")).toString();
            if (ty == QStringLiteral("profile_set_avatar_ok")) {
                if (o.value(QStringLiteral("corr")).toVariant().toLongLong() != corr) {
                    return;
                }
                ok = true;
                timer.stop();
                loop.quit();
                return;
            }
            if (ty == QStringLiteral("error")) {
                if (o.value(QStringLiteral("corr")).toVariant().toLongLong() != corr) {
                    return;
                }
                err = o.value(QStringLiteral("message")).toString();
                timer.stop();
                loop.quit();
            }
        });

    QJsonObject req;
    req.insert(QStringLiteral("type"), QStringLiteral("profile_set_avatar"));
    req.insert(QStringLiteral("token"), m_sessionToken);
    req.insert(QStringLiteral("corr"), corr);
    req.insert(QStringLiteral("avatar_b64"), QString::fromLatin1(jpeg.toBase64()));
    m_tcp->sendJsonObject(req);

    timer.start(12000);
    loop.exec();
    QObject::disconnect(con);
    if (ok) {
        return true;
    }
    if (errOut) {
        *errOut = err.isEmpty() ? QStringLiteral("同步头像超时，请检查网络后重试") : err;
    }
    return false;
}

void ProfileDialog::loadFromLocal()
{
    LocalProfile::Data d;
    LocalProfile::load(m_email, &d);
    if (m_nicknameEdit) {
        m_nicknameEdit->setText(d.nickname);
    }
    if (m_bioEdit) {
        m_bioEdit->setPlainText(d.bio);
    }
    m_avatarState = AvatarState::Unchanged;
    m_newAvatarPixmap = QPixmap();
}

void ProfileDialog::refreshAvatarPreview()
{
    if (!m_avatarLabel) {
        return;
    }
    if (m_avatarState == AvatarState::NewImage && !m_newAvatarPixmap.isNull()) {
        m_avatarLabel->setText(QString());
        m_avatarLabel->setStyleSheet(QStringLiteral("QLabel#profileAvatar { background: transparent; }"));
        m_avatarLabel->setPixmap(makeCircularAvatar(m_newAvatarPixmap, kAvatarPreview));
        return;
    }
    QPixmap circ;
    if (LocalProfile::loadAvatarPixmap(m_email, kAvatarPreview, &circ) && !circ.isNull()) {
        m_avatarLabel->setText(QString());
        m_avatarLabel->setStyleSheet(QStringLiteral("QLabel#profileAvatar { background: transparent; }"));
        m_avatarLabel->setPixmap(circ);
        return;
    }
    m_avatarLabel->clear();
    m_avatarLabel->setPixmap(QPixmap());
    m_avatarLabel->setText(QStringLiteral("👤"));
    m_avatarLabel->setStyleSheet(QStringLiteral(
        "QLabel#profileAvatar { background: #e6f7ff; border-radius: %1px; font-size: 44px; }")
                                     .arg(kAvatarPreview / 2));
}

void ProfileDialog::refreshCounters()
{
    if (m_nickCountLabel && m_nicknameEdit) {
        const int n = m_nicknameEdit->text().length();
        m_nickCountLabel->setText(QStringLiteral("%1/%2").arg(n).arg(kNickMax));
    }
    if (m_bioCountLabel && m_bioEdit) {
        const int n = m_bioEdit->toPlainText().length();
        m_bioCountLabel->setText(QStringLiteral("%1/%2").arg(n).arg(kBioMax));
    }
}

void ProfileDialog::onNicknameChanged(const QString &text)
{
    Q_UNUSED(text);
    if (m_nicknameEdit && m_nicknameEdit->text().length() > kNickMax) {
        m_nicknameEdit->setText(m_nicknameEdit->text().left(kNickMax));
    }
    refreshCounters();
}

void ProfileDialog::onBioChanged()
{
    if (!m_bioEdit) {
        return;
    }
    QString t = m_bioEdit->toPlainText();
    if (t.length() > kBioMax) {
        t = t.left(kBioMax);
        const int pos = m_bioEdit->textCursor().position();
        m_bioEdit->setPlainText(t);
        QTextCursor c = m_bioEdit->textCursor();
        c.setPosition(qMin(pos, kBioMax));
        m_bioEdit->setTextCursor(c);
    }
    refreshCounters();
}

void ProfileDialog::onPickAvatar()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择头像"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    QPixmap pix;
    if (!pix.load(path)) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("无法加载该图片。"));
        return;
    }
    m_newAvatarPixmap = pix;
    m_avatarState = AvatarState::NewImage;
    refreshAvatarPreview();
}

void ProfileDialog::onSaveClicked()
{
    if (!m_nicknameEdit || !m_bioEdit) {
        return;
    }
    const QString nick = m_nicknameEdit->text();
    const QString bio = m_bioEdit->toPlainText();
    if (nick.length() > kNickMax || bio.length() > kBioMax) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("昵称或个签超出长度限制。"));
        return;
    }
    QString syncErr;
    if (!pushNicknameToServer(nick, &syncErr)) {
        QMessageBox::warning(this, QStringLiteral("同步失败"), syncErr);
        return;
    }
    if (!LocalProfile::saveMeta(m_email, nick, bio)) {
        QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("无法保存资料。"));
        return;
    }
    if (m_avatarState == AvatarState::NewImage && !m_newAvatarPixmap.isNull()) {
        if (!LocalProfile::saveAvatarImage(m_email, m_newAvatarPixmap)) {
            QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("无法保存头像文件。"));
            return;
        }
        QString avErr;
        if (!pushAvatarJpegFileToServer(&avErr)) {
            QMessageBox::warning(this, QStringLiteral("同步失败"), avErr);
            return;
        }
    }
    accept();
}
