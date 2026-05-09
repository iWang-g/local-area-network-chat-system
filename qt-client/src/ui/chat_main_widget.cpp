#include "ui/chat_main_widget.h"

#include "net/lan_tcp_client.h"
#include "style/app_style.h"
#include "ui/add_friend_search_dialog.h"
#include "ui/chat_history_dialog.h"
#include "ui/contacts_widget.h"
#include "ui/image_preview_dialog.h"
#include "ui/sticker_picker_popup.h"
#include "utils/avatar_utils.h"
#include "utils/local_profile.h"

#include <QByteArray>
#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QEvent>
#include <QMouseEvent>
#include <QObject>
#include <QDebug>
#include <QSignalBlocker>
#include <QSettings>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QAction>
#include <QMenu>
#include <QUrl>

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMovie>
#include <QMessageBox>
#include <QStandardPaths>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QIcon>
#include <QListWidgetItem>
#include <QPixmap>
#include <QStringList>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QPlainTextEdit>

#include <limits>

namespace {

/// 会话区（`m_leftStack`）宽度范围，与分割器中间列一致。
constexpr int kSessionColumnMinWidth = 240;
constexpr int kSessionColumnMaxWidth = 340;

constexpr qint64 kMaxFileTransferBytes = 512LL * 1024 * 1024;
/// 与 `vsserver::kErrPeerOffline` 一致。
constexpr int kErrPeerOffline = 5009;

bool isImageFileNameString(const QString &name)
{
    const QString suf = QFileInfo(name).suffix().toLower();
    static const QStringList kImageSuffixes = {QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
                                               QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
                                               QStringLiteral("ico"),  QStringLiteral("heic"), QStringLiteral("heif")};
    return kImageSuffixes.contains(suf);
}

constexpr int kStickerDisplayMaxPx = 120;

bool fileContentJsonIsSticker(const QJsonObject &o)
{
    return o.value(QStringLiteral("as_sticker")).toBool() || o.value(QStringLiteral("sticker")).toBool();
}

/// 从 JSON 读取 64 位整数（兼容 number / string），避免仅依赖 `toVariant().toLongLong()` 在字符串等类型下得到 0。
/// 说明：Qt 6.5 的 QJsonValue 尚无 `isInteger()`；JSON 整型一般以 Double 存储，超大整数可能受 double 精度限制。
static qint64 jsonInt64(const QJsonValue &v, qint64 def = 0)
{
    if (v.isUndefined() || v.isNull()) {
        return def;
    }
    if (v.isDouble()) {
        const double d = v.toDouble();
        if (d > static_cast<double>((std::numeric_limits<qint64>::max)())
            || d < static_cast<double>((std::numeric_limits<qint64>::min)())) {
            return def;
        }
        return static_cast<qint64>(d);
    }
    if (v.isString()) {
        bool ok = false;
        const qint64 x = v.toString().trimmed().toLongLong(&ok);
        return ok ? x : def;
    }
    bool ok = false;
    const qint64 x = v.toVariant().toLongLong(&ok);
    return ok ? x : def;
}

static qint64 jsonInt64Member(const QJsonObject &o, const QString &key, qint64 def = 0)
{
    return jsonInt64(o.value(key), def);
}

/// 在气泡内展示表情图：GIF 用 QMovie，静态图缩放；`movieParent` 用于托管 QMovie 生命周期。
static void applyStickerImageToLabel(QLabel *imgLb, const QString &path, QObject *movieParent)
{
    if (!imgLb || path.isEmpty() || !QFile::exists(path) || !movieParent) {
        if (!path.isEmpty()) {
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] applySticker skip path=%1 exists=%2 label=%3 "
                                                 "parent=%4")
                                      .arg(path)
                                      .arg(QFile::exists(path) ? 1 : 0)
                                      .arg(imgLb ? 1 : 0)
                                      .arg(movieParent ? 1 : 0);
        }
        return;
    }
    imgLb->clear();
    imgLb->setStyleSheet(QString());
    if (path.endsWith(QLatin1String(".gif"), Qt::CaseInsensitive)) {
        auto *mov = new QMovie(path, QByteArray(), movieParent);
        mov->setCacheMode(QMovie::CacheAll);
        mov->jumpToFrame(0);
        const QSize fs = mov->frameRect().size();
        if (fs.isValid() && (fs.width() > kStickerDisplayMaxPx || fs.height() > kStickerDisplayMaxPx)) {
            mov->setScaledSize(fs.scaled(kStickerDisplayMaxPx, kStickerDisplayMaxPx, Qt::KeepAspectRatio));
        }
        imgLb->setMovie(mov);
        mov->start();
        if (!mov->isValid()) {
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] QMovie invalid path=%1 err=%2")
                                      .arg(path, mov->lastErrorString());
        }
    } else {
        QPixmap pm;
        if (pm.load(path)) {
            imgLb->setPixmap(pm.scaled(kStickerDisplayMaxPx, kStickerDisplayMaxPx, Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
        } else {
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] QPixmap load failed path=%1").arg(path);
        }
    }
}

bool parseFileOrImageMessage(const QString &content, QJsonObject *out)
{
    const QByteArray utf8 = content.trimmed().toUtf8();
    if (utf8.isEmpty() || utf8.at(0) != '{') {
        return false;
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(utf8, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    const QJsonObject o = doc.object();
    const QString kind = o.value(QStringLiteral("kind")).toString();
    if (kind != QStringLiteral("file") && kind != QStringLiteral("image")) {
        return false;
    }
    *out = o;
    return true;
}

QString formatSessionPreviewText(const QString &rawContent)
{
    const QString s = rawContent.trimmed();
    QJsonObject fo;
    if (parseFileOrImageMessage(s, &fo)) {
        if (fileContentJsonIsSticker(fo)) {
            return QStringLiteral("[表情]");
        }
        const QString kind = fo.value(QStringLiteral("kind")).toString();
        const QString name = fo.value(QStringLiteral("name")).toString();
        if (kind == QStringLiteral("image") || isImageFileNameString(name)) {
            return QStringLiteral("[图片]");
        }
        return QStringLiteral("[文件] %1").arg(name.isEmpty() ? QStringLiteral("文件") : name);
    }
    /// 好友列表 `last_message_preview` 曾在服务端被截成短串，整段 JSON 无法 parse；用子串兜底避免会话列表露出原始 JSON。
    if (s.startsWith(QLatin1Char('{')) && s.contains(QLatin1String("\"kind\""))) {
        if (s.contains(QLatin1String("\"as_sticker\":true")) || s.contains(QLatin1String("\"sticker\":true"))) {
            return QStringLiteral("[表情]");
        }
        if (s.contains(QLatin1String("\"kind\":\"image\"")) || s.contains(QLatin1String("\"kind\": \"image\""))) {
            return QStringLiteral("[图片]");
        }
        if (s.contains(QLatin1String("\"kind\":\"file\"")) || s.contains(QLatin1String("\"kind\": \"file\""))) {
            return QStringLiteral("[文件]");
        }
    }
    return rawContent;
}

QString humanFileSize(qint64 bytes)
{
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024LL * 1024) {
        return QStringLiteral("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    }
    return QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
}

QByteArray sha256OfFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd()) {
        const QByteArray b = f.read(4 * 1024 * 1024);
        if (b.isEmpty()) {
            break;
        }
        h.addData(b);
    }
    return h.result();
}

class ImageBubbleClickFilter final : public QObject {
public:
    ImageBubbleClickFilter(QObject *parent, QWidget *dlgParent, QString path)
        : QObject(parent), m_dlgParent(dlgParent), m_path(std::move(path))
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        Q_UNUSED(watched);
        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *me = static_cast<const QMouseEvent *>(event);
            if (me->button() == Qt::LeftButton && m_dlgParent) {
                ImagePreviewDialog::showForPath(m_path, m_dlgParent.data());
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QPointer<QWidget> m_dlgParent;
    QString m_path;
};

} // namespace

ChatMainWidget::ChatMainWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("LanChatMainWidget"));
    setStyleSheet(AppStyle::chatMainStyle());

    QPixmap raw(64, 64);
    raw.fill(QColor(QStringLiteral("#12b7f5")));
    m_avatarPix = makeCircularAvatar(raw, 36);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    m_leftStack = new QStackedWidget(this);
    m_contacts = new ContactsWidget(this);
    m_leftStack->addWidget(buildSessionPanel());
    m_leftStack->addWidget(m_contacts);
    m_leftStack->setCurrentIndex(0);
    /// 宽度限制加在 QStackedWidget 上：仅限制内层会话页会导致中间格被拉宽时出现「会话列右侧空白」。
    m_leftStack->setMinimumWidth(kSessionColumnMinWidth);
    m_leftStack->setMaximumWidth(kSessionColumnMaxWidth);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName(QStringLiteral("lanMainSplitter"));
    splitter->setHandleWidth(1);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(buildIconRail());
    splitter->addWidget(m_leftStack);
    m_rightStack = new QStackedWidget(this);
    m_rightStack->setObjectName(QStringLiteral("lanRightStack"));
    m_chatPanel = buildChatPanel();
    m_chatPanel->setMinimumWidth(280);
    m_rightStack->addWidget(m_chatPanel);
    m_rightStack->addWidget(buildContactModePlaceholder());
    m_rightStack->setCurrentIndex(0);
    splitter->addWidget(m_rightStack);
    /// 启动时中间列使用最小宽度，右侧聊天区占满剩余空间。
    splitter->setSizes({56, kSessionColumnMinWidth, 700});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    outer->addWidget(splitter, 1);

    if (m_railSessionBtn && m_railContactsBtn) {
        connect(m_railSessionBtn, &QToolButton::toggled, this, [this](bool on) {
            if (on && m_leftStack && m_rightStack) {
                m_leftStack->setCurrentIndex(0);
                m_rightStack->setCurrentIndex(0);
                requestFriendListForSessions();
            }
        });
        connect(m_railContactsBtn, &QToolButton::toggled, this, [this](bool on) {
            if (on && m_leftStack && m_rightStack) {
                m_leftStack->setCurrentIndex(1);
                m_rightStack->setCurrentIndex(1);
                if (m_contacts) {
                    m_contacts->refreshAll();
                }
            }
        });
    }

    m_msgPollTimer = new QTimer(this);
    m_msgPollTimer->setInterval(2000);
    connect(m_msgPollTimer, &QTimer::timeout, this, &ChatMainWidget::onMsgPollTick);

    m_fileSendTimer = new QTimer(this);
    m_fileSendTimer->setInterval(1);
    connect(m_fileSendTimer, &QTimer::timeout, this, &ChatMainWidget::onFileSendTick);

    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(sc, &QShortcut::activated, this, &ChatMainWidget::onSendClicked);
}

void ChatMainWidget::setUserEmail(const QString &email)
{
    m_userEmail = email;
    if (m_userEmail.trimmed().isEmpty()) {
        m_pinnedPeerIds.clear();
        m_hiddenSessionPeers.clear();
    } else {
        loadSessionUiPrefs();
    }
    if (m_railLogoutBtn) {
        m_railLogoutBtn->setToolTip(email.isEmpty()
                                        ? QStringLiteral("退出登录")
                                        : QStringLiteral("退出登录\n当前账号：%1").arg(email));
    }
    if (m_stickerPicker) {
        m_stickerPicker->setUserEmail(m_userEmail);
        m_stickerPicker->reloadAndRefresh();
    }
}

void ChatMainWidget::setSession(LanTcpClient *client, const QString &token, qint64 userId)
{
    m_sessionClient = client;
    m_sessionToken = token;
    m_sessionUserId = userId;
    m_localPathByTransferId.clear();
    LocalProfile::loadTransferPathMap(m_userEmail, &m_localPathByTransferId);
    if (m_contacts) {
        m_contacts->setTcpClient(client);
        m_contacts->setToken(token);
    }
    if (m_addFriendDialog) {
        m_addFriendDialog->setTcpClient(client, token);
    }
    if (m_chatHistoryDialog) {
        m_chatHistoryDialog->close();
    }
    if (m_stickerPicker) {
        m_stickerPicker->hide();
    }
    m_pendingHistoryMsgFetch = false;
    m_historyFetchPeerId = 0;
    requestFriendListForSessions();
}

void ChatMainWidget::clearSession()
{
    stopMsgPolling();
    cancelStickerArtifactPull();
    m_stickerPullQueue.clear();
    resetFileSendState();
    cleanupFileReceive(false);
    m_outgoingFileOfferBubble.clear();
    m_incomingFileOfferParamsByTid.clear();
    m_incomingFileOfferBubbleByTid.clear();
    m_peerAvatarPix = QPixmap();
    m_peerAvatarPixByPeer.clear();
    m_peerAvatarRevByPeer.clear();
    m_currentPeerId = 0;
    m_lastMsgId = 0;
    m_seenMessageIds.clear();
    m_unreadByPeer.clear();
    m_localPathByTransferId.clear();
    if (m_friendList) {
        m_friendList->clear();
    }
    if (m_centerStack && m_centerEmpty) {
        m_centerStack->setCurrentWidget(m_centerEmpty);
    }
    m_sessionClient = nullptr;
    m_sessionToken.clear();
    m_sessionUserId = 0;
    m_scrollAfterNextMsgFetch = false;
    if (m_contacts) {
        m_contacts->setTcpClient(nullptr);
        m_contacts->setToken(QString());
    }
    if (m_addFriendDialog) {
        m_addFriendDialog->clearSession();
        m_addFriendDialog->close();
    }
    m_pendingHistoryMsgFetch = false;
    m_historyFetchPeerId = 0;
    if (m_chatHistoryDialog) {
        m_chatHistoryDialog->close();
    }
    if (m_stickerPicker) {
        m_stickerPicker->hide();
    }
}

void ChatMainWidget::openAddFriendSearchDialog()
{
    if (!m_addFriendDialog) {
        m_addFriendDialog = new AddFriendSearchDialog(this);
    }
    m_addFriendDialog->setTcpClient(m_sessionClient, m_sessionToken);
    m_addFriendDialog->show();
    m_addFriendDialog->raise();
    m_addFriendDialog->activateWindow();
}

void ChatMainWidget::onChatHistoryDialogFinished()
{
    m_pendingHistoryMsgFetch = false;
    m_historyFetchPeerId = 0;
}

void ChatMainWidget::openChatHistoryDialog()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择会话"));
        return;
    }
    if (!m_chatHistoryDialog) {
        m_chatHistoryDialog = new ChatHistoryDialog(this);
        connect(m_chatHistoryDialog, &QDialog::finished, this, &ChatMainWidget::onChatHistoryDialogFinished);
    }
    const QString peerTitle = m_peerTitleLabel ? m_peerTitleLabel->text() : QString();
    m_chatHistoryDialog->setSession(m_sessionUserId, m_currentPeerId, peerTitle, m_userEmail, m_peerAvatarPix);
    m_pendingHistoryMsgFetch = true;
    m_historyFetchPeerId = m_currentPeerId;
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("msg_fetch"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("peer_user_id"), m_currentPeerId);
    o.insert(QStringLiteral("after_id"), 0);
    o.insert(QStringLiteral("limit"), 200);
    m_sessionClient->sendJsonObject(o);
    m_chatHistoryDialog->show();
    m_chatHistoryDialog->raise();
    m_chatHistoryDialog->activateWindow();
}

