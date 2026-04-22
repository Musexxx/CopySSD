#include "logger.h"
#include <QDir>
#include <QFileInfo>

Logger::Logger() {
    setLogFile("app.log");
}

Logger::~Logger() {
    if (logFile.isOpen()) {
        logStream.flush();
        logFile.close();
    }
}

void Logger::setLogFile(const QString& path) {
    QMutexLocker locker(&mutex);

    if (logFile.isOpen()) {
        logStream.flush();
        logFile.close();
    }

    logFile.setFileName(path);
    if (!logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    logStream.setDevice(&logFile);
}

void Logger::log(LogLevel level, const QString& message) {
    QMutexLocker locker(&mutex);

    checkLogFileSize();

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString logEntry = QString("%1 [%2] %3\n")
                           .arg(timestamp)
                           .arg(getLogLevelString(level))
                           .arg(message);

    logStream << logEntry;
    logStream.flush();
}

QString Logger::getLogLevelString(LogLevel level) {
    switch (level) {
    case Debug:   return "DEBUG";
    case Info:    return "INFO ";
    case Warning: return "WARN ";
    case Error:   return "ERROR";
    default:      return "UNKNOWN";
    }
}

void Logger::checkLogFileSize() {
    if (logFile.size() >= MAX_LOG_SIZE) {
        rotateLogFiles();
    }
}

void Logger::rotateLogFiles() {
    logStream.flush();
    logFile.close();

    // 删除最旧的日志文件
    QString lastFile = logFile.fileName() + "." + QString::number(MAX_BACKUP_COUNT);
    QFile::remove(lastFile);

    // 重命名现有的备份文件
    for (int i = MAX_BACKUP_COUNT - 1; i >= 1; --i) {
        QString oldName = logFile.fileName() + "." + QString::number(i);
        QString newName = logFile.fileName() + "." + QString::number(i + 1);
        QFile::rename(oldName, newName);
    }

    // 重命名当前日志文件
    QFile::rename(logFile.fileName(), logFile.fileName() + ".1");

    // 重新打开新的日志文件
    logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    logStream.setDevice(&logFile);
}
