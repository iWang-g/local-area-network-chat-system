#include "ui/chat_main_widget.h"

#include "style/app_style.h"
#include "utils/avatar_utils.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTime>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QPlainTextEdit>

ChatMainWidget::ChatMainWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("LanChatMainWidget"));
    setStyleSheet(AppStyle::chatMainStyle());

    QPixmap raw(64, 64);
    raw.fill(QColor(QStringLiteral("#12b7f5")));
    m_avatarPix = makeCircularAvatar(raw, 36);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    auto *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(buildIconRail());
    splitter->addWidget(buildSessionPanel());
    splitter->addWidget(buildChatPanel());
    splitter->setSizes({56, 280, 700});
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 1);
    outer->addWidget(splitter, 1);

    populateSessionList();

    auto *sc = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), this);
    connect(sc, &QShortcut::activated, this, &ChatMainWidget::onSendClicked);
}

void ChatMainWidget::setUserEmail(const QString &email)
{
    m_userEmail = email;
}

QWidget *ChatMainWidget::buildIconRail()
{
    auto *rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("lanIconRail"));
    rail->setFixedWidth(56);
    auto *v = new QVBoxLayout(rail);
    v->setContentsMargins(6, 20, 6, 12);
    v->setSpacing(6);

    auto *group = new QButtonGroup(rail);
    const QString railTxt[] = {QStringLiteral("💬"), QStringLiteral("👥")};
    const QString railTips[] = {QStringLiteral("会话"), QStringLiteral("联系人")};
    for (int i = 0; i < 2; ++i) {
        auto *tb = new QToolButton(rail);
        tb->setText(railTxt[i]);
        tb->setToolTip(railTips[i]);
        tb->setCheckable(true);
        tb->setObjectName(QStringLiteral("lanRailButton"));
        tb->setMinimumSize(44, 44);
        tb->setCursor(Qt::PointingHandCursor);
        group->addButton(tb);
        v->addWidget(tb, 0, Qt::AlignHCenter);
        if (i == 0) {
            tb->setChecked(true);
        }
    }
    group->setExclusive(true);

    v->addStretch(1);

    auto *menuBtn = new QToolButton(rail);
    menuBtn->setText(QStringLiteral("≡"));
    menuBtn->setObjectName(QStringLiteral("lanRailButton"));
    menuBtn->setMinimumSize(44, 44);
    menuBtn->setCursor(Qt::PointingHandCursor);
    menuBtn->setToolTip(QStringLiteral("菜单"));
    auto *menu = new QMenu(menuBtn);
    menu->addAction(QStringLiteral("退出登录"), this, &ChatMainWidget::logoutRequested);
    menuBtn->setMenu(menu);
    menuBtn->setPopupMode(QToolButton::InstantPopup);
    v->addWidget(menuBtn, 0, Qt::AlignHCenter);

    return rail;
}

QWidget *ChatMainWidget::createSessionRowWidget(const QString &name, const QString &preview,
                                                const QString &time, bool online)
{
    auto *wrap = new QWidget();
    wrap->setObjectName(QStringLiteral("sessionRowWrap"));

    auto *h = new QHBoxLayout(wrap);
    h->setContentsMargins(10, 8, 10, 8);
    h->setSpacing(10);

    auto *av = new QLabel(wrap);
    av->setFixedSize(44, 44);
    av->setObjectName(QStringLiteral("sessionAvatar"));
    QPixmap dot(64, 64);
    dot.fill(QColor(QStringLiteral("#bae7ff")));
    av->setPixmap(makeCircularAvatar(dot, 44));

    h->addWidget(av, 0, Qt::AlignTop);

    auto *textCol = new QVBoxLayout();
    textCol->setSpacing(4);
    auto *nameLb = new QLabel(name, wrap);
    nameLb->setObjectName(QStringLiteral("sessionNameLabel"));
    auto *prevLb = new QLabel(preview, wrap);
    prevLb->setObjectName(QStringLiteral("sessionPreviewLabel"));
    textCol->addWidget(nameLb);
    textCol->addWidget(prevLb);
    h->addLayout(textCol, 1);

    auto *rightCol = new QVBoxLayout();
    rightCol->setSpacing(6);
    auto *timeLb = new QLabel(time, wrap);
    timeLb->setObjectName(QStringLiteral("sessionTimeLabel"));
    timeLb->setAlignment(Qt::AlignRight | Qt::AlignTop);
    auto *dotLb = new QLabel(online ? QStringLiteral("●") : QStringLiteral("○"), wrap);
    dotLb->setObjectName(online ? QStringLiteral("sessionOnlineDot") : QStringLiteral("sessionOfflineDot"));
    dotLb->setAlignment(Qt::AlignRight);
    rightCol->addWidget(timeLb);
    rightCol->addWidget(dotLb);
    h->addLayout(rightCol);

    return wrap;
}

