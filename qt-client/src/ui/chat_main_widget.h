#ifndef CHAT_MAIN_WIDGET_H
#define CHAT_MAIN_WIDGET_H

#include <QPixmap>
#include <QWidget>

class QListWidget;
class QPlainTextEdit;
class QScrollArea;
class QLabel;
class QStackedWidget;
class QVBoxLayout;

/// 主聊天界面：左功能栏 + 会话列表 + 聊天区（布局参考常见 PC 端 IM）。
class ChatMainWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatMainWidget(QWidget *parent = nullptr);
    void setUserEmail(const QString &email);

signals:
    void logoutRequested();

private slots:
    void onFriendSelectionChanged();
    void onSendClicked();
    void scrollMessagesToBottom();

private:
    QWidget *buildIconRail();
    QWidget *buildSessionPanel();
    QWidget *buildChatPanel();
    QWidget *createSessionRowWidget(const QString &name, const QString &preview, const QString &time,
                                    bool online);
    QWidget *createBubble(const QString &text, bool isOutgoing);
    void populateSessionList();
    void clearMessageList();
    void reloadDemoThreadForPeer(const QString &name);
    void updateChatHeader(const QString &name, bool online);
    void refreshSessionRowHighlight();

    QString m_userEmail;
    QListWidget *m_friendList = nullptr;
    QStackedWidget *m_centerStack = nullptr;
    QWidget *m_centerEmpty = nullptr;
    QWidget *m_chatArea = nullptr;
    QLabel *m_peerTitleLabel = nullptr;
    QLabel *m_onlineDotLabel = nullptr;
    QScrollArea *m_messageScroll = nullptr;
    QWidget *m_messageContainer = nullptr;
    QVBoxLayout *m_messageLayout = nullptr;
    QPlainTextEdit *m_inputEdit = nullptr;
    QPixmap m_avatarPix;
};

#endif // CHAT_MAIN_WIDGET_H
