#ifndef PROFILE_DIALOG_H
#define PROFILE_DIALOG_H

#include <QDialog>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class LanTcpClient;

/// 查看/编辑个人资料：昵称经 `profile_set` 同步；头像经 `profile_set_avatar` 同步（JPEG）；个签仅存本机。
class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileDialog(const QString &accountEmail, const QString &username, LanTcpClient *tcp = nullptr,
                           const QString &sessionToken = QString(), QWidget *parent = nullptr);

    QString accountEmail() const { return m_email; }

private slots:
    void onPickAvatar();
    void onSaveClicked();
    void onNicknameChanged(const QString &text);
    void onBioChanged();

private:
    void loadFromLocal();
    void refreshAvatarPreview();
    void refreshCounters();
    /// 已连接且 token 非空时提交 `profile_set` 并等待 `profile_set_ok`（或带 `corr` 的 `error`）。
    bool pushNicknameToServer(const QString &nickname, QString *errOut);
    /// 读取本机 `avatar.jpg` 为 Base64 提交 `profile_set_avatar`，等待 `profile_set_avatar_ok` 或 `error`。
    bool pushAvatarJpegFileToServer(QString *errOut);

    QString m_email;
    QString m_username;
    LanTcpClient *m_tcp = nullptr;
    QString m_sessionToken;
    QLabel *m_avatarLabel = nullptr;
    QLineEdit *m_accountEdit = nullptr;
    QLineEdit *m_usernameEdit = nullptr;
    QLineEdit *m_nicknameEdit = nullptr;
    QPlainTextEdit *m_bioEdit = nullptr;
    QLabel *m_nickCountLabel = nullptr;
    QLabel *m_bioCountLabel = nullptr;

    enum class AvatarState { Unchanged, NewImage };
    AvatarState m_avatarState = AvatarState::Unchanged;
    QPixmap m_newAvatarPixmap; // 用户新选的图（保存前预览）
};

#endif // PROFILE_DIALOG_H