void ChatMainWidget::populateSessionList()
{
    m_friendList->clear();
    const struct
    {
        QString name;
        QString preview;
        QString time;
        bool online;
    } rows[] = {{QStringLiteral("张三"), QStringLiteral("下午开会吗？"), QStringLiteral("20:28"), true},
                {QStringLiteral("李四"), QStringLiteral("文件已发"), QStringLiteral("昨天"), true},
                {QStringLiteral("王五"), QStringLiteral("明天见"), QStringLiteral("星期三"), false},
                {QStringLiteral("赵六"), QStringLiteral("收到"), QStringLiteral("15:10"), true}};

    for (const auto &r : rows) {
        auto *item = new QListWidgetItem(m_friendList);
        QWidget *row = createSessionRowWidget(r.name, r.preview, r.time, r.online);
        row->setFixedHeight(64);
        item->setSizeHint(QSize(0, 64));
        item->setData(Qt::UserRole, r.name);
        item->setData(Qt::UserRole + 1, r.online);
        m_friendList->addItem(item);
        m_friendList->setItemWidget(item, row);
    }
    m_friendList->setCurrentRow(0);
}

QWidget *ChatMainWidget::buildSessionPanel()
{
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("lanSessionListPanel"));
    panel->setMinimumWidth(240);
    panel->setMaximumWidth(340);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(12, 12, 12, 8);
    layout->setSpacing(10);

    auto *searchRow = new QWidget(panel);
    auto *searchLay = new QHBoxLayout(searchRow);
    searchLay->setContentsMargins(0, 0, 0, 0);
    searchLay->setSpacing(8);

    auto *searchEdit = new QLineEdit(searchRow);
    searchEdit->setObjectName(QStringLiteral("lanSearch"));
    searchEdit->setPlaceholderText(QStringLiteral("搜索"));
    searchLay->addWidget(searchEdit, 1);

    auto *plusBtn = new QToolButton(searchRow);
    plusBtn->setObjectName(QStringLiteral("lanPlusButton"));
    plusBtn->setText(QStringLiteral("+"));
    plusBtn->setCursor(Qt::PointingHandCursor);
    plusBtn->setToolTip(QStringLiteral("添加会话（演示）"));
    searchLay->addWidget(plusBtn);

    layout->addWidget(searchRow);

    m_friendList = new QListWidget(panel);
    m_friendList->setObjectName(QStringLiteral("lanFriendList"));
    m_friendList->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_friendList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layout->addWidget(m_friendList, 1);

    connect(m_friendList, &QListWidget::itemSelectionChanged, this, &ChatMainWidget::onFriendSelectionChanged);

    return panel;
}

