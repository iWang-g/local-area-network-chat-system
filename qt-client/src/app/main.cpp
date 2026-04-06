#include "ui/mainwindow.h"
#include "utils/logger.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QObject>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setApplicationName(QStringLiteral("局域网聊天"));
    a.setOrganizationName(QStringLiteral("LANChat"));

    Logger::init();
    QObject::connect(&a, &QCoreApplication::aboutToQuit, []() { Logger::shutdown(); });

    QFont font(QStringLiteral("Microsoft YaHei UI"));
    if (!font.exactMatch()) {
        font = QFont();
        font.setPointSize(10);
    }
    a.setFont(font);

    MainWindow w;
    w.show();
    return a.exec();
}
