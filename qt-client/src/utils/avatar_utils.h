#ifndef AVATAR_UTILS_H
#define AVATAR_UTILS_H

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

inline QPixmap makeCircularAvatar(const QPixmap &source, int size)
{
    if (source.isNull() || size <= 0) {
        return {};
    }
    const QPixmap scaled =
        source.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    QPixmap round(size, size);
    round.fill(Qt::transparent);

    QPainter p(&round);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    QPainterPath path;
    path.addEllipse(0, 0, size, size);
    p.setClipPath(path);
    const int x = (size - scaled.width()) / 2;
    const int y = (size - scaled.height()) / 2;
    p.drawPixmap(x, y, scaled);
    return round;
}

#endif // AVATAR_UTILS_H