void ChatMainWidget::handleServerJson(const QJsonObject &obj)
{
    const QString t = obj.value(QStringLiteral("type")).toString();
    if (t == QStringLiteral("error")) {
        if (m_stickerPullTransferId > 0 || m_stickerPullFile) {
            const int code = obj.value(QStringLiteral("code")).toInt();
            const QString msg = obj.value(QStringLiteral("message")).toString();
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] server error during pull code=%1 msg=%2")
                                      .arg(code)
                                      .arg(msg);
            cancelStickerArtifactPull();
            tryStartNextStickerPullInQueue();
        }
        handleBusinessErrorFrame(obj);
        if (m_pendingHistoryMsgFetch && m_chatHistoryDialog) {
            m_chatHistoryDialog->notifyFetchError(obj.value(QStringLiteral("message")).toString());
            m_pendingHistoryMsgFetch = false;
            m_historyFetchPeerId = 0;
        }
        if (m_addFriendDialog) {
            m_addFriendDialog->handleServerJson(obj);
        }
        if (m_contacts) {
            m_contacts->handleServerJson(obj);
        }
        return;
    }
    if (t == QStringLiteral("profile_set_ok")) {
        requestFriendListForSessions();
    }
    if (t == QStringLiteral("profile_set_avatar_ok")) {
        requestFriendListForSessions();
    }
    if (t == QStringLiteral("peer_avatar_ok")) {
        const qint64 pid = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
        const qint64 rev = obj.value(QStringLiteral("avatar_rev")).toVariant().toLongLong();
        const QString b64s = obj.value(QStringLiteral("avatar_b64")).toString();
        const QByteArray raw = QByteArray::fromBase64(b64s.trimmed().toUtf8());
        QPixmap circ;
        if (rev > 0 && !raw.isEmpty()) {
            QPixmap pm;
            if (pm.loadFromData(raw, "JPEG")) {
                circ = makeCircularAvatar(pm, 36);
            }
        }
        if (circ.isNull()) {
            m_peerAvatarPixByPeer.remove(pid);
            m_peerAvatarRevByPeer.remove(pid);
        } else {
            m_peerAvatarPixByPeer.insert(pid, circ);
            m_peerAvatarRevByPeer.insert(pid, rev);
        }
        if (m_currentPeerId == pid) {
            m_peerAvatarPix = circ;
            refreshIncomingAvatarLabels();
        }
        updateSessionListAvatarForPeer(pid, true);
    }
    if (t == QStringLiteral("friend_notify")) {
        const QString ev = obj.value(QStringLiteral("event")).toString();
        if (ev == QStringLiteral("presence")) {
            const qint64 pid = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
            const bool on = obj.value(QStringLiteral("online")).toBool();
            updateFriendPresenceUi(pid, on);
        } else if (ev == QStringLiteral("nickname")) {
            requestFriendListForSessions();
        } else if (ev == QStringLiteral("avatar")) {
            const qint64 pid = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
            const qint64 rev = obj.value(QStringLiteral("avatar_rev")).toVariant().toLongLong();
            if (m_friendList && pid > 0) {
                for (int i = 0; i < m_friendList->count(); ++i) {
                    QListWidgetItem *it = m_friendList->item(i);
                    if (!it || it->data(Qt::UserRole).toLongLong() != pid) {
                        continue;
                    }
                    it->setData(Qt::UserRole + 4, rev);
                    break;
                }
            }
            m_peerAvatarPixByPeer.remove(pid);
            m_peerAvatarRevByPeer.remove(pid);
            if (m_currentPeerId == pid) {
                syncPeerAvatarForCurrentSession();
            }
            updateSessionListAvatarForPeer(pid, m_currentPeerId != pid);
        }
    }
    if (t == QStringLiteral("file_incoming")) {
        handleFileIncoming(obj);
    } else if (t == QStringLiteral("file_offer_delivered")) {
        handleFileOfferDelivered(obj);
    } else if (t == QStringLiteral("file_offer_ok")) {
        m_pendingOfferTransferId = jsonInt64Member(obj, QStringLiteral("transfer_id"));
        m_offerChunkPlainMax = obj.value(QStringLiteral("chunk_plain_max")).toInt(65536);
        if (m_offerChunkPlainMax < 4096) {
            m_offerChunkPlainMax = 65536;
        }
        m_fileSendGateReady = false;
        if (m_outgoingFileOfferBubble) {
            m_outgoingFileOfferBubble->setProperty("transferId",
                                                   QVariant::fromValue(m_pendingOfferTransferId));
        }
    } else if (t == QStringLiteral("file_send_ready")) {
        const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
        if (tid > 0 && tid == m_pendingOfferTransferId && !m_pendingOfferPath.isEmpty()) {
            m_fileSendGateReady = true;
            tryStartOutgoingFileTransfer();
        }
    } else if (t == QStringLiteral("file_chunk_push")) {
        handleFileChunkPush(obj);
    } else if (t == QStringLiteral("file_transfer_done")) {
        handleFileTransferDone(obj);
    } else if (t == QStringLiteral("file_aborted")) {
        handleFileAborted(obj);
    } else if (t == QStringLiteral("file_sticker_pull_ok")) {
        handleStickerPullOk(obj);
    } else if (t == QStringLiteral("file_sticker_pull_chunk")) {
        handleStickerPullChunk(obj);
    } else if (t == QStringLiteral("file_sticker_pull_done")) {
        handleStickerPullDone(obj);
    }
    if (t == QStringLiteral("msg_fetch_ok")) {
        const qint64 fetchPeer = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
        if (m_pendingHistoryMsgFetch && m_chatHistoryDialog && fetchPeer == m_historyFetchPeerId
            && m_historyFetchPeerId > 0) {
            m_pendingHistoryMsgFetch = false;
            m_historyFetchPeerId = 0;
            m_chatHistoryDialog->applyMsgFetchOk(obj);
        } else {
            handleMsgFetchOk(obj);
        }
    } else if (t == QStringLiteral("msg_send_ok")) {
        handleMsgSendOk(obj);
    } else if (t == QStringLiteral("msg_push")) {
        handleMsgPush(obj);
    } else if (t == QStringLiteral("msg_clear_ok")) {
        m_pendingMsgClearPeer = 0;
        const qint64 peer = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
        applyConversationClearedUi(peer);
    } else if (t == QStringLiteral("msg_conv_cleared")) {
        const qint64 by = obj.value(QStringLiteral("by_user_id")).toVariant().toLongLong();
        applyConversationClearedUi(by);
    } else if (t == QStringLiteral("friend_list_ok")) {
        applyFriendListToSessions(obj.value(QStringLiteral("friends")).toArray());
    } else if (t == QStringLiteral("friend_delete_ok")) {
        requestFriendListForSessions();
    }
    if (m_addFriendDialog) {
        m_addFriendDialog->handleServerJson(obj);
    }
    if (m_contacts) {
        m_contacts->handleServerJson(obj);
    }
}

QWidget *ChatMainWidget::buildIconRail()
{
    auto *rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("lanIconRail"));
    rail->setFixedWidth(56);
    auto *v = new QVBoxLayout(rail);
    v->setContentsMargins(6, 20, 6, 12);
    v->setSpacing(6);

    auto *profileBtn = new QToolButton(rail);
    profileBtn->setObjectName(QStringLiteral("lanRailButton"));
    profileBtn->setMinimumSize(44, 44);
    profileBtn->setCursor(Qt::PointingHandCursor);
    profileBtn->setToolTip(QStringLiteral("个人资料"));
    profileBtn->setIcon(QIcon(QStringLiteral(":/icons/personal_information_icon.svg")));
    profileBtn->setIconSize(QSize(22, 22));
    profileBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    profileBtn->setFocusPolicy(Qt::NoFocus);
    connect(profileBtn, &QToolButton::clicked, this, &ChatMainWidget::profileEditRequested);
    v->addWidget(profileBtn, 0, Qt::AlignHCenter);

    auto *group = new QButtonGroup(rail);
    const QString railIcons[] = {QStringLiteral(":/icons/message_icon.svg"), QStringLiteral(":/icons/contact_icon.svg")};
    const QString railTips[] = {QStringLiteral("会话"), QStringLiteral("联系人")};
    for (int i = 0; i < 2; ++i) {
        auto *tb = new QToolButton(rail);
        tb->setToolTip(railTips[i]);
        tb->setCheckable(true);
        tb->setObjectName(QStringLiteral("lanRailButton"));
        tb->setMinimumSize(44, 44);
        tb->setCursor(Qt::PointingHandCursor);
        tb->setIcon(QIcon(railIcons[i]));
        tb->setIconSize(QSize(22, 22));
        tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
        group->addButton(tb);
        v->addWidget(tb, 0, Qt::AlignHCenter);
        if (i == 0) {
            tb->setChecked(true);
            m_railSessionBtn = tb;
        } else {
            m_railContactsBtn = tb;
        }
    }
    group->setExclusive(true);

    v->addStretch(1);

    m_railTcpDebugBtn = new QToolButton(rail);
    m_railTcpDebugBtn->setObjectName(QStringLiteral("lanRailButton"));
    m_railTcpDebugBtn->setMinimumSize(44, 44);
    m_railTcpDebugBtn->setCursor(Qt::PointingHandCursor);
    m_railTcpDebugBtn->setToolTip(QStringLiteral("TCP 联调"));
    m_railTcpDebugBtn->setIcon(QIcon(QStringLiteral(":/icons/tool_icon.svg")));
    m_railTcpDebugBtn->setIconSize(QSize(22, 22));
    m_railTcpDebugBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_railTcpDebugBtn->setFocusPolicy(Qt::NoFocus);
    connect(m_railTcpDebugBtn, &QToolButton::clicked, this, &ChatMainWidget::tcpDebugRequested);
    v->addWidget(m_railTcpDebugBtn, 0, Qt::AlignHCenter);

    m_railLogoutBtn = new QToolButton(rail);
    m_railLogoutBtn->setObjectName(QStringLiteral("lanRailButton"));
    m_railLogoutBtn->setMinimumSize(44, 44);
    m_railLogoutBtn->setCursor(Qt::PointingHandCursor);
    m_railLogoutBtn->setIcon(QIcon(QStringLiteral(":/icons/login_out_icon.svg")));
    m_railLogoutBtn->setIconSize(QSize(22, 22));
    m_railLogoutBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_railLogoutBtn->setFocusPolicy(Qt::NoFocus);
    connect(m_railLogoutBtn, &QToolButton::clicked, this, &ChatMainWidget::logoutRequested);
    v->addWidget(m_railLogoutBtn, 0, Qt::AlignHCenter);

    return rail;
}

QWidget *ChatMainWidget::createSessionRowWidget(const QString &name, const QString &preview,
                                                const QString &time, bool online, int unreadCount, bool pinned)
{
    auto *wrap = new QWidget();
    wrap->setObjectName(QStringLiteral("sessionRowWrap"));

    auto *h = new QHBoxLayout(wrap);
    h->setContentsMargins(10, 8, 10, 8);
    h->setSpacing(10);

    auto *av = new QLabel(wrap);
    av->setFixedSize(44, 44);
    av->setObjectName(QStringLiteral("sessionAvatar"));
    QPixmap dot(64, 64);
    dot.fill(QColor(QStringLiteral("#bae7ff")));
    av->setPixmap(makeCircularAvatar(dot, 44));

    h->addWidget(av, 0, Qt::AlignTop);

    auto *textCol = new QVBoxLayout();
    textCol->setSpacing(4);
    auto *nameLb = new QLabel(name, wrap);
    nameLb->setObjectName(QStringLiteral("sessionNameLabel"));
    auto *prevLb = new QLabel(preview, wrap);
    prevLb->setObjectName(QStringLiteral("sessionPreviewLabel"));
    prevLb->setWordWrap(false);
    textCol->addWidget(nameLb);
    textCol->addWidget(prevLb);
    h->addLayout(textCol, 1);

    auto *rightCol = new QVBoxLayout();
    rightCol->setSpacing(4);
    auto *badgeRow = new QHBoxLayout();
    badgeRow->setContentsMargins(0, 0, 0, 0);
    badgeRow->addStretch(1);
    if (pinned) {
        auto *pinLb = new QLabel(QStringLiteral("顶"), wrap);
        pinLb->setObjectName(QStringLiteral("sessionPinBadge"));
        pinLb->setStyleSheet(QStringLiteral("QLabel#sessionPinBadge { color: #1890ff; font-size: 11px; }"));
        badgeRow->addWidget(pinLb, 0, Qt::AlignRight | Qt::AlignTop);
    }
    auto *unreadLb = new QLabel(wrap);
    unreadLb->setObjectName(QStringLiteral("sessionRowUnread"));
    unreadLb->setAlignment(Qt::AlignCenter);
    unreadLb->setStyleSheet(
        QStringLiteral("QLabel#sessionRowUnread { background: #ff4d4f; color: white; border-radius: 9px; "
                       "padding: 1px 6px; font-size: 11px; min-width: 16px; }"));
    if (unreadCount > 0) {
        unreadLb->setText(unreadCount > 99 ? QStringLiteral("99+") : QString::number(unreadCount));
        unreadLb->show();
    } else {
        unreadLb->hide();
    }
    badgeRow->addWidget(unreadLb, 0, Qt::AlignRight | Qt::AlignTop);
    rightCol->addLayout(badgeRow);

    auto *timeLb = new QLabel(time, wrap);
    timeLb->setObjectName(QStringLiteral("sessionTimeLabel"));
    timeLb->setAlignment(Qt::AlignRight | Qt::AlignTop);
    auto *dotLb = new QLabel(wrap);
    dotLb->setObjectName(QStringLiteral("sessionPresenceDot"));
    {
        const int d = 20;
        dotLb->setPixmap(QIcon(online ? QStringLiteral(":/icons/online_dot_icon.svg")
                                       : QStringLiteral(":/icons/offline_dot_icon.svg"))
                             .pixmap(d, d));
    }
    dotLb->setAlignment(Qt::AlignRight);
    rightCol->addWidget(timeLb);
    rightCol->addWidget(dotLb);
    h->addLayout(rightCol);

    return wrap;
}

