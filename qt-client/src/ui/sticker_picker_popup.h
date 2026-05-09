#ifndef STICKER_PICKER_POPUP_H
#define STICKER_PICKER_POPUP_H

#include <QPointer>
#include <QWidget>

class QGridLayout;
class QScrollArea;
class QShowEvent;
class QToolButton;

/// 输入栏旁弹出的自定义表情面板：首格添加，其余为缩略图，点击发出 `stickerChosen`。
class StickerPickerPopup : public QWidget
{
    Q_OBJECT

public:
    explicit StickerPickerPopup(QWidget *parent = nullptr);

    void setUserEmail(const QString &email);
    void setAnchorButton(QToolButton *anchor);
    void reloadAndRefresh();

signals:
    void stickerChosen(const QString &absoluteFilePath);

private slots:
    void onAddClicked();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void rebuildGrid();
    static QPixmap makeThumbnail(const QString &path, int px);

    QString m_email;
    QPointer<QToolButton> m_anchorBtn;
    QScrollArea *m_scroll = nullptr;
    QWidget *m_gridHost = nullptr;
    QGridLayout *m_grid = nullptr;
};

#endif // STICKER_PICKER_POPUP_H
