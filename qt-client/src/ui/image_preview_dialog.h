#ifndef IMAGE_PREVIEW_DIALOG_H
#define IMAGE_PREVIEW_DIALOG_H

#include <QString>

class QWidget;

/// 聊天图片气泡左键放大预览（模态对话框）。
class ImagePreviewDialog
{
public:
    static void showForPath(const QString &imagePath, QWidget *parent);
};

#endif
