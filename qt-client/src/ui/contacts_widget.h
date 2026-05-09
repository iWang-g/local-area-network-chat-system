#ifndef CONTACTS_WIDGET_H
#define CONTACTS_WIDGET_H

#include <QJsonObject>
#include <QWidget>

class LanTcpClient;
class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

/// 好友：发起申请、处理申请、好友列表与删除（协议 `friend_*`）。分组为可折叠树，右键分组标题刷新。
class ContactsWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ContactsWidget(QWidget *parent = nullptr);

    void setTcpClient(LanTcpClient *client);
    void setToken(const QString &token);

    /// 进入「联系人」页时拉取申请列表与好友列表。
    void refreshAll();

    /// 仅刷新好友申请列表（供「添加好友」对话框在发出申请后调用）。
    void refreshFriendRequestList();

    /// 主窗口转发的业务 JSON（`friend_*` 响应与 `error`）。
    void handleServerJson(const QJsonObject &obj);

private slots:
    void onRefreshRequestsClicked();
    void onRefreshFriendsClicked();

private:
    void sendJson(const QJsonObject &obj);
    void clearSectionChildren(QTreeWidgetItem *root);
    void applyRequestList(const QJsonObject &obj);
    void applyFriendList(const QJsonArray &friends);
    void addIncomingRow(qint64 requestId, qint64 fromUserId, const QString &email, const QString &nickname);
    void addOutgoingRow(qint64 requestId, qint64 toUserId, const QString &email, const QString &nickname);
    void addFriendRow(qint64 userId, const QString &email, const QString &nickname);
    void requestHandle(qint64 requestId, const QString &action);
    void requestDeleteFriend(qint64 peerUserId);
    void showSectionContextMenu(const QPoint &pos);

    LanTcpClient *m_client = nullptr;
    QString m_token;

    QTreeWidget *m_tree = nullptr;
    QTreeWidgetItem *m_incomingRoot = nullptr;
    QTreeWidgetItem *m_outgoingRoot = nullptr;
    QTreeWidgetItem *m_friendsRoot = nullptr;
    QLabel *m_statusLabel = nullptr;
};

#endif // CONTACTS_WIDGET_H
