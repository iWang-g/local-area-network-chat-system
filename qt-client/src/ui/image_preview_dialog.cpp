#include "ui/image_preview_dialog.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QGuiApplication>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QScreen>
#include <QSize>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>

void ImagePreviewDialog::showForPath(const QString &imagePath, QWidget *parent)
{
    if (imagePath.isEmpty() || !QFile::exists(imagePath)) {
        QMessageBox::warning(parent, QStringLiteral("预览"),
                             QStringLiteral("图片未保存到本机或文件已删除，无法预览。"));
        return;
    }
    QPixmap pm;
    if (!pm.load(imagePath)) {
        QMessageBox::warning(parent, QStringLiteral("预览"), QStringLiteral("无法加载该图片。"));
        return;
    }

    QWidget *hostWin = parent ? parent->window() : nullptr;
    QRect ag;
    if (hostWin && hostWin->isVisible()) {
        ag = hostWin->frameGeometry();
    }
    if (ag.isEmpty()) {
        QScreen *scr = nullptr;
        if (hostWin) {
            scr = QGuiApplication::screenAt(hostWin->mapToGlobal(hostWin->rect().center()));
        }
        if (!scr) {
            scr = QGuiApplication::primaryScreen();
        }
        ag = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 720);
    }

    // 预览区上限：优先比聊天主窗口略小；无父窗口时用屏幕一部分
    int maxPreviewW;
    int maxPreviewH;
    if (hostWin && hostWin->width() > 100 && hostWin->height() > 100) {
        maxPreviewW = std::max(280, hostWin->width() - 80);
        maxPreviewH = std::max(220, hostWin->height() - 140);
    } else {
        maxPreviewW = std::min(720, static_cast<int>(ag.width() * 0.55));
        maxPreviewH = std::min(540, static_cast<int>(ag.height() * 0.55));
    }

    // 在矩形内「尽可能大」显示（小图会放大，大图会缩小），保持宽高比
    const QSize previewMax(maxPreviewW, maxPreviewH);
    const QSize targetSize = pm.size().scaled(previewMax, Qt::KeepAspectRatio);
    const QPixmap display =
        pm.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    QDialog dlg(hostWin);
    dlg.setWindowTitle(QStringLiteral("图片预览"));
    dlg.setModal(true);
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto *root = new QVBoxLayout(&dlg);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto *pic = new QLabel(&dlg);
    pic->setAlignment(Qt::AlignCenter);
    pic->setPixmap(display);
    pic->setFixedSize(display.size());
    pic->setScaledContents(false);
    root->addWidget(pic, 0, Qt::AlignHCenter);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    root->addWidget(buttonBox, 0, Qt::AlignRight);

    const int chromeW = 12 * 2 + 4;
    const int chromeH = 12 * 2 + 10 + 36;
    int dlgW = display.width() + chromeW;
    int dlgH = display.height() + chromeH;

    if (hostWin && hostWin->width() > 0 && hostWin->height() > 0) {
        const int mw = std::max(200, hostWin->width() - 24);
        const int mh = std::max(120, hostWin->height() - 24);
        dlg.setMaximumSize(mw, mh);
        dlgW = std::min(dlgW, mw);
        dlgH = std::min(dlgH, mh);
    }
    dlgW = std::max(dlgW, 200);
    dlgH = std::max(dlgH, 120);
    dlg.resize(dlgW, dlgH);
    dlg.exec();
}
