#ifndef LOCAL_PROFILE_H
#define LOCAL_PROFILE_H

#include <QHash>
#include <QString>
#include <QtGlobal>

class QPixmap;

/// 本机个人资料（按登录邮箱隔离）。昵称与 `auth_ok` / `profile_set` 与服务端同步；个签与头像仅存本机。
namespace LocalProfile {

struct Data {
    QString nickname;
    QString bio;
    bool hasAvatar = false;
};

/// 规范化邮箱（小写、trim）后参与哈希，作为存储键的一部分。
QString emailKey(const QString &email);

QString profileStorageRoot();

/// 头像落盘路径（JPEG，可能不存在）。
QString avatarFilePath(const QString &email);

void load(const QString &email, Data *out);

bool saveMeta(const QString &email, const QString &nickname, const QString &bio);

/// 将源图缩放后存为 JPEG（最长边不超过 maxSide，质量 0–100）。
bool saveAvatarImage(const QString &email, const QPixmap &pixmap, int maxSide = 512, int quality = 85);

void clearAvatar(const QString &email);

/// 若存在头像文件则载入并裁成圆形；否则返回 false。
bool loadAvatarPixmap(const QString &email, int displaySize, QPixmap *out);

/// 本机 `transfer_id` → 文件绝对路径（用于重启后恢复图片/文件气泡与「打开文件夹」）。
void rememberTransferLocalPath(const QString &email, qint64 transferId, const QString &absolutePath);
void loadTransferPathMap(const QString &email, QHash<qint64, QString> *out);

} // namespace LocalProfile

#endif // LOCAL_PROFILE_H