QWidget *ChatMainWidget::buildSessionPanel()
{
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("lanSessionListPanel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 8);
    layout->setSpacing(10);

    auto *searchRow = new QWidget(panel);
    auto *searchLay = new QHBoxLayout(searchRow);
    searchLay->setContentsMargins(0, 0, 0, 0);
    searchLay->setSpacing(8);

    auto *searchFieldWrap = new QWidget(searchRow);
    searchFieldWrap->setObjectName(QStringLiteral("lanSearchFieldWrap"));
    auto *searchFieldLay = new QHBoxLayout(searchFieldWrap);
    searchFieldLay->setContentsMargins(8, 0, 10, 0);
    searchFieldLay->setSpacing(6);

    auto *searchIcon = new QLabel(searchFieldWrap);
    searchIcon->setObjectName(QStringLiteral("lanSearchIcon"));
    searchIcon->setPixmap(QIcon(QStringLiteral(":/icons/search_icon.svg")).pixmap(18, 18));
    searchIcon->setFixedSize(18, 18);
    searchIcon->setScaledContents(true);

    auto *searchEdit = new QLineEdit(searchFieldWrap);
    m_sessionSearchEdit = searchEdit;
    searchEdit->setObjectName(QStringLiteral("lanSessionSearch"));
    searchEdit->setPlaceholderText(QStringLiteral("搜索"));
    searchFieldLay->addWidget(searchIcon, 0, Qt::AlignVCenter);
    searchFieldLay->addWidget(searchEdit, 1);
    searchLay->addWidget(searchFieldWrap, 1);

    auto *plusBtn = new QToolButton(searchRow);
    plusBtn->setObjectName(QStringLiteral("lanPlusButton"));
    plusBtn->setIcon(QIcon(QStringLiteral(":/icons/add_icon.svg")));
    plusBtn->setIconSize(QSize(18, 18));
    plusBtn->setToolButtonStyle(Qt::ToolButtonIconOnly);
    plusBtn->setCursor(Qt::PointingHandCursor);
    plusBtn->setToolTip(QStringLiteral("添加"));
    plusBtn->setPopupMode(QToolButton::InstantPopup);
    auto *addMenu = new QMenu(plusBtn);
    auto *actAddFriend =
        addMenu->addAction(QIcon(QStringLiteral(":/icons/add_friend_icon.svg")), QStringLiteral("加好友"));
    plusBtn->setMenu(addMenu);
    connect(actAddFriend, &QAction::triggered, this, &ChatMainWidget::openAddFriendSearchDialog);
    searchLay->addWidget(plusBtn);

    layout->addWidget(searchRow);

    m_friendList = new QListWidget(panel);
    m_friendList->setObjectName(QStringLiteral("lanFriendList"));
    m_friendList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_friendList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_friendList->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(m_friendList, 1);

    connect(m_friendList, &QListWidget::itemSelectionChanged, this, &ChatMainWidget::onFriendSelectionChanged);
    connect(m_friendList, &QListWidget::customContextMenuRequested, this,
            &ChatMainWidget::onSessionListContextMenu);
    connect(searchEdit, &QLineEdit::textChanged, this, &ChatMainWidget::applySessionListSearchFilter);

    return panel;
}

QWidget *ChatMainWidget::buildChatPanel()
{
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("lanCenterPanel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_centerStack = new QStackedWidget(panel);

    m_centerEmpty = new QWidget(panel);
    auto *emptyLay = new QVBoxLayout(m_centerEmpty);
    emptyLay->setAlignment(Qt::AlignCenter);
    emptyLay->setSpacing(12);
    auto *emptyMain = new QLabel(QStringLiteral("选择一个会话开始聊天"), m_centerEmpty);
    emptyMain->setObjectName(QStringLiteral("lanEmptyMain"));
    emptyMain->setAlignment(Qt::AlignCenter);
    auto *emptySub = new QLabel(QStringLiteral("在左侧列表中选择好友"), m_centerEmpty);
    emptySub->setObjectName(QStringLiteral("lanEmptySub"));
    emptySub->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(emptyMain);
    emptyLay->addWidget(emptySub);
    m_centerStack->addWidget(m_centerEmpty);

    m_chatArea = new QWidget(panel);
    auto *chatLay = new QVBoxLayout(m_chatArea);
    chatLay->setContentsMargins(0, 0, 0, 0);
    chatLay->setSpacing(0);

    auto *headerBar = new QWidget(m_chatArea);
    headerBar->setObjectName(QStringLiteral("lanChatHeaderBar"));
    headerBar->setFixedHeight(56);
    auto *headerLay = new QHBoxLayout(headerBar);
    headerLay->setContentsMargins(16, 0, 16, 0);
    headerLay->setSpacing(0);

    auto *titleRow = new QHBoxLayout();
    titleRow->setSpacing(10);
    titleRow->setContentsMargins(0, 0, 0, 0);
    m_peerTitleLabel = new QLabel(headerBar);
    m_peerTitleLabel->setObjectName(QStringLiteral("lanPeerTitle"));
    m_peerPresenceIcon = new QLabel(headerBar);
    m_peerPresenceIcon->setObjectName(QStringLiteral("lanPeerPresenceIcon"));
    m_peerPresenceIcon->setFixedSize(28, 28);
    m_peerPresenceText = new QLabel(headerBar);
    m_peerPresenceText->setObjectName(QStringLiteral("lanPeerOnline"));
    titleRow->addWidget(m_peerTitleLabel, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_peerPresenceIcon, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_peerPresenceText, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    headerLay->addLayout(titleRow, 1);

    chatLay->addWidget(headerBar);

    m_messageScroll = new QScrollArea(m_chatArea);
    m_messageScroll->setObjectName(QStringLiteral("lanMessageScroll"));
    m_messageScroll->setWidgetResizable(true);
    m_messageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messageContainer = new QWidget();
    m_messageContainer->setObjectName(QStringLiteral("lanMessageContainer"));
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(16, 16, 16, 16);
    m_messageLayout->setSpacing(10);
    m_messageLayout->addStretch(1);
    m_messageScroll->setWidget(m_messageContainer);
    chatLay->addWidget(m_messageScroll, 1);

    auto *inputWrap = new QWidget(m_chatArea);
    inputWrap->setObjectName(QStringLiteral("lanInputWrap"));
    auto *inputLay = new QVBoxLayout(inputWrap);
    inputLay->setContentsMargins(12, 8, 12, 10);
    inputLay->setSpacing(6);

    auto *toolRow = new QHBoxLayout();
    toolRow->setSpacing(4);
    const QString toolIconPaths[] = {QStringLiteral(":/icons/expression_icon.svg"),
                                     QStringLiteral(":/icons/file_icon.svg"),
                                     QStringLiteral(":/icons/picture_icon.svg")};
    const QString toolTips[] = {QStringLiteral("表情"), QStringLiteral("发送文件"), QStringLiteral("图片")};
    for (int i = 0; i < 3; ++i) {
        auto *t = new QToolButton(inputWrap);
        t->setIcon(QIcon(toolIconPaths[i]));
        t->setIconSize(QSize(20, 20));
        t->setToolTip(toolTips[i]);
        t->setObjectName(QStringLiteral("lanInputTool"));
        t->setCursor(Qt::PointingHandCursor);
        t->setAutoRaise(true);
        if (i == 0) {
            m_inputExpressionBtn = t;
            connect(t, &QToolButton::clicked, this, &ChatMainWidget::onInputExpressionClicked);
        } else if (i == 1) {
            connect(t, &QToolButton::clicked, this, &ChatMainWidget::onPickSendFileClicked);
        } else if (i == 2) {
            connect(t, &QToolButton::clicked, this, &ChatMainWidget::onPickSendImageClicked);
        }
        toolRow->addWidget(t);
    }
    toolRow->addStretch(1);
    auto *histBtn = new QToolButton(inputWrap);
    histBtn->setIcon(QIcon(QStringLiteral(":/icons/history.svg")));
    histBtn->setIconSize(QSize(20, 20));
    histBtn->setToolTip(QStringLiteral("聊天记录"));
    histBtn->setObjectName(QStringLiteral("lanInputTool"));
    histBtn->setCursor(Qt::PointingHandCursor);
    histBtn->setAutoRaise(true);
    connect(histBtn, &QToolButton::clicked, this, &ChatMainWidget::openChatHistoryDialog);
    toolRow->addWidget(histBtn);
    inputLay->addLayout(toolRow);

    m_inputEdit = new QPlainTextEdit(inputWrap);
    m_inputEdit->setObjectName(QStringLiteral("lanMessageInput"));
    m_inputEdit->setPlaceholderText(QStringLiteral("输入消息，Ctrl+Enter 发送"));
    m_inputEdit->setMaximumHeight(120);
    inputLay->addWidget(m_inputEdit);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto *sendBtn = new QPushButton(QStringLiteral("发送"), inputWrap);
    sendBtn->setObjectName(QStringLiteral("lanSendButton"));
    sendBtn->setCursor(Qt::PointingHandCursor);
    sendBtn->setFixedWidth(88);
    connect(sendBtn, &QPushButton::clicked, this, &ChatMainWidget::onSendClicked);
    btnRow->addWidget(sendBtn);
    inputLay->addLayout(btnRow);

    chatLay->addWidget(inputWrap);

    m_centerStack->addWidget(m_chatArea);
    m_centerStack->setCurrentWidget(m_chatArea);

    layout->addWidget(m_centerStack);
    return panel;
}

QWidget *ChatMainWidget::buildContactModePlaceholder()
{
    auto *w = new QWidget(this);
    w->setObjectName(QStringLiteral("lanContactModePlaceholder"));
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addStretch(1);
    auto *iconLb = new QLabel(w);
    iconLb->setObjectName(QStringLiteral("lanContactModePlaceholderIcon"));
    iconLb->setAlignment(Qt::AlignCenter);
    const qreal dpr = w->devicePixelRatioF() > 0 ? w->devicePixelRatioF() : 1.0;
    const int side = static_cast<int>(280.0 * dpr + 0.5);
    QPixmap pm = QIcon(QStringLiteral(":/icons/contact_mode_placeholder_icon.svg")).pixmap(
        QSize(side, side), QIcon::Normal, QIcon::Off);
    if (!pm.isNull()) {
        pm.setDevicePixelRatio(dpr);
        iconLb->setPixmap(pm);
    }
    lay->addWidget(iconLb, 0, Qt::AlignHCenter);
    lay->addStretch(2);
    return w;
}

QWidget *ChatMainWidget::createCenteredTimeRow(qint64 createdAtSec)
{
    auto *w = new QWidget(m_messageContainer);
    auto *lay = new QHBoxLayout(w);
    lay->setContentsMargins(0, 6, 0, 6);
    lay->setSpacing(0);
    lay->addStretch(1);
    auto *lb = new QLabel(formatMsgDividerTime(createdAtSec), w);
    lb->setObjectName(QStringLiteral("lanMsgTimeCapsule"));
    lb->setAlignment(Qt::AlignCenter);
    lay->addWidget(lb, 0, Qt::AlignCenter);
    lay->addStretch(1);
    return w;
}

QWidget *ChatMainWidget::createBubble(const QString &text, bool isOutgoing)
{
    auto *row = new QWidget();
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(4, 4, 4, 4);
    rowLay->setSpacing(10);

    auto *av = new QLabel(row);
    av->setFixedSize(36, 36);
    if (isOutgoing) {
        QPixmap selfPix;
        if (!m_userEmail.isEmpty() && LocalProfile::loadAvatarPixmap(m_userEmail, 36, &selfPix) && !selfPix.isNull()) {
            av->setPixmap(selfPix);
        } else if (!m_avatarPix.isNull()) {
            av->setPixmap(m_avatarPix);
        }
    } else {
        av->setObjectName(QStringLiteral("incomingSessionAvatar"));
        setIncomingAvatarOnLabel(av);
    }

    auto *bubble = new QFrame(row);
    bubble->setObjectName(isOutgoing ? QStringLiteral("bubbleOut") : QStringLiteral("bubbleIn"));
    auto *bubbleLay = new QVBoxLayout(bubble);
    bubbleLay->setContentsMargins(12, 10, 12, 10);
    bubbleLay->setSpacing(0);
    auto *content = new QLabel(text, bubble);
    content->setWordWrap(true);
    content->setTextInteractionFlags(Qt::TextSelectableByMouse);
    content->setObjectName(isOutgoing ? QStringLiteral("bubbleTextOut") : QStringLiteral("bubbleTextIn"));
    bubbleLay->addWidget(content);
    bubble->setMaximumWidth(440);

    if (!isOutgoing) {
        rowLay->addWidget(av, 0, Qt::AlignTop);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addStretch(1);
    } else {
        rowLay->addStretch(1);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addWidget(av, 0, Qt::AlignTop);
    }
    return row;
}

void ChatMainWidget::updateChatHeader(const QString &name, bool online)
{
    m_peerTitleLabel->setText(name);
    if (m_peerPresenceText) {
        m_peerPresenceText->setText(online ? QStringLiteral("在线") : QStringLiteral("离线"));
        m_peerPresenceText->setObjectName(online ? QStringLiteral("lanPeerOnline") : QStringLiteral("lanPeerOffline"));
        m_peerPresenceText->style()->unpolish(m_peerPresenceText);
        m_peerPresenceText->style()->polish(m_peerPresenceText);
    }
    if (m_peerPresenceIcon) {
        const int d = 24;
        m_peerPresenceIcon->setPixmap(QIcon(online ? QStringLiteral(":/icons/online_dot_icon.svg")
                                                   : QStringLiteral(":/icons/offline_dot_icon.svg"))
                                          .pixmap(d, d));
    }
}

void ChatMainWidget::updateFriendPresenceUi(qint64 peerUserId, bool online)
{
    if (!m_friendList || peerUserId <= 0) {
        return;
    }
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        if (!it || it->data(Qt::UserRole).toLongLong() != peerUserId) {
            continue;
        }
        it->setData(Qt::UserRole + 2, online);
        QWidget *row = m_friendList->itemWidget(it);
        if (row) {
            if (auto *dot = row->findChild<QLabel *>(QStringLiteral("sessionPresenceDot"))) {
                const int d = 20;
                dot->setPixmap(QIcon(online ? QStringLiteral(":/icons/online_dot_icon.svg")
                                            : QStringLiteral(":/icons/offline_dot_icon.svg"))
                                 .pixmap(d, d));
            }
        }
        if (m_currentPeerId == peerUserId) {
            const QString title = it->data(Qt::UserRole + 1).toString();
            updateChatHeader(title.isEmpty() ? QStringLiteral("好友") : title, online);
        }
        break;
    }
}

void ChatMainWidget::clearMessageList()
{
    cancelStickerArtifactPull();
    m_stickerPullQueue.clear();
    m_outgoingFileOfferBubble.clear();
    m_incomingFileOfferParamsByTid.clear();
    m_incomingFileOfferBubbleByTid.clear();
    m_prevMsgCreatedAtForDivider = 0;
    while (m_messageLayout->count() > 1) {
        QLayoutItem *li = m_messageLayout->takeAt(0);
        if (li->widget() != nullptr) {
            delete li->widget();
        }
        delete li;
    }
}

void ChatMainWidget::scrollMessagesToBottom()
{
    if (!m_messageScroll || !m_messageLayout) {
        return;
    }
    /// 批量插入消息后，QScrollArea 的 range 要等布局事件后才更新；若立刻 `setValue(max)` 且此时
    /// `maximum()==0`，会停在顶部。延后一帧并多次尝试，并对最后一条消息 `ensureWidgetVisible`。
    const auto apply = [this]() {
        if (!m_messageScroll || !m_messageLayout) {
            return;
        }
        const int n = m_messageLayout->count();
        if (n > 1) {
            if (QLayoutItem *li = m_messageLayout->itemAt(n - 2)) {
                if (QWidget *w = li->widget()) {
                    m_messageScroll->ensureWidgetVisible(w, 0, 24);
                }
            }
        }
        QScrollBar *sb = m_messageScroll->verticalScrollBar();
        sb->setValue(sb->maximum());
    };
    apply();
    QTimer::singleShot(0, this, apply);
    QTimer::singleShot(10, this, apply);
}

void ChatMainWidget::refreshSessionRowHighlight()
{
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        QWidget *w = m_friendList->itemWidget(it);
        if (!w) {
            continue;
        }
        const bool sel = it->isSelected();
        w->setStyleSheet(sel ? QStringLiteral("QWidget#sessionRowWrap { background: #e3eef9; border-radius: 0px; }")
                              : QStringLiteral("QWidget#sessionRowWrap { background: transparent; border-radius: 0px; }"));
    }
}

void ChatMainWidget::onFriendSelectionChanged()
{
    refreshSessionRowHighlight();
    QListWidgetItem *item = m_friendList->currentItem();
    if (!item) {
        m_peerAvatarPix = QPixmap();
        stopMsgPolling();
        m_currentPeerId = 0;
        m_lastMsgId = 0;
        m_seenMessageIds.clear();
        m_centerStack->setCurrentWidget(m_centerEmpty);
        return;
    }
    const qint64 peerId = item->data(Qt::UserRole).toLongLong();
    const QString title = item->data(Qt::UserRole + 1).toString();
    const bool online = item->data(Qt::UserRole + 2).toBool();
    m_unreadByPeer.remove(peerId);
    syncSessionRowUnreadUI(peerId);
    m_currentPeerId = peerId;
    m_lastMsgId = 0;
    m_seenMessageIds.clear();
    updateChatHeader(title.isEmpty() ? QStringLiteral("好友") : title, online);
    m_centerStack->setCurrentWidget(m_chatArea);
    clearMessageList();
    requestMsgFetchInitial();
    startMsgPolling();
    syncPeerAvatarForCurrentSession();
}

void ChatMainWidget::onSendClicked()
{
    if (!m_inputEdit) {
        return;
    }
    const QString t = m_inputEdit->toPlainText().trimmed();
    if (t.isEmpty()) {
        return;
    }
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        return;
    }
    m_inputEdit->clear();
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("msg_send");
    o[QStringLiteral("token")] = m_sessionToken;
    o[QStringLiteral("peer_user_id")] = m_currentPeerId;
    o[QStringLiteral("text")] = t;
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::requestFriendListForSessions()
{
    if (!m_sessionClient || m_sessionToken.isEmpty()) {
        return;
    }
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("friend_list");
    o[QStringLiteral("token")] = m_sessionToken;
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::requestPeerAvatar(const qint64 peerUserId)
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || peerUserId <= 0) {
        return;
    }
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("peer_avatar"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("peer_user_id"), peerUserId);
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::applySessionRowAvatar(QListWidgetItem *item, const qint64 peerUserId, const bool fetchIfMissing)
{
    if (!item || !m_friendList || peerUserId <= 0) {
        return;
    }
    QWidget *const row = m_friendList->itemWidget(item);
    if (!row) {
        return;
    }
    auto *av = row->findChild<QLabel *>(QStringLiteral("sessionAvatar"));
    if (!av) {
        return;
    }
    const auto setPlaceholder = [av]() {
        QPixmap dot(64, 64);
        dot.fill(QColor(QStringLiteral("#bae7ff")));
        av->setPixmap(makeCircularAvatar(dot, 44));
    };
    const qint64 listRev = item->data(Qt::UserRole + 4).toLongLong();
    if (listRev <= 0) {
        setPlaceholder();
        return;
    }
    const qint64 cachedRev = m_peerAvatarRevByPeer.value(peerUserId, -1);
    if (cachedRev == listRev) {
        const QPixmap p = m_peerAvatarPixByPeer.value(peerUserId);
        if (!p.isNull()) {
            av->setPixmap(p.scaled(44, 44, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            return;
        }
    }
    setPlaceholder();
    if (fetchIfMissing) {
        requestPeerAvatar(peerUserId);
    }
}

void ChatMainWidget::updateSessionListAvatarForPeer(const qint64 peerUserId, const bool fetchIfMissing)
{
    if (!m_friendList || peerUserId <= 0) {
        return;
    }
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *const it = m_friendList->item(i);
        if (!it || it->data(Qt::UserRole).toLongLong() != peerUserId) {
            continue;
        }
        applySessionRowAvatar(it, peerUserId, fetchIfMissing);
        break;
    }
}

void ChatMainWidget::setIncomingAvatarOnLabel(QLabel *av)
{
    if (!av) {
        return;
    }
    if (!m_peerAvatarPix.isNull()) {
        av->setPixmap(m_peerAvatarPix);
        av->setText(QString());
        av->setStyleSheet(QString());
        return;
    }
    av->clear();
    av->setPixmap(QPixmap());
    av->setText(QStringLiteral("👤"));
    av->setAlignment(Qt::AlignCenter);
    av->setStyleSheet(QStringLiteral(
        "QLabel { background: #e6f7ff; border-radius: 18px; font-size: 20px; min-width:36px; max-width:36px; "
        "min-height:36px; max-height:36px; }"));
}

void ChatMainWidget::refreshIncomingAvatarLabels()
{
    if (!m_messageLayout) {
        return;
    }
    for (int i = 0; i < m_messageLayout->count() - 1; ++i) {
        QLayoutItem *li = m_messageLayout->itemAt(i);
        QWidget *row = li ? li->widget() : nullptr;
        if (!row) {
            continue;
        }
        if (auto *lab = row->findChild<QLabel *>(QStringLiteral("incomingSessionAvatar"))) {
            setIncomingAvatarOnLabel(lab);
        }
    }
}

void ChatMainWidget::syncPeerAvatarForCurrentSession()
{
    m_peerAvatarPix = QPixmap();
    if (m_currentPeerId <= 0 || !m_friendList) {
        return;
    }
    QListWidgetItem *const item = m_friendList->currentItem();
    if (!item) {
        return;
    }
    const qint64 rev = item->data(Qt::UserRole + 4).toLongLong();
    if (rev <= 0) {
        m_peerAvatarPixByPeer.remove(m_currentPeerId);
        m_peerAvatarRevByPeer.remove(m_currentPeerId);
        refreshIncomingAvatarLabels();
        return;
    }
    const qint64 cachedRev = m_peerAvatarRevByPeer.value(m_currentPeerId, -1);
    if (cachedRev == rev && m_peerAvatarPixByPeer.contains(m_currentPeerId)) {
        m_peerAvatarPix = m_peerAvatarPixByPeer.value(m_currentPeerId);
        refreshIncomingAvatarLabels();
        return;
    }
    requestPeerAvatar(m_currentPeerId);
}

void ChatMainWidget::requestMsgFetchInitial()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        return;
    }
    m_scrollAfterNextMsgFetch = true;
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("msg_fetch");
    o[QStringLiteral("token")] = m_sessionToken;
    o[QStringLiteral("peer_user_id")] = m_currentPeerId;
    o[QStringLiteral("after_id")] = 0;
    o[QStringLiteral("limit")] = 50;
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::requestMsgFetchPoll()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        return;
    }
    m_scrollAfterNextMsgFetch = false;
    QJsonObject o;
    o[QStringLiteral("type")] = QStringLiteral("msg_fetch");
    o[QStringLiteral("token")] = m_sessionToken;
    o[QStringLiteral("peer_user_id")] = m_currentPeerId;
    o[QStringLiteral("after_id")] = m_lastMsgId;
    o[QStringLiteral("limit")] = 50;
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::applyFriendListToSessions(const QJsonArray &friends)
{
    if (!m_friendList) {
        return;
    }
    /// 刷新列表时 `clear` / `setCurrentRow` 会触发 `itemSelectionChanged`，进而调用
    /// `onFriendSelectionChanged` → `clearMessageList` + 重新 `msg_fetch`，会取消进行中的
    /// `file_sticker_pull` 并删掉占位气泡，导致离线表情永远停在「表情」。
    const qint64 peerBeforeRefresh = m_currentPeerId;
    const QSignalBlocker listSigBlock(*m_friendList);
    m_friendList->setUpdatesEnabled(false);

    QSet<qint64> friendIds;
    QList<QJsonObject> visibleRows;
    visibleRows.reserve(friends.size());
    for (const QJsonValue &v : friends) {
        const QJsonObject fo = v.toObject();
        const qint64 uid = fo.value(QStringLiteral("user_id")).toVariant().toLongLong();
        if (uid <= 0) {
            continue;
        }
        friendIds.insert(uid);
        if (m_hiddenSessionPeers.contains(uid)) {
            continue;
        }
        visibleRows.append(fo);
    }

    QList<qint64> newPinned;
    for (const qint64 p : m_pinnedPeerIds) {
        if (friendIds.contains(p)) {
            newPinned.append(p);
        }
    }
    m_pinnedPeerIds = newPinned;

    for (auto it = m_hiddenSessionPeers.begin(); it != m_hiddenSessionPeers.end();) {
        if (!friendIds.contains(*it)) {
            it = m_hiddenSessionPeers.erase(it);
        } else {
            ++it;
        }
    }

    QList<QJsonObject> ordered;
    QSet<qint64> placed;
    for (const qint64 pid : m_pinnedPeerIds) {
        for (const QJsonObject &fo : visibleRows) {
            const qint64 uid = fo.value(QStringLiteral("user_id")).toVariant().toLongLong();
            if (uid == pid) {
                ordered.append(fo);
                placed.insert(uid);
                break;
            }
        }
    }
    for (const QJsonObject &fo : visibleRows) {
        const qint64 uid = fo.value(QStringLiteral("user_id")).toVariant().toLongLong();
        if (!placed.contains(uid)) {
            ordered.append(fo);
        }
    }

    m_friendList->clear();
    int selectRow = 0;
    for (const QJsonObject &fo : ordered) {
        const qint64 uid = fo.value(QStringLiteral("user_id")).toVariant().toLongLong();
        const QString email = fo.value(QStringLiteral("email")).toString();
        QString nick = fo.value(QStringLiteral("nickname")).toString();
        const QString title = nick.isEmpty() ? email : nick;
        const bool online = fo.value(QStringLiteral("online")).toBool();
        const qint64 lastAt = fo.value(QStringLiteral("last_message_at")).toVariant().toLongLong();
        QString preview;
        QString timeStr;
        if (lastAt > 0) {
            const QString raw = fo.value(QStringLiteral("last_message_preview")).toString();
            const qint64 lastFrom = fo.value(QStringLiteral("last_message_from_user_id")).toVariant().toLongLong();
            if (lastFrom == m_sessionUserId) {
                preview = QStringLiteral("你：") + clipSessionPreview(formatSessionPreviewText(raw));
            } else {
                preview = clipSessionPreview(formatSessionPreviewText(raw));
            }
            timeStr = formatSessionListTime(lastAt);
        } else {
            preview = QStringLiteral("点击开始聊天");
        }
        const int unread = m_unreadByPeer.value(uid, 0);
        const int rowIndex = m_friendList->count();
        const bool pinned = m_pinnedPeerIds.contains(uid);
        auto *item = new QListWidgetItem(m_friendList);
        QWidget *rowW = createSessionRowWidget(title, preview, timeStr, online, unread, pinned);
        rowW->setFixedHeight(64);
        item->setSizeHint(QSize(0, 64));
        item->setData(Qt::UserRole, uid);
        item->setData(Qt::UserRole + 1, title);
        item->setData(Qt::UserRole + 2, online);
        item->setData(Qt::UserRole + 3, email);
        item->setData(Qt::UserRole + 4, fo.value(QStringLiteral("avatar_rev")).toVariant().toLongLong());
        m_friendList->addItem(item);
        m_friendList->setItemWidget(item, rowW);
        applySessionRowAvatar(item, uid);
        if (peerBeforeRefresh > 0 && uid == peerBeforeRefresh) {
            selectRow = rowIndex;
        }
    }
    if (m_friendList->count() > 0) {
        m_friendList->setCurrentRow(selectRow);
    } else {
        m_centerStack->setCurrentWidget(m_centerEmpty);
    }
    applySessionListSearchFilter();
    m_friendList->setUpdatesEnabled(true);

    qint64 newPeer = 0;
    if (QListWidgetItem *cur = m_friendList->currentItem()) {
        newPeer = cur->data(Qt::UserRole).toLongLong();
    }
    if (newPeer != peerBeforeRefresh) {
        onFriendSelectionChanged();
    }
}

