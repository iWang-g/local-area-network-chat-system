#include "ui/contacts_widget.h"

#include "net/lan_tcp_client.h"
#include "style/app_style.h"

#include <QFont>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QSize>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

namespace {

constexpr char kIncoming[] = "incoming";
constexpr char kOutgoing[] = "outgoing";
constexpr char kFriends[] = "friends";
const int kSectionDataRole = static_cast<int>(Qt::UserRole);

void styleSectionHeader(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
}

} // namespace

ContactsWidget::ContactsWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("LanContactsWidget"));
    setStyleSheet(AppStyle::contactsWidgetStyle());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(8);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("lanContactsStatus"));
    m_statusLabel->setStyleSheet(QStringLiteral("color: #8c8c8c; font-size: 12px;"));
    m_statusLabel->setWordWrap(true);
    root->addWidget(m_statusLabel);

    m_tree = new QTreeWidget(this);
    m_tree->setObjectName(QStringLiteral("lanContactsTree"));
    m_tree->setHeaderHidden(true);
    m_tree->setColumnCount(1);
    m_tree->setRootIsDecorated(true);
    m_tree->setAnimated(true);
    m_tree->setIndentation(18);
    m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_tree, &QTreeWidget::customContextMenuRequested, this, &ContactsWidget::showSectionContextMenu);

    m_incomingRoot = new QTreeWidgetItem();
    m_incomingRoot->setText(0, QStringLiteral("收到的申请（0）"));
    m_incomingRoot->setData(0, kSectionDataRole, QString::fromUtf8(kIncoming));
    styleSectionHeader(m_incomingRoot);
    m_incomingRoot->setExpanded(true);

    m_outgoingRoot = new QTreeWidgetItem();
    m_outgoingRoot->setText(0, QStringLiteral("我发出的申请（0）"));
    m_outgoingRoot->setData(0, kSectionDataRole, QString::fromUtf8(kOutgoing));
    styleSectionHeader(m_outgoingRoot);
    m_outgoingRoot->setExpanded(true);

    m_friendsRoot = new QTreeWidgetItem();
    m_friendsRoot->setText(0, QStringLiteral("好友（0）"));
    m_friendsRoot->setData(0, kSectionDataRole, QString::fromUtf8(kFriends));
    styleSectionHeader(m_friendsRoot);
    m_friendsRoot->setExpanded(true);

    m_tree->addTopLevelItem(m_incomingRoot);
    m_tree->addTopLevelItem(m_outgoingRoot);
    m_tree->addTopLevelItem(m_friendsRoot);

    root->addWidget(m_tree, 1);
}

void ContactsWidget::clearSectionChildren(QTreeWidgetItem *root)
{
    if (!root) {
        return;
    }
    while (root->childCount() > 0) {
        delete root->takeChild(0);
    }
}

void ContactsWidget::showSectionContextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = m_tree->itemAt(pos);
    if (!item || item->parent()) {
        return;
    }
    const QString sec = item->data(0, kSectionDataRole).toString();
    QMenu menu(this);
    if (sec == QString::fromUtf8(kIncoming) || sec == QString::fromUtf8(kOutgoing)) {
        auto *a = menu.addAction(QStringLiteral("刷新申请列表"));
        connect(a, &QAction::triggered, this, &ContactsWidget::onRefreshRequestsClicked);
    } else if (sec == QString::fromUtf8(kFriends)) {
        auto *a = menu.addAction(QStringLiteral("刷新好友列表"));
        connect(a, &QAction::triggered, this, &ContactsWidget::onRefreshFriendsClicked);
    } else {
        return;
    }
    menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

void ContactsWidget::setTcpClient(LanTcpClient *client)
{
    m_client = client;
}

void ContactsWidget::setToken(const QString &token)
{
    m_token = token;
}

void ContactsWidget::sendJson(const QJsonObject &obj)
{
    if (!m_client || !m_client->isConnected()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("未连接服务器"));
        return;
    }
    if (m_token.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("未登录，请重新登录"));
        return;
    }
    m_client->sendJsonObject(obj);
}

