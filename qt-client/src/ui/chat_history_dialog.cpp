#include "ui/chat_history_dialog.h"

#include "style/app_style.h"
#include "utils/avatar_utils.h"
#include "utils/local_profile.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QPainter>
#include <QPushButton>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QTabBar>
#include <QVBoxLayout>

namespace {

constexpr int kTabAll = 0;
constexpr int kTabImageVideo = 1;
constexpr int kTabEmoji = 2;
constexpr int kTabFile = 3;
constexpr int kTabLink = 4;

bool textHasLink(const QString &s)
{
    static const QRegularExpression re(QStringLiteral(R"(https?://\S+)"));
    return re.match(s).hasMatch();
}

bool textLikelyEmojiHeavy(const QString &s)
{
    if (s.size() > 64) {
        return false;
    }
    int nonAscii = 0;
    for (QChar c : s) {
        const ushort u = c.unicode();
        if (u >= 0x203C) {
            ++nonAscii;
        }
    }
    return nonAscii >= 1 && s.trimmed().length() <= 16;
}

} // namespace

bool ChatHistoryDialog::isImageFileNameString(const QString &name)
{
    const QString suf = QFileInfo(name).suffix().toLower();
    static const QStringList kImageSuffixes = {QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("png"),
                                               QStringLiteral("gif"),  QStringLiteral("bmp"),  QStringLiteral("webp"),
                                               QStringLiteral("ico"),  QStringLiteral("heic"), QStringLiteral("heif")};
    return kImageSuffixes.contains(suf);
}

bool ChatHistoryDialog::parseFileOrImageMessage(const QString &content, QJsonObject *out)
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

ChatHistoryDialog::ChatHistoryDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("ChatHistoryDialog"));
    setWindowTitle(QStringLiteral("聊天记录"));
    setModal(false);
    resize(440, 520);
    setMinimumSize(360, 360);
    setStyleSheet(AppStyle::chatHistoryDialogStyle());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    auto *searchRow = new QHBoxLayout();
    searchRow->setSpacing(8);
    auto *searchIcon = new QLabel(this);
    searchIcon->setObjectName(QStringLiteral("chatHistorySearchIcon"));
    searchIcon->setPixmap(QIcon(QStringLiteral(":/icons/search_icon.svg")).pixmap(18, 18));
    searchIcon->setFixedSize(18, 18);
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("chatHistorySearchInput"));
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索"));
    m_searchEdit->setClearButtonEnabled(true);
    searchRow->addWidget(searchIcon, 0, Qt::AlignVCenter);
    searchRow->addWidget(m_searchEdit, 1);
    root->addLayout(searchRow);

    auto *tabRow = new QHBoxLayout();
    tabRow->setSpacing(0);
    m_tabBar = new QTabBar(this);
    m_tabBar->setObjectName(QStringLiteral("chatHistoryTabBar"));
    m_tabBar->setExpanding(false);
    m_tabBar->addTab(QStringLiteral("全部"));
    m_tabBar->addTab(QStringLiteral("图片/视频"));
    m_tabBar->addTab(QStringLiteral("表情"));
    m_tabBar->addTab(QStringLiteral("文件"));
    m_tabBar->addTab(QStringLiteral("链接"));
    tabRow->addWidget(m_tabBar, 0, Qt::AlignLeft);
    tabRow->addStretch(1);
    auto *filterHint = new QPushButton(QStringLiteral("筛选"), this);
    filterHint->setObjectName(QStringLiteral("chatHistoryFilterHint"));
    filterHint->setCursor(Qt::PointingHandCursor);
    filterHint->setFlat(true);
    filterHint->setToolTip(QStringLiteral("敬请期待"));
    filterHint->setEnabled(false);
    tabRow->addWidget(filterHint, 0, Qt::AlignVCenter);
    root->addLayout(tabRow);

    m_list = new QListWidget(this);
    m_list->setObjectName(QStringLiteral("chatHistoryList"));
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(m_list, 1);

    m_statusLabel = new QLabel(QStringLiteral("打开时将向服务器拉取最近记录"), this);
    m_statusLabel->setObjectName(QStringLiteral("chatHistoryStatus"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this]() { rebuildList(); });
    connect(m_tabBar, &QTabBar::currentChanged, this, [this](int) { rebuildList(); });
}