void ChatMainWidget::applySessionListSearchFilter()
{
    if (!m_friendList) {
        return;
    }
    const QString needle = m_sessionSearchEdit ? m_sessionSearchEdit->text().trimmed() : QString();

    QListWidgetItem *const current = m_friendList->currentItem();

    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        bool match = true;
        if (!needle.isEmpty()) {
            QString hay;
            hay += it->data(Qt::UserRole + 1).toString();
            hay += QLatin1Char(' ');
            hay += it->data(Qt::UserRole + 3).toString();
            if (QWidget *row = m_friendList->itemWidget(it)) {
                if (auto *nameLb = row->findChild<QLabel *>(QStringLiteral("sessionNameLabel"))) {
                    hay += QLatin1Char(' ');
                    hay += nameLb->text();
                }
                if (auto *prevLb = row->findChild<QLabel *>(QStringLiteral("sessionPreviewLabel"))) {
                    hay += QLatin1Char(' ');
                    hay += prevLb->text();
                }
            }
            match = hay.contains(needle, Qt::CaseInsensitive);
        }
        it->setHidden(!match);
    }

    if (current && !current->isHidden()) {
        return;
    }
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        if (!it->isHidden()) {
            m_friendList->setCurrentItem(it);
            return;
        }
    }
    m_friendList->clearSelection();
}