void ContactsWidget::onRefreshRequestsClicked()
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("friend_request_list"));
    o.insert(QStringLiteral("token"), m_token);
    sendJson(o);
}

void ContactsWidget::onRefreshFriendsClicked()
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("friend_list"));
    o.insert(QStringLiteral("token"), m_token);
    sendJson(o);
}

void ContactsWidget::refreshFriendRequestList()
{
    onRefreshRequestsClicked();
}

void ContactsWidget::refreshAll()
{
    onRefreshRequestsClicked();
    onRefreshFriendsClicked();
}

void ContactsWidget::applyRequestList(const QJsonObject &obj)
{
    clearSectionChildren(m_incomingRoot);
    clearSectionChildren(m_outgoingRoot);
    const QJsonArray inc = obj.value(QStringLiteral("incoming")).toArray();
    for (const QJsonValue &v : inc) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        addIncomingRow(o.value(QStringLiteral("request_id")).toVariant().toLongLong(),
                       o.value(QStringLiteral("from_user_id")).toVariant().toLongLong(),
                       o.value(QStringLiteral("email")).toString(),
                       o.value(QStringLiteral("nickname")).toString());
    }
    const QJsonArray out = obj.value(QStringLiteral("outgoing")).toArray();
    for (const QJsonValue &v : out) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        addOutgoingRow(o.value(QStringLiteral("request_id")).toVariant().toLongLong(),
                       o.value(QStringLiteral("to_user_id")).toVariant().toLongLong(),
                       o.value(QStringLiteral("email")).toString(),
                       o.value(QStringLiteral("nickname")).toString());
    }
    m_incomingRoot->setText(0, QStringLiteral("收到的申请（%1）").arg(inc.size()));
    m_outgoingRoot->setText(0, QStringLiteral("我发出的申请（%1）").arg(out.size()));
    m_statusLabel->setText(QStringLiteral("申请列表已更新"));
}

void ContactsWidget::applyFriendList(const QJsonArray &friends)
{
    clearSectionChildren(m_friendsRoot);
    for (const QJsonValue &v : friends) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject o = v.toObject();
        addFriendRow(o.value(QStringLiteral("user_id")).toVariant().toLongLong(),
                     o.value(QStringLiteral("email")).toString(), o.value(QStringLiteral("nickname")).toString());
    }
    m_friendsRoot->setText(0, QStringLiteral("好友（%1）").arg(friends.size()));
    m_statusLabel->setText(QStringLiteral("好友列表已更新（共 %1 人）").arg(friends.size()));
}

void ContactsWidget::addIncomingRow(qint64 requestId, qint64 fromUserId, const QString &email,
                                    const QString &nickname)
{
    auto *item = new QTreeWidgetItem(m_incomingRoot);
    auto *w = new QWidget(m_tree);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(8, 6, 8, 6);
    const QString nick = nickname.isEmpty() ? QStringLiteral("（无昵称）") : nickname;
    auto *lb = new QLabel(QStringLiteral("%1\n%2").arg(nick, email), w);
    lb->setStyleSheet(QStringLiteral("font-size: 12px;"));
    h->addWidget(lb, 1);
    auto *acc = new QPushButton(QStringLiteral("同意"), w);
    auto *rej = new QPushButton(QStringLiteral("拒绝"), w);
    acc->setCursor(Qt::PointingHandCursor);
    rej->setCursor(Qt::PointingHandCursor);
    connect(acc, &QPushButton::clicked, this, [this, requestId]() { requestHandle(requestId, QStringLiteral("accept")); });
    connect(rej, &QPushButton::clicked, this, [this, requestId]() { requestHandle(requestId, QStringLiteral("reject")); });
    h->addWidget(acc);
    h->addWidget(rej);
    Q_UNUSED(fromUserId);
    item->setSizeHint(0, QSize(0, 52));
    m_tree->setItemWidget(item, 0, w);
}