void ChatHistoryDialog::setSession(qint64 sessionUserId, qint64 peerId, const QString &peerTitle,
                                   const QString &userEmail, const QPixmap &peerAvatarPixmap)
{
    m_sessionUserId = sessionUserId;
    m_peerId = peerId;
    m_userEmail = userEmail;
    m_peerAvatar = peerAvatarPixmap;
    const QString t = peerTitle.isEmpty() ? QStringLiteral("聊天") : peerTitle;
    setWindowTitle(QStringLiteral("聊天记录 · %1").arg(t));
    m_entries.clear();
    m_list->clear();
    m_statusLabel->setText(QStringLiteral("正在加载…"));
}

void ChatHistoryDialog::applyMsgFetchOk(const QJsonObject &obj)
{
    const qint64 peer = obj.value(QStringLiteral("peer_user_id")).toVariant().toLongLong();
    if (peer != m_peerId || m_peerId <= 0) {
        return;
    }
    m_entries.clear();
    const QJsonArray arr = obj.value(QStringLiteral("messages")).toArray();
    m_entries.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject m = v.toObject();
        ChatHistoryEntry e;
        e.messageId = m.value(QStringLiteral("message_id")).toVariant().toLongLong();
        e.fromUserId = m.value(QStringLiteral("from_user_id")).toVariant().toLongLong();
        e.content = m.value(QStringLiteral("content")).toString();
        e.createdAt = m.value(QStringLiteral("created_at")).toVariant().toLongLong();
        m_entries.append(e);
    }
    m_statusLabel->setText(QStringLiteral("共 %1 条（单次最多 200 条）").arg(m_entries.size()));
    rebuildList();
}

void ChatHistoryDialog::notifyFetchError(const QString &message)
{
    m_statusLabel->setText(message.isEmpty() ? QStringLiteral("加载失败") : message);
}

QString ChatHistoryDialog::formatLineText(const QString &content) const
{
    QJsonObject fo;
    if (parseFileOrImageMessage(content, &fo)) {
        if (fo.value(QStringLiteral("as_sticker")).toBool() || fo.value(QStringLiteral("sticker")).toBool()) {
            return QStringLiteral("[表情]");
        }
        const QString kind = fo.value(QStringLiteral("kind")).toString();
        const QString name = fo.value(QStringLiteral("name")).toString();
        if (kind == QStringLiteral("image") || isImageFileNameString(name)) {
            return QStringLiteral("[图片]");
        }
        return QStringLiteral("[文件] %1").arg(name.isEmpty() ? QStringLiteral("文件") : name);
    }
    return content;
}

bool ChatHistoryDialog::entryPassesTab(const ChatHistoryEntry &e) const
{
    const int tab = m_tabBar ? m_tabBar->currentIndex() : 0;
    QJsonObject fo;
    const bool isFile = parseFileOrImageMessage(e.content, &fo);
    if (isFile) {
        const QString kind = fo.value(QStringLiteral("kind")).toString();
        const QString name = fo.value(QStringLiteral("name")).toString();
        const bool asImage = (kind == QStringLiteral("image")) || isImageFileNameString(name);
        if (tab == kTabAll) {
            return true;
        }
        if (tab == kTabImageVideo) {
            return asImage;
        }
        if (tab == kTabFile) {
            return !asImage && kind == QStringLiteral("file");
        }
        return false;
    }
    const QString plain = e.content;
    if (tab == kTabAll) {
        return true;
    }
    if (tab == kTabLink) {
        return textHasLink(plain);
    }
    if (tab == kTabEmoji) {
        return textLikelyEmojiHeavy(plain);
    }
    return false;
}