QString ChatMainWidget::formatMsgDividerTime(qint64 secsSinceEpoch)
{
    if (secsSinceEpoch <= 0) {
        return {};
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(secsSinceEpoch);
    const QDate d = dt.date();
    if (d == QDate::currentDate()) {
        return dt.toString(QStringLiteral("HH:mm"));
    }
    return dt.toString(QStringLiteral("yyyy/MM/dd HH:mm"));
}

void ChatMainWidget::prependTimeDividerIfNeeded(qint64 createdAtSec)
{
    const int idx = m_messageLayout->count() - 1;
    if (createdAtSec > 0) {
        bool needDivider = false;
        if (m_prevMsgCreatedAtForDivider <= 0) {
            needDivider = true;
        } else {
            const QDate curD = QDateTime::fromSecsSinceEpoch(createdAtSec).date();
            const QDate prevD = QDateTime::fromSecsSinceEpoch(m_prevMsgCreatedAtForDivider).date();
            const qint64 gap = qAbs(createdAtSec - m_prevMsgCreatedAtForDivider);
            if (curD != prevD || gap >= 300) {
                needDivider = true;
            }
        }
        if (needDivider) {
            m_messageLayout->insertWidget(idx, createCenteredTimeRow(createdAtSec));
        }
        m_prevMsgCreatedAtForDivider = createdAtSec;
    }
}

void ChatMainWidget::appendMessageLine(const QString &content, bool outgoing, qint64 createdAtSec, qint64 messageId)
{
    if (messageId > 0 && m_seenMessageIds.contains(messageId)) {
        return;
    }
    if (messageId > 0) {
        m_seenMessageIds.insert(messageId);
        if (messageId > m_lastMsgId) {
            m_lastMsgId = messageId;
        }
    }
    prependTimeDividerIfNeeded(createdAtSec);
    QJsonObject fileObj;
    if (parseFileOrImageMessage(content, &fileObj)) {
        const qint64 tid = jsonInt64Member(fileObj, QStringLiteral("transfer_id"));
        removePlaceholderBubblesForFileTransfer(tid, outgoing);
        const QString kind = fileObj.value(QStringLiteral("kind")).toString();
        const QString fileName = fileObj.value(QStringLiteral("name")).toString();
        const QString state = fileObj.value(QStringLiteral("state")).toString();
        const bool isSticker = fileContentJsonIsSticker(fileObj);
        const bool asImage =
            (kind == QStringLiteral("image")) || (kind == QStringLiteral("file") && isImageFileNameString(fileName));
        qDebug().noquote() << QStringLiteral("[LANCS/msg] file_line mid=%1 out=%2 tid=%3 kind=%4 sticker=%5 state=%6 "
                                             "has_local=%7")
                                  .arg(messageId)
                                  .arg(outgoing ? 1 : 0)
                                  .arg(tid)
                                  .arg(kind)
                                  .arg(isSticker ? 1 : 0)
                                  .arg(state)
                                  .arg(QFile::exists(m_localPathByTransferId.value(tid)) ? 1 : 0);
        if (asImage) {
            QWidget *row = createImageBubble(fileObj, outgoing, tid);
            m_messageLayout->insertWidget(m_messageLayout->count() - 1, row);
            if (!outgoing && state != QStringLiteral("failed") && tid > 0) {
                const QString path = m_localPathByTransferId.value(tid);
                if (path.isEmpty() || !QFile::exists(path)) {
                    if (isSticker) {
                        if (QLabel *pic = row->findChild<QLabel *>(QStringLiteral("lanStickerBubblePic"))) {
                            beginStickerArtifactPullIfNeeded(tid, fileName, pic);
                        } else {
                            qDebug().noquote() << QStringLiteral("[LANCS/sticker] missing lanStickerBubblePic tid=%1")
                                                      .arg(tid);
                        }
                    } else if (QLabel *pic = row->findChild<QLabel *>(QStringLiteral("lanImageBubblePic"))) {
                        beginStickerArtifactPullIfNeeded(tid, fileName, pic);
                    }
                }
            }
        } else {
            QWidget *row = createFileBubble(fileObj, outgoing, tid);
            m_messageLayout->insertWidget(m_messageLayout->count() - 1, row);
            /// 非图片类文件不走 `file_sticker_pull`（服务端仅缓存离线表情/图片到 sticker_cache）。
        }
    } else {
        m_messageLayout->insertWidget(m_messageLayout->count() - 1, createBubble(content, outgoing));
    }
}

QWidget *ChatMainWidget::createFileBubble(const QJsonObject &fileObj, bool isOutgoing, qint64 transferId)
{
    const QString name = fileObj.value(QStringLiteral("name")).toString();
    const qint64 sz = jsonInt64Member(fileObj, QStringLiteral("size"));
    const QString state = fileObj.value(QStringLiteral("state")).toString();
    const QString reason = fileObj.value(QStringLiteral("reason")).toString();

    auto *row = new QWidget(m_messageContainer);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(4, 4, 4, 4);
    rowLay->setSpacing(10);

    auto *av = new QLabel(row);
    av->setFixedSize(36, 36);
    if (isOutgoing) {
        QPixmap selfPix;
        if (!m_userEmail.isEmpty() && LocalProfile::loadAvatarPixmap(m_userEmail, 36, &selfPix) && !selfPix.isNull()) {
            av->setPixmap(selfPix);
        } else if (!m_avatarPix.isNull()) {
            av->setPixmap(m_avatarPix);
        }
    } else {
        av->setObjectName(QStringLiteral("incomingSessionAvatar"));
        setIncomingAvatarOnLabel(av);
    }

    auto *bubble = new QFrame(row);
    bubble->setObjectName(isOutgoing ? QStringLiteral("bubbleOut") : QStringLiteral("bubbleIn"));
    auto *bubbleLay = new QHBoxLayout(bubble);
    bubbleLay->setContentsMargins(12, 10, 12, 10);
    bubbleLay->setSpacing(10);

    auto *textCol = new QWidget(bubble);
    auto *textLay = new QVBoxLayout(textCol);
    textLay->setContentsMargins(0, 0, 0, 0);
    textLay->setSpacing(4);

    QFontMetrics fm(font());
    const QString elidedName =
        fm.elidedText(name.isEmpty() ? QStringLiteral("未命名文件") : name, Qt::ElideMiddle, 240);

    auto *nameLb = new QLabel(elidedName, textCol);
    nameLb->setWordWrap(false);
    nameLb->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameLb->setObjectName(isOutgoing ? QStringLiteral("bubbleTextOut") : QStringLiteral("bubbleTextIn"));

    QString statusText;
    if (state == QStringLiteral("failed")) {
        statusText = reason.isEmpty() ? QStringLiteral("失败") : QStringLiteral("失败 · %1").arg(reason);
    } else if (isOutgoing) {
        statusText = QStringLiteral("已发送 · %1").arg(humanFileSize(sz));
    } else {
        statusText = QStringLiteral("已接收 · %1").arg(humanFileSize(sz));
    }
    auto *metaLb = new QLabel(statusText, textCol);
    metaLb->setObjectName(QStringLiteral("lanFileMeta"));
    metaLb->setStyleSheet(QStringLiteral("color: rgba(0,0,0,0.55); font-size: 12px;"));

    textLay->addWidget(nameLb);
    textLay->addWidget(metaLb);
    bubbleLay->addWidget(textCol, 1);

    auto *icon = new QLabel(bubble);
    icon->setPixmap(qApp->style()->standardIcon(QStyle::SP_FileIcon).pixmap(32, 32));
    bubbleLay->addWidget(icon, 0, Qt::AlignVCenter);

    bubble->setMaximumWidth(440);

    bubble->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bubble, &QWidget::customContextMenuRequested, this, [this, transferId, bubble](const QPoint &pos) {
        const QString path = m_localPathByTransferId.value(transferId);
        QMenu menu(bubble);
        QAction *openDir = menu.addAction(QStringLiteral("打开所在文件夹"));
        const bool ok = !path.isEmpty() && QFile::exists(path);
        openDir->setEnabled(ok);
        if (ok) {
            connect(openDir, &QAction::triggered, this, [path]() {
                const QFileInfo fi(path);
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
            });
        } else {
            openDir->setToolTip(QStringLiteral("仅在本机有对应文件路径时可用"));
        }
        menu.exec(bubble->mapToGlobal(pos));
    });

    if (!isOutgoing) {
        rowLay->addWidget(av, 0, Qt::AlignTop);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addStretch(1);
    } else {
        rowLay->addStretch(1);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addWidget(av, 0, Qt::AlignTop);
    }
    return row;
}

QWidget *ChatMainWidget::createImageBubble(const QJsonObject &fileObj, bool isOutgoing, qint64 transferId)
{
    const QString name = fileObj.value(QStringLiteral("name")).toString();
    const qint64 sz = jsonInt64Member(fileObj, QStringLiteral("size"));
    const QString state = fileObj.value(QStringLiteral("state")).toString();
    const QString reason = fileObj.value(QStringLiteral("reason")).toString();
    const bool sticker = fileContentJsonIsSticker(fileObj);

    auto *row = new QWidget(m_messageContainer);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(4, 4, 4, 4);
    rowLay->setSpacing(10);

    auto *av = new QLabel(row);
    av->setFixedSize(36, 36);
    if (isOutgoing) {
        QPixmap selfPix;
        if (!m_userEmail.isEmpty() && LocalProfile::loadAvatarPixmap(m_userEmail, 36, &selfPix) && !selfPix.isNull()) {
            av->setPixmap(selfPix);
        } else if (!m_avatarPix.isNull()) {
            av->setPixmap(m_avatarPix);
        }
    } else {
        av->setObjectName(QStringLiteral("incomingSessionAvatar"));
        setIncomingAvatarOnLabel(av);
    }

    auto *bubble = new QFrame(row);
    auto *bubbleLay = new QVBoxLayout(bubble);
    bubbleLay->setContentsMargins(12, 10, 12, 10);
    bubbleLay->setSpacing(6);
    if (sticker && state != QStringLiteral("failed")) {
        bubble->setObjectName(QStringLiteral("lanStickerBubble"));
        bubbleLay->setContentsMargins(0, 0, 0, 0);
        bubbleLay->setSpacing(0);
        bubble->setStyleSheet(QStringLiteral(
            "QFrame#lanStickerBubble { background: transparent; border: none; }"));
    } else {
        bubble->setObjectName(isOutgoing ? QStringLiteral("bubbleOut") : QStringLiteral("bubbleIn"));
    }

    if (state == QStringLiteral("failed")) {
        auto *errLb = new QLabel(reason.isEmpty() ? QStringLiteral("失败") : QStringLiteral("失败 · %1").arg(reason),
                                 bubble);
        errLb->setWordWrap(true);
        errLb->setObjectName(isOutgoing ? QStringLiteral("bubbleTextOut") : QStringLiteral("bubbleTextIn"));
        bubbleLay->addWidget(errLb);
    } else {
        auto *imgLb = new QLabel(bubble);
        imgLb->setAlignment(Qt::AlignCenter);
        imgLb->setWordWrap(false);
        imgLb->setObjectName(sticker ? QStringLiteral("lanStickerBubblePic") : QStringLiteral("lanImageBubblePic"));
        const QString path = m_localPathByTransferId.value(transferId);
        if (!path.isEmpty() && QFile::exists(path)) {
            if (sticker) {
                applyStickerImageToLabel(imgLb, path, bubble);
                imgLb->setCursor(Qt::PointingHandCursor);
                auto *f = new ImageBubbleClickFilter(imgLb, this, path);
                imgLb->installEventFilter(f);
            } else {
                QPixmap pm;
                if (pm.load(path)) {
                    imgLb->setPixmap(pm.scaledToWidth(260, Qt::SmoothTransformation));
                    imgLb->setCursor(Qt::PointingHandCursor);
                    auto *f = new ImageBubbleClickFilter(imgLb, this, path);
                    imgLb->installEventFilter(f);
                } else {
                    imgLb->setText(QStringLiteral("无法预览"));
                    imgLb->setObjectName(QStringLiteral("bubbleTextIn"));
                }
            }
        } else {
            if (sticker) {
                imgLb->setText(QStringLiteral("表情"));
            } else {
                imgLb->setText(name.isEmpty() ? QStringLiteral("图片") : name);
            }
            imgLb->setStyleSheet(QStringLiteral("color: rgba(0,0,0,0.45);"));
        }
        bubbleLay->addWidget(imgLb);

        if (!sticker) {
            QString statusText;
            if (isOutgoing) {
                statusText = QStringLiteral("已发送 · %1").arg(humanFileSize(sz));
            } else {
                statusText = QStringLiteral("已接收 · %1").arg(humanFileSize(sz));
            }
            auto *metaLb = new QLabel(statusText, bubble);
            metaLb->setObjectName(QStringLiteral("lanFileMeta"));
            if (isOutgoing) {
                metaLb->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.75); font-size: 12px;"));
            } else {
                metaLb->setStyleSheet(QStringLiteral("color: rgba(0,0,0,0.55); font-size: 12px;"));
            }
            bubbleLay->addWidget(metaLb);
        }
    }

    bubble->setMaximumWidth(sticker && state != QStringLiteral("failed") ? kStickerDisplayMaxPx + 24 : 440);

    bubble->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bubble, &QWidget::customContextMenuRequested, this, [this, transferId, bubble](const QPoint &pos) {
        const QString path = m_localPathByTransferId.value(transferId);
        QMenu menu(bubble);
        QAction *openFile = menu.addAction(QStringLiteral("打开文件"));
        QAction *openDir = menu.addAction(QStringLiteral("打开所在文件夹"));
        const bool ok = !path.isEmpty() && QFile::exists(path);
        openFile->setEnabled(ok);
        openDir->setEnabled(ok);
        if (ok) {
            connect(openFile, &QAction::triggered, this, [path]() {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()));
            });
            connect(openDir, &QAction::triggered, this, [path]() {
                const QFileInfo fi(path);
                QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
            });
        } else {
            openFile->setToolTip(QStringLiteral("仅在本机有对应文件路径时可用"));
            openDir->setToolTip(QStringLiteral("仅在本机有对应文件路径时可用"));
        }
        menu.exec(bubble->mapToGlobal(pos));
    });

    if (!isOutgoing) {
        rowLay->addWidget(av, 0, Qt::AlignTop);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addStretch(1);
    } else {
        rowLay->addStretch(1);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addWidget(av, 0, Qt::AlignTop);
    }
    return row;
}

void ChatMainWidget::handleMsgFetchOk(const QJsonObject &obj)
{
    const qint64 peer = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
    if (peer != m_currentPeerId || m_currentPeerId <= 0) {
        return;
    }
    const QJsonArray arr = obj.value(QStringLiteral("messages")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject m = v.toObject();
        const qint64 mid = m.value(QStringLiteral("message_id")).toVariant().toLongLong();
        const qint64 from = m.value(QStringLiteral("from_user_id")).toVariant().toLongLong();
        const QString content = m.value(QStringLiteral("content")).toString();
        const qint64 cat = m.value(QStringLiteral("created_at")).toVariant().toLongLong();
        appendMessageLine(content, from == m_sessionUserId, cat, mid);
    }
    if (m_scrollAfterNextMsgFetch) {
        m_scrollAfterNextMsgFetch = false;
        scrollMessagesToBottom();
    }
}

