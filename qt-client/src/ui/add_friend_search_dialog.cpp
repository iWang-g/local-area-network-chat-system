#include "ui/add_friend_search_dialog.h"

#include "net/lan_tcp_client.h"
#include "style/app_style.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

AddFriendSearchDialog::AddFriendSearchDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("AddFriendSearchDialog"));
    setWindowTitle(QStringLiteral("添加好友"));
    setModal(false);
    resize(480, 420);
    setMinimumWidth(400);
    setStyleSheet(AppStyle::addFriendSearchDialogStyle());

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto *searchRow = new QHBoxLayout();
    searchRow->setSpacing(8);

    auto *searchIcon = new QLabel(this);
    searchIcon->setObjectName(QStringLiteral("addFriendSearchIcon"));
    searchIcon->setPixmap(QIcon(QStringLiteral(":/icons/search_icon.svg")).pixmap(18, 18));
    searchIcon->setFixedSize(18, 18);
    searchIcon->setScaledContents(true);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setObjectName(QStringLiteral("addFriendSearchInput"));
    m_searchEdit->setPlaceholderText(QStringLiteral("输入邮箱片段，如 @qq.com"));
    m_searchEdit->setClearButtonEnabled(true);

    auto *searchBtn = new QPushButton(QStringLiteral("搜索"), this);
    searchBtn->setObjectName(QStringLiteral("addFriendSearchButton"));
    searchBtn->setCursor(Qt::PointingHandCursor);
    searchBtn->setDefault(true);
    connect(searchBtn, &QPushButton::clicked, this, &AddFriendSearchDialog::onSearchClicked);

    searchRow->addWidget(searchIcon, 0, Qt::AlignVCenter);
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(searchBtn);
    root->addLayout(searchRow);

    m_searchList = new QListWidget(this);
    m_searchList->setObjectName(QStringLiteral("addFriendSearchList"));
    m_searchList->setMinimumHeight(220);
    root->addWidget(m_searchList, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName(QStringLiteral("addFriendSearchStatus"));
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setText(QStringLiteral("输入关键字后点击搜索"));
    root->addWidget(m_statusLabel);

    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(sc, &QShortcut::activated, this, &AddFriendSearchDialog::onSearchClicked);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &AddFriendSearchDialog::onSearchClicked);
}

void AddFriendSearchDialog::setTcpClient(LanTcpClient *client, const QString &token)
{
    m_client = client;
    m_token = token;
}

void AddFriendSearchDialog::clearSession()
{
    m_client = nullptr;
    m_token.clear();
    clearSearchResults();
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("输入关键字后点击搜索"));
    }
}

void AddFriendSearchDialog::sendJson(const QJsonObject &obj)
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

void AddFriendSearchDialog::onSearchClicked()
{
    const QString q = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (q.isEmpty()) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("请输入搜索内容"));
        }
        return;
    }
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("friend_search"));
    o.insert(QStringLiteral("token"), m_token);
    o.insert(QStringLiteral("query"), q);
    o.insert(QStringLiteral("limit"), 20);
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("正在搜索…"));
    }
    sendJson(o);
}

void AddFriendSearchDialog::clearSearchResults()
{
    if (m_searchList) {
        m_searchList->clear();
    }
}

void AddFriendSearchDialog::addSearchResultRow(qint64 userId, const QString &email, const QString &nickname)
{
    auto *item = new QListWidgetItem(m_searchList);
    auto *w = new QWidget(m_searchList);
    auto *h = new QHBoxLayout(w);
    h->setContentsMargins(8, 8, 8, 8);
    const QString nick = nickname.isEmpty() ? QStringLiteral("（无昵称）") : nickname;
    auto *lb = new QLabel(QStringLiteral("%1\n%2").arg(nick, email), w);
    lb->setObjectName(QStringLiteral("addFriendResultText"));
    lb->setStyleSheet(QStringLiteral("font-size: 13px;"));
    h->addWidget(lb, 1);
    auto *btn = new QPushButton(QStringLiteral("添加好友"), w);
    btn->setObjectName(QStringLiteral("addFriendResultBtn"));
    btn->setFixedWidth(88);
    btn->setCursor(Qt::PointingHandCursor);
    connect(btn, &QPushButton::clicked, this, [this, userId]() { requestAddFriend(userId); });
    h->addWidget(btn);
    item->setSizeHint(QSize(0, 56));
    m_searchList->addItem(item);
    m_searchList->setItemWidget(item, w);
}

void AddFriendSearchDialog::applySearchResults(const QJsonArray &users)
{
    clearSearchResults();
    for (const QJsonValue &v : users) {
        if (!v.isObject()) {
            continue;
        }
        const QJsonObject u = v.toObject();
        const qint64 id = u.value(QStringLiteral("user_id")).toVariant().toLongLong();
        const QString em = u.value(QStringLiteral("email")).toString();
        const QString nick = u.value(QStringLiteral("nickname")).toString();
        if (id > 0) {
            addSearchResultRow(id, em, nick);
        }
    }
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("找到 %1 个用户").arg(users.size()));
    }
}

void AddFriendSearchDialog::requestAddFriend(qint64 userId)
{
    QJsonObject o;
    o.insert(QStringLiteral("type"), QStringLiteral("friend_request_send"));
    o.insert(QStringLiteral("token"), m_token);
    o.insert(QStringLiteral("target_user_id"), userId);
    if (m_statusLabel) {
        m_statusLabel->setText(QStringLiteral("正在发送好友申请…"));
    }
    sendJson(o);
}

void AddFriendSearchDialog::handleServerJson(const QJsonObject &obj)
{
    if (!isVisible()) {
        return;
    }
    const QString t = obj.value(QStringLiteral("type")).toString();
    if (t == QStringLiteral("friend_search_result")) {
        applySearchResults(obj.value(QStringLiteral("users")).toArray());
        return;
    }
    if (t == QStringLiteral("friend_request_sent")) {
        if (m_statusLabel) {
            m_statusLabel->setText(QStringLiteral("好友申请已发送"));
        }
        QMessageBox::information(this, QStringLiteral("成功"), QStringLiteral("好友申请已发送"));
        return;
    }
    if (t == QStringLiteral("error")) {
        const QString msg = obj.value(QStringLiteral("message")).toString();
        if (m_statusLabel) {
            m_statusLabel->setText(msg);
        }
    }
}
