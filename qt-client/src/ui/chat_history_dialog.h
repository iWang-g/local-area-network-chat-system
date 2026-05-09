#ifndef CHAT_HISTORY_DIALOG_H
#define CHAT_HISTORY_DIALOG_H

#include <QDialog>
#include <QJsonObject>
#include <QPixmap>
#include <QVector>

class QListWidget;
class QLineEdit;
class QTabBar;
class QLabel;

struct ChatHistoryEntry {
    qint64 messageId = 0;
    qint64 fromUserId = 0;
    QString content;
    qint64 createdAt = 0;
};

/// 会话内「聊天记录」：拉取 `msg_fetch` 结果，支持搜索与分类筛选（仿 QQ 布局简化版）。
class ChatHistoryDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ChatHistoryDialog(QWidget *parent = nullptr);

    void setSession(qint64 sessionUserId, qint64 peerId, const QString &peerTitle, const QString &userEmail,
                    const QPixmap &peerAvatarPixmap);

    void applyMsgFetchOk(const QJsonObject &obj);
    void notifyFetchError(const QString &message);

    qint64 peerId() const { return m_peerId; }

private:
    void rebuildList();
    QString formatLineText(const QString &content) const;
    static bool parseFileOrImageMessage(const QString &content, QJsonObject *out);
    static bool isImageFileNameString(const QString &name);
    bool entryPassesTab(const ChatHistoryEntry &e) const;
    bool entryPassesSearch(const ChatHistoryEntry &e) const;
    void appendDateRowIfNeeded(const QDate &msgDate, QDate *lastDateOut);

    qint64 m_sessionUserId = 0;
    qint64 m_peerId = 0;
    QString m_userEmail;
    QPixmap m_peerAvatar;

    QVector<ChatHistoryEntry> m_entries;

    QListWidget *m_list = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QTabBar *m_tabBar = nullptr;
    QLabel *m_statusLabel = nullptr;
};

#endif // CHAT_HISTORY_DIALOG_H
