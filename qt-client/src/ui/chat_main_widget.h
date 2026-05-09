#ifndef CHAT_MAIN_WIDGET_H
#define CHAT_MAIN_WIDGET_H

#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPixmap>
#include <QPoint>
#include <QSaveFile>
#include <QSet>
#include <QWidget>

#include <memory>

class AddFriendSearchDialog;
class ChatHistoryDialog;
class ContactsWidget;
class StickerPickerPopup;
class LanTcpClient;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPlainTextEdit;
class QScrollArea;
class QLabel;
class QStackedWidget;
class QTimer;
class QToolButton;
class QVBoxLayout;

/// 主聊天界面：左功能栏 + 会话列表 + 聊天区（布局参考常见 PC 端 IM）。
class ChatMainWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ChatMainWidget(QWidget *parent = nullptr);
    void setUserEmail(const QString &email);
    void setSession(LanTcpClient *client, const QString &token, qint64 userId);
    void clearSession();
    QString userEmail() const { return m_userEmail; }
    /// 登录后主窗口转发的 TCP 业务 JSON（好友等）。
    void handleServerJson(const QJsonObject &obj);

signals:
    void logoutRequested();
    void tcpDebugRequested();
    void profileEditRequested();

private slots:
    void onFriendSelectionChanged();
    void onSendClicked();
    void scrollMessagesToBottom();
    void onMsgPollTick();
    void onPickSendFileClicked();
    void onPickSendImageClicked();
    void onInputExpressionClicked();
    void onFileSendTick();
    void openChatHistoryDialog();
    void onChatHistoryDialogFinished();
    void onSessionListContextMenu(const QPoint &pos);

