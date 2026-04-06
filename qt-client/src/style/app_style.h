#ifndef APP_STYLE_H
#define APP_STYLE_H

#include <QString>

/// 全局 QSS 片段（仿常见 PC 端 IM 登录页：浅色渐变 + 白卡片 + 主色按钮）。
namespace AppStyle {

QString loginWidgetStyle();

/// 主聊天窗口（左栏 + 会话列表 + 聊天区）QSS。
QString chatMainStyle();

} // namespace AppStyle

#endif // APP_STYLE_H
