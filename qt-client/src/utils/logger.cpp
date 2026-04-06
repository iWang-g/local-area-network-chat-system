#include "utils/logger.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QMutexLocker>
#include <QTextStream>
#include <QtGlobal>
#include <cstdio>

QFile Logger::s_logFile;
QMutex Logger::s_mutex;
bool Logger::s_initialized = false;

void Logger::messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QMutexLocker locker(&s_mutex);

    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz"));
    const char *levels[] = {"DEBUG", "INFO ", "WARN ", "ERROR", "FATAL"};
    int idx = 0;
    switch (type) {
    case QtDebugMsg:
        idx = 0;
        break;
    case QtInfoMsg:
        idx = 1;
        break;
    case QtWarningMsg:
        idx = 2;
        break;
    case QtCriticalMsg:
        idx = 3;
        break;
    case QtFatalMsg:
        idx = 4;
        break;
    }

    QString formatted = QStringLiteral("[%1] [%2] %3").arg(timestamp, QString::fromUtf8(levels[idx]), msg);
    if (context.file != nullptr && context.line > 0) {
        QString file = QString::fromUtf8(context.file);
        const int lastSlash = file.lastIndexOf(QLatin1Char('/'));
        const int lastBackslash = file.lastIndexOf(QLatin1Char('\\'));
        const int pos = qMax(lastSlash, lastBackslash);
        if (pos >= 0) {
            file = file.mid(pos + 1);
        }
        formatted += QStringLiteral(" (%1:%2)").arg(file).arg(context.line);
    }

    fprintf(stderr, "%s\n", formatted.toLocal8Bit().constData());
    fflush(stderr);

    if (s_logFile.isOpen()) {
        QTextStream stream(&s_logFile);
        stream << formatted << QLatin1Char('\n');
        stream.flush();
    }
}

void Logger::init()
{
    if (s_initialized) {
        return;
    }

    QString root;
#ifdef PROJECT_ROOT_DIR
    root = QStringLiteral(PROJECT_ROOT_DIR);
#else
    root = QCoreApplication::applicationDirPath();
#endif
    const QString logDir = root + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    const QString logPath =
        logDir + QLatin1Char('/') + QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd")) + QStringLiteral(".log");

    s_logFile.setFileName(logPath);
    if (!s_logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        fprintf(stderr, "[Logger] 无法打开日志文件: %s\n", logPath.toLocal8Bit().constData());
    }

    qInstallMessageHandler(messageHandler);
    s_initialized = true;

    qInfo() << "====== 应用启动 ======";
    qInfo() << "日志文件:" << logPath;
}

void Logger::shutdown()
{
    qInstallMessageHandler(nullptr);

    QMutexLocker locker(&s_mutex);
    if (s_logFile.isOpen()) {
        QTextStream stream(&s_logFile);
        stream << QStringLiteral("[%1] [INFO ] ====== 应用关闭 ======\n")
                      .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh:mm:ss.zzz")));
        s_logFile.close();
    }
    s_initialized = false;
}

QString Logger::logFilePath()
{
    return s_logFile.fileName();
}
