#ifndef LOCAL_STICKERS_H
#define LOCAL_STICKERS_H

#include <QtGlobal>

#include <QString>
#include <QStringList>

/// 本机自定义表情（按登录邮箱隔离目录），供表情面板与发送复用。
namespace LocalStickers {

/// 单文件上限（字节），与面板体验匹配，避免过大 GIF 卡 UI。
constexpr qint64 kMaxStickerImportBytes = 8 * 1024 * 1024;

/// 目录内最多保存的自定义表情数量。
constexpr int kMaxStickerCount = 120;

QString stickerDirectoryForUser(const QString &email);

/// 按修改时间从新到旧列出可识别后缀的文件绝对路径。
QStringList listStickerPaths(const QString &email);

/// 将用户选择的文件拷贝到本机表情目录；成功返回新文件绝对路径。
bool importStickerFile(const QString &email, const QString &sourceAbsolutePath, QString *outStoredPath,
                        QString *errOut);

} // namespace LocalStickers

#endif // LOCAL_STICKERS_H