void ChatMainWidget::handleMsgSendOk(const QJsonObject &obj)
{
    const qint64 to = obj.value(QStringLiteral("to_user_id")).toVariant().toLongLong();
    const qint64 from = obj.value(QStringLiteral("from_user_id")).toVariant().toLongLong();
    if (from != m_sessionUserId) {
        return;
    }
    const QString content = obj.value(QStringLiteral("content")).toString();
    const qint64 mid = obj.value(QStringLiteral("message_id")).toVariant().toLongLong();
    const qint64 cat = obj.value(QStringLiteral("created_at")).toVariant().toLongLong();
    if (to == m_currentPeerId && m_currentPeerId > 0) {
        appendMessageLine(content, true, cat, mid);
        scrollMessagesToBottom();
    }
    updateSessionRowPreview(to, content, cat, true);
}

void ChatMainWidget::handleMsgPush(const QJsonObject &obj)
{
    const qint64 to = obj.value(QStringLiteral("to_user_id")).toVariant().toLongLong();
    const qint64 from = obj.value(QStringLiteral("from_user_id")).toVariant().toLongLong();
    if (from != m_sessionUserId && to != m_sessionUserId) {
        return;
    }
    const qint64 peerInMsg = (from == m_sessionUserId) ? to : from;
    if (m_hiddenSessionPeers.contains(peerInMsg)) {
        m_hiddenSessionPeers.remove(peerInMsg);
        saveSessionUiPrefs();
        requestFriendListForSessions();
    }
    const QString content = obj.value(QStringLiteral("content")).toString();
    const qint64 mid = obj.value(QStringLiteral("message_id")).toVariant().toLongLong();
    const qint64 cat = obj.value(QStringLiteral("created_at")).toVariant().toLongLong();
    const bool viewingThis = (m_currentPeerId > 0 && peerInMsg == m_currentPeerId);
    if (viewingThis) {
        const bool outgoing = (from == m_sessionUserId);
        appendMessageLine(content, outgoing, cat, mid);
    } else {
        if (to == m_sessionUserId && from != m_sessionUserId) {
            m_unreadByPeer[from] = m_unreadByPeer.value(from, 0) + 1;
        }
    }
    updateSessionRowPreview(peerInMsg, content, cat, from == m_sessionUserId);
}

void ChatMainWidget::handleBusinessErrorFrame(const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("corr"))) {
        return;
    }
    const int code = obj.value(QStringLiteral("code")).toInt();
    const QString msg = obj.value(QStringLiteral("message")).toString();
    if (m_pendingMsgClearPeer > 0) {
        const bool fileBand = (code >= 5001 && code <= 5009);
        if (!fileBand) {
            QMessageBox::warning(this, QStringLiteral("清空失败"),
                                 msg.isEmpty() ? QStringLiteral("未知错误") : msg);
            m_pendingMsgClearPeer = 0;
            return;
        }
    }
    if (code == kErrPeerOffline) {
        updateOutgoingFileOfferError(QStringLiteral("待发送 · 对方已离线"));
        resetFileSendState();
        return;
    }
    if (m_outgoingFileOfferBubble) {
        updateOutgoingFileOfferError(
            QStringLiteral("发送失败 · %1").arg(msg.isEmpty() ? QStringLiteral("错误") : msg));
    }
    resetFileSendState();
    cleanupFileReceive(false);
}

void ChatMainWidget::updateOutgoingFileOfferError(const QString &statusLine)
{
    if (!m_outgoingFileOfferBubble) {
        return;
    }
    if (auto *meta = m_outgoingFileOfferBubble->findChild<QLabel *>(QStringLiteral("lanOfferStatus"))) {
        meta->setText(statusLine);
    }
}

void ChatMainWidget::removeMessageWidget(QWidget *w)
{
    if (!w || !m_messageLayout) {
        return;
    }
    m_messageLayout->removeWidget(w);
    w->deleteLater();
}

void ChatMainWidget::removePlaceholderBubblesForFileTransfer(qint64 transferId, bool outgoing)
{
    if (transferId <= 0) {
        return;
    }
    if (outgoing) {
        if (!m_outgoingFileOfferBubble) {
            return;
        }
        const QVariant v = m_outgoingFileOfferBubble->property("transferId");
        if (v.toLongLong() == transferId) {
            removeMessageWidget(m_outgoingFileOfferBubble);
            m_outgoingFileOfferBubble.clear();
        }
        return;
    }
    const auto it = m_incomingFileOfferBubbleByTid.find(transferId);
    if (it == m_incomingFileOfferBubbleByTid.end()) {
        return;
    }
    QWidget *const w = it.value().data();
    m_incomingFileOfferBubbleByTid.remove(transferId);
    m_incomingFileOfferParamsByTid.remove(transferId);
    if (w) {
        removeMessageWidget(w);
    }
}

void ChatMainWidget::handleFileOfferDelivered(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid <= 0) {
        return;
    }
    if (m_pendingOfferTransferId != 0 && tid != m_pendingOfferTransferId) {
        return;
    }
    m_pendingOfferTransferId = tid;
    if (!m_outgoingFileOfferBubble) {
        return;
    }
    m_outgoingFileOfferBubble->setProperty("transferId", QVariant::fromValue(tid));
    if (m_pendingOfferAsSticker) {
        return;
    }
    if (auto *meta = m_outgoingFileOfferBubble->findChild<QLabel *>(QStringLiteral("lanOfferStatus"))) {
        if (m_pendingOfferIsImage) {
            meta->setText(QStringLiteral("已发送 · 图片 · %1").arg(humanFileSize(m_pendingOfferFileSize)));
        } else {
            meta->setText(QStringLiteral("已发送 · %1").arg(humanFileSize(m_pendingOfferFileSize)));
        }
    }
}

void ChatMainWidget::appendOutgoingFileOfferRow(const QString &name, qint64 sizeBytes, bool isImage,
                                                const QString &localPreviewPath, bool asSticker)
{
    if (!m_messageLayout) {
        return;
    }
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    prependTimeDividerIfNeeded(now);
    auto *row = new QWidget(m_messageContainer);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(4, 4, 4, 4);
    rowLay->setSpacing(10);
    row->setProperty("transferId", QVariant::fromValue(static_cast<qint64>(0)));

    auto *av = new QLabel(row);
    av->setFixedSize(36, 36);
    QPixmap selfPix;
    if (!m_userEmail.isEmpty() && LocalProfile::loadAvatarPixmap(m_userEmail, 36, &selfPix) && !selfPix.isNull()) {
        av->setPixmap(selfPix);
    } else if (!m_avatarPix.isNull()) {
        av->setPixmap(m_avatarPix);
    }

    auto *bubble = new QFrame(row);
    auto *bubbleLay = new QHBoxLayout(bubble);
    bubbleLay->setContentsMargins(0, 0, 0, 0);
    bubbleLay->setSpacing(0);

    if (asSticker && isImage && !localPreviewPath.isEmpty()) {
        bubble->setObjectName(QStringLiteral("lanStickerBubble"));
        bubble->setStyleSheet(QStringLiteral(
            "QFrame#lanStickerBubble { background: transparent; border: none; }"));
        auto *prev = new QLabel(bubble);
        prev->setObjectName(QStringLiteral("lanStickerInlinePreview"));
        prev->setAlignment(Qt::AlignCenter);
        applyStickerImageToLabel(prev, localPreviewPath, bubble);
        prev->setCursor(Qt::PointingHandCursor);
        auto *f = new ImageBubbleClickFilter(prev, this, localPreviewPath);
        prev->installEventFilter(f);
        bubbleLay->addWidget(prev);
        bubble->setMaximumWidth(kStickerDisplayMaxPx + 24);
    } else {
        bubble->setObjectName(QStringLiteral("bubbleOut"));
        bubbleLay->setContentsMargins(12, 10, 12, 10);
        bubbleLay->setSpacing(10);

        auto *textCol = new QWidget(bubble);
        auto *textLay = new QVBoxLayout(textCol);
        textLay->setContentsMargins(0, 0, 0, 0);
        textLay->setSpacing(4);

        if (isImage && !localPreviewPath.isEmpty()) {
            QPixmap pm;
            if (pm.load(localPreviewPath)) {
                auto *prev = new QLabel(textCol);
                prev->setPixmap(pm.scaledToWidth(260, Qt::SmoothTransformation));
                prev->setObjectName(QStringLiteral("lanOfferImagePreview"));
                textLay->addWidget(prev);
            }
        }

        QFontMetrics fm(font());
        const QString elidedName =
            fm.elidedText(name.isEmpty() ? QStringLiteral("未命名文件") : name, Qt::ElideMiddle, 240);
        auto *nameLb = new QLabel(elidedName, textCol);
        nameLb->setWordWrap(false);
        nameLb->setTextInteractionFlags(Qt::TextSelectableByMouse);
        nameLb->setObjectName(QStringLiteral("bubbleTextOut"));

        auto *metaLb =
            new QLabel(isImage ? QStringLiteral("发送中 · 图片 · %1").arg(humanFileSize(sizeBytes))
                               : QStringLiteral("发送中 · %1").arg(humanFileSize(sizeBytes)),
                       textCol);
        metaLb->setObjectName(QStringLiteral("lanOfferStatus"));
        metaLb->setStyleSheet(QStringLiteral("color: rgba(255,255,255,0.85); font-size: 12px;"));

        textLay->addWidget(nameLb);
        textLay->addWidget(metaLb);
        bubbleLay->addWidget(textCol, 1);

        if (!isImage) {
            auto *icon = new QLabel(bubble);
            icon->setPixmap(qApp->style()->standardIcon(QStyle::SP_FileIcon).pixmap(32, 32));
            bubbleLay->addWidget(icon, 0, Qt::AlignVCenter);
        }

        bubble->setMaximumWidth(440);
    }

    rowLay->addStretch(1);
    rowLay->addWidget(bubble, 0, Qt::AlignTop);
    rowLay->addWidget(av, 0, Qt::AlignTop);

    m_messageLayout->insertWidget(m_messageLayout->count() - 1, row);
    m_outgoingFileOfferBubble = row;
    scrollMessagesToBottom();
}

void ChatMainWidget::appendIncomingFileOfferRow(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid <= 0 || m_incomingFileOfferBubbleByTid.contains(tid)) {
        return;
    }
    m_incomingFileOfferParamsByTid.insert(tid, obj);
    const QString name = obj.value(QStringLiteral("file_name")).toString();
    const qint64 sz = jsonInt64Member(obj, QStringLiteral("file_size"));
    const bool incomingIsImage = isImageFileNameString(name);

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    prependTimeDividerIfNeeded(now);
    auto *row = new QWidget(m_messageContainer);
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(4, 4, 4, 4);
    rowLay->setSpacing(10);

    auto *av = new QLabel(row);
    av->setFixedSize(36, 36);
    av->setObjectName(QStringLiteral("incomingSessionAvatar"));
    setIncomingAvatarOnLabel(av);

    auto *bubble = new QFrame(row);
    bubble->setObjectName(QStringLiteral("bubbleIn"));
    auto *bubbleLay = new QHBoxLayout(bubble);
    bubbleLay->setContentsMargins(12, 10, 12, 10);
    bubbleLay->setSpacing(10);

    auto *textCol = new QWidget(bubble);
    auto *textLay = new QVBoxLayout(textCol);
    textLay->setContentsMargins(0, 0, 0, 0);
    textLay->setSpacing(4);

    QFontMetrics fm(font());
    const QString elidedName = fm.elidedText(name.isEmpty() ? QStringLiteral("未命名文件") : name, Qt::ElideMiddle, 240);
    auto *nameLb = new QLabel(elidedName, textCol);
    nameLb->setWordWrap(false);
    nameLb->setTextInteractionFlags(Qt::TextSelectableByMouse);
    nameLb->setObjectName(QStringLiteral("bubbleTextIn"));

    auto *metaLb =
        new QLabel(incomingIsImage ? QStringLiteral("待接收 · 图片 · %1").arg(humanFileSize(sz))
                                   : QStringLiteral("待接收 · %1").arg(humanFileSize(sz)),
                   textCol);
    metaLb->setObjectName(QStringLiteral("lanIncomingOfferStatus"));
    metaLb->setStyleSheet(QStringLiteral("color: rgba(0,0,0,0.55); font-size: 12px;"));

    textLay->addWidget(nameLb);
    textLay->addWidget(metaLb);
    bubbleLay->addWidget(textCol, 1);

    if (!incomingIsImage) {
        auto *icon = new QLabel(bubble);
        icon->setPixmap(qApp->style()->standardIcon(QStyle::SP_FileIcon).pixmap(32, 32));
        bubbleLay->addWidget(icon, 0, Qt::AlignVCenter);
    }

    bubble->setMaximumWidth(440);

    bubble->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(bubble, &QWidget::customContextMenuRequested, this, [this, tid, bubble](const QPoint &pos) {
        QMenu menu(bubble);
        QAction *recv = menu.addAction(QStringLiteral("接收"));
        const bool canRecv = (m_recvTransferId == 0) && m_sessionClient && !m_sessionToken.isEmpty();
        recv->setEnabled(canRecv);
        if (!canRecv && m_recvTransferId != 0) {
            recv->setToolTip(QStringLiteral("已有文件正在接收"));
        }
        QAction *chosen = menu.exec(bubble->mapToGlobal(pos));
        if (chosen == recv && canRecv) {
            acceptIncomingFileOffer(tid);
        }
    });

    rowLay->addWidget(av, 0, Qt::AlignTop);
    rowLay->addWidget(bubble, 0, Qt::AlignTop);
    rowLay->addStretch(1);

    m_messageLayout->insertWidget(m_messageLayout->count() - 1, row);
    m_incomingFileOfferBubbleByTid.insert(tid, row);

    if (incomingIsImage) {
        QTimer::singleShot(0, this, [this, tid]() {
            if (m_recvTransferId == 0) {
                acceptIncomingFileOffer(tid);
            }
        });
    }
}

void ChatMainWidget::acceptIncomingFileOffer(qint64 transferId)
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || transferId <= 0) {
        return;
    }
    const QJsonObject obj = m_incomingFileOfferParamsByTid.value(transferId);
    if (obj.isEmpty()) {
        return;
    }
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    const QString name = obj.value(QStringLiteral("file_name")).toString();
    const qint64 sz = jsonInt64Member(obj, QStringLiteral("file_size"));
    const QString sha = obj.value(QStringLiteral("sha256_hex")).toString();
    if (tid != transferId || name.isEmpty() || sz <= 0) {
        return;
    }
    if (m_recvTransferId != 0) {
        return;
    }

    QString dir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (dir.isEmpty()) {
        dir = QDir::homePath();
    }
    const QFileInfo nameFi(name);
    const QString nameStem = nameFi.completeBaseName();
    const QString nameExt = nameFi.suffix();

    QString savePath = QDir(dir).filePath(name);
    for (int n = 1; QFile::exists(savePath); ++n) {
        const QString disambig =
            nameExt.isEmpty()
                ? QStringLiteral("%1 (%2)").arg(nameStem).arg(n)
                : QStringLiteral("%1 (%2).%3").arg(nameStem).arg(n).arg(nameExt);
        savePath = QDir(dir).filePath(disambig);
    }
    auto out = std::make_unique<QSaveFile>(savePath);
    if (!out->open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, QStringLiteral("接收文件"), QStringLiteral("无法创建保存文件"));
        return;
    }
    cleanupFileReceive(false);
    m_recvTransferId = tid;
    m_recvExpectedSize = sz;
    m_recvWritten = 0;
    m_recvExpectedSha = sha;
    m_recvFile = std::move(out);
    m_recvHash = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);

    if (QWidget *w = m_incomingFileOfferBubbleByTid.value(tid).data()) {
        if (auto *meta = w->findChild<QLabel *>(QStringLiteral("lanIncomingOfferStatus"))) {
            meta->setText(QStringLiteral("接收中 · %1").arg(humanFileSize(sz)));
        }
    }

    QJsonObject aj;
    aj.insert(QStringLiteral("type"), QStringLiteral("file_accept"));
    aj.insert(QStringLiteral("token"), m_sessionToken);
    aj.insert(QStringLiteral("transfer_id"), tid);
    m_sessionClient->sendJsonObject(aj);
}

