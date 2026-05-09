#include "style/app_style.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QRect>
#include <QString>

namespace AppStyle {

void paintAuthPageBackground(QPainter *p, const QRect &rect)
{
    if (!p || rect.isEmpty()) {
        return;
    }
    p->setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient grad(0, 0, qreal(rect.width()), qreal(rect.height()));
    grad.setColorAt(0, QColor(0xfd, 0xde, 0xbd));
    grad.setColorAt(1, QColor(0xab, 0xd8, 0xdf));
    p->fillRect(rect, grad);
}

QString loginWidgetStyle()
{
    return QStringLiteral(
        R"QSS(
        LanLoginWidget,
        LanRegisterWidget {
            background: transparent;
        }
        #loginCard {
            background-color: #ffffff;
            border-radius: 14px;
            border: 1px solid rgba(0,0,0,0.06);
        }
        #loginTitle {
            font-size: 20px;
            font-weight: bold;
            color: #262626;
        }
        #loginSubtitle {
            font-size: 13px;
            color: #8c8c8c;
        }
        #loginErrorLabel {
            font-size: 12px;
            color: #ff4d4f;
        }
        #loginInput {
            border: 1px solid #d9d9d9;
            border-radius: 6px;
            padding: 8px 12px;
            font-size: 14px;
            background: #fafafa;
            min-height: 20px;
        }
        #loginInput:focus {
            border: 1px solid #12b7f5;
            background: #ffffff;
        }
        #loginPrimaryButton {
            background-color: #12b7f5;
            color: #ffffff;
            border: none;
            border-radius: 6px;
            font-size: 15px;
            font-weight: bold;
            padding: 10px;
            min-height: 20px;
        }
        #loginPrimaryButton:hover {
            background-color: #0ea8e0;
        }
        #loginPrimaryButton:pressed {
            background-color: #0c96c8;
        }
        #loginPrimaryButton:disabled {
            background-color: #91d5ff;
            color: #f5f5f5;
        }
        #loginTextButton {
            color: #12b7f5;
            border: none;
            background: transparent;
            font-size: 13px;
            padding: 4px;
        }
        #loginTextButton:hover {
            color: #0ea8e0;
            text-decoration: underline;
        }
        #loginSecondaryButton {
            color: #12b7f5;
            border: 1px solid #12b7f5;
            border-radius: 6px;
            background: #ffffff;
            font-size: 13px;
            padding: 4px 8px;
        }
        #loginSecondaryButton:hover {
            background: #e6f7ff;
        }
        #loginSecondaryButton:disabled {
            color: #bfbfbf;
            border-color: #d9d9d9;
            background: #f5f5f5;
        }
        QCheckBox#loginRemember {
            font-size: 12px;
            color: #595959;
        }
        QCheckBox#passwordVisibleCheck {
            font-size: 12px;
            color: #595959;
            spacing: 6px;
        }
        QLabel#loginAvatar {
            background: transparent;
        }
        /* 去掉点击/聚焦时的虚线焦点框（QAbstractButton 含 QPushButton、QCheckBox） */
        LanLoginWidget QAbstractButton:focus,
        LanRegisterWidget QAbstractButton:focus {
            outline: none;
        }
        )QSS");
}

