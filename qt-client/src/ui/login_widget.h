#ifndef LOGIN_WIDGET_H
#define LOGIN_WIDGET_H

#include <QDialog>

class QEvent;
class QLabel;
class QLineEdit;
class QKeyEvent;
class QPaintEvent;
class QPushButton;
class QCheckBox;
class QPushButton;

/// 登录页（布局与配色参考 PC 端 QQ 类 IM：居中卡片 + 邮箱或用户名/密码）。
/// 使用 QDialog 基类与参考 LoginWindow 一致；嵌入 MainWindow 的 QStackedWidget 时不单独成窗。
class LoginWidget : public QDialog
{
    Q_OBJECT

public:
    explicit LoginWidget(QWidget *parent = nullptr);

    void clearError();
    void showError(const QString &message);

    QString serverHost() const;
    /// 解析失败时返回默认 28888。
    quint16 serverPort() const;
    QString emailInput() const;
    QString passwordInput() const;
    void setServerFields(const QString &host, const QString &portText);

    /// 按邮箱加载本机缓存头像，刷新卡片顶部圆形区域（无邮箱或无头像则显示默认占位）。
    void refreshAvatarFromLocalProfile(const QString &emailForKey);

signals:
    /// `loginMode`：`"email"` 使用邮箱登录；`"username"` 使用注册时用户名登录（老账号仅邮箱）。
    void loginRequested(const QString &account, const QString &password, const QString &loginMode);
    void registerRequested();

private slots:
    void onLoginClicked();
    void onRegisterClicked();
    void onPasswordVisibleToggled(bool checked);
    void onEmailTextChanged(const QString &text);
    void onToggleLoginMode();

private:
    void buildUi();
    void applyLoginModeUi();
    void updateWelcomeTitle();
    static bool isValidUsername(const QString &text);
    bool cycleFieldFocusWithArrow(QLineEdit *current, int key);
    void applyCtrlHToPasswordField();

    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    QLineEdit *m_serverHost = nullptr;
    QLineEdit *m_serverPort = nullptr;
    QLineEdit *m_email = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_remember = nullptr;
    QCheckBox *m_passwordVisibleCheck = nullptr;
    QLabel *m_avatarLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_subtitleLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QPushButton *m_toggleLoginModeBtn = nullptr;
    bool m_loginByEmail = true;
};

#endif // LOGIN_WIDGET_H