bool ChatHistoryDialog::entryPassesSearch(const ChatHistoryEntry &e) const
{
    const QString q = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (q.isEmpty()) {
        return true;
    }
    const QString line = formatLineText(e.content);
    return line.contains(q, Qt::CaseInsensitive) || e.content.contains(q, Qt::CaseInsensitive);
}

void ChatHistoryDialog::appendDateRowIfNeeded(const QDate &msgDate, QDate *lastDateOut)
{
    if (!lastDateOut || !m_list) {
        return;
    }
    if (!lastDateOut->isValid() || msgDate != *lastDateOut) {
        *lastDateOut = msgDate;
        auto *item = new QListWidgetItem(m_list);
        item->setFlags(Qt::NoItemFlags);
        auto *lb = new QLabel(msgDate.toString(QStringLiteral("yyyy/MM/dd")), m_list);
        lb->setObjectName(QStringLiteral("chatHistoryDateRow"));
        lb->setAlignment(Qt::AlignCenter);
        item->setSizeHint(QSize(0, 28));
        m_list->addItem(item);
        m_list->setItemWidget(item, lb);
    }
}

void ChatHistoryDialog::rebuildList()
{
    if (!m_list) {
        return;
    }
    m_list->clear();

    auto makePlaceholder = [](const QColor &fill) {
        QPixmap ph(32, 32);
        ph.fill(Qt::transparent);
        QPainter p(&ph);
        p.setRenderHint(QPainter::Antialiasing);
        p.setBrush(fill);
        p.setPen(Qt::NoPen);
        p.drawEllipse(0, 0, 31, 31);
        return ph;
    };

    QPixmap selfAv;
    (void) LocalProfile::loadAvatarPixmap(m_userEmail, 32, &selfAv);
    if (!selfAv.isNull()) {
        selfAv = makeCircularAvatar(selfAv, 32);
    } else {
        selfAv = makePlaceholder(QColor(QStringLiteral("#d9d9d9")));
    }
    QPixmap peerAv = m_peerAvatar.isNull() ? QPixmap() : makeCircularAvatar(m_peerAvatar, 32);
    if (peerAv.isNull()) {
        peerAv = makePlaceholder(QColor(QStringLiteral("#c5e8f7")));
    }

    QDate lastDate;
    for (const ChatHistoryEntry &e : m_entries) {
        if (!entryPassesTab(e) || !entryPassesSearch(e)) {
            continue;
        }
        const QDate d = QDateTime::fromSecsSinceEpoch(e.createdAt > 0 ? e.createdAt : 0).date();
        appendDateRowIfNeeded(d, &lastDate);

        auto *item = new QListWidgetItem(m_list);
        auto *row = new QWidget(m_list);
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(6, 4, 6, 4);
        h->setSpacing(8);

        auto *av = new QLabel(row);
        av->setFixedSize(32, 32);
        const bool outgoing = (e.fromUserId == m_sessionUserId);
        av->setPixmap(outgoing ? selfAv : peerAv);

        const QString who = outgoing ? QStringLiteral("我") : QStringLiteral("对方");
        const QString oneLine = QStringLiteral("%1：%2").arg(who, formatLineText(e.content));
        auto *text = new QLabel(oneLine, row);
        text->setObjectName(QStringLiteral("chatHistoryLineText"));
        text->setWordWrap(true);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        h->addWidget(av, 0, Qt::AlignTop);
        h->addWidget(text, 1, Qt::AlignTop);

        const int hHint = qMax(40, row->sizeHint().height());
        item->setSizeHint(QSize(0, hHint));
        m_list->addItem(item);
        m_list->setItemWidget(item, row);
    }

    if (m_list->count() == 0 && !m_entries.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("没有符合条件的记录"));
    }
}