QString chatMainStyle()
{
    return QStringLiteral(
        R"QSS(
        LanChatMainWidget {
            background-color: #e8eef2;
        }
        QWidget#lanIconRail {
            background-color: #dde8f0;
            /* 不与 QSplitter handle 叠双线：与会话区|聊天区一致，只由 lanMainSplitter 的 1px handle 画竖线 */
            border: none;
        }
        QToolButton#lanRailButton {
            background: transparent;
            border: none;
            border-radius: 8px;
            font-size: 18px;
            color: #595959;
        }
        QToolButton#lanRailButton:checked {
            background-color: #c5e8f7;
            color: #12b7f5;
        }
        QToolButton#lanRailButton:hover {
            background-color: rgba(18,183,245,0.12);
        }
        QWidget#lanSessionListPanel {
            background-color: #eef3f7;
            border: none;
        }
        QSplitter#lanMainSplitter::handle {
            background: #d5dee6;
        }
        QLineEdit#lanSearch {
            border: 1px solid #d9d9d9;
            border-radius: 8px;
            padding: 6px 10px;
            background: #ffffff;
            font-size: 13px;
        }
        QWidget#lanSearchFieldWrap {
            border: none;
            border-radius: 8px;
            background: #e4edf4;
            min-height: 32px;
            max-height: 32px;
        }
        QLabel#lanSearchIcon {
            background: transparent;
        }
        QLineEdit#lanSessionSearch {
            border: none;
            background: transparent;
            padding: 4px 0px;
            font-size: 13px;
            color: #4a5968;
            selection-background-color: #12b7f5;
            selection-color: #ffffff;
        }
        QLineEdit#lanSessionSearch:focus {
            border: none;
        }
        QListWidget#lanFriendList {
            border: none;
            background: transparent;
            outline: none;
        }
        QListWidget#lanFriendList::item {
            border: none;
            /* 左右无内边距，选中条才能横向铺满列表视口（类似 QQ 会话列表） */
            padding: 1px 0px;
        }
        QToolButton#lanPlusButton {
            border: none;
            border-radius: 8px;
            background: #e4edf4;
            min-width: 32px;
            max-width: 32px;
            min-height: 32px;
            max-height: 32px;
            padding: 0px;
        }
        /* 有关联 QMenu 时默认会画右下角小三角，与 QQ 一致只保留「+」图标 */
        QToolButton#lanPlusButton::menu-indicator {
            image: none;
            width: 0;
            height: 0;
        }
        QToolButton#lanPlusButton:hover {
            background: #d8e4ee;
        }
        QToolButton#lanPlusButton:pressed {
            background: #ccd9e6;
        }
        QWidget#lanCenterPanel {
            background-color: #f5f7fa;
        }
        QStackedWidget#lanRightStack {
            border: none;
            /* 与 lanCenterPanel 同色，避免透明底在切换页时先被擦除再绘制导致闪白/闪烁 */
            background-color: #f5f7fa;
        }
        QWidget#lanContactModePlaceholder {
            border: none;
            background-color: #f5f7fa;
        }
        QLabel#lanContactModePlaceholderIcon {
            background: transparent;
        }
        QWidget#lanChatHeaderBar {
            background-color: #fafbfc;
            border-bottom: 1px solid #e8e8e8;
        }
        QLabel#lanPeerTitle {
            font-size: 16px;
            font-weight: bold;
            color: #262626;
        }
        QLabel#lanPeerOnline { color: #52c41a; font-size: 12px; }
        QLabel#lanPeerOffline { color: #8c8c8c; font-size: 12px; }
        QScrollArea#lanMessageScroll {
            border: none;
            background: #f0f2f5;
        }
        QWidget#lanMessageContainer {
            background: #f0f2f5;
        }
        QFrame#bubbleOut {
            background-color: #12b7f5;
            border-radius: 10px;
        }
        QFrame#bubbleIn {
            background-color: #ffffff;
            border: 1px solid #e8e8e8;
            border-radius: 10px;
        }
        QLabel#bubbleTextOut { color: #ffffff; font-size: 14px; }
        QLabel#bubbleTextIn { color: #262626; font-size: 14px; }
        QLabel#bubbleMetaOut { color: rgba(255,255,255,0.85); font-size: 11px; }
        QLabel#bubbleMetaIn { color: #8c8c8c; font-size: 11px; }
        QWidget#lanInputWrap {
            background: #fafbfc;
            border: none;
        }
        QPlainTextEdit#lanMessageInput {
            border: 1px solid #e8e8e8;
            border-radius: 6px;
            background: #ffffff;
            font-size: 14px;
            padding: 6px;
        }
        QPushButton#lanSendButton {
            background-color: #12b7f5;
            color: #ffffff;
            border: none;
            border-radius: 6px;
            padding: 6px 16px;
            font-size: 13px;
        }
        QPushButton#lanSendButton:hover { background-color: #0ea8e0; }
        QLabel#lanEmptyMain { font-size: 15px; color: #8c8c8c; }
        QLabel#lanEmptySub { font-size: 13px; color: #bfbfbf; }
        QLabel#lanMsgTimeCapsule {
            background: rgba(0, 0, 0, 0.06);
            color: #8c8c8c;
            font-size: 12px;
            padding: 4px 12px;
            border-radius: 10px;
        }
        QLabel#sessionNameLabel { font-size: 14px; font-weight: bold; color: #262626; }
        QLabel#sessionPreviewLabel { font-size: 12px; color: #8c8c8c; }
        QLabel#sessionTimeLabel { font-size: 11px; color: #bfbfbf; }
        QLabel#sessionPresenceDot {
            background: transparent;
            min-width: 20px;
            min-height: 20px;
        }
        QToolButton#lanInputTool {
            border: none;
            padding: 4px;
            border-radius: 4px;
        }
        QToolButton#lanInputTool:hover { background: rgba(0,0,0,0.06); }
        LanChatMainWidget QAbstractButton:focus {
            outline: none;
        }
        )QSS");
}