void ContactsWidget::addOutgoingRow(qint64 requestId, qint64 toUserId, const QString &email,
                                    const QString &nickname)
{
    auto *item = new QTreeWidgetItem(m_outgoingRoot);
    auto *w = new QWidget(m_tree);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(8, 6, 8, 6);
    const QString nick = nickname.isEmpty() ? QStringLiteral("（无昵称）") : nickname;
    auto *lb = new QLabel(QStringLiteral("→ %1\n%2  （待对方处理）").arg(nick, email), w);
    lb->setStyleSheet(QStringLiteral("font-size: 12px; color: #595959;"));
    h->addWidget(lb, 1);
    Q_UNUSED(requestId);
    Q_UNUSED(toUserId);
    item->setSizeHint(0, QSize(0, 52));
    m_tree->setItemWidget(item, 0, w);
}

void ContactsWidget::addFriendRow(qint64 userId, const QString &email, const QString &nickname)
{
    auto *item = new QTreeWidgetItem(m_friendsRoot);
    auto *w = new QWidget(m_tree);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(8, 6, 8, 6);
    const QString nick = nickname.isEmpty() ? QStringLiteral("（无昵称）") : nickname;
    auto *lb = new QLabel(QStringLiteral("%1\n%2").arg(nick, email), w);
    lb->setStyleSheet(QStringLiteral("font-size: 12px;"));
    h->addWidget(lb, 1);
    auto *del = new QPushButton(QStringLiteral("删除"), w);
    del->setFixedWidth(56);
    del->setCursor(Qt::PointingHandCursor);
    connect(del, &QPushButton::clicked, this, [this, userId]() { requestDeleteFriend(userId); });
    h->addWidget(del);
    item->setSizeHint(0, QSize(0, 52));
    m_tree->setItemWidget(item, 0, w);
}

void ContactsWidget::requestHandle(qint64 requestId, const QString &action)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("friend_request_handle"));
    o.insert(QStringLiteral("token"), m_token);
    o.insert(QStringLiteral("request_id"), requestId);
    o.insert(QStringLiteral("action"), action);
    sendJson(o);
}

void ContactsWidget::requestDeleteFriend(qint64 peerUserId)
{
    const auto ret =
        QMessageBox::question(this, QStringLiteral("删除好友"), QStringLiteral("确定删除该好友？"),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) {
        return;
    }
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("friend_delete"));
    o.insert(QStringLiteral("token"), m_token);
    o.insert(QStringLiteral("peer_user_id"), peerUserId);
    sendJson(o);
}

void ContactsWidget::handleServerJson(const QJsonObject &obj)
{
    const QString t = obj.value(QStringLiteral("type")).toString();
    if (t == QStringLiteral("friend_notify")) {
        const QString ev = obj.value(QStringLiteral("event")).toString();
        if (ev == QStringLiteral("request_accepted")) {
            m_statusLabel->setText(QStringLiteral("对方已同意好友申请"));
            refreshAll();
        } else if (ev == QStringLiteral("nickname") || ev == QStringLiteral("avatar")) {
            refreshAll();
        }
        return;
    }
    if (t == QStringLiteral("friend_request_sent")) {
        m_statusLabel->setText(QStringLiteral("好友申请已发送"));
        onRefreshRequestsClicked();
        return;
    }
    if (t == QStringLiteral("friend_request_list_ok")) {
        applyRequestList(obj);
        return;
    }
    if (t == QStringLiteral("friend_request_handled")) {
        m_statusLabel->setText(QStringLiteral("已处理申请"));
        onRefreshRequestsClicked();
        onRefreshFriendsClicked();
        return;
    }
    if (t == QStringLiteral("friend_list_ok")) {
        applyFriendList(obj.value(QStringLiteral("friends")).toArray());
        return;
    }
    if (t == QStringLiteral("friend_delete_ok")) {
        m_statusLabel->setText(QStringLiteral("已删除好友"));
        onRefreshFriendsClicked();
        return;
    }
    if (t == QStringLiteral("error")) {
        const int code = obj.value(QStringLiteral("code")).toInt();
        const QString msg = obj.value(QStringLiteral("message")).toString();
        m_statusLabel->setText(msg);
        /// 文件/表情等业务错误由聊天页处理，避免重复弹窗。
        if (code < 5001 || code > 5009) {
            QMessageBox::warning(this, QStringLiteral("错误"), msg);
        }
    }
}
