#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QDateTime>
#include <QMutex>

class Logger {
public:
    enum LogLevel {
        Debug,
        Info,
        Warning,
        Error
    };

    static Logger& getInstance() {
        static Logger instance;
        return instance;
    }

    void log(LogLevel level, const QString& message);
    void setLogFile(const QString& path);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    QString getLogLevelString(LogLevel level);
    void checkLogFileSize();
    void rotateLogFiles();

    QFile logFile;
    QTextStream logStream;
    QMutex mutex;
    const qint64 MAX_LOG_SIZE = 10 * 1024 * 1024; // 10MB
    const int MAX_BACKUP_COUNT = 5;
};

#define LOG_DEBUG(msg) Logger::getInstance().log(Logger::Debug, msg)
#define LOG_INFO(msg) Logger::getInstance().log(Logger::Info, msg)
#define LOG_WARNING(msg) Logger::getInstance().log(Logger::Warning, msg)
#define LOG_ERROR(msg) Logger::getInstance().log(Logger::Error, msg)

#endif