QString profileDialogStyle()
{
    return QStringLiteral(
        R"QSS(
        ProfileDialog#ProfileDialog {
            background-color: #f2f7fb;
        }
        QLabel#profileDlgTitle {
            font-size: 17px;
            font-weight: bold;
            color: #262626;
        }
        QLabel#profileFieldLabel {
            color: #595959;
            font-size: 13px;
        }
        QLabel#profileCounter {
            color: #bfbfbf;
            font-size: 12px;
            min-width: 52px;
        }
        QLabel#profileAvatar {
            background: #e6f7ff;
            border-radius: 48px;
        }
        QLineEdit#profileReadOnlyField {
            background: #fafafa;
            border: 1px solid #e8e8e8;
            border-radius: 12px;
            padding: 10px 14px;
            font-size: 13px;
            color: #595959;
        }
        QLineEdit#profileEditField {
            background: #ffffff;
            border: 1px solid #e8e8e8;
            border-radius: 12px;
            padding: 10px 14px;
            font-size: 13px;
            color: #262626;
        }
        QLineEdit#profileEditField:focus {
            border: 1px solid #12b7f5;
        }
        QPlainTextEdit#profileBioEdit {
            background: #ffffff;
            border: 1px solid #e8e8e8;
            border-radius: 12px;
            padding: 10px 14px;
            font-size: 13px;
            color: #262626;
        }
        QPlainTextEdit#profileBioEdit:focus {
            border: 1px solid #12b7f5;
        }
        QPushButton#profilePrimaryBtn {
            background-color: #12b7f5;
            color: #ffffff;
            border: none;
            border-radius: 20px;
            padding: 8px 28px;
            font-size: 14px;
            min-width: 88px;
        }
        QPushButton#profilePrimaryBtn:hover { background-color: #0ea8e0; }
        QPushButton#profileSecondaryBtn {
            background: #ffffff;
            color: #262626;
            border: 1px solid #d9d9d9;
            border-radius: 20px;
            padding: 8px 20px;
            font-size: 13px;
        }
        QPushButton#profileSecondaryBtn:hover { background: #fafafa; }
        QPushButton#profileLinkBtn {
            color: #12b7f5;
            border: none;
            background: transparent;
            font-size: 13px;
        }
        QPushButton#profileLinkBtn:hover { text-decoration: underline; }
        ProfileDialog QAbstractButton:focus {
            outline: none;
        }
        )QSS");
}