void ChatMainWidget::startMsgPolling()
{
    if (m_msgPollTimer) {
        m_msgPollTimer->start();
    }
}

void ChatMainWidget::stopMsgPolling()
{
    if (m_msgPollTimer) {
        m_msgPollTimer->stop();
    }
}

void ChatMainWidget::onMsgPollTick()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        return;
    }
    if (m_lastMsgId <= 0) {
        return;
    }
    requestMsgFetchPoll();
}

QString ChatMainWidget::formatSessionListTime(qint64 secsSinceEpoch)
{
    if (secsSinceEpoch <= 0) {
        return QString();
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(secsSinceEpoch);
    const QDate d = dt.date();
    const QDate today = QDate::currentDate();
    if (d == today) {
        return dt.toString(QStringLiteral("HH:mm"));
    }
    if (d == today.addDays(-1)) {
        return QStringLiteral("昨天");
    }
    if (d.year() == today.year()) {
        return dt.toString(QStringLiteral("M/d"));
    }
    return dt.toString(QStringLiteral("yy/M/d"));
}

QString ChatMainWidget::clipSessionPreview(const QString &text)
{
    constexpr int kMaxChars = 40;
    if (text.length() <= kMaxChars) {
        return text;
    }
    return text.left(kMaxChars - 1) + QStringLiteral("…");
}

void ChatMainWidget::updateSessionRowPreview(qint64 peerId, const QString &rawContent, qint64 createdAtSec,
                                             bool sentBySelf)
{
    if (!m_friendList || peerId <= 0) {
        return;
    }
    QString p = clipSessionPreview(formatSessionPreviewText(rawContent));
    if (sentBySelf) {
        p = QStringLiteral("你：") + p;
    }
    const QString timeText = formatSessionListTime(createdAtSec);
    const int unread = m_unreadByPeer.value(peerId, 0);
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        if (it->data(Qt::UserRole).toLongLong() != peerId) {
            continue;
        }
        QWidget *w = m_friendList->itemWidget(it);
        if (!w) {
            return;
        }
        if (auto *pl = w->findChild<QLabel *>(QStringLiteral("sessionPreviewLabel"))) {
            pl->setText(p);
        }
        if (auto *tl = w->findChild<QLabel *>(QStringLiteral("sessionTimeLabel"))) {
            tl->setText(timeText);
        }
        if (auto *ul = w->findChild<QLabel *>(QStringLiteral("sessionRowUnread"))) {
            if (unread > 0) {
                ul->setText(unread > 99 ? QStringLiteral("99+") : QString::number(unread));
                ul->show();
            } else {
                ul->hide();
            }
        }
        break;
    }
}

void ChatMainWidget::syncSessionRowUnreadUI(qint64 peerId)
{
    if (!m_friendList || peerId <= 0) {
        return;
    }
    const int unread = m_unreadByPeer.value(peerId, 0);
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        if (it->data(Qt::UserRole).toLongLong() != peerId) {
            continue;
        }
        QWidget *w = m_friendList->itemWidget(it);
        if (!w) {
            return;
        }
        if (auto *ul = w->findChild<QLabel *>(QStringLiteral("sessionRowUnread"))) {
            if (unread > 0) {
                ul->setText(unread > 99 ? QStringLiteral("99+") : QString::number(unread));
                ul->show();
            } else {
                ul->hide();
            }
        }
        break;
    }
}

void ChatMainWidget::resetFileSendState()
{
    if (m_fileSendTimer) {
        m_fileSendTimer->stop();
    }
    m_outgoingFile.reset();
    m_pendingOfferPath.clear();
    m_pendingOfferFileSize = 0;
    m_pendingOfferIsImage = false;
    m_pendingOfferAsSticker = false;
    m_pendingOfferTransferId = 0;
    m_fileSendGateReady = false;
}

void ChatMainWidget::rememberTransferDiskPath(const qint64 transferId, const QString &absolutePath)
{
    if (transferId <= 0 || absolutePath.isEmpty() || m_userEmail.isEmpty()) {
        return;
    }
    const QString abs = QFileInfo(absolutePath).absoluteFilePath();
    m_localPathByTransferId.insert(transferId, abs);
    LocalProfile::rememberTransferLocalPath(m_userEmail, transferId, abs);
}

void ChatMainWidget::cleanupFileReceive(const bool commit)
{
    if (m_recvFile) {
        if (commit) {
            m_recvFile->commit();
        } else {
            m_recvFile->cancelWriting();
        }
        m_recvFile.reset();
    }
    m_recvHash.reset();
    m_recvTransferId = 0;
    m_recvExpectedSize = 0;
    m_recvWritten = 0;
    m_recvExpectedSha.clear();
}

void ChatMainWidget::tryStartOutgoingFileTransfer()
{
    if (!m_sessionClient || m_sessionToken.isEmpty()) {
        return;
    }
    if (!m_fileSendGateReady || m_pendingOfferTransferId <= 0 || m_pendingOfferPath.isEmpty()) {
        return;
    }
    if (m_outgoingFile) {
        return;
    }
    auto f = std::make_unique<QFile>(m_pendingOfferPath);
    if (!f->open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("发送文件"), QStringLiteral("无法打开本地文件"));
        updateOutgoingFileOfferError(QStringLiteral("发送失败 · 无法打开本地文件"));
        resetFileSendState();
        return;
    }
    m_outgoingFile = std::move(f);
    m_outgoingSeq = 0;
    m_fileSendTimer->start();
}

void ChatMainWidget::onPickSendFileClicked()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        QMessageBox::information(this, QStringLiteral("发送文件"), QStringLiteral("请先选择一个好友会话"));
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择要发送的文件"));
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo fi(path);
    const qint64 sz = fi.size();
    if (sz <= 0 || sz > kMaxFileTransferBytes) {
        QMessageBox::warning(this, QStringLiteral("发送文件"), QStringLiteral("文件无效或超过 512MB"));
        return;
    }
    const QByteArray digest = sha256OfFile(path);
    if (digest.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("发送文件"), QStringLiteral("无法读取文件内容"));
        return;
    }
    const QString hexLower = QString::fromLatin1(digest.toHex()).toLower();
    resetFileSendState();
    m_pendingOfferPath = path;
    m_pendingOfferFileSize = sz;
    m_pendingOfferIsImage = false;
    m_pendingOfferTransferId = 0;
    m_fileSendGateReady = false;
    appendOutgoingFileOfferRow(fi.fileName(), sz, false, QString());

    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("file_offer"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("peer_user_id"), m_currentPeerId);
    o.insert(QStringLiteral("file_name"), fi.fileName());
    o.insert(QStringLiteral("file_size"), sz);
    o.insert(QStringLiteral("sha256_hex"), hexLower);
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::sendLocalImageAsFileOffer(const QString &path, const bool asSticker)
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        QMessageBox::information(this, QStringLiteral("发送图片"), QStringLiteral("请先选择一个好友会话"));
        return;
    }
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo fi(path);
    const qint64 sz = fi.size();
    if (sz <= 0 || sz > kMaxFileTransferBytes) {
        QMessageBox::warning(this, QStringLiteral("发送图片"), QStringLiteral("文件无效或超过 512MB"));
        return;
    }
    if (!isImageFileNameString(fi.fileName())) {
        QMessageBox::warning(this, QStringLiteral("发送图片"), QStringLiteral("请选择常见图片格式（如 PNG、JPEG、GIF）"));
        return;
    }
    const QByteArray digest = sha256OfFile(path);
    if (digest.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("发送图片"), QStringLiteral("无法读取文件内容"));
        return;
    }
    const QString hexLower = QString::fromLatin1(digest.toHex()).toLower();
    resetFileSendState();
    m_pendingOfferPath = path;
    m_pendingOfferFileSize = sz;
    m_pendingOfferIsImage = true;
    m_pendingOfferAsSticker = asSticker;
    m_pendingOfferTransferId = 0;
    m_fileSendGateReady = false;
    appendOutgoingFileOfferRow(fi.fileName(), sz, true, path, asSticker);

    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("file_offer"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("peer_user_id"), m_currentPeerId);
    o.insert(QStringLiteral("file_name"), fi.fileName());
    o.insert(QStringLiteral("file_size"), sz);
    o.insert(QStringLiteral("sha256_hex"), hexLower);
    if (asSticker) {
        o.insert(QStringLiteral("as_sticker"), true);
    }
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::onPickSendImageClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择图片"),
        QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.gif *.webp);;所有文件 (*.*)"));
    if (path.isEmpty()) {
        return;
    }
    sendLocalImageAsFileOffer(path);
}

void ChatMainWidget::onInputExpressionClicked()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || m_currentPeerId <= 0) {
        QMessageBox::information(this, QStringLiteral("表情"), QStringLiteral("请先选择一个好友会话"));
        return;
    }
    if (m_stickerPicker && m_stickerPicker->isVisible()) {
        m_stickerPicker->hide();
        return;
    }
    if (!m_stickerPicker) {
        m_stickerPicker = new StickerPickerPopup(this);
        connect(m_stickerPicker.data(), &StickerPickerPopup::stickerChosen, this,
                [this](const QString &p) { sendLocalImageAsFileOffer(p, true); });
    }
    m_stickerPicker->setUserEmail(m_userEmail);
    m_stickerPicker->setAnchorButton(m_inputExpressionBtn);
    m_stickerPicker->reloadAndRefresh();
    m_stickerPicker->show();
    m_stickerPicker->raise();
}

void ChatMainWidget::onFileSendTick()
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || !m_outgoingFile) {
        if (m_fileSendTimer) {
            m_fileSendTimer->stop();
        }
        return;
    }
    const QByteArray chunk = m_outgoingFile->read(m_offerChunkPlainMax);
    if (chunk.isEmpty()) {
        if (m_outgoingFile->atEnd()) {
            if (m_pendingOfferTransferId > 0 && !m_pendingOfferPath.isEmpty()) {
                rememberTransferDiskPath(m_pendingOfferTransferId, m_pendingOfferPath);
            }
            QJsonObject o;
            o.insert(QStringLiteral("type"), QStringLiteral("file_sender_done"));
            o.insert(QStringLiteral("token"), m_sessionToken);
            o.insert(QStringLiteral("transfer_id"), m_pendingOfferTransferId);
            m_sessionClient->sendJsonObject(o);
            m_fileSendTimer->stop();
            m_outgoingFile.reset();
            m_pendingOfferPath.clear();
            m_pendingOfferTransferId = 0;
            m_pendingOfferIsImage = false;
            m_pendingOfferAsSticker = false;
            m_fileSendGateReady = false;
        }
        return;
    }
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("file_chunk"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("transfer_id"), m_pendingOfferTransferId);
    o.insert(QStringLiteral("seq"), static_cast<qint64>(m_outgoingSeq++));
    o.insert(QStringLiteral("data_b64"), QString::fromLatin1(chunk.toBase64()));
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::handleFileIncoming(const QJsonObject &obj)
{
    if (!m_sessionClient || m_sessionToken.isEmpty()) {
        return;
    }
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    const qint64 fromUid = obj.value(QStringLiteral("from_user_id")).toVariant().toLongLong();
    const QString name = obj.value(QStringLiteral("file_name")).toString();
    const qint64 sz = jsonInt64Member(obj, QStringLiteral("file_size"));
    if (tid <= 0 || fromUid <= 0 || name.isEmpty() || sz <= 0) {
        return;
    }
    if (fromUid != m_currentPeerId || m_currentPeerId <= 0) {
        return;
    }
    appendIncomingFileOfferRow(obj);
}

void ChatMainWidget::handleFileChunkPush(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid != m_recvTransferId || !m_recvFile || !m_recvHash) {
        return;
    }
    const QString b64 = obj.value(QStringLiteral("data_b64")).toString();
    const QByteArray raw = QByteArray::fromBase64(b64.toUtf8());
    if (raw.isEmpty() && !b64.isEmpty()) {
        cleanupFileReceive(false);
        QMessageBox::warning(this, QStringLiteral("接收文件"), QStringLiteral("分片数据无效，已中止"));
        m_recvTransferId = 0;
        return;
    }
    if (m_recvFile->write(raw) != raw.size()) {
        cleanupFileReceive(false);
        QMessageBox::warning(this, QStringLiteral("接收文件"), QStringLiteral("写入失败，已中止"));
        m_recvTransferId = 0;
        return;
    }
    m_recvHash->addData(raw);
    m_recvWritten += raw.size();
}

void ChatMainWidget::handleFileTransferDone(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid != m_recvTransferId || !m_recvFile || !m_recvHash) {
        return;
    }
    if (m_recvWritten != m_recvExpectedSize) {
        cleanupFileReceive(false);
        QMessageBox::warning(this, QStringLiteral("接收文件"),
                             QStringLiteral("字节数与声明不一致（%1 / %2），已丢弃")
                                 .arg(m_recvWritten)
                                 .arg(m_recvExpectedSize));
        m_recvTransferId = 0;
        return;
    }
    const QByteArray got = m_recvHash->result();
    const QString gotHex = QString::fromLatin1(got.toHex()).toLower();
    if (!m_recvExpectedSha.isEmpty() &&
        QString::compare(gotHex, m_recvExpectedSha, Qt::CaseInsensitive) != 0) {
        cleanupFileReceive(false);
        QMessageBox::warning(this, QStringLiteral("接收文件"), QStringLiteral("SHA256 校验失败，已丢弃"));
        m_recvTransferId = 0;
        return;
    }
    const QString savedName = m_recvFile->fileName();
    if (!m_recvFile->commit()) {
        m_recvFile->cancelWriting();
        m_recvFile.reset();
        m_recvHash.reset();
        m_recvTransferId = 0;
        QMessageBox::warning(this, QStringLiteral("接收文件"), QStringLiteral("保存失败"));
        return;
    }
    rememberTransferDiskPath(tid, savedName);
    m_recvFile.reset();
    m_recvHash.reset();
    m_recvTransferId = 0;
    m_recvExpectedSize = 0;
    m_recvWritten = 0;
    m_recvExpectedSha.clear();
}

