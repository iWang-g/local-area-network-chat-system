#include "utils/local_profile.h"

#include "utils/avatar_utils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QPixmap>
#include <QSettings>
#include <QStandardPaths>

namespace LocalProfile {

namespace {

QString hashKey(const QString &email)
{
    const QByteArray h =
        QCryptographicHash::hash(email.trimmed().toLower().toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(h.toHex());
}

QString settingsFilePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir + QLatin1String("/local_profile.ini");
}

} // namespace

QString emailKey(const QString &email)
{
    return hashKey(email);
}

QString profileStorageRoot()
{
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QLatin1String("/lncs_profile");
    QDir().mkpath(root);
    return root;
}

QString avatarFilePath(const QString &email)
{
    if (email.trimmed().isEmpty()) {
        return {};
    }
    return profileStorageRoot() + QLatin1Char('/') + hashKey(email) + QLatin1String("/avatar.jpg");
}

void load(const QString &email, Data *out)
{
    if (!out) {
        return;
    }
    *out = {};
    if (email.trimmed().isEmpty()) {
        return;
    }
    const QString hk = hashKey(email);
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.beginGroup(QStringLiteral("profiles"));
    s.beginGroup(hk);
    out->nickname = s.value(QStringLiteral("nickname")).toString();
    out->bio = s.value(QStringLiteral("bio")).toString();
    s.endGroup();
    s.endGroup();
    out->hasAvatar = QFile::exists(avatarFilePath(email));
}

bool saveMeta(const QString &email, const QString &nickname, const QString &bio)
{
    if (email.trimmed().isEmpty()) {
        return false;
    }
    const QString hk = hashKey(email);
    QSettings s(settingsFilePath(), QSettings::IniFormat);
    s.beginGroup(QStringLiteral("profiles"));
    s.beginGroup(hk);
    s.setValue(QStringLiteral("nickname"), nickname);
    s.setValue(QStringLiteral("bio"), bio);
    s.endGroup();
    s.endGroup();
    s.sync();
    return true;
}

bool saveAvatarImage(const QString &email, const QPixmap &pixmap, int maxSide, int quality)
{
    if (email.trimmed().isEmpty() || pixmap.isNull() || maxSide <= 0) {
        return false;
    }
    const QString path = avatarFilePath(email);
    const QString parent = QFileInfo(path).absolutePath();
    QDir().mkpath(parent);

    QPixmap p = pixmap;
    if (p.width() > maxSide || p.height() > maxSide) {
        p = p.scaled(maxSide, maxSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return p.save(path, "JPEG", quality);
}

void clearAvatar(const QString &email)
{
    const QString p = avatarFilePath(email);
    if (!p.isEmpty()) {
        QFile::remove(p);
    }
}

bool loadAvatarPixmap(const QString &email, int displaySize, QPixmap *out)
{
    if (!out || displaySize <= 0) {
        return false;
    }
    const QString path = avatarFilePath(email);
    if (path.isEmpty() || !QFile::exists(path)) {
        return false;
    }
    QPixmap raw;
    if (!raw.load(path)) {
        return false;
    }
    *out = makeCircularAvatar(raw, displaySize);
    return !out->isNull();
}

static QString transferPathsJsonPath(const QString &email)
{
    if (email.trimmed().isEmpty()) {
        return {};
    }
    const QString dir = profileStorageRoot() + QLatin1Char('/') + hashKey(email);
    QDir().mkpath(dir);
    return dir + QLatin1String("/transfer_paths.json");
}

void rememberTransferLocalPath(const QString &email, qint64 transferId, const QString &absolutePath)
{
    if (email.trimmed().isEmpty() || transferId <= 0 || absolutePath.trimmed().isEmpty()) {
        return;
    }
    const QString fp = transferPathsJsonPath(email);
    if (fp.isEmpty()) {
        return;
    }
    const QString abs = QFileInfo(absolutePath).absoluteFilePath();
    QJsonObject o;
    if (QFile f(fp); f.exists() && f.open(QIODevice::ReadOnly)) {
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            o = doc.object();
        }
    }
    o.insert(QString::number(transferId), abs);
    QFile out(fp);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    out.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

void loadTransferPathMap(const QString &email, QHash<qint64, QString> *out)
{
    if (!out) {
        return;
    }
    out->clear();
    if (email.trimmed().isEmpty()) {
        return;
    }
    const QString fp = transferPathsJsonPath(email);
    if (fp.isEmpty() || !QFile::exists(fp)) {
        return;
    }
    QFile f(fp);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }
    QJsonParseError pe{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const qint64 tid = it.key().toLongLong();
        if (tid <= 0) {
            continue;
        }
        const QString p = it.value().toString();
        if (!p.isEmpty()) {
            out->insert(tid, p);
        }
    }
}

} // namespace LocalProfile
