#ifndef REGISTER_WIDGET_H
#define REGISTER_WIDGET_H

#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;

/// 注册页（与 LoginWidget 同套视觉：居中卡片 + 服务器 + 邮箱/密码/确认/昵称）。
class RegisterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit RegisterWidget(QWidget *parent = nullptr);

    void clearError();
    void showError(const QString &message);

    void setServerFields(const QString &host, const QString &portText);
    QString serverHost() const;
    quint16 serverPort() const;

signals:
    void registerSubmitted(const QString &email, const QString &password, const QString &nickname);
    void backToLoginRequested();

private slots:
    void onSubmitClicked();
    void onBackClicked();

private:
    void buildUi();

    QLineEdit *m_serverHost = nullptr;
    QLineEdit *m_serverPort = nullptr;
    QLineEdit *m_email = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_password2 = nullptr;
    QLineEdit *m_nickname = nullptr;
    QLabel *m_errorLabel = nullptr;
};

#endif // REGISTER_WIDGET_H