QWidget *ChatMainWidget::buildChatPanel()
{
    auto *panel = new QWidget(this);
    panel->setObjectName(QStringLiteral("lanCenterPanel"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_centerStack = new QStackedWidget(panel);

    m_centerEmpty = new QWidget(panel);
    auto *emptyLay = new QVBoxLayout(m_centerEmpty);
    emptyLay->setAlignment(Qt::AlignCenter);
    emptyLay->setSpacing(12);
    auto *emptyMain = new QLabel(QStringLiteral("选择一个会话开始聊天"), m_centerEmpty);
    emptyMain->setObjectName(QStringLiteral("lanEmptyMain"));
    emptyMain->setAlignment(Qt::AlignCenter);
    auto *emptySub = new QLabel(QStringLiteral("在左侧列表中选择好友"), m_centerEmpty);
    emptySub->setObjectName(QStringLiteral("lanEmptySub"));
    emptySub->setAlignment(Qt::AlignCenter);
    emptyLay->addWidget(emptyMain);
    emptyLay->addWidget(emptySub);
    m_centerStack->addWidget(m_centerEmpty);

    m_chatArea = new QWidget(panel);
    auto *chatLay = new QVBoxLayout(m_chatArea);
    chatLay->setContentsMargins(0, 0, 0, 0);
    chatLay->setSpacing(0);

    auto *headerBar = new QWidget(m_chatArea);
    headerBar->setObjectName(QStringLiteral("lanChatHeaderBar"));
    headerBar->setFixedHeight(56);
    auto *headerLay = new QHBoxLayout(headerBar);
    headerLay->setContentsMargins(16, 0, 16, 0);
    headerLay->setSpacing(0);

    auto *titleRow = new QHBoxLayout();
    titleRow->setSpacing(10);
    titleRow->setContentsMargins(0, 0, 0, 0);
    m_peerTitleLabel = new QLabel(headerBar);
    m_peerTitleLabel->setObjectName(QStringLiteral("lanPeerTitle"));
    m_onlineDotLabel = new QLabel(headerBar);
    m_onlineDotLabel->setObjectName(QStringLiteral("lanPeerOnline"));
    titleRow->addWidget(m_peerTitleLabel, 0, Qt::AlignVCenter);
    titleRow->addWidget(m_onlineDotLabel, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    headerLay->addLayout(titleRow, 1);

    chatLay->addWidget(headerBar);

    m_messageScroll = new QScrollArea(m_chatArea);
    m_messageScroll->setObjectName(QStringLiteral("lanMessageScroll"));
    m_messageScroll->setWidgetResizable(true);
    m_messageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_messageContainer = new QWidget();
    m_messageContainer->setObjectName(QStringLiteral("lanMessageContainer"));
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(16, 16, 16, 16);
    m_messageLayout->setSpacing(10);
    m_messageLayout->addStretch(1);
    m_messageScroll->setWidget(m_messageContainer);
    chatLay->addWidget(m_messageScroll, 1);

    auto *divider = new QFrame(m_chatArea);
    divider->setFrameShape(QFrame::HLine);
    divider->setObjectName(QStringLiteral("inputDivider"));
    chatLay->addWidget(divider);

    auto *inputWrap = new QWidget(m_chatArea);
    inputWrap->setObjectName(QStringLiteral("lanInputWrap"));
    auto *inputLay = new QVBoxLayout(inputWrap);
    inputLay->setContentsMargins(12, 8, 12, 10);
    inputLay->setSpacing(6);

    auto *toolRow = new QHBoxLayout();
    toolRow->setSpacing(4);
    QStyle *sty = qApp->style();
    const QStyle::StandardPixmap pix[] = {QStyle::SP_DialogYesButton, QStyle::SP_DesktopIcon,
                                          QStyle::SP_FileIcon, QStyle::SP_DialogOpenButton};
    const QString toolTips[] = {QStringLiteral("表情"), QStringLiteral("截图"), QStringLiteral("附件"),
                                QStringLiteral("图片")};
    for (int i = 0; i < 4; ++i) {
        auto *t = new QToolButton(inputWrap);
        t->setIcon(sty->standardIcon(pix[i]));
        t->setIconSize(QSize(20, 20));
        t->setToolTip(toolTips[i]);
        t->setObjectName(QStringLiteral("lanInputTool"));
        t->setCursor(Qt::PointingHandCursor);
        t->setAutoRaise(true);
        toolRow->addWidget(t);
    }
    toolRow->addStretch(1);
    inputLay->addLayout(toolRow);

    m_inputEdit = new QPlainTextEdit(inputWrap);
    m_inputEdit->setObjectName(QStringLiteral("lanMessageInput"));
    m_inputEdit->setPlaceholderText(QStringLiteral("输入消息，Ctrl+Enter 发送"));
    m_inputEdit->setMaximumHeight(120);
    inputLay->addWidget(m_inputEdit);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    auto *sendBtn = new QPushButton(QStringLiteral("发送"), inputWrap);
    sendBtn->setObjectName(QStringLiteral("lanSendButton"));
    sendBtn->setCursor(Qt::PointingHandCursor);
    sendBtn->setFixedWidth(88);
    connect(sendBtn, &QPushButton::clicked, this, &ChatMainWidget::onSendClicked);
    btnRow->addWidget(sendBtn);
    inputLay->addLayout(btnRow);

    chatLay->addWidget(inputWrap);

    m_centerStack->addWidget(m_chatArea);
    m_centerStack->setCurrentWidget(m_chatArea);

    layout->addWidget(m_centerStack);
    return panel;
}

QWidget *ChatMainWidget::createBubble(const QString &text, bool isOutgoing)
{
    auto *row = new QWidget();
    auto *rowLay = new QHBoxLayout(row);
    rowLay->setContentsMargins(4, 6, 4, 6);
    rowLay->setSpacing(10);

    auto *av = new QLabel(row);
    av->setFixedSize(36, 36);
    if (!m_avatarPix.isNull()) {
        av->setPixmap(m_avatarPix);
    }

    auto *bubble = new QFrame(row);
    bubble->setObjectName(isOutgoing ? QStringLiteral("bubbleOut") : QStringLiteral("bubbleIn"));
    auto *bubbleLay = new QVBoxLayout(bubble);
    bubbleLay->setContentsMargins(12, 10, 12, 8);
    bubbleLay->setSpacing(4);
    auto *content = new QLabel(text, bubble);
    content->setWordWrap(true);
    content->setTextInteractionFlags(Qt::TextSelectableByMouse);
    content->setObjectName(isOutgoing ? QStringLiteral("bubbleTextOut") : QStringLiteral("bubbleTextIn"));
    bubbleLay->addWidget(content);
    auto *meta = new QLabel(QTime::currentTime().toString(QStringLiteral("HH:mm")), bubble);
    meta->setObjectName(isOutgoing ? QStringLiteral("bubbleMetaOut") : QStringLiteral("bubbleMetaIn"));
    meta->setAlignment(isOutgoing ? Qt::AlignRight : Qt::AlignLeft);
    bubbleLay->addWidget(meta);
    bubble->setMaximumWidth(440);

    if (!isOutgoing) {
        rowLay->addWidget(av, 0, Qt::AlignTop);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addStretch(1);
    } else {
        rowLay->addStretch(1);
        rowLay->addWidget(bubble, 0, Qt::AlignTop);
        rowLay->addWidget(av, 0, Qt::AlignTop);
    }
    return row;
}

void ChatMainWidget::updateChatHeader(const QString &name, bool online)
{
    m_peerTitleLabel->setText(name);
    m_onlineDotLabel->setText(online ? QStringLiteral("● 在线") : QStringLiteral("○ 离线"));
    m_onlineDotLabel->setObjectName(online ? QStringLiteral("lanPeerOnline") : QStringLiteral("lanPeerOffline"));
    m_onlineDotLabel->style()->unpolish(m_onlineDotLabel);
    m_onlineDotLabel->style()->polish(m_onlineDotLabel);
}

void ChatMainWidget::clearMessageList()
{
    while (m_messageLayout->count() > 1) {
        QLayoutItem *li = m_messageLayout->takeAt(0);
        if (li->widget() != nullptr) {
            delete li->widget();
        }
        delete li;
    }
}

void ChatMainWidget::reloadDemoThreadForPeer(const QString &name)
{
    clearMessageList();
    auto insertBeforeStretch = [this](QWidget *w) {
        const int idx = m_messageLayout->count() - 1;
        m_messageLayout->insertWidget(idx, w);
    };
    insertBeforeStretch(createBubble(QStringLiteral("这是与 %1 的演示会话（消息收发待对接服务端）").arg(name), false));
    insertBeforeStretch(createBubble(QStringLiteral("好的，知道了。"), true));
    insertBeforeStretch(createBubble(QStringLiteral("下午若方便再语音。"), false));
    QTimer::singleShot(0, this, &ChatMainWidget::scrollMessagesToBottom);
}

void ChatMainWidget::scrollMessagesToBottom()
{
    if (!m_messageScroll) {
        return;
    }
    QScrollBar *sb = m_messageScroll->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void ChatMainWidget::refreshSessionRowHighlight()
{
    for (int i = 0; i < m_friendList->count(); ++i) {
        QListWidgetItem *it = m_friendList->item(i);
        QWidget *w = m_friendList->itemWidget(it);
        if (!w) {
            continue;
        }
        const bool sel = it->isSelected();
        w->setStyleSheet(sel ? QStringLiteral("QWidget#sessionRowWrap { background: #e3eef9; border-radius: 10px; }")
                              : QStringLiteral("QWidget#sessionRowWrap { background: transparent; border-radius: 10px; }"));
    }
}

void ChatMainWidget::onFriendSelectionChanged()
{
    refreshSessionRowHighlight();
    QListWidgetItem *item = m_friendList->currentItem();
    if (!item) {
        m_centerStack->setCurrentWidget(m_centerEmpty);
        return;
    }
    const QString name = item->data(Qt::UserRole).toString();
    const bool online = item->data(Qt::UserRole + 1).toBool();
    const QString title = name.isEmpty() ? item->text() : name;
    updateChatHeader(title, online);
    m_centerStack->setCurrentWidget(m_chatArea);
    reloadDemoThreadForPeer(title);
}

void ChatMainWidget::onSendClicked()
{
    if (!m_inputEdit) {
        return;
    }
    const QString t = m_inputEdit->toPlainText().trimmed();
    if (t.isEmpty()) {
        return;
    }
    m_inputEdit->clear();
    const int idx = m_messageLayout->count() - 1;
    m_messageLayout->insertWidget(idx, createBubble(t, true));
    scrollMessagesToBottom();
}