void ChatMainWidget::handleFileAborted(const QJsonObject &obj)
{
    const QString reason = obj.value(QStringLiteral("reason")).toString();
    cleanupFileReceive(false);
    resetFileSendState();
    if (!reason.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("文件传输"), QStringLiteral("传输中止：%1").arg(reason));
    }
}

void ChatMainWidget::cancelStickerArtifactPull()
{
    const qint64 tidLog = m_stickerPullTransferId;
    const QString pathLog = m_stickerPullSavePath;
    if (m_stickerPullFile || tidLog > 0) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] cancel transfer_id=%1 path=%2")
                                  .arg(tidLog)
                                  .arg(pathLog);
    }
    if (m_stickerPullFile) {
        m_stickerPullFile->cancelWriting();
        m_stickerPullFile.reset();
    }
    if (!m_stickerPullSavePath.isEmpty()) {
        QFile::remove(m_stickerPullSavePath);
        m_stickerPullSavePath.clear();
    }
    m_stickerPullTransferId = 0;
    m_stickerPullExpectedSize = 0;
    m_stickerPullWritten = 0;
    m_stickerPullNextSeq = 0;
    m_stickerPullChunkPlainMax = 49152;
    m_stickerPullTargetLabel.clear();
}

void ChatMainWidget::tryStartNextStickerPullInQueue()
{
    if (m_stickerPullTransferId != 0 || m_stickerPullFile) {
        return;
    }
    while (!m_stickerPullQueue.isEmpty()) {
        const PendingStickerPull p = m_stickerPullQueue.takeFirst();
        if (p.transferId <= 0) {
            continue;
        }
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] dequeue next transfer_id=%1").arg(p.transferId);
        beginStickerArtifactPullImmediate(p);
        return;
    }
}

void ChatMainWidget::beginStickerArtifactPullIfNeeded(const qint64 transferId, const QString &fileName,
                                                      QLabel *imgLabel)
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || transferId <= 0) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] skip pull (no session) transfer_id=%1 client=%2 token=%3")
                                  .arg(transferId)
                                  .arg(m_sessionClient ? 1 : 0)
                                  .arg(m_sessionToken.isEmpty() ? 0 : 1);
        return;
    }
    PendingStickerPull spec;
    spec.transferId = transferId;
    spec.fileName = fileName;
    spec.label = imgLabel;
    if (m_stickerPullTransferId != 0 || m_stickerPullFile) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] queue pull transfer_id=%1 queue_len=%2 active_tid=%3")
                                  .arg(transferId)
                                  .arg(m_stickerPullQueue.size() + 1)
                                  .arg(m_stickerPullTransferId);
        m_stickerPullQueue.append(spec);
        return;
    }
    beginStickerArtifactPullImmediate(spec);
}

void ChatMainWidget::beginStickerArtifactPullImmediate(const PendingStickerPull &spec)
{
    const qint64 transferId = spec.transferId;
    const QString &fileName = spec.fileName;
    QLabel *const imgLabel = spec.label.data();

    QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext.isEmpty()) {
        ext = QStringLiteral("bin");
    }
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/lancs_chat/stickers");
    QDir().mkpath(base);
    m_stickerPullSavePath = QDir(base).filePath(QStringLiteral("%1.%2").arg(transferId).arg(ext));
    m_stickerPullFile = std::make_unique<QSaveFile>(m_stickerPullSavePath);
    if (!m_stickerPullFile->open(QIODevice::WriteOnly)) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] open save file failed transfer_id=%1 path=%2")
                                  .arg(transferId)
                                  .arg(m_stickerPullSavePath);
        m_stickerPullFile.reset();
        m_stickerPullSavePath.clear();
        tryStartNextStickerPullInQueue();
        return;
    }
    m_stickerPullTransferId = transferId;
    m_stickerPullWritten = 0;
    m_stickerPullExpectedSize = 0;
    m_stickerPullNextSeq = 0;
    m_stickerPullChunkPlainMax = 49152;
    m_stickerPullTargetLabel = imgLabel;

    qDebug().noquote() << QStringLiteral("[LANCS/sticker] send file_sticker_pull transfer_id=%1 name=%2 save=%3 "
                                         "label=%4")
                              .arg(transferId)
                              .arg(fileName)
                              .arg(m_stickerPullSavePath)
                              .arg(imgLabel ? QStringLiteral("ok") : QStringLiteral("null"));

    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("file_sticker_pull"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("transfer_id"), transferId);
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::handleStickerPullOk(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid <= 0 || tid != m_stickerPullTransferId || !m_stickerPullFile) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_ok ignored tid=%1 expect_tid=%2 has_file=%3")
                                  .arg(tid)
                                  .arg(m_stickerPullTransferId)
                                  .arg(m_stickerPullFile ? 1 : 0);
        return;
    }
    m_stickerPullExpectedSize = jsonInt64Member(obj, QStringLiteral("file_size"));
    m_stickerPullChunkPlainMax = obj.value(QStringLiteral("chunk_plain_max")).toInt(49152);
    if (m_stickerPullChunkPlainMax < 4096) {
        m_stickerPullChunkPlainMax = 49152;
    }
    qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_ok transfer_id=%1 file_size=%2 chunk_plain_max=%3")
                              .arg(tid)
                              .arg(m_stickerPullExpectedSize)
                              .arg(m_stickerPullChunkPlainMax);
}

void ChatMainWidget::handleStickerPullChunk(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid <= 0 || tid != m_stickerPullTransferId || !m_stickerPullFile) {
        return;
    }
    const quint32 seq = static_cast<quint32>(obj.value(QStringLiteral("seq")).toVariant().toULongLong());
    if (seq != m_stickerPullNextSeq) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] chunk seq mismatch got=%1 expect=%2 tid=%3")
                                  .arg(seq)
                                  .arg(m_stickerPullNextSeq)
                                  .arg(tid);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    const QString b64 = obj.value(QStringLiteral("data_b64")).toString();
    const QByteArray raw = QByteArray::fromBase64(b64.toUtf8());
    if (raw.isEmpty() && !b64.isEmpty()) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] chunk base64 decode failed tid=%1 seq=%2").arg(tid).arg(seq);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    if (m_stickerPullExpectedSize > 0 && m_stickerPullWritten + raw.size() > m_stickerPullExpectedSize) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] chunk over expected size written=%1 +%2 > %3")
                                  .arg(m_stickerPullWritten)
                                  .arg(raw.size())
                                  .arg(m_stickerPullExpectedSize);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    if (m_stickerPullFile->write(raw) != raw.size()) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] chunk write failed tid=%1 seq=%2").arg(tid).arg(seq);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    m_stickerPullWritten += raw.size();
    ++m_stickerPullNextSeq;
    if (seq == 0) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] first chunk tid=%1 bytes=%2").arg(tid).arg(raw.size());
    }
}

void ChatMainWidget::handleStickerPullDone(const QJsonObject &obj)
{
    const qint64 tid = jsonInt64Member(obj, QStringLiteral("transfer_id"));
    if (tid <= 0 || tid != m_stickerPullTransferId || !m_stickerPullFile) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_done ignored tid=%1 expect_tid=%2 has_file=%3")
                                  .arg(tid)
                                  .arg(m_stickerPullTransferId)
                                  .arg(m_stickerPullFile ? 1 : 0);
        return;
    }
    if (m_stickerPullExpectedSize > 0 && m_stickerPullWritten != m_stickerPullExpectedSize) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_done size mismatch written=%1 expect=%2")
                                  .arg(m_stickerPullWritten)
                                  .arg(m_stickerPullExpectedSize);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    const QString expectSha = obj.value(QStringLiteral("sha256_hex")).toString();
    if (!m_stickerPullFile->flush()) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_done flush failed tid=%1").arg(tid);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    if (!m_stickerPullFile->commit()) {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_done commit failed tid=%1").arg(tid);
        cancelStickerArtifactPull();
        tryStartNextStickerPullInQueue();
        return;
    }
    m_stickerPullFile.reset();

    if (!expectSha.isEmpty()) {
        QFile rf(m_stickerPullSavePath);
        if (!rf.open(QIODevice::ReadOnly)) {
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] sha verify open failed tid=%1").arg(tid);
            QFile::remove(m_stickerPullSavePath);
            cancelStickerArtifactPull();
            tryStartNextStickerPullInQueue();
            return;
        }
        QCryptographicHash h(QCryptographicHash::Sha256);
        if (!h.addData(&rf)) {
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] sha hash read failed tid=%1").arg(tid);
            QFile::remove(m_stickerPullSavePath);
            cancelStickerArtifactPull();
            tryStartNextStickerPullInQueue();
            return;
        }
        const QString gotHex = QString::fromLatin1(h.result().toHex()).toLower();
        if (QString::compare(gotHex, expectSha, Qt::CaseInsensitive) != 0) {
            qDebug().noquote() << QStringLiteral("[LANCS/sticker] sha mismatch tid=%1 expect=%2 got=%3")
                                      .arg(tid)
                                      .arg(expectSha)
                                      .arg(gotHex);
            QFile::remove(m_stickerPullSavePath);
            cancelStickerArtifactPull();
            tryStartNextStickerPullInQueue();
            return;
        }
    }

    const QString savedPath = QFileInfo(m_stickerPullSavePath).absoluteFilePath();
    QLabel *pic = m_stickerPullTargetLabel.data();
    rememberTransferDiskPath(tid, savedPath);
    if (pic) {
        QWidget *bubble = pic->parentWidget();
        applyStickerImageToLabel(pic, savedPath, bubble);
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_done OK tid=%1 path=%2 label=%3")
                                  .arg(tid)
                                  .arg(savedPath)
                                  .arg(QStringLiteral("ok"));
    } else {
        qDebug().noquote() << QStringLiteral("[LANCS/sticker] pull_done OK tid=%1 path=%2 label=null (UI已销毁，已缓存路径)")
                                  .arg(tid)
                                  .arg(savedPath);
    }

    m_stickerPullTransferId = 0;
    m_stickerPullExpectedSize = 0;
    m_stickerPullWritten = 0;
    m_stickerPullNextSeq = 0;
    m_stickerPullChunkPlainMax = 49152;
    m_stickerPullSavePath.clear();
    m_stickerPullTargetLabel.clear();
    tryStartNextStickerPullInQueue();
}

void ChatMainWidget::loadSessionUiPrefs()
{
    m_pinnedPeerIds.clear();
    m_hiddenSessionPeers.clear();
    if (m_userEmail.trimmed().isEmpty()) {
        return;
    }
    const QString key = LocalProfile::emailKey(m_userEmail);
    QSettings s;
    s.beginGroup(QStringLiteral("lancs_session/%1").arg(key));
    const QStringList pin = s.value(QStringLiteral("pinned_peer_ids")).toStringList();
    for (const QString &x : pin) {
        bool ok = false;
        const qint64 id = x.toLongLong(&ok);
        if (ok && id > 0) {
            m_pinnedPeerIds.append(id);
        }
    }
    const QStringList hid = s.value(QStringLiteral("hidden_peer_ids")).toStringList();
    for (const QString &x : hid) {
        bool ok = false;
        const qint64 id = x.toLongLong(&ok);
        if (ok && id > 0) {
            m_hiddenSessionPeers.insert(id);
        }
    }
    s.endGroup();
}

void ChatMainWidget::saveSessionUiPrefs()
{
    if (m_userEmail.trimmed().isEmpty()) {
        return;
    }
    const QString key = LocalProfile::emailKey(m_userEmail);
    QSettings s;
    s.beginGroup(QStringLiteral("lancs_session/%1").arg(key));
    QStringList pin;
    pin.reserve(m_pinnedPeerIds.size());
    for (const qint64 id : m_pinnedPeerIds) {
        pin.append(QString::number(id));
    }
    s.setValue(QStringLiteral("pinned_peer_ids"), pin);
    QStringList hid;
    for (const qint64 id : m_hiddenSessionPeers) {
        hid.append(QString::number(id));
    }
    s.setValue(QStringLiteral("hidden_peer_ids"), hid);
    s.endGroup();
}

void ChatMainWidget::onSessionListContextMenu(const QPoint &pos)
{
    if (!m_friendList) {
        return;
    }
    QListWidgetItem *item = m_friendList->itemAt(pos);
    if (!item || item->isHidden()) {
        return;
    }
    const qint64 peerId = item->data(Qt::UserRole).toLongLong();
    if (peerId <= 0) {
        return;
    }

    QMenu menu(this);
    const bool isPinned = m_pinnedPeerIds.contains(peerId);
    QAction *aPin = menu.addAction(isPinned ? QStringLiteral("取消置顶") : QStringLiteral("置顶"));
    QAction *aHide = menu.addAction(QStringLiteral("从消息列表中移除"));
    QAction *aClear = menu.addAction(QStringLiteral("清空聊天记录"));
    QAction *chosen = menu.exec(m_friendList->mapToGlobal(pos));
    if (!chosen) {
        return;
    }
    if (chosen == aPin) {
        if (isPinned) {
            m_pinnedPeerIds.removeAll(peerId);
        } else {
            m_pinnedPeerIds.append(peerId);
        }
        saveSessionUiPrefs();
        requestFriendListForSessions();
    } else if (chosen == aHide) {
        m_hiddenSessionPeers.insert(peerId);
        saveSessionUiPrefs();
        requestFriendListForSessions();
    } else if (chosen == aClear) {
        if (!m_sessionClient || m_sessionToken.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("未连接服务器。"));
            return;
        }
        const auto r =
            QMessageBox::question(this, QStringLiteral("清空聊天记录"),
                                  QStringLiteral("将删除双方在本会话中的全部消息记录，且不可恢复。确定继续吗？"),
                                  QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (r != QMessageBox::Yes) {
            return;
        }
        sendMsgClearRequest(peerId);
    }
}

void ChatMainWidget::sendMsgClearRequest(qint64 peerId)
{
    if (!m_sessionClient || m_sessionToken.isEmpty() || peerId <= 0) {
        return;
    }
    m_pendingMsgClearPeer = peerId;
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("msg_clear"));
    o.insert(QStringLiteral("token"), m_sessionToken);
    o.insert(QStringLiteral("peer_user_id"), peerId);
    m_sessionClient->sendJsonObject(o);
}

void ChatMainWidget::applyConversationClearedUi(qint64 peerInConversation)
{
    if (peerInConversation <= 0) {
        return;
    }
    if (m_currentPeerId == peerInConversation) {
        clearMessageList();
        m_lastMsgId = 0;
        m_seenMessageIds.clear();
        m_prevMsgCreatedAtForDivider = 0;
    }
    if (m_chatHistoryDialog && m_pendingHistoryMsgFetch && m_historyFetchPeerId == peerInConversation) {
        m_chatHistoryDialog->close();
        m_pendingHistoryMsgFetch = false;
        m_historyFetchPeerId = 0;
    }
    requestFriendListForSessions();
}
