#ifndef ADD_FRIEND_SEARCH_DIALOG_H
#define ADD_FRIEND_SEARCH_DIALOG_H

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>

class LanTcpClient;
class QListWidget;
class QLineEdit;
class QLabel;

/// 会话列表「添加 → 加好友」打开的搜索对话框（协议 `friend_search` / `friend_request_send`）。
class AddFriendSearchDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AddFriendSearchDialog(QWidget *parent = nullptr);

    void setTcpClient(LanTcpClient *client, const QString &token);
    void clearSession();

    /// 处理 `friend_search_result`、`friend_request_sent`、好友相关 `error`（对话框可见时）。
    void handleServerJson(const QJsonObject &obj);

private slots:
    void onSearchClicked();

private:
    void sendJson(const QJsonObject &obj);
    void clearSearchResults();
    void applySearchResults(const QJsonArray &users);
    void addSearchResultRow(qint64 userId, const QString &email, const QString &nickname);
    void requestAddFriend(qint64 userId);

    LanTcpClient *m_client = nullptr;
    QString m_token;

    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_searchList = nullptr;
    QLabel *m_statusLabel = nullptr;
};

#endif // ADD_FRIEND_SEARCH_DIALOG_H