QString addFriendSearchDialogStyle()
{
    return QStringLiteral(
        R"QSS(
        AddFriendSearchDialog {
            background-color: #ffffff;
        }
        QLineEdit#addFriendSearchInput {
            border: 1px solid #e0e0e0;
            border-radius: 8px;
            padding: 8px 12px;
            font-size: 13px;
            background: #fafafa;
            min-height: 20px;
        }
        QLineEdit#addFriendSearchInput:focus {
            border: 1px solid #12b7f5;
            background: #ffffff;
        }
        QPushButton#addFriendSearchButton {
            background-color: #12b7f5;
            color: #ffffff;
            border: none;
            border-radius: 8px;
            font-size: 13px;
            padding: 8px 20px;
            min-height: 20px;
        }
        QPushButton#addFriendSearchButton:hover { background-color: #0ea8e0; }
        QListWidget#addFriendSearchList {
            border: 1px solid #eeeeee;
            border-radius: 8px;
            background: #fafafa;
            outline: none;
        }
        QListWidget#addFriendSearchList::item {
            border: none;
            padding: 2px 4px;
        }
        QLabel#addFriendSearchStatus {
            color: #8c8c8c;
            font-size: 12px;
        }
        QPushButton#addFriendResultBtn {
            background: #ffffff;
            color: #12b7f5;
            border: 1px solid #12b7f5;
            border-radius: 6px;
            font-size: 12px;
            padding: 4px 8px;
        }
        QPushButton#addFriendResultBtn:hover {
            background: #e6f7ff;
        }
        AddFriendSearchDialog QAbstractButton:focus {
            outline: none;
        }
        )QSS");
}

QString contactsWidgetStyle()
{
    return QStringLiteral(
        R"QSS(
        LanContactsWidget {
            background-color: #eef3f7;
        }
        QTreeWidget#lanContactsTree {
            border: none;
            background-color: #eef3f7;
            outline: none;
        }
        QTreeWidget#lanContactsTree::item {
            border: none;
            padding: 2px 0;
        }
        QTreeWidget#lanContactsTree::item:hover {
            background: rgba(18, 183, 245, 0.08);
        }
        QTreeWidget#lanContactsTree::item:selected {
            background: rgba(18, 183, 245, 0.16);
        }
        )QSS");
}

QString chatHistoryDialogStyle()
{
    return QStringLiteral(
        R"QSS(
        ChatHistoryDialog {
            background-color: #f0f4f8;
        }
        QLineEdit#chatHistorySearchInput {
            border: 1px solid #b8d4f0;
            border-radius: 8px;
            padding: 8px 12px;
            font-size: 13px;
            background: #ffffff;
            min-height: 20px;
        }
        QLineEdit#chatHistorySearchInput:focus {
            border: 1px solid #12b7f5;
        }
        QTabBar#chatHistoryTabBar::tab {
            min-width: 56px;
            padding: 6px 8px;
            font-size: 12px;
            color: #595959;
            background: transparent;
            border: none;
            border-bottom: 2px solid transparent;
        }
        QTabBar#chatHistoryTabBar::tab:selected {
            color: #12b7f5;
            font-weight: bold;
            border-bottom: 2px solid #12b7f5;
        }
        QTabBar#chatHistoryTabBar::tab:hover:!selected {
            color: #262626;
        }
        QPushButton#chatHistoryFilterHint {
            color: #8c8c8c;
            font-size: 12px;
            padding: 4px 8px;
        }
        QListWidget#chatHistoryList {
            border: 1px solid #e8eef2;
            border-radius: 8px;
            background: #fafcfd;
            outline: none;
        }
        QListWidget#chatHistoryList::item {
            border: none;
            border-bottom: 1px solid #e8eef2;
            padding: 0;
        }
        QLabel#chatHistoryDateRow {
            color: #8c8c8c;
            font-size: 12px;
            background: transparent;
        }
        QLabel#chatHistoryLineText {
            font-size: 13px;
            color: #262626;
        }
        QLabel#chatHistoryStatus {
            color: #8c8c8c;
            font-size: 12px;
        }
        ChatHistoryDialog QAbstractButton:focus {
            outline: none;
        }
        )QSS");
}

} // namespace AppStyle