private:
    QWidget *buildIconRail();
    QWidget *buildSessionPanel();
    QWidget *buildChatPanel();
    QWidget *buildContactModePlaceholder();
    QWidget *createSessionRowWidget(const QString &name, const QString &preview, const QString &time,
                                    bool online, int unreadCount, bool pinned);
    QWidget *createBubble(const QString &text, bool isOutgoing);
    QWidget *createCenteredTimeRow(qint64 createdAtSec);
    void clearMessageList();
    void updateChatHeader(const QString &name, bool online);
    void updateFriendPresenceUi(qint64 peerUserId, bool online);
    void syncPeerAvatarForCurrentSession();
    void applySessionRowAvatar(QListWidgetItem *item, qint64 peerUserId, bool fetchIfMissing = true);
    void updateSessionListAvatarForPeer(qint64 peerUserId, bool fetchIfMissing = true);
    void setIncomingAvatarOnLabel(QLabel *av);
    void refreshIncomingAvatarLabels();
    void refreshSessionRowHighlight();
    void requestFriendListForSessions();
    void requestPeerAvatar(qint64 peerUserId);
    void requestMsgFetchInitial();
    void requestMsgFetchPoll();
    void applyFriendListToSessions(const QJsonArray &friends);
    void applySessionListSearchFilter();
    void handleMsgFetchOk(const QJsonObject &obj);
    void handleMsgSendOk(const QJsonObject &obj);
    void handleMsgPush(const QJsonObject &obj);
    void prependTimeDividerIfNeeded(qint64 createdAtSec);
    void appendMessageLine(const QString &content, bool outgoing, qint64 createdAtSec, qint64 messageId);
    QWidget *createFileBubble(const QJsonObject &fileObj, bool isOutgoing, qint64 transferId);
    QWidget *createImageBubble(const QJsonObject &fileObj, bool isOutgoing, qint64 transferId);
    void startMsgPolling();
    void stopMsgPolling();
    static QString formatSessionListTime(qint64 secsSinceEpoch);
    static QString formatMsgDividerTime(qint64 secsSinceEpoch);
    static QString clipSessionPreview(const QString &text);
    void updateSessionRowPreview(qint64 peerId, const QString &rawContent, qint64 createdAtSec, bool sentBySelf);
    void syncSessionRowUnreadUI(qint64 peerId);
    void resetFileSendState();
    void tryStartOutgoingFileTransfer();
    void handleFileIncoming(const QJsonObject &obj);
    void handleFileOfferDelivered(const QJsonObject &obj);
    void appendOutgoingFileOfferRow(const QString &name, qint64 sizeBytes, bool isImage = false,
                                    const QString &localPreviewPath = QString(), bool asSticker = false);
    void appendIncomingFileOfferRow(const QJsonObject &obj);
    void acceptIncomingFileOffer(qint64 transferId);
    void removePlaceholderBubblesForFileTransfer(qint64 transferId, bool outgoing);
    void removeMessageWidget(QWidget *w);
    void updateOutgoingFileOfferError(const QString &statusLine);
    void handleBusinessErrorFrame(const QJsonObject &obj);
    void loadSessionUiPrefs();
    void saveSessionUiPrefs();
    void sendMsgClearRequest(qint64 peerId);
    void applyConversationClearedUi(qint64 peerInConversation);
    void handleFileChunkPush(const QJsonObject &obj);
    void handleFileTransferDone(const QJsonObject &obj);
    void handleFileAborted(const QJsonObject &obj);
    void beginStickerArtifactPullIfNeeded(qint64 transferId, const QString &fileName, QLabel *imgLabel);
    struct PendingStickerPull {
        qint64 transferId = 0;
        QString fileName;
        QPointer<QLabel> label;
    };
    void beginStickerArtifactPullImmediate(const PendingStickerPull &spec);
    void tryStartNextStickerPullInQueue();
    void cancelStickerArtifactPull();
    void handleStickerPullOk(const QJsonObject &obj);
    void handleStickerPullChunk(const QJsonObject &obj);
    void handleStickerPullDone(const QJsonObject &obj);
    void cleanupFileReceive(bool commit);
    void rememberTransferDiskPath(qint64 transferId, const QString &absolutePath);
    void openAddFriendSearchDialog();
    void sendLocalImageAsFileOffer(const QString &path, bool asSticker = false);

    QString m_userEmail;
    LanTcpClient *m_sessionClient = nullptr;
    QString m_sessionToken;
    qint64 m_sessionUserId = 0;

    QStackedWidget *m_leftStack = nullptr;
    /// 分割器第三栏：会话模式显示聊天面板，联系人模式显示占位页。
    QStackedWidget *m_rightStack = nullptr;
    QWidget *m_chatPanel = nullptr;
    ContactsWidget *m_contacts = nullptr;
    QPointer<AddFriendSearchDialog> m_addFriendDialog;
    QPointer<ChatHistoryDialog> m_chatHistoryDialog;
    bool m_pendingHistoryMsgFetch = false;
    qint64 m_historyFetchPeerId = 0;
    QToolButton *m_railSessionBtn = nullptr;
    QToolButton *m_railContactsBtn = nullptr;
    QToolButton *m_railTcpDebugBtn = nullptr;
    QToolButton *m_railLogoutBtn = nullptr;

    QListWidget *m_friendList = nullptr;
    QLineEdit *m_sessionSearchEdit = nullptr;
    QStackedWidget *m_centerStack = nullptr;
    QWidget *m_centerEmpty = nullptr;
    QWidget *m_chatArea = nullptr;
    QLabel *m_peerTitleLabel = nullptr;
    QLabel *m_peerPresenceIcon = nullptr;
    QLabel *m_peerPresenceText = nullptr;
    QScrollArea *m_messageScroll = nullptr;
    QWidget *m_messageContainer = nullptr;
    QVBoxLayout *m_messageLayout = nullptr;
    QPlainTextEdit *m_inputEdit = nullptr;
    QToolButton *m_inputExpressionBtn = nullptr;
    QPointer<StickerPickerPopup> m_stickerPicker;
    QPixmap m_avatarPix;
    /// 当前会话对方头像（圆形 36px）；无服务端头像时为空，气泡侧显示默认占位。
    QPixmap m_peerAvatarPix;
    QHash<qint64, QPixmap> m_peerAvatarPixByPeer;
    QHash<qint64, qint64> m_peerAvatarRevByPeer;

    QTimer *m_msgPollTimer = nullptr;
    /// 仅对「切换会话后的首次 msg_fetch」为 true，用于与轮询拉取区分是否自动滚到底部。
    bool m_scrollAfterNextMsgFetch = false;
    /// 用于插入「中间时间条」：上一条已展示消息的 `created_at`（秒），`clearMessageList` 时归零。
    qint64 m_prevMsgCreatedAtForDivider = 0;
    qint64 m_currentPeerId = 0;
    qint64 m_lastMsgId = 0;
    QSet<qint64> m_seenMessageIds;
    /// 非当前会话收到的新消息条数（仅内存，随 `friend_list_ok` 重绘列表时保留）
    QHash<qint64, int> m_unreadByPeer;

    /// 会话列表置顶顺序（仅客户端，`QSettings` 持久化）。
    QList<qint64> m_pinnedPeerIds;
    /// 从会话列表隐藏的好友；对端发来新消息时自动取消隐藏。
    QSet<qint64> m_hiddenSessionPeers;
    qint64 m_pendingMsgClearPeer = 0;

    QString m_pendingOfferPath;
    qint64 m_pendingOfferFileSize = 0;
    bool m_pendingOfferIsImage = false;
    bool m_pendingOfferAsSticker = false;
    qint64 m_pendingOfferTransferId = 0;
    QPointer<QWidget> m_outgoingFileOfferBubble;
    QHash<qint64, QJsonObject> m_incomingFileOfferParamsByTid;
    QHash<qint64, QPointer<QWidget>> m_incomingFileOfferBubbleByTid;
    int m_offerChunkPlainMax = 65536;
    bool m_fileSendGateReady = false;
    std::unique_ptr<QFile> m_outgoingFile;
    quint32 m_outgoingSeq = 0;
    QTimer *m_fileSendTimer = nullptr;

    qint64 m_recvTransferId = 0;
    /// 本机路径（发送方源文件 / 接收方保存路径），供文件气泡「打开所在文件夹」。
    QHash<qint64, QString> m_localPathByTransferId;
    QString m_recvExpectedSha;
    qint64 m_recvExpectedSize = 0;
    qint64 m_recvWritten = 0;
    std::unique_ptr<QSaveFile> m_recvFile;
    std::unique_ptr<QCryptographicHash> m_recvHash;

    qint64 m_stickerPullTransferId = 0;
    qint64 m_stickerPullExpectedSize = 0;
    qint64 m_stickerPullWritten = 0;
    quint32 m_stickerPullNextSeq = 0;
    int m_stickerPullChunkPlainMax = 49152;
    QString m_stickerPullSavePath;
    std::unique_ptr<QSaveFile> m_stickerPullFile;
    QPointer<QLabel> m_stickerPullTargetLabel;
    QList<PendingStickerPull> m_stickerPullQueue;
};

#endif // CHAT_MAIN_WIDGET_H
