#ifndef APP_STYLE_H
#define APP_STYLE_H

#include <QString>

class QPainter;
class QRect;

/// 全局 QSS 片段（仿常见 PC 端 IM 登录页：浅色渐变 + 白卡片 + 主色按钮）。
namespace AppStyle {

/// 登录/注册页全幅背景（与参考 LoginWindow::paintEvent 一致的斜向渐变）。
void paintAuthPageBackground(QPainter *p, const QRect &rect);

QString loginWidgetStyle();

/// 主聊天窗口（左栏 + 会话列表 + 聊天区）QSS。
QString chatMainStyle();

/// 个人资料对话框（浅色圆角卡片风）。
QString profileDialogStyle();

/// 「添加好友」搜索对话框。
QString addFriendSearchDialogStyle();

/// 联系人页（分组树：申请 / 好友）。
QString contactsWidgetStyle();

/// 「聊天记录」对话框。
QString chatHistoryDialogStyle();

} // namespace AppStyle

#endif // APP_STYLE_H
