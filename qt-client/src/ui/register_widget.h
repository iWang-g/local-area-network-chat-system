#ifndef REGISTER_WIDGET_H
#define REGISTER_WIDGET_H

#include <QDialog>

class QEvent;
class QLabel;
class QLineEdit;
class QKeyEvent;
class QPaintEvent;
class QPushButton;
class QTimer;
class QCheckBox;

/// 注册页（与 LoginWidget 同套视觉：居中卡片 + 服务器 + 邮箱/验证码/密码等）。
class RegisterWidget : public QDialog
{
    Q_OBJECT

public:
    explicit RegisterWidget(QWidget *parent = nullptr);

    void clearError();
    void showError(const QString &message);
    void showCodeSentHint();
    void clearCodeSentHint();

    void setServerFields(const QString &host, const QString &portText);
    QString serverHost() const;
    quint16 serverPort() const;
    /// 当前邮箱输入（用于请求验证码）。
    QString registerEmailInput() const;

    void startResendCooldown(int seconds);

signals:
    void registerSubmitted(const QString &email, const QString &password, const QString &username,
                           const QString &emailCode);
    /// 用户点击「获取验证码」（需已由 MainWindow 连上并完成 hello）。
    void requestEmailCode(const QString &email);
    void backToLoginRequested();

private slots:
    void onSubmitClicked();
    void onBackClicked();
    void onSendCodeClicked();
    void onCooldownTick();
    void onPwdVisibleToggled(bool checked);
    void onPwd2VisibleToggled(bool checked);
    void onEmailTextChanged(const QString &text);
    static bool isValidUsername(const QString &text);

private:
    void buildUi();
    void updateTitle();
    bool cycleFieldFocusWithArrow(QLineEdit *current, int key);
    void applyCtrlHToFocusedPassword(QLineEdit *field);

    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

    QLineEdit *m_serverHost = nullptr;
    QLineEdit *m_serverPort = nullptr;
    QLineEdit *m_email = nullptr;
    QLineEdit *m_emailCode = nullptr;
    QPushButton *m_sendCodeBtn = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_password2 = nullptr;
    QLineEdit *m_username = nullptr;
    QCheckBox *m_pwdVisibleCheck = nullptr;
    QCheckBox *m_pwd2VisibleCheck = nullptr;
    QLabel *m_titleLabel = nullptr;
    QLabel *m_errorLabel = nullptr;
    QLabel *m_codeHintLabel = nullptr;
    QTimer *m_cooldownTimer = nullptr;
    int m_cooldownLeft = 0;
};

#endif // REGISTER_WIDGET_H
