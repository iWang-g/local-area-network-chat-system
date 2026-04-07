#include "style/app_style.h"

#include <QString>

namespace AppStyle {

QString loginWidgetStyle()
{
    return QStringLiteral(
        R"QSS(
        LanLoginWidget,
        LanRegisterWidget {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 #c8e8f5, stop:0.45 #e8f4fc, stop:1 #b8dce8);
        }
        #loginCard {
            background-color: #ffffff;
            border-radius: 12px;
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
            border-right: 1px solid #cfd8e0;
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
            border-right: 1px solid #d5dee6;
        }
        QLineEdit#lanSearch {
            border: 1px solid #d9d9d9;
            border-radius: 16px;
            padding: 6px 12px;
            background: #ffffff;
            font-size: 13px;
        }
        QListWidget#lanFriendList {
            border: none;
            background: transparent;
            outline: none;
        }
        QListWidget#lanFriendList::item {
            border: none;
            padding: 2px 4px;
        }
        QToolButton#lanPlusButton {
            border: 1px solid #d9d9d9;
            border-radius: 14px;
            background: #ffffff;
            font-weight: bold;
            color: #595959;
            min-width: 28px;
            max-width: 28px;
            min-height: 28px;
            max-height: 28px;
        }
        QWidget#lanCenterPanel {
            background-color: #f5f7fa;
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
            border-top: 1px solid #e8e8e8;
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
        QLabel#sessionNameLabel { font-size: 14px; font-weight: bold; color: #262626; }
        QLabel#sessionPreviewLabel { font-size: 12px; color: #8c8c8c; }
        QLabel#sessionTimeLabel { font-size: 11px; color: #bfbfbf; }
        QLabel#sessionOnlineDot { color: #52c41a; font-size: 12px; }
        QLabel#sessionOfflineDot { color: #d9d9d9; font-size: 12px; }
        QToolButton#lanInputTool {
            border: none;
            padding: 4px;
            border-radius: 4px;
        }
        QToolButton#lanInputTool:hover { background: rgba(0,0,0,0.06); }
        )QSS");
}

} // namespace AppStyle
