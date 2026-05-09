#include "utils/local_stickers.h"

#include "utils/local_profile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

namespace LocalStickers {

QString stickerDirectoryForUser(const QString &email)
{
    if (email.trimmed().isEmpty()) {
        return {};
    }
    const QString root = LocalProfile::profileStorageRoot();
    const QString dir = root + QLatin1String("/stickers/") + LocalProfile::emailKey(email);
    QDir().mkpath(dir);
    return dir;
}

static bool allowedImageSuffix(const QString &suffixLower)
{
    static const QSet<QString> k{QStringLiteral("gif"), QStringLiteral("png"), QStringLiteral("jpg"),
                                   QStringLiteral("jpeg"), QStringLiteral("webp"), QStringLiteral("bmp")};
    return k.contains(suffixLower);
}

QStringList listStickerPaths(const QString &email)
{
    const QString dir = stickerDirectoryForUser(email);
    if (dir.isEmpty()) {
        return {};
    }
    QStringList names = QDir(dir).entryList(QStringList{QStringLiteral("*.gif"), QStringLiteral("*.png"),
                                                        QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                                                        QStringLiteral("*.webp"), QStringLiteral("*.bmp")},
                                            QDir::Files, QDir::Time | QDir::Reversed);
    QStringList out;
    out.reserve(names.size());
    for (const QString &n : names) {
        out.append(dir + QLatin1Char('/') + n);
    }
    return out;
}

bool importStickerFile(const QString &email, const QString &sourceAbsolutePath, QString *outStoredPath,
                       QString *errOut)
{
    if (outStoredPath) {
        outStoredPath->clear();
    }
    const QString dir = stickerDirectoryForUser(email);
    if (dir.isEmpty()) {
        if (errOut) {
            *errOut = QStringLiteral("未登录账号，无法保存表情");
        }
        return false;
    }
    const QFileInfo srcFi(sourceAbsolutePath);
    if (!srcFi.exists() || !srcFi.isFile()) {
        if (errOut) {
            *errOut = QStringLiteral("源文件不存在");
        }
        return false;
    }
    const qint64 sz = srcFi.size();
    if (sz <= 0 || sz > kMaxStickerImportBytes) {
        if (errOut) {
            *errOut = QStringLiteral("文件无效或超过 8MB");
        }
        return false;
    }
    const QString suf = srcFi.suffix().toLower();
    if (!allowedImageSuffix(suf)) {
        if (errOut) {
            *errOut = QStringLiteral("仅支持 gif / png / jpg / webp");
        }
        return false;
    }
    if (listStickerPaths(email).size() >= kMaxStickerCount) {
        if (errOut) {
            *errOut = QStringLiteral("自定义表情数量已达上限");
        }
        return false;
    }
    const QString base = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString destPath = dir + QLatin1Char('/') + base + QLatin1Char('.') + suf;
    if (QFile::exists(destPath)) {
        if (errOut) {
            *errOut = QStringLiteral("目标已存在，请重试");
        }
        return false;
    }
    if (!QFile::copy(srcFi.absoluteFilePath(), destPath)) {
        if (errOut) {
            *errOut = QStringLiteral("复制文件失败");
        }
        return false;
    }
    if (outStoredPath) {
        *outStoredPath = QFileInfo(destPath).absoluteFilePath();
    }
    return true;
}

} // namespace LocalStickers
