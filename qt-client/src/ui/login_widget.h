#ifndef LOGIN_WIDGET_H
#define LOGIN_WIDGET_H

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QCheckBox;

/// 登录页（布局与配色参考 PC 端 QQ 类 IM：居中卡片 + 邮箱/密码）。
class LoginWidget : public QWidget
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

signals:
    void loginRequested(const QString &email, const QString &password);
    void registerRequested();

private slots:
    void onLoginClicked();
    void onRegisterClicked();

private:
    void buildUi();

    QLineEdit *m_serverHost = nullptr;
    QLineEdit *m_serverPort = nullptr;
    QLineEdit *m_email = nullptr;
    QLineEdit *m_password = nullptr;
    QCheckBox *m_remember = nullptr;
    QLabel *m_errorLabel = nullptr;
};

#endif // LOGIN_WIDGET_H
