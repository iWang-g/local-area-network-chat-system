#include "ui/sticker_picker_popup.h"

#include "utils/local_stickers.h"

#include <QApplication>
#include <QPointer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QShowEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QScreen>
#include <QScrollArea>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

constexpr int kThumb = 56;
constexpr int kCols = 6;

} // namespace

StickerPickerPopup::StickerPickerPopup(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint)
{
    setObjectName(QStringLiteral("StickerPickerPopup"));
    setAttribute(Qt::WA_TranslucentBackground, false);
    setFocusPolicy(Qt::StrongFocus);
    setStyleSheet(QStringLiteral(
        "QWidget#StickerPickerPopup { background: #fafafa; border: 1px solid #d9d9d9; border-radius: 8px; }"
        "QToolButton#lanStickerCell { background: #fff; border: 1px solid #e8e8e8; border-radius: 6px; padding: 2px; }"
        "QToolButton#lanStickerCell:hover { border-color: #1890ff; background: #f0f7ff; }"
        "QToolButton#lanStickerAddCell { background: #fff; border: 2px dashed #bfbfbf; border-radius: 6px; color: #8c8c8c; "
        "font-size: 28px; font-weight: bold; }"
        "QToolButton#lanStickerAddCell:hover { border-color: #1890ff; color: #1890ff; }"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    auto *hint = new QLabel(QStringLiteral("点击表情发送（与「图片」相同通道）"), this);
    hint->setStyleSheet(QStringLiteral("color: #8c8c8c; font-size: 12px;"));
    root->addWidget(hint);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setMinimumHeight(200);
    m_scroll->setMaximumHeight(280);

    m_gridHost = new QWidget(m_scroll);
    m_grid = new QGridLayout(m_gridHost);
    m_grid->setContentsMargins(4, 4, 4, 4);
    m_grid->setSpacing(6);
    m_scroll->setWidget(m_gridHost);
    root->addWidget(m_scroll, 1);

    setFixedSize(420, 320);
}

void StickerPickerPopup::setUserEmail(const QString &email)
{
    m_email = email.trimmed();
}

void StickerPickerPopup::setAnchorButton(QToolButton *anchor)
{
    m_anchorBtn = anchor;
}

void StickerPickerPopup::reloadAndRefresh()
{
    rebuildGrid();
}

QPixmap StickerPickerPopup::makeThumbnail(const QString &path, const int px)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (reader.supportsAnimation()) {
        reader.jumpToImage(0);
    }
    QImage img = reader.read();
    if (img.isNull()) {
        return {};
    }
    return QPixmap::fromImage(img.scaled(px, px, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

void StickerPickerPopup::rebuildGrid()
{
    if (!m_grid) {
        return;
    }
    while (QLayoutItem *it = m_grid->takeAt(0)) {
        if (it->widget()) {
            delete it->widget();
        }
        delete it;
    }

    auto *addBtn = new QToolButton(m_gridHost);
    addBtn->setObjectName(QStringLiteral("lanStickerAddCell"));
    addBtn->setFixedSize(kThumb + 8, kThumb + 8);
    addBtn->setText(QStringLiteral("+"));
    addBtn->setToolTip(QStringLiteral("添加表情（gif/png/jpg/webp）"));
    addBtn->setCursor(Qt::PointingHandCursor);
    connect(addBtn, &QToolButton::clicked, this, &StickerPickerPopup::onAddClicked);
    m_grid->addWidget(addBtn, 0, 0);

    const QStringList paths = LocalStickers::listStickerPaths(m_email);
    for (int j = 0; j < paths.size(); ++j) {
        const QString path = paths.at(j);
        auto *cell = new QToolButton(m_gridHost);
        cell->setObjectName(QStringLiteral("lanStickerCell"));
        cell->setFixedSize(kThumb + 8, kThumb + 8);
        cell->setIconSize(QSize(kThumb, kThumb));
        cell->setToolTip(QFileInfo(path).fileName());
        cell->setCursor(Qt::PointingHandCursor);
        const QPixmap pm = makeThumbnail(path, kThumb);
        if (!pm.isNull()) {
            cell->setIcon(QIcon(pm));
        } else {
            cell->setText(QStringLiteral("?"));
        }
        connect(cell, &QToolButton::clicked, this, [this, path]() {
            emit stickerChosen(path);
            hide();
        });
        const int slot = j + 1;
        m_grid->addWidget(cell, slot / kCols, slot % kCols);
    }
}

void StickerPickerPopup::onAddClicked()
{
    if (m_email.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("表情"), QStringLiteral("请先登录后再添加表情。"));
        return;
    }
    // Popup 失焦会先被关掉，不能以本窗口为 QFileDialog 父窗口；延后到下一轮事件循环并用顶层主窗口作父。
    QWidget *dlgParent = nullptr;
    if (QWidget *pw = parentWidget()) {
        dlgParent = pw->window();
    }
    if (!dlgParent) {
        dlgParent = QApplication::activeWindow();
    }
    QPointer<StickerPickerPopup> guard(this);
    QTimer::singleShot(0, this, [this, dlgParent, guard]() {
        if (!guard) {
            return;
        }
        const QStringList files = QFileDialog::getOpenFileNames(
            dlgParent, QStringLiteral("选择表情图片"), QString(),
            QStringLiteral("图片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;所有文件 (*.*)"));
        if (!guard || files.isEmpty()) {
            return;
        }
        for (const QString &f : files) {
            QString err;
            QString stored;
            if (!LocalStickers::importStickerFile(m_email, f, &stored, &err)) {
                QMessageBox::warning(dlgParent ? dlgParent : this, QStringLiteral("添加失败"),
                                     QStringLiteral("%1\n%2").arg(f, err.isEmpty() ? QStringLiteral("未知错误") : err));
                break;
            }
        }
        if (guard) {
            rebuildGrid();
        }
    });
}

void StickerPickerPopup::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_anchorBtn) {
        return;
    }
    const QPoint topLeft = m_anchorBtn->mapToGlobal(QPoint(0, 0));
    const int h = height() > 0 ? height() : sizeHint().height();
    QPoint p(topLeft.x(), topLeft.y() - h - 4);
    if (p.y() < 0) {
        p.setY(topLeft.y() + m_anchorBtn->height() + 4);
    }
    if (QScreen *scr = QGuiApplication::screenAt(topLeft)) {
        const QRect ag = scr->availableGeometry();
        if (p.x() + width() > ag.right()) {
            p.setX(ag.right() - width() - 8);
        }
        if (p.x() < ag.left()) {
            p.setX(ag.left() + 8);
        }
        if (p.y() + height() > ag.bottom()) {
            p.setY(ag.bottom() - height() - 8);
        }
    }
    move(p);
}
