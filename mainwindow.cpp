#include "mainwindow.h"
#include "workorderselectdialog.h"
#include "copycontroldialog.h"
#include "fullscreenalertdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QProgressDialog>
#include <QDebug>
#include <QApplication>
#include <QTimer>
#include <exception>
#include <QRegularExpression>
#include <QLabel>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QThread>
#include <QDialogButtonBox>
#include <QProcess>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QtConcurrent>
#else
#include <QtConcurrent/QtConcurrentRun>
#endif
#include <QFileInfo>
#include <QSet>
#include <algorithm>

namespace {

// 拷贝机记录第5字段：用于同工单一致性；排除 0、空白、含空格/制表符、非数字、逗号千分位外其它字符
bool parseField5AsCopySizeMiB(const QString& field, qint64& out)
{
    QString s = field.trimmed();
    if (s.isEmpty())
        return false;
    if (s.contains(QLatin1Char(' ')) || s.contains(QLatin1Char('\t')))
        return false;
    s.remove(QLatin1Char(','));
    if (s.isEmpty())
        return false;
    for (QChar c : s) {
        if (!c.isDigit())
            return false;
    }
    bool ok = false;
    const qint64 v = s.toLongLong(&ok);
    if (!ok || v <= 0)
        return false;
    out = v;
    return true;
}

QString buildWorkOrderGobListPath(const QString& workordersRoot, const QString& orderNumber)
{
    if (workordersRoot.isEmpty())
        return QString();
    if (workordersRoot.startsWith(QLatin1String("\\\\"))) {
        return workordersRoot + QLatin1String("\\") + orderNumber + QLatin1String("\\") + orderNumber + QLatin1String(".txt");
    }
    if (workordersRoot.startsWith(QLatin1Char('\\')) && !workordersRoot.startsWith(QLatin1String("\\\\"))) {
        return workordersRoot + QLatin1String("\\") + orderNumber + QLatin1String("\\") + orderNumber + QLatin1String(".txt");
    }
    if (workordersRoot.startsWith(QLatin1Char('/'))) {
        return workordersRoot + QLatin1String("/") + orderNumber + QLatin1String("/") + orderNumber + QLatin1String(".txt");
    }
    return QDir(workordersRoot).filePath(orderNumber + QLatin1String("/") + orderNumber + QLatin1String(".txt"));
}

bool readFirstNonEmptyGobLine(const QString& path, QString& lineOut)
{
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream in(&f);
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.isEmpty()) {
            lineOut = line;
            return true;
        }
    }
    return false;
}

bool parseGobLineToParts(const QString& line, QString& subfolder, QString& baseNoExt)
{
    const int idx = line.indexOf(QLatin1Char('_'));
    if (idx <= 0 || idx >= line.size() - 1)
        return false;
    subfolder = line.left(idx).trimmed();
    QString rest = line.mid(idx + 1).trimmed();
    if (subfolder.isEmpty() || rest.isEmpty())
        return false;
    if (rest.endsWith(QLatin1String(".gob"), Qt::CaseInsensitive))
        rest = rest.left(rest.size() - 4);
    baseNoExt = rest;
    return !baseNoExt.isEmpty();
}

qint64 getFileSizeBytesWithRetry(const QString& path, int maxAttempts, int intervalMs)
{
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        QFileInfo fi(path);
        if (fi.exists() && fi.isFile()) {
            const qint64 s = fi.size();
            if (s >= 0)
                return s;
        }
        LOG_WARNING(QString("大小校验取文件大小失败 (尝试 %1/%2): %3").arg(attempt).arg(maxAttempts).arg(path));
        if (attempt < maxAttempts)
            QThread::msleep(intervalMs);
    }
    LOG_ERROR(QString("大小校验取文件大小失败 (已达最大重试): %1").arg(path));
    return -1;
}

qint64 resolveGobBytesOnRoutes(const QString& route1, const QString& route2,
                               const QString& subfolder, const QString& baseNoExt)
{
    auto tryRoute = [&](const QString& route) -> qint64 {
        if (route.isEmpty())
            return -1;
        const QString dirPath = QDir(route).filePath(subfolder);
        const QString fullPath = QDir(dirPath).filePath(baseNoExt + QLatin1String(".gob"));
        return getFileSizeBytesWithRetry(fullPath, 3, 500);
    };
    qint64 b = tryRoute(route1);
    if (b >= 0)
        return b;
    b = tryRoute(route2);
    return b;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , isProcessing(false)
    , workOrdersLoaded(false)
    , WORK_ORDERS_FILENAME("saved_workorders.json")
    , overflowDialog(nullptr)
    , workorderSettings(nullptr)
    , m_webSocketClient(nullptr)
    , batchProgressDialog(nullptr)
    , totalFilesToProcess(0)
    , processedFilesCount(0)
    , isBatchProcessing(false)
    , pendingOrderTimer(nullptr)
    , autoConnectTimer(nullptr)
{
    try {
        // 加载配置
        initLogger();
        // ============ 在这里插入代码 (Start) ============
        LOG_INFO("检查并执行 netuse.bat ...");
        QString batPath = QApplication::applicationDirPath() + "/netuse.bat";
        if (QFile::exists(batPath)) {
            int exitCode = QProcess::execute(batPath);
            if (exitCode == 0) {
                LOG_INFO(QString("netuse.bat 执行成功 (退出码: %1)").arg(exitCode));
            } else {
                LOG_WARNING(QString("netuse.bat 执行失败 (退出码: %1)").arg(exitCode));
            }
        } else {
            LOG_INFO("未找到 netuse.bat，跳过网络映射连接。");
        }
               // ============ 在这里插入代码 ============

        LOG_INFO("开始加载配置...");
        loadConfig();

        LOG_INFO("设置文件监控...");

        // 设置文件监控
        fileWatcher = new QFileSystemWatcher(this);

        // 初始化队列处理系统
        processTimer = new QTimer(this);
        processTimer->setInterval(100);
        connect(processTimer, &QTimer::timeout, this, &MainWindow::processNextFile);
        processTimer->start();

        // 初始化同步定时器（每5秒同步一次工单计数）
        syncTimer = new QTimer(this);
        syncTimer->setInterval(30000); // 30秒
        connect(syncTimer, &QTimer::timeout, this, &MainWindow::syncAllWorkOrderCounts);
        syncTimer->start();

        // 初始化队列进度更新定时器（每2秒更新一次）
        QTimer* queueProgressTimer = new QTimer(this);
        queueProgressTimer->setInterval(2000); // 2秒
        connect(queueProgressTimer, &QTimer::timeout, this, &MainWindow::updateQueueProgress);
        queueProgressTimer->start();

        // 设置通知清理定时器
        QTimer* notificationCleanupTimer = new QTimer(this);
        notificationCleanupTimer->setInterval(24 * 60 * 60 * 1000); // 24小时清理一次
        connect(notificationCleanupTimer, &QTimer::timeout, this, &MainWindow::cleanupExpiredNotifications);
        notificationCleanupTimer->start();

        // 初始化等待确认工单处理定时器
        pendingOrderTimer = new QTimer(this);
        pendingOrderTimer->setInterval(5000); // 每5秒检查一次等待确认的工单
        connect(pendingOrderTimer, &QTimer::timeout, this, &MainWindow::processPendingOrders);
        pendingOrderTimer->start();

        // 设置UI
        setupUI();

        // 设置WebSocket客户端（在UI设置之后，因为需要访问UI元素）
        setupWebSocket();
        
        // 设置自动连接定时器
        setupAutoConnectTimer();

        // 先询问是否加载保存的工单信息，然后再执行启动自检
        QTimer::singleShot(100, this, [this]() {
            try {
                askToLoadSavedWorkOrders();
            } catch (const std::exception& e) {
                LOG_ERROR(QString("加载保存的工单信息时发生错误：%1").arg(e.what()));
            }
        });

        // 监控A路径
        if (QDir(watchPath).exists()) {
            fileWatcher->addPath(watchPath);
            connect(fileWatcher, &QFileSystemWatcher::directoryChanged,
                    this, &MainWindow::onDirectoryChanged);
        } else {
            QMessageBox::warning(this, "警告",
                                 QString("监控路径 %1 不存在，请检查配置文件").arg(watchPath));
        }

        // 设置工单文件夹监控（延迟执行，确保工单已加载）
        QTimer::singleShot(1000, this, &MainWindow::setupWorkOrderMonitoring);

        // 确保归档路径存在，但不监控它
        if (!QDir(archivePath).exists()) {
            // 检查是否为网络路径
            if (!isNetworkPath(archivePath)) {
                QDir().mkpath(archivePath);
            } else {
                LOG_INFO(QString("归档路径为网络路径，跳过创建: %1").arg(archivePath));
            }
        }
        processedOrders.clear();

    } catch (const std::exception& e) {
        LOG_ERROR(QString("程序初始化失败：%1").arg(e.what()));
        QMessageBox::critical(this, "错误",
                              QString("程序初始化失败：%1").arg(e.what()));
    }
}

MainWindow::~MainWindow()
{
    if (workorderSettings) {
        delete workorderSettings;
    }
    saveWorkOrders();
    processTimer->stop();
    delete processTimer;
    qDeleteAll(workOrders);
}
// 新增验证函数
int MainWindow::getWorkOrderTotal(const QString& orderNumber)
{
    // 新逻辑：从工单根目录下对应工单文件夹中的 NUM.txt 读取
    return readWorkOrderTotalFromNumFile(orderNumber);
}
bool MainWindow::validateBarcode(const QString& filePath, const QString& fileContent) {
    // 获取文件名（不含路径和扩展名）作为条码
    QFileInfo fileInfo(filePath);
    QString barcodeFromFileName = fileInfo.baseName();

    // 从文件内容中提取条码
    QStringList parts = fileContent.split("|");
    if (parts.size() < 2) {
        LOG_ERROR(QString("文件格式错误: %1").arg(filePath));
        return false;
    }

    QString barcodeFromContent = parts[1].trimmed();
    
    // 新格式支持：工单号|条码|料号描述，但条码验证只需要前两部分
    LOG_INFO(QString("文件内容解析 - 工单号: %1, 条码: %2, 字段数: %3")
                .arg(parts[0].trimmed())
                .arg(barcodeFromContent)
                .arg(parts.size()));

    // 比对条码
    return barcodeFromFileName == barcodeFromContent;
}

// 处理不匹配文件
void MainWindow::handleMismatchedFile(const QString& filePath) {
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();

    // 确保目标目录存在
    QDir dir(mismatchPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 移动文件到不匹配目录
    QString targetPath = mismatchPath + "/" + fileName;
    if (QFile::rename(filePath, targetPath)) {
        QString message = QString("发现条码不匹配文件：\n%1\n已移动至：%2").arg(fileName).arg(mismatchPath);
        LOG_WARNING(message);

        // 显示提示窗口
        QMessageBox::warning(this,
                             "条码不匹配提醒",
                             message,
                             QMessageBox::Ok);
    } else {
        LOG_ERROR(QString("移动不匹配文件失败: %1").arg(filePath));
    }
}

void MainWindow::handleErrorFile(const QString& filePath, const QString& orderNumber, const QString& barcode, const QString& errorType) {
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();

    // 确保错误目录存在
    QDir dir(errorPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // 移动文件到错误目录
    QString targetPath = errorPath + "/" + fileName;
    if (QFile::rename(filePath, targetPath)) {
        LOG_WARNING(QString("错误文件处理 - 工单: %1, 条码: %2, 错误类型: %3, 文件: %4, 已移动至: %5")
                       .arg(orderNumber.isEmpty() ? "未知" : orderNumber)
                       .arg(barcode.isEmpty() ? "无" : barcode)
                       .arg(errorType)
                       .arg(fileName)
                       .arg(errorPath));
    } else {
        LOG_ERROR(QString("移动错误文件失败: %1").arg(filePath));
    }
}

void MainWindow::handleErrorFileWithOrder(const QString& filePath, const QString& orderNumber, const QString& barcode, const QString& errorType)
{
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.fileName();

    // 确保错误目录存在
    QDir dir(errorPath);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QString targetPath;
    if (!orderNumber.isEmpty()) {
        // 有工单号，创建工单子文件夹
        QString orderErrorPath = errorPath + "/" + orderNumber;
        QDir orderDir(orderErrorPath);
        if (!orderDir.exists()) {
            orderDir.mkpath(".");
        }
        targetPath = orderErrorPath + "/" + fileName;
    } else {
        // 无工单号，直接放入根目录
        targetPath = errorPath + "/" + fileName;
    }

    if (QFile::rename(filePath, targetPath)) {
        LOG_WARNING(QString("错误文件处理 - 工单: %1, 条码: %2, 错误类型: %3, 文件: %4, 已移动至: %5")
                       .arg(orderNumber.isEmpty() ? "未知" : orderNumber)
                       .arg(barcode.isEmpty() ? "无" : barcode)
                       .arg(errorType)
                       .arg(fileName)
                       .arg(targetPath));
    } else {
        LOG_ERROR(QString("移动错误文件失败: %1").arg(filePath));
    }
}

void MainWindow::initLogger()
{
    QString logPath = QDir::currentPath() + "/logs";
    QDir().mkpath(logPath);

    QString logFile = logPath + "/app_" +
                      QDateTime::currentDateTime().toString("yyyy-MM-dd") + ".log";

    Logger::getInstance().setLogFile(logFile);
    LOG_INFO("应用程序启动");
}

void MainWindow::processNextFile()
{
    if (isProcessing || fileQueue.isEmpty()) {
        return;
    }

    isProcessing = true;
    QString filePath = fileQueue.dequeue();

    // 更新处理计数
    if (isBatchProcessing) {
        processedFilesCount++;
    }

    LOG_INFO(QString("从队列中取出文件进行处理: %1, 剩余队列长度: %2")
                 .arg(filePath)
                 .arg(fileQueue.size()));

    try {
        if (!QFile::exists(filePath)) {
            LOG_WARNING(QString("队列中的文件不存在: %1").arg(filePath));
            isProcessing = false;
            return;
        }

        bool success = processLogFile(filePath);
        LOG_INFO(QString("队列文件处理%1: %2")
                     .arg(success ? "成功" : "失败")
                     .arg(filePath));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("处理队列文件时发生异常: %1, 文件: %2")
                      .arg(e.what())
                      .arg(filePath));
    }

    isProcessing = false;

    // 如果批量处理完成，显示完成消息
    if (isBatchProcessing && fileQueue.isEmpty()) {
        isBatchProcessing = false;
        LOG_INFO(QString("批量文件处理完成，共处理 %1 个文件").arg(processedFilesCount));
    }
}
void MainWindow::addToProcessQueue(const QString& filePath)
{
    //LOG_INFO(QString("尝试添加文件到队列: %1").arg(filePath));

    // 检查文件是否属于等待确认的工单
    QFileInfo fileInfo(filePath);
    QString fileName = fileInfo.baseName();

    // 尝试从文件名解析工单号（假设文件名就是条码）
    // 这里需要读取文件内容来获取工单号，但为了性能考虑，我们先添加到队列
    // 在processLogFile中再进行料号验证

    if (!fileQueue.contains(filePath)) {
        fileQueue.enqueue(filePath);
        LOG_INFO(QString("文件已添加到队列: %1, 当前队列长度: %2")
                     .arg(filePath)
                     .arg(fileQueue.size()));
    } else {
    //    LOG_INFO(QString("文件已在队列中: %1").arg(filePath));
    }
}

QStringList MainWindow::getLogFiles(const QString& dirPath)
{
    QDir dir(dirPath);
    QStringList filters;
    filters << "*.log" << "*.txt";  // 同时支持.log和.txt文件
    QStringList allFiles = dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot);

    // 过滤掉TimeCount*.txt文件
    QStringList filteredFiles;
    for (const QString& file : allFiles) {
        if (!(file.startsWith("TimeCount") && file.endsWith(".txt"))) {
            filteredFiles << file;
        }
    }

    return filteredFiles;
}

QString MainWindow::ensureLogExtension(const QString& filePath)
{
    QFileInfo fileInfo(filePath);
    QString extension = fileInfo.suffix().toLower();
    QString fileName = fileInfo.fileName();

    // 检查是否是TimeCount*.txt文件，如果是则直接返回，不进行转换
    if (fileName.startsWith("TimeCount") && fileName.endsWith(".txt")) {
        LOG_INFO(QString("跳过TimeCount文件转换: %1").arg(fileName));
        return filePath;
    }

    // 如果是.txt文件，重命名为.log
    if (extension == "txt") {
        QString newPath = fileInfo.path() + "/" + fileInfo.completeBaseName() + ".log";

        // 检查目标文件是否已存在
        if (QFile::exists(newPath)) {
            LOG_WARNING(QString("目标.log文件已存在，跳过重命名: %1").arg(newPath));
            return filePath;  // 返回原路径，避免覆盖
        }

        // 重命名文件
        if (QFile::rename(filePath, newPath)) {
            LOG_INFO(QString("成功将.txt文件重命名为.log: %1 -> %2").arg(filePath).arg(newPath));
            return newPath;
        } else {
            LOG_ERROR(QString("重命名文件失败: %1 -> %2").arg(filePath).arg(newPath));
            return filePath;  // 重命名失败，返回原路径
        }
    }

    // 如果已经是.log文件，直接返回
    return filePath;
}

int MainWindow::getNextDuplicateNumber(const QString& barcode, const QString& orderNumber, const QString& subFolder)
{
    try {
        QDir duplicateDir(duplicatePath);
        QString duplicateOrderPath = duplicateDir.filePath(orderNumber);
        QDir dir(duplicateOrderPath);

        if (!dir.exists()) {
            return 1;
        }

        // 获取所有.log文件
        QStringList filters;
        filters << "*.log";
        QStringList files = dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot);

        if (files.isEmpty()) {
            return 1;
        }

        // 查找最大的重复序号
        int maxNumber = 0;
        // 注意：barcode可能包含正则特殊字符，这里需要转义后再拼接
        QRegularExpression rx(QRegularExpression::escape(barcode) + "\\+(\\d+)\\.log");

        for (const QString& file : files) {
            QRegularExpressionMatch match = rx.match(file);
            if (match.hasMatch()) {
                int number = match.captured(1).toInt();
                maxNumber = qMax(maxNumber, number);
            }
        }

        LOG_INFO(QString("获取重复序号 - 条码: %1, 工单: %2, 位置: %3, 最大序号: %4, 返回: %5")
                     .arg(barcode)
                     .arg(orderNumber)
                     .arg(subFolder)
                     .arg(maxNumber)
                     .arg(maxNumber + 1));

        return maxNumber + 1;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("获取重复序号时发生异常: %1").arg(e.what()));
        return 1;
    }
}

bool MainWindow::writeTimeRecord(const QString& orderNumber, const QString& barcode,
                                 const QString& createTime, int copyCount)
{
    try {
        // 手动构造网络路径，避免QDir::filePath()的问题
        QString orderPath;
        QString timeFilePath;

        if (isNetworkPath(archivePath)) {
            // 网络路径：手动构造
            if (archivePath.startsWith("\\")) {
                // 单个反斜杠开头的网络路径
                orderPath = archivePath + "\\" + orderNumber;
                timeFilePath = orderPath + "\\TimeCount" + machineId + ".txt";
            } else if (archivePath.startsWith("\\\\")) {
                // 双反斜杠开头的网络路径
                orderPath = archivePath + "\\" + orderNumber;
                timeFilePath = orderPath + "\\TimeCount" + machineId + ".txt";
            } else {
                // 正斜杠开头的网络路径
                orderPath = archivePath + "/" + orderNumber;
                timeFilePath = orderPath + "/TimeCount" + machineId + ".txt";
            }
        } else {
            // 本地路径：使用QDir::filePath()
            QDir archiveDir(archivePath);
            orderPath = archiveDir.filePath(orderNumber);
            timeFilePath = QDir(orderPath).filePath(QString("TimeCount%1.txt").arg(machineId));
        }

        // 确保工单文件夹存在
        QDir dir;
        if (!dir.exists(orderPath)) {
            LOG_INFO(QString("创建工单文件夹: %1").arg(orderPath));
            if (!dir.mkpath(orderPath)) {
                LOG_ERROR(QString("无法创建工单文件夹: %1").arg(orderPath));
                return false;
            }
        }

        // 检查条码在TimeCount文件中出现的次数
        int actualCount = 1;  // 默认为1
        if (QFile::exists(timeFilePath)) {
            QFile timeFile(timeFilePath);
            if (timeFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream in(&timeFile);
                QString line;
                while (!in.atEnd()) {
                    line = in.readLine();
                    if (line.contains(QString("条码：%1").arg(barcode))) {
                        actualCount++;
                    }
                }
                timeFile.close();
            }
        }

        // 获取文件创建时间
        //QFileInfo fileInfo(sourceFile);
        //QString createTime = fileInfo.birthTime().toString("yyyy-MM-dd hh:mm:ss.zzz");

        // 从配置文件获取工单总数
        int totalCount = readWorkOrderTotalFromNumFile(orderNumber);
        LOG_INFO(QString("从NUM.txt读取工单总数 - 工单: %1, 总数: %2").arg(orderNumber).arg(totalCount));

        // 计算已完成数量（工单文件夹下所有子目录的log文件数，包括overlog）
        int completedCount = countAllLogFilesInOrderFolder(orderNumber, true);
        LOG_INFO(QString("计算已完成数量 - 工单: %1, 完成数: %2").arg(orderNumber).arg(completedCount));

        // 计算剩余量
        int remainingCount = totalCount > completedCount ? totalCount - completedCount : 0;
        QString currentTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        // 准备写入的记录（新格式）
        QString record = QString("创建时间：%1|工单号：%2|条码：%3|出现次数：%4|工单总数：%5|已完成数量：%6|剩余量：%7|当前时间：%8\n")
                             .arg(createTime)
                             .arg(orderNumber)
                             .arg(barcode)
                             .arg(actualCount)
                             .arg(totalCount)
                             .arg(completedCount)
                             .arg(remainingCount)
                             .arg(currentTime);  // 添加当前时间

        // 打开文件追加写入
        QFile timeFile(timeFilePath);
        if (!timeFile.open(QIODevice::Append | QIODevice::Text)) {
            LOG_ERROR(QString("无法打开时间记录文件: %1, 错误: %2")
                          .arg(timeFilePath)
                          .arg(timeFile.errorString()));
            return false;
        }

        QTextStream out(&timeFile);
        out << record;
        out.flush();
        timeFile.close();

        LOG_INFO(QString("成功写入时间记录 - 工单: %1, 条码: %2, 出现次数: %3, 总数: %4, 完成数: %5, 剩余: %6")
                     .arg(orderNumber)
                     .arg(barcode)
                     .arg(actualCount)
                     .arg(totalCount)
                     .arg(completedCount)
                     .arg(remainingCount));

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("写入时间记录时发生异常: %1").arg(e.what()));
        return false;
    }
}

void MainWindow::loadConfig()
{
    // 检查配置文件是否存在
    if (!QFile::exists("config.ini")) {
        LOG_WARNING("配置文件不存在，使用默认配置");
        QMessageBox::warning(this, "警告",
                             "配置文件 config.ini 不存在，将使用默认配置");

        // 创建默认配置文件
        createDefaultConfig();
    }

    QSettings settings("config.ini", QSettings::IniFormat);

    // 读取配置并设置默认值
    machineId = settings.value("Machine/ID", "DEFAULT").toString();
    workorderConfigPath = settings.value("Path/WorkorderConfig", "D:/Config/workorder.ini").toString();
    // 新增：工单根目录（网络路径），每个工单文件夹内含 NUM.txt
    workordersRootPath = settings.value("Path/WorkordersRoot", "").toString();
    // 路径配置
    watchPath = settings.value("Path/WatchFolder", "D:/Watch").toString();
    archivePath = settings.value("Path/ArchiveFolder", "D:/Archive").toString();
    duplicatePath = settings.value("Path/DuplicateFolder", "D:/Duplicate").toString();
    mismatchPath = settings.value("Path/MismatchFolder", "D:/Mismatch").toString();  // 新增不匹配文件路径
    ssdInfoPath = settings.value("SSDInfo/SSDInfoPath", "E:/SSDInfo").toString();  // 新增SSD信息文件路径
    LOG_INFO(QString("SSD信息文件路径: %1").arg(ssdInfoPath));
    
    errorPath = settings.value("Path/ErrorFolder", "D:/Error").toString();  // 新增错误状态文件路径
    LOG_INFO(QString("错误状态文件路径: %1").arg(errorPath));
    
    materialCachePath = settings.value("Path/MaterialCachePath", "E:/MaterialCache").toString();  // 新增料号缓存路径
    LOG_INFO(QString("料号缓存路径: %1").arg(materialCachePath));

    gobRoute1 = settings.value("Path/GobRoute1", "").toString();
    gobRoute2 = settings.value("Path/GobRoute2", "").toString();
    copyMachineLogPath = settings.value("Path/CopyMachineLogPath", "").toString();
    syncValidationInterval = settings.value("Settings/SyncValidationInterval", 20).toInt();
    if (syncValidationInterval < 1)
        syncValidationInterval = 1;
    LOG_INFO(QString("GobRoute1: %1, GobRoute2: %2, CopyMachineLogPath: %3, SyncValidationInterval: %4")
                 .arg(gobRoute1)
                 .arg(gobRoute2)
                 .arg(copyMachineLogPath)
                 .arg(syncValidationInterval));

    if (!QFile::exists(workorderConfigPath)) {
        LOG_WARNING(QString("工单配置文件不存在: %1").arg(workorderConfigPath));
        QMessageBox::warning(this, "警告",
                             QString("工单配置文件不存在: %1\n将使用手动输入模式").arg(workorderConfigPath));
    } else {
        workorderSettings = new QSettings(workorderConfigPath, QSettings::IniFormat);
        LOG_INFO(QString("已加载工单配置文件: %1").arg(workorderConfigPath));
    }
    // 验证配置有效性
    if (machineId.isEmpty() || machineId == "DEFAULT") {
        LOG_WARNING("未配置机器ID，使用默认值");
        QMessageBox::warning(this, "警告", "未配置机器ID，请检查配置文件");
    }

    // 确保所有路径存在
    QStringList paths = {
        watchPath,
        archivePath,
        duplicatePath,
        mismatchPath,
        workordersRootPath,
        materialCachePath
    };
    if (!copyMachineLogPath.isEmpty())
        paths << copyMachineLogPath;

    QDir dir;
    bool pathError = false;
    QString errorPaths;

    for (const QString& path : paths) {
        if (path.isEmpty()) {
            pathError = true;
            errorPaths += "空路径\n";
            continue;
        }

        if (!dir.exists(path)) {
            if (isNetworkPath(path)) {
                // 网络路径特殊处理：只检查是否可访问，不尝试创建
                if (!checkNetworkPathAccessibility(path)) {
                    pathError = true;
                    errorPaths += QString("网络路径无法访问: %1\n").arg(path);
                    LOG_ERROR(QString("网络路径无法访问: %1").arg(path));
                } else {
                    LOG_INFO(QString("网络路径可访问: %1").arg(path));
                }
            } else {
                // 本地路径：尝试创建
                if (!dir.mkpath(path)) {
                    pathError = true;
                    errorPaths += QString("无法创建目录: %1\n").arg(path);
                    LOG_ERROR(QString("无法创建目录: %1").arg(path));
                } else {
                    LOG_INFO(QString("已创建目录: %1").arg(path));
                }
            }
        } else {
            LOG_INFO(QString("路径已存在: %1").arg(path));
        }
    }

    if (pathError) {
        QString errorMsg = "以下路径配置无效或无法创建：\n" + errorPaths;
        LOG_ERROR(errorMsg);
        QMessageBox::critical(this, "配置错误", errorMsg);
        qApp->quit();
        return;
    }

    // 记录配置信息
    LOG_INFO(QString("配置加载完成:\n"
                     "- 机器ID: %1\n"
                     "- 监控路径: %2\n"
                     "- 归档路径: %3\n"
                     "- 重复文件路径: %4\n"
                     "- 不匹配文件路径: %5\n"
                     "- 工单配置文件: %6\n"
                     "- 工单根目录: %7\n"
                     "- 料号缓存路径: %8")
                 .arg(machineId)
                 .arg(watchPath)
                 .arg(archivePath)
                 .arg(duplicatePath)
                 .arg(mismatchPath)
                 .arg(workorderConfigPath)
                 .arg(workordersRootPath)
                 .arg(materialCachePath));

    // 调试网络路径识别
    LOG_INFO(QString("网络路径识别调试:\n"
                     "- 归档路径: '%1' (isNetworkPath: %2)\n"
                     "- 工单根目录: '%3' (isNetworkPath: %4)")
                 .arg(archivePath)
                 .arg(isNetworkPath(archivePath) ? "true" : "false")
                 .arg(workordersRootPath)
                 .arg(isNetworkPath(workordersRootPath) ? "true" : "false"));

    // 调试路径格式分析
    LOG_INFO(QString("路径格式分析:\n"
                     "- 归档路径前缀: '%1' (长度: %2)\n"
                     "- 工单根目录前缀: '%3' (长度: %4)")
                 .arg(archivePath.left(3))
                 .arg(archivePath.left(3).length())
                 .arg(workordersRootPath.left(3))
                 .arg(workordersRootPath.left(3).length()));
}

void MainWindow::createDefaultConfig()
{
    QSettings settings("config.ini", QSettings::IniFormat);

    // 机器配置
    settings.setValue("Machine/ID", "DEFAULT");

    // 路径配置
    settings.setValue("Path/WatchFolder", "D:/Watch");
    settings.setValue("Path/ArchiveFolder", "D:/Archive");
    settings.setValue("Path/DuplicateFolder", "D:/Duplicate");
    settings.setValue("Path/MismatchFolder", "D:/Mismatch");
    settings.setValue("Path/MaterialCachePath", "E:/MaterialCache");
    settings.setValue("Path/GobRoute1", "\\\\172.18.10.246\\COPYSDD");
    settings.setValue("Path/GobRoute2", "\\\\172.18.10.246\\COPYSDD");
    settings.setValue("Path/CopyMachineLogPath", "D:/CopyLogs/");
    settings.setValue("Settings/SyncValidationInterval", 20);

    settings.sync();

    LOG_INFO("已创建默认配置文件");
}
void MainWindow::performInitialCheck()
{
    try {
        // 检查路径是否存在
        if (!QDir(watchPath).exists()) {
            QMessageBox::warning(this, "警告",
                                 QString("监控路径 %1 不存在，请检查配置文件").arg(watchPath));
            return;
        }

        // 处理现有文件
        bool result = processAllFilesOptimized();

        if (result) {
            LOG_INFO("启动自检完成，所有文件已添加到处理队列");
        } else {
            LOG_WARNING("启动自检过程中发现一些问题，请检查日志文件");
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("系统自检时发生异常：%1").arg(e.what()));
        QMessageBox::critical(this, "错误",
                              QString("系统自检时发生异常：%1").arg(e.what()));
    }
}

bool MainWindow::processAllFiles()
{
    try {
        if (!QDir(watchPath).exists()) {
            qDebug() << "监控路径不存在:" << watchPath;
            return false;
        }

        // 暂时禁用文件监控
        if (fileWatcher->directories().contains(watchPath)) {
            fileWatcher->removePath(watchPath);
        }

        QStringList files = getLogFiles(watchPath);

        // 如果目录为空，显示消息并返回
        if (files.isEmpty()) {
            // 重新启用监控
            if (!fileWatcher->directories().contains(watchPath)) {
                fileWatcher->addPath(watchPath);
            }
            qDebug() << "监控路径中没有需要处理的文件:" << watchPath;
            return true;
        }

        // 创建进度对话框
        QProgressDialog progress("正在添加文件到处理队列...", QString(), 0, files.size(), this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.setCancelButton(nullptr);
        progress.show();

        int count = 0;
        for (const QString& file : files) {
            QString filePath = QDir(watchPath).filePath(file);
            if (QFile::exists(filePath)) {
                // 确保文件扩展名为.log（如果是.txt则重命名）
                QString processedPath = ensureLogExtension(filePath);
                addToProcessQueue(processedPath);
            }
            progress.setValue(++count);
            QApplication::processEvents();
        }

        // 重新启用监控
        if (!fileWatcher->directories().contains(watchPath)) {
            fileWatcher->addPath(watchPath);
        }

        return true;
    } catch (const std::exception& e) {
        qDebug() << "批量处理文件时发生异常:" << e.what();
        // 确保监控被重新启用
        if (!fileWatcher->directories().contains(watchPath)) {
            fileWatcher->addPath(watchPath);
        }
        return false;
    }
}

bool MainWindow::processAllFilesOptimized()
{
    try {
        if (!QDir(watchPath).exists()) {
            LOG_WARNING(QString("监控路径不存在: %1").arg(watchPath));
            return false;
        }

        // 暂时禁用文件监控
        if (fileWatcher->directories().contains(watchPath)) {
            fileWatcher->removePath(watchPath);
        }

        QStringList files = getLogFiles(watchPath);

        // 如果目录为空，显示消息并返回
        if (files.isEmpty()) {
            // 重新启用监控
            if (!fileWatcher->directories().contains(watchPath)) {
                fileWatcher->addPath(watchPath);
            }
            LOG_INFO(QString("监控路径中没有需要处理的文件: %1").arg(watchPath));
            return true;
        }

        LOG_INFO(QString("发现 %1 个文件需要处理").arg(files.size()));

        // 如果文件数量较少（<100），使用原来的方式
        if (files.size() < 100) {
            // 重新启用监控
            if (!fileWatcher->directories().contains(watchPath)) {
                fileWatcher->addPath(watchPath);
            }
            return processAllFiles();
        }

        // 大量文件处理：分批次添加到队列
        isBatchProcessing = true;
        totalFilesToProcess = files.size();
        processedFilesCount = 0;

        // 创建进度对话框
        batchProgressDialog = new QProgressDialog("正在扫描文件...", "取消", 0, totalFilesToProcess, this);
        batchProgressDialog->setWindowModality(Qt::WindowModal);
        batchProgressDialog->setMinimumDuration(0);
        batchProgressDialog->setWindowTitle("批量文件处理");
        batchProgressDialog->show();

        // 分批处理文件，避免界面卡死
        int batchSize = 50; // 每批处理50个文件
        int currentBatch = 0;

        for (int i = 0; i < files.size(); i += batchSize) {
            // 检查是否取消
            if (batchProgressDialog->wasCanceled()) {
                LOG_INFO("用户取消了批量文件处理");
                break;
            }

            // 更新进度
            batchProgressDialog->setValue(i);
            batchProgressDialog->setLabelText(QString("正在扫描文件... (%1/%2)").arg(i).arg(totalFilesToProcess));
            QApplication::processEvents();

            // 处理当前批次
            int endIndex = qMin(i + batchSize, files.size());
            for (int j = i; j < endIndex; ++j) {
                QString filePath = QDir(watchPath).filePath(files[j]);
                if (QFile::exists(filePath)) {
                    // 确保文件扩展名为.log（如果是.txt则重命名）
                    QString processedPath = ensureLogExtension(filePath);
                    addToProcessQueue(processedPath);
                }
            }

            // 短暂延迟，让界面保持响应
            QThread::msleep(10);
        }

        // 重新启用监控
        if (!fileWatcher->directories().contains(watchPath)) {
            fileWatcher->addPath(watchPath);
        }

        // 更新进度对话框为处理模式
        if (batchProgressDialog) {
            batchProgressDialog->setLabelText("正在处理文件...");
            batchProgressDialog->setValue(0);
        }

        LOG_INFO(QString("批量文件扫描完成，共添加 %1 个文件到处理队列").arg(files.size()));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("批量处理文件时发生异常: %1").arg(e.what()));
        // 确保监控被重新启用
        if (!fileWatcher->directories().contains(watchPath)) {
            fileWatcher->addPath(watchPath);
        }
        isBatchProcessing = false;
        if (batchProgressDialog) {
            batchProgressDialog->close();
            delete batchProgressDialog;
            batchProgressDialog = nullptr;
        }
        return false;
    }
}

void MainWindow::setupUI()
{
    setWindowTitle(QString("MSI-工单管理系统 - 机器号: %1").arg(machineId));
    resize(1200, 800);  // 增加窗口大小以容纳双表格

    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QVBoxLayout* layout = new QVBoxLayout(centralWidget);

    // 设置双表格UI
    setupDualTableUI();
    
    // 将双表格布局添加到主布局
    layout->addLayout(createDualTableLayout());

    // 创建按钮布局
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(10);
    buttonLayout->setContentsMargins(0, 0, 0, 0);

    // 添加工单按钮
    QPushButton* addButton = new QPushButton("添加工单", this);
    addButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    addButton->setMinimumHeight(40);
    connect(addButton, &QPushButton::clicked, this, &MainWindow::onAddWorkOrder);

    // 已处理工单按钮
    QPushButton* processedOrdersButton = new QPushButton("已处理工单", this);
    processedOrdersButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    processedOrdersButton->setMinimumHeight(40);
    connect(processedOrdersButton, &QPushButton::clicked, this, &MainWindow::showProcessedOrders);

    // 刷新按钮
    QPushButton* refreshButton = new QPushButton("刷新", this);
    refreshButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    refreshButton->setMinimumHeight(40);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::onRefresh);

    // 连接中控按钮
    m_connectButton = new QPushButton("连接中控", this);
    m_connectButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_connectButton->setMinimumHeight(40);
    connect(m_connectButton, &QPushButton::clicked, this, &MainWindow::onConnectButtonClicked);

    // 新增：开始拷贝按钮
    QPushButton* copyButton = new QPushButton("开始拷贝", this);
    copyButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    copyButton->setMinimumHeight(40);
    copyButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #9C27B0;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #7B1FA2;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #6A1B9A;"
        "}"
    );
    connect(copyButton, &QPushButton::clicked, this, &MainWindow::onStartCopyClicked);

    // 将按钮添加到水平布局
    buttonLayout->addWidget(addButton, 1);
    buttonLayout->addWidget(processedOrdersButton, 1);
    buttonLayout->addWidget(refreshButton, 1);
    buttonLayout->addWidget(m_connectButton, 1);
    buttonLayout->addWidget(copyButton, 1);  // 添加拷贝按钮

    // 将按钮布局添加到主布局
    layout->addLayout(buttonLayout);
    tableWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    // 连接信号槽
    connect(tableWidget, &QTableWidget::itemDoubleClicked,
            this, &MainWindow::onTableItemDoubleClicked);
    connect(tableWidget, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::onTableCustomContextMenuRequested);
}

void MainWindow::setupDualTableUI()
{
    // 创建未完成工单表格
    incompleteTableWidget = new QTableWidget(this);
    incompleteTableWidget->setColumnCount(6);
    incompleteTableWidget->setHorizontalHeaderLabels({
        "工单号", "总数量", "已完成", "失败数量", "剩余数量", "操作"
    });
    incompleteTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    incompleteTableWidget->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    incompleteTableWidget->setColumnWidth(5, 300);
    incompleteTableWidget->setMaximumHeight(350);  // 限制高度
    incompleteTableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    incompleteTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);  // 禁止编辑
    
    // 连接未完成工单表格的信号槽
    connect(incompleteTableWidget, &QTableWidget::itemDoubleClicked,
            this, &MainWindow::onTableItemDoubleClicked);
    connect(incompleteTableWidget, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::onTableCustomContextMenuRequested);

    // 创建已完成工单表格
    completedTableWidget = new QTableWidget(this);
    completedTableWidget->setColumnCount(6);
    completedTableWidget->setHorizontalHeaderLabels({
        "工单号", "总数量", "已完成", "失败数量", "剩余数量", "操作"
    });
    completedTableWidget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    completedTableWidget->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Fixed);
    completedTableWidget->setColumnWidth(5, 300);
    completedTableWidget->setMaximumHeight(350);  // 限制高度
    completedTableWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    completedTableWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);  // 禁止编辑
    
    // 连接已完成工单表格的信号槽
    connect(completedTableWidget, &QTableWidget::itemDoubleClicked,
            this, &MainWindow::onTableItemDoubleClicked);
    connect(completedTableWidget, &QTableWidget::customContextMenuRequested,
            this, &MainWindow::onTableCustomContextMenuRequested);

    // 保留原有表格用于兼容性（隐藏）
    tableWidget = new QTableWidget(this);
    tableWidget->setVisible(false);  // 隐藏原有表格
}

QVBoxLayout* MainWindow::createDualTableLayout()
{
    QVBoxLayout* dualTableLayout = new QVBoxLayout();
    
    // 未完成工单区域
    QLabel* incompleteLabel = new QLabel("未完成工单", this);
    incompleteLabel->setStyleSheet("font-weight: bold; font-size: 14px; color: #1976D2;");
    dualTableLayout->addWidget(incompleteLabel);
    dualTableLayout->addWidget(incompleteTableWidget);
    
    // 分割线
    QFrame* separator = new QFrame();
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    separator->setStyleSheet("color: #BDBDBD;");
    dualTableLayout->addWidget(separator);
    
    // 已完成工单区域
    QLabel* completedLabel = new QLabel("已完成工单", this);
    completedLabel->setStyleSheet("font-weight: bold; font-size: 14px; color: #388E3C;");
    dualTableLayout->addWidget(completedLabel);
    dualTableLayout->addWidget(completedTableWidget);
    
    return dualTableLayout;
}

void MainWindow::onRefresh()
{
    try {
        LOG_INFO("开始执行刷新操作");

        // 创建进度对话框
        QProgressDialog progress("正在刷新...", QString(), 0, 3, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        progress.setCancelButton(nullptr);
        progress.show();

        // 第一步：检查监控路径
        progress.setValue(0);
        progress.setLabelText("正在检查监控路径...");
        QApplication::processEvents();

        if (!QDir(watchPath).exists()) {
            QMessageBox::warning(this, "警告",
                                 QString("监控路径 %1 不存在").arg(watchPath));
            return;
        }

        // 保存当前所有监控的路径
        QStringList previousWatchedPaths = fileWatcher->directories();

        // 暂时禁用所有文件监控
        if (!previousWatchedPaths.isEmpty()) {
            fileWatcher->removePaths(previousWatchedPaths);
        }

        // 第二步：处理未归档的文件
        progress.setValue(1);
        progress.setLabelText("正在处理未归档文件...");
        QApplication::processEvents();

        QStringList files = getLogFiles(watchPath);

        if (!files.isEmpty()) {
            LOG_INFO(QString("发现 %1 个未归档文件").arg(files.size()));

            // 创建文件处理进度对话框
            QProgressDialog fileProgress("正在处理文件...", QString(), 0, files.size(), this);
            fileProgress.setWindowModality(Qt::WindowModal);
            fileProgress.setMinimumDuration(0);
            fileProgress.setCancelButton(nullptr);
            fileProgress.show();

            int processedCount = 0;
            for (const QString& file : files) {
                QString filePath = QDir(watchPath).filePath(file);
                // 确保文件扩展名为.log（如果是.txt则重命名）
                QString processedPath = ensureLogExtension(filePath);
                addToProcessQueue(processedPath);
                fileProgress.setValue(++processedCount);
                QApplication::processEvents();
            }
        }

        // 第三步：更新工单状态
        progress.setValue(2);
        progress.setLabelText("正在更新工单状态...");
        QApplication::processEvents();

        // 更新所有工单的状态（基于实际文件夹状态）
        for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
            // 始终基于工单文件夹的实际状态更新计数
            it.value()->completedCount = countAllLogFilesInOrderFolder(it.key(), true);
        }

        // 更新表格显示
        updateTable();

        // 恢复文件监控
        progress.setValue(3);
        progress.setLabelText("正在恢复文件监控...");
        QApplication::processEvents();

        // 重新添加所有需要监控的路径
        QStringList pathsToWatch;
        int watchFolderCount = 0;
        int workOrderFolderCount = 0;

        // 添加监控路径
        if (QDir(watchPath).exists()) {
            pathsToWatch << watchPath;
            watchFolderCount++;
            LOG_INFO(QString("添加监控路径: %1").arg(watchPath));
        }

        // 添加所有工单文件夹路径及其子文件夹
        for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
            QString orderPath;
            if (isNetworkPath(archivePath)) {
                if (archivePath.startsWith("\\")) {
                    orderPath = archivePath + "\\" + it.key();
                } else if (archivePath.startsWith("\\\\")) {
                    orderPath = archivePath + "\\" + it.key();
                } else {
                    orderPath = archivePath + "/" + it.key();
                }
            } else {
                QDir archiveDir(archivePath);
                orderPath = archiveDir.filePath(it.key());
            }

            if (QDir(orderPath).exists()) {
                pathsToWatch << orderPath;
                workOrderFolderCount++;
                LOG_INFO(QString("添加工单路径: %1").arg(orderPath));

                // 添加completed子文件夹
                QString completedPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        completedPath = orderPath + "\\completed";
                    } else if (archivePath.startsWith("\\\\")) {
                        completedPath = orderPath + "\\completed";
                    } else {
                        completedPath = orderPath + "/completed";
                    }
                } else {
                    completedPath = QDir(orderPath).filePath("completed");
                }

                if (QDir(completedPath).exists()) {
                    pathsToWatch << completedPath;
                    workOrderFolderCount++;
                    LOG_INFO(QString("添加completed路径: %1").arg(completedPath));
                }

                // 添加overlog子文件夹
                QString overlogPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        overlogPath = orderPath + "\\overlog";
                    } else if (archivePath.startsWith("\\\\")) {
                        overlogPath = orderPath + "\\overlog";
                    } else {
                        overlogPath = orderPath + "/overlog";
                    }
                } else {
                    overlogPath = QDir(orderPath).filePath("overlog");
                }

                if (QDir(overlogPath).exists()) {
                    pathsToWatch << overlogPath;
                    workOrderFolderCount++;
                    LOG_INFO(QString("添加overlog路径: %1").arg(overlogPath));
                }
            }
        }

        // 添加所有路径到监控
        if (!pathsToWatch.isEmpty()) {
            fileWatcher->addPaths(pathsToWatch);
            LOG_INFO(QString("监控路径统计:\n"
                             "- 监控文件夹: %1\n"
                             "- 工单文件夹: %2\n"
                             "- 总计: %3")
                         .arg(watchFolderCount)
                         .arg(workOrderFolderCount)
                         .arg(pathsToWatch.size()));
        }

        LOG_INFO("刷新操作完成");
        QMessageBox::information(this, "完成",
                                 QString("刷新完成\n"
                                         "- 处理了 %1 个未归档文件\n"
                                         "- 恢复监控路径统计：\n"
                                         "  · 监控文件夹: %2\n"
                                         "  · 工单文件夹: %3\n"
                                         "  · 总计: %4")
                                     .arg(files.size())
                                     .arg(watchFolderCount)
                                     .arg(workOrderFolderCount)
                                     .arg(pathsToWatch.size()));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("刷新操作时发生异常: %1").arg(e.what()));
        QMessageBox::critical(this, "错误",
                              QString("刷新操作时发生错误：%1").arg(e.what()));

        // 确保恢复所有文件监控
        QStringList pathsToWatch;
        if (QDir(watchPath).exists()) {
            pathsToWatch << watchPath;
        }

        // 恢复工单文件夹监控
        for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
            QString orderPath = archivePath + "/" + it.key();
            if (QDir(orderPath).exists()) {
                pathsToWatch << orderPath;
            }
        }

        if (!pathsToWatch.isEmpty()) {
            fileWatcher->addPaths(pathsToWatch);
            LOG_INFO(QString("异常处理：已恢复监控 %1 个路径").arg(pathsToWatch.size()));
        }
    }
}
void MainWindow::onTableItemDoubleClicked(QTableWidgetItem* item)//行处理
{
    // 只处理第一列的双击事件
    if (item && item->column() == 0) {
        moveRowToTop(item->row());
    }
}
void MainWindow::onTableCustomContextMenuRequested(const QPoint& pos)
{
    QTableWidgetItem* item = tableWidget->itemAt(pos);
    if (item && item->column() == 0) {
        moveRowToBottom(item->row());
    }
}
void MainWindow::moveRowToTop(int sourceRow)
{
    if (sourceRow <= 0 || sourceRow >= tableWidget->rowCount()) {
        return;
    }

    // 保存要移动的行的所有单元格属性
    QList<QTableWidgetItem*> rowItems;
    for (int col = 0; col < tableWidget->columnCount() - 1; ++col) {
        QTableWidgetItem* oldItem = tableWidget->item(sourceRow, col);
        QTableWidgetItem* newItem = new QTableWidgetItem(*oldItem); // 创建完整副本
        rowItems.append(newItem);
    }

    // 将其他行向下移动一行
    for (int row = sourceRow; row > 0; --row) {
        for (int col = 0; col < tableWidget->columnCount() - 1; ++col) {
            QTableWidgetItem* item = tableWidget->takeItem(row - 1, col);
            if (item) {
                tableWidget->setItem(row, col, item);
            }
        }
        // 重新创建操作按钮
        createActionButtons(row);
    }

    // 将保存的行数据设置到第一行
    for (int col = 0; col < tableWidget->columnCount() - 1; ++col) {
        tableWidget->setItem(0, col, rowItems[col]);
        // 确保单元格不可编辑
        rowItems[col]->setFlags(rowItems[col]->flags() & ~Qt::ItemIsEditable);
    }

    // 为第一行创建操作按钮
    createActionButtons(0);

    // 更新选中行
    tableWidget->selectRow(0);
    LOG_INFO(QString("工单已移至顶部 - 行号: %1").arg(sourceRow));
}

void MainWindow::moveRowToBottom(int sourceRow)
{
    if (sourceRow < 0 || sourceRow >= tableWidget->rowCount() - 1) {
        return;
    }

    // 保存要移动的行的所有单元格属性
    QList<QTableWidgetItem*> rowItems;
    for (int col = 0; col < tableWidget->columnCount() - 1; ++col) {
        QTableWidgetItem* oldItem = tableWidget->item(sourceRow, col);
        QTableWidgetItem* newItem = new QTableWidgetItem(*oldItem); // 创建完整副本
        rowItems.append(newItem);
    }

    // 将其他行向上移动一行
    for (int row = sourceRow; row < tableWidget->rowCount() - 1; ++row) {
        for (int col = 0; col < tableWidget->columnCount() - 1; ++col) {
            QTableWidgetItem* item = tableWidget->takeItem(row + 1, col);
            if (item) {
                tableWidget->setItem(row, col, item);
            }
        }
        // 重新创建操作按钮
        createActionButtons(row);
    }

    // 将保存的行数据设置到最后一行
    int lastRow = tableWidget->rowCount() - 1;
    for (int col = 0; col < tableWidget->columnCount() - 1; ++col) {
        tableWidget->setItem(lastRow, col, rowItems[col]);
        // 确保单元格不可编辑
        rowItems[col]->setFlags(rowItems[col]->flags() & ~Qt::ItemIsEditable);
    }

    // 为最后一行创建操作按钮
    createActionButtons(lastRow);

    // 更新选中行
    tableWidget->selectRow(lastRow);
    LOG_INFO(QString("工单已移至底部 - 行号: %1").arg(sourceRow));
}
void MainWindow::createActionButtons(int row)
{
    QWidget* widget = new QWidget();
    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(5, 0, 5, 0);
    layout->setSpacing(5);

    // 修改总量按钮
    QPushButton* modifyBtn = new QPushButton("修改总量");
    modifyBtn->setProperty("row", row);
    connect(modifyBtn, &QPushButton::clicked, [this, row]() {
        onModifyTotalCount(row, tableWidget);
    });

    // 停止计数按钮
    QPushButton* stopBtn = new QPushButton("停止计数");
    stopBtn->setProperty("row", row);
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStopCounting);

    // 删除工单按钮
    QPushButton* deleteBtn = new QPushButton("删除工单");
    deleteBtn->setProperty("row", row);
    connect(deleteBtn, &QPushButton::clicked, [this, row]() {
        onDeleteWorkOrder(row, tableWidget);
    });

    // 新增查看溢出按钮
    QPushButton* checkOverflowBtn = new QPushButton("查看溢出");
    checkOverflowBtn->setProperty("row", row);
    connect(checkOverflowBtn, &QPushButton::clicked, this, &MainWindow::onCheckOverflow);

    layout->addWidget(modifyBtn);
    layout->addWidget(stopBtn);
    layout->addWidget(deleteBtn);
    layout->addWidget(checkOverflowBtn);

    tableWidget->setCellWidget(row, 5, widget);
}

// 新增处理查看溢出的槽函数
void MainWindow::onCheckOverflow()
{
    try {
        QPushButton* button = qobject_cast<QPushButton*>(sender());
        if (!button) return;

        int row = button->property("row").toInt();
        
        // 获取按钮来源的表格
        QWidget* tableWidget = button->property("tableWidget").value<QWidget*>();
        QTableWidget* table = qobject_cast<QTableWidget*>(tableWidget);
        
        if (!table || !table->item(row, 0)) {
            return; // 无法确定工单号
        }
        
        QString orderNumber = table->item(row, 0)->text();

        // 构造overlog文件夹路径
        QDir archiveDir(archivePath);
        QString orderPath = archiveDir.filePath(orderNumber);
        QString overlogPath = QDir(orderPath).filePath("overlog");

        // 检查overlog文件夹是否存在
        QDir overlogDir(overlogPath);
        if (!overlogDir.exists()) {
            QMessageBox::information(this, "提示",
                                     QString("工单 %1 没有溢出日志").arg(orderNumber));
            LOG_INFO(QString("工单 %1 无溢出日志文件夹").arg(orderNumber));
            return;
        }

        // 检查是否有log文件
        QStringList filters;
        filters << "*.log";
        QStringList logFiles = overlogDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot);

        if (logFiles.isEmpty()) {
            QMessageBox::information(this, "提示",
                                     QString("工单 %1 的溢出文件夹为空").arg(orderNumber));
            LOG_INFO(QString("工单 %1 的溢出文件夹为空").arg(orderNumber));
            return;
        }

        // 使用系统默认程序打开文件夹
#ifdef Q_OS_WIN
        QProcess::startDetached("explorer.exe", {QDir::toNativeSeparators(overlogPath)});
#else
        QProcess::startDetached("xdg-open", {overlogPath});
#endif

        LOG_INFO(QString("已打开工单 %1 的溢出文件夹").arg(orderNumber));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("查看溢出文件夹时发生异常: %1").arg(e.what()));
        QMessageBox::critical(this, "错误",
                              QString("查看溢出文件夹时发生错误：%1").arg(e.what()));
    }
}

void MainWindow::onAddWorkOrder()
{
    bool ok;
    QString orderNumber = QInputDialog::getText(this, "添加工单",
                                                "请输入工单号:", QLineEdit::Normal,
                                                "", &ok);
    if (!ok || orderNumber.isEmpty()) {
        return;
    }

    // 检查工单是否已存在
    if (workOrders.contains(orderNumber)) {
        QMessageBox::warning(this, "警告", "工单已存在！");
        return;
    }

    // 从NUM.txt获取总数
    int total = getWorkOrderTotal(orderNumber);
    // 如果NUM.txt没有找到或无效，则手动输入
    if (total <= 0) {
        total = QInputDialog::getInt(this, "设置总数",
                                     QString("工单 %1 未在NUM.txt中找到总数，\n请手动输入总数:").arg(orderNumber),
                                     0, 0, 999999, 1, &ok);
        if (!ok) {
            return;
        }
        
        // 手动输入总数后，创建NUM.txt文件
        if (!writeWorkOrderTotalToNumFile(orderNumber, total)) {
            // 创建全屏NUM.txt文件操作失败报警
            FullScreenAlertDialog* errorDialog = new FullScreenAlertDialog(
                FullScreenAlertDialog::FileOperationError,
                "NUM.txt文件操作失败",
                QString("NUM.txt文件创建失败！\n\n工单: %1\n总数: %2\n\n请检查工单文件夹权限或网络连接。").arg(orderNumber).arg(total),
                this
            );
            
            errorDialog->setOrderNumber(orderNumber);
            errorDialog->setAutoClose(0); // 不自动关闭，需要用户手动确认
            
            // 连接关闭信号，确保对话框被正确清理
            connect(errorDialog, &FullScreenAlertDialog::closed, [errorDialog]() {
                errorDialog->deleteLater();
            });
            
            connect(errorDialog, &FullScreenAlertDialog::confirmed, [errorDialog]() {
                errorDialog->deleteLater();
            });
            
            LOG_ERROR(QString("NUM.txt创建失败 - 工单: %1, 总数: %2").arg(orderNumber).arg(total));
        } else {
            LOG_INFO(QString("NUM.txt创建成功 - 工单: %1, 总数: %2").arg(orderNumber).arg(total));
        }
    } else {
        // 找到NUM.txt的总数，直接使用，无需确认
        LOG_INFO(QString("工单 %1 从NUM.txt获取总数: %2，直接使用").arg(orderNumber).arg(total));
    }

    // 创建新工单
    WorkOrder* order = new WorkOrder(orderNumber, total);
    
    // 统计实际完成数量
    order->completedCount = countAllLogFilesInOrderFolder(orderNumber, true);
    
    workOrders.insert(orderNumber, order);
    suppressedAutoSyncOrders.remove(orderNumber);

    // 检查并创建工单文件夹，同时添加监控
    checkAndCreateOrderFolder(orderNumber);

    // 分类工单到相应区域
    classifyWorkOrder(order);

    // 更新表格显示
    updateTable();
    sendNewWorkOrderToCentral(orderNumber);
    LOG_INFO(QString("已添加工单: %1, 总数: %2").arg(orderNumber).arg(total));

    // 立即设置新工单的监控
    QTimer::singleShot(100, this, [this, orderNumber]() {
        setupWorkOrderMonitoring();
    });
}

void MainWindow::onStopCounting()
{
    try {
        QPushButton* button = qobject_cast<QPushButton*>(sender());
        if (!button) return;

        int row = button->property("row").toInt();
        
        // 获取按钮来源的表格
        QWidget* tableWidget = button->property("tableWidget").value<QWidget*>();
        QTableWidget* table = qobject_cast<QTableWidget*>(tableWidget);
        
        if (!table || !table->item(row, 0)) {
            return; // 无法确定工单号
        }
        
        QString orderNumber = table->item(row, 0)->text();

        if (workOrders.contains(orderNumber)) {
            workOrders[orderNumber]->isCountingStopped =
                !workOrders[orderNumber]->isCountingStopped;
            button->setText(workOrders[orderNumber]->isCountingStopped ?
                                "继续计数" : "停止计数");

            qDebug() << QString("工单 %1 计数状态: %2")
                            .arg(orderNumber)
                            .arg(workOrders[orderNumber]->isCountingStopped ? "已停止" : "已继续");
        }
    } catch (const std::exception& e) {
        QMessageBox::critical(this, "错误",
                              QString("切换计数状态时发生异常：%1").arg(e.what()));
    }
}

void MainWindow::updateTable()
{
    try {
        // 更新双表格
        updateIncompleteTable();
        updateCompletedTable();
        
        // 保持原有表格的兼容性（隐藏状态）
        tableWidget->setRowCount(workOrders.size());
        int row = 0;
        for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
            updateRow(row, it.value());
            createActionButtons(row);
            row++;
        }

    } catch (const std::exception& e) {
        qDebug() << "更新表格时发生异常:" << e.what();
    }
}

void MainWindow::updateRow(int row, WorkOrder* order)
{
    try {
        // 始终基于工单文件夹的实际状态更新计数
        int actualCount = countAllLogFilesInOrderFolder(order->orderNumber, true);
        int oldCount = order->completedCount;
        order->completedCount = actualCount;

        // 检查是否溢出（新计数超过总数）
        if (actualCount > order->totalCount) {
            LOG_WARNING(QString("检测到工单溢出 - 工单: %1, 当前: %2, 总数: %3")
                            .arg(order->orderNumber)
                            .arg(actualCount)
                            .arg(order->totalCount));

            // 触发溢出报警
            showOverflowAlert(order->orderNumber);
        } else if (oldCount > order->totalCount && actualCount <= order->totalCount) {
            // 工单从溢出状态恢复到正常状态
            LOG_INFO(QString("工单 %1 溢出问题已解决 - 当前: %2, 总数: %3")
                    .arg(order->orderNumber)
                    .arg(actualCount)
                    .arg(order->totalCount));
            
            // 通知溢出报警管理器
            OverflowAlertManager::getInstance()->onOverflowResolved(order->orderNumber);
        }

        // 获取各个子目录的文件数量用于颜色判断
        QString orderPath;
        if (isNetworkPath(archivePath)) {
            if (archivePath.startsWith("\\")) {
                orderPath = archivePath + "\\" + order->orderNumber;
            } else if (archivePath.startsWith("\\\\")) {
                orderPath = archivePath + "\\" + order->orderNumber;
            } else {
                orderPath = archivePath + "/" + order->orderNumber;
            }
        } else {
            QDir archiveDir(archivePath);
            orderPath = archiveDir.filePath(order->orderNumber);
        }

        QDir orderDir(orderPath);
        QStringList filters;
        filters << "*.log";
        int normalCount = orderDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();

        // 统计overlog文件夹下的文件数量
        QString overlogPath;
        if (isNetworkPath(archivePath)) {
            if (archivePath.startsWith("\\")) {
                overlogPath = orderPath + "\\overlog";
            } else if (archivePath.startsWith("\\\\")) {
                overlogPath = orderPath + "\\overlog";
            } else {
                overlogPath = orderPath + "/overlog";
            }
        } else {
            overlogPath = QDir(orderPath).filePath("overlog");
        }

        int overflowCount = 0;
        if (QDir(overlogPath).exists()) {
            QDir overflowDir(overlogPath);
            overflowCount = overflowDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
        }

        QTableWidgetItem* items[] = {
            new QTableWidgetItem(order->orderNumber),
            new QTableWidgetItem(QString::number(order->totalCount)),
            new QTableWidgetItem(QString::number(order->completedCount)),
            new QTableWidgetItem(QString::number(order->failedCount)),
            new QTableWidgetItem(QString::number(order->getRemainingCount()))
        };

        // 计算剩余比例
        int remaining = order->getRemainingCount();
        double remainingRatio = (double)remaining / order->totalCount;

        // 设置颜色
        QColor rowColor;
        if (overflowCount > 0) {
            // 有溢出文件 - 红色
            rowColor = QColor(255, 200, 200);  // 浅红色
            LOG_WARNING(QString("工单 %1 完成数量超过总数").arg(order->orderNumber));
        } else if (remaining == 0) {
            // 剩余数为0 - 绿色
            rowColor = QColor(200, 255, 200);  // 浅绿色
            LOG_INFO(QString("工单 %1 已完成").arg(order->orderNumber));
        } else if (remainingRatio <= 0.1) {
            // 剩余不足10% - 黄色
            rowColor = QColor(255, 255, 200);  // 浅黄色
            LOG_INFO(QString("工单 %1 剩余数量不足10%").arg(order->orderNumber));
        }

        // 应用颜色到所有列
        for (int i = 0; i < 5; i++) {
            items[i]->setFlags(items[i]->flags() & ~Qt::ItemIsEditable);
            if (rowColor.isValid()) {
                items[i]->setBackground(rowColor);
            }
            tableWidget->setItem(row, i, items[i]);
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("更新行时发生异常: %1").arg(e.what()));
        qDebug() << "更新行时发生异常:" << e.what();
    }
}

void MainWindow::checkAndCreateOrderFolder(const QString& orderNumber)
{
    try {
        QDir archiveDir(archivePath);
        QString orderPath = archiveDir.filePath(orderNumber);
        QDir dir;
        if (!dir.exists(orderPath)) {
            if (isNetworkPath(archivePath)) {
                // 网络路径：尝试创建文件夹
                LOG_INFO(QString("尝试在网络路径创建工单文件夹: %1").arg(orderPath));
                if (createNetworkFolder(orderPath)) {
                    LOG_INFO(QString("成功在网络路径创建工单文件夹: %1").arg(orderPath));
                } else {
                    // 如果创建失败，检查父目录是否可访问
                    QString parentPath = QFileInfo(orderPath).path();
                    if (!QDir(parentPath).exists()) {
                        throw std::runtime_error(QString("网络路径无法访问: %1").arg(parentPath).toStdString());
                    }
                    LOG_INFO(QString("网络路径工单文件夹检查完成（创建失败但可访问）: %1").arg(orderPath));
                }
            } else {
                // 本地路径：尝试创建
                if (!dir.mkpath(orderPath)) {
                    throw std::runtime_error("无法创建工单文件夹");
                }
                LOG_INFO(QString("已创建本地工单文件夹: %1").arg(orderPath));
            }

            // 尝试添加到文件监控（网络路径可能无法监控，但不影响程序运行）
            if (!fileWatcher->addPath(orderPath)) {
                LOG_WARNING(QString("无法添加文件夹到监控: %1").arg(orderPath));
            } else {
                LOG_INFO(QString("已添加新工单文件夹到监控: %1").arg(orderPath));
            }

            // 同时监控completed和overlog子文件夹
            QString completedPath = QDir(orderPath).filePath("completed");
            QString overlogPath = QDir(orderPath).filePath("overlog");

            if (QDir(completedPath).exists()) {
                if (fileWatcher->addPath(completedPath)) {
                    LOG_INFO(QString("已添加completed路径到监控: %1").arg(completedPath));
                }
            }

            if (QDir(overlogPath).exists()) {
                if (fileWatcher->addPath(overlogPath)) {
                    LOG_INFO(QString("已添加overlog路径到监控: %1").arg(overlogPath));
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("创建工单文件夹时发生异常: %1").arg(e.what()));
        throw;
    }
}

int MainWindow::countLogFiles(const QString& orderNumber)
{
    try {
        return countAllLogFilesInOrderFolder(orderNumber);
    } catch (const std::exception& e) {
        qDebug() << "统计日志文件时发生异常:" << e.what();
        return 0;
    }
}

int MainWindow::countAllLogFilesInOrderFolder(const QString& orderNumber, bool includeOverlog) const
{
    try {
        // 手动构造网络路径，避免QDir::filePath()的问题
        QString orderPath;

        if (isNetworkPath(archivePath)) {
            // 网络路径：手动构造
            if (archivePath.startsWith("\\")) {
                // 单个反斜杠开头的网络路径
                orderPath = archivePath + "\\" + orderNumber;
            } else if (archivePath.startsWith("\\\\")) {
                // 双反斜杠开头的网络路径
                orderPath = archivePath + "\\" + orderNumber;
            } else {
                // 正斜杠开头的网络路径
                orderPath = archivePath + "/" + orderNumber;
            }
        } else {
            // 本地路径：使用QDir::filePath()
            QDir archiveDir(archivePath);
            orderPath = archiveDir.filePath(orderNumber);
        }

        QDir orderDir(orderPath);
        if (!orderDir.exists()) {
            LOG_WARNING(QString("工单文件夹不存在: %1").arg(orderPath));
            return 0;
        }

        int totalCount = 0;
        QStringList filters;
        filters << "*.log";

        // 1. 统计工单文件夹根目录下的log文件
        int mainCount = orderDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
        totalCount += mainCount;
        //(QString("工单 %1 根目录log文件数: %2").arg(orderNumber).arg(mainCount));

        // 2. 统计completed文件夹下的log文件
        QString completedPath;
        if (isNetworkPath(archivePath)) {
            if (archivePath.startsWith("\\")) {
                completedPath = orderPath + "\\completed";
            } else if (archivePath.startsWith("\\\\")) {
                completedPath = orderPath + "\\completed";
            } else {
                completedPath = orderPath + "/completed";
            }
        } else {
            completedPath = QDir(orderPath).filePath("completed");
        }

        QDir completedDir(completedPath);
        int completedCount = 0;
        if (completedDir.exists()) {
            completedCount = completedDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
            totalCount += completedCount;
            //LOG_INFO(QString("工单 %1 completed文件夹log文件数: %2").arg(orderNumber).arg(completedCount));
        }

        // 3. 统计overlog文件夹下的log文件（根据参数决定）
        int overlogCount = 0;
        if (includeOverlog) {
            QString overlogPath;
            if (isNetworkPath(archivePath)) {
                if (archivePath.startsWith("\\")) {
                    overlogPath = orderPath + "\\overlog";
                } else if (archivePath.startsWith("\\\\")) {
                    overlogPath = orderPath + "\\overlog";
                } else {
                    overlogPath = orderPath + "/overlog";
                }
            } else {
                overlogPath = QDir(orderPath).filePath("overlog");
            }

            QDir overlogDir(overlogPath);
            if (overlogDir.exists()) {
                overlogCount = overlogDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
                totalCount += overlogCount;
            //    LOG_INFO(QString("工单 %1 overlog文件夹log文件数: %2").arg(orderNumber).arg(overlogCount));
            }
        }

        // if (includeOverlog) {
        //     LOG_INFO(QString("工单 %1 总log文件数: %2 (根目录:%3 + completed:%4 + overlog:%5)")
        //                  .arg(orderNumber)
        //                  .arg(totalCount)
        //                  .arg(mainCount)
        //                  .arg(completedCount)
        //                  .arg(overlogCount));
        // } else {
        //     LOG_INFO(QString("工单 %1 有效log文件数: %2 (根目录:%3 + completed:%4)")
        //                  .arg(orderNumber)
        //                  .arg(totalCount)
        //                  .arg(mainCount)
        //                  .arg(completedCount));
        // }

        return totalCount;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("统计工单所有log文件时发生异常: %1").arg(e.what()));
        return 0;
    }
}

void MainWindow::updateWorkOrderCount(const QString& orderNumber)
{
    try {
        if (!workOrders.contains(orderNumber)) {
            return;
        }

        // 始终基于工单文件夹的实际状态更新计数
        int actualCount = countAllLogFilesInOrderFolder(orderNumber, true);
        int oldCount = workOrders[orderNumber]->completedCount;
        workOrders[orderNumber]->completedCount = actualCount;

        LOG_INFO(QString("更新工单计数 - 工单: %1, 旧计数: %2, 新计数: %3").arg(orderNumber).arg(oldCount).arg(actualCount));

        // 检查是否溢出（新计数超过总数）
        if (actualCount > workOrders[orderNumber]->totalCount) {
            LOG_WARNING(QString("检测到工单溢出 - 工单: %1, 当前: %2, 总数: %3")
                            .arg(orderNumber)
                            .arg(actualCount)
                            .arg(workOrders[orderNumber]->totalCount));

            // 触发溢出报警
            showOverflowAlert(orderNumber);
        } else if (oldCount > workOrders[orderNumber]->totalCount && actualCount <= workOrders[orderNumber]->totalCount) {
            // 工单从溢出状态恢复到正常状态
            LOG_INFO(QString("工单 %1 溢出问题已解决 - 当前: %2, 总数: %3")
                    .arg(orderNumber)
                    .arg(actualCount)
                    .arg(workOrders[orderNumber]->totalCount));
            
            // 通知溢出报警管理器
            OverflowAlertManager::getInstance()->onOverflowResolved(orderNumber);
        }

        updateTable();
    } catch (const std::exception& e) {
        LOG_ERROR(QString("更新工单计数时发生异常: %1").arg(e.what()));
    }
}

void MainWindow::updateWorkOrderCountFromPath(const QString& path)
{
    try {
        // 从路径中提取工单号
        QString orderNumber;
        if (path.contains(archivePath)) {
            QString relativePath = path;
            if (relativePath.startsWith(archivePath)) {
                relativePath = relativePath.mid(archivePath.length());
                // 移除开头的路径分隔符
                if (relativePath.startsWith("/") || relativePath.startsWith("\\")) {
                    relativePath = relativePath.mid(1);
                }
                // 提取工单号（路径的第一部分）
                QStringList pathParts = relativePath.split(QRegularExpression("[\\\\/]"), Qt::SkipEmptyParts);
                if (!pathParts.isEmpty()) {
                    orderNumber = pathParts[0];
                }
            }
        }

        if (!orderNumber.isEmpty() && workOrders.contains(orderNumber)) {
            LOG_INFO(QString("从路径更新工单计数 - 路径: %1, 工单: %2").arg(path).arg(orderNumber));
            updateWorkOrderCount(orderNumber);
        } else {
            LOG_INFO(QString("路径不包含有效工单号或工单不存在 - 路径: %1").arg(path));
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("从路径更新工单计数时发生异常: %1, 路径: %2").arg(e.what()).arg(path));
    }
}

void MainWindow::syncAllWorkOrderCounts()
{
    try {
        LOG_INFO("开始定期同步所有工单计数");

        syncCounter++;
        if (syncCounter >= syncValidationInterval) {
            syncCounter = 0;
            maybeStartAsyncSizeValidation();
        }

        bool hasChanges = false;
        for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
            QString orderNumber = it.key();
            WorkOrder* order = it.value();

            // 双重保险：先转换工单文件夹下的txt文件为log文件
            convertTxtToLogInOrderFolder(orderNumber);

            // 获取实际文件数量
            int actualCount = countAllLogFilesInOrderFolder(orderNumber, true);

            // 如果数量有变化，更新计数
            if (actualCount != order->completedCount) {
                LOG_INFO(QString("检测到工单计数变化 - 工单: %1, 旧计数: %2, 新计数: %3")
                             .arg(orderNumber)
                             .arg(order->completedCount)
                             .arg(actualCount));

                order->completedCount = actualCount;
                hasChanges = true;

                // 检查是否溢出（新计数超过总数）
                if (actualCount > order->totalCount) {
                    LOG_WARNING(QString("检测到工单溢出 - 工单: %1, 当前: %2, 总数: %3")
                                    .arg(orderNumber)
                                    .arg(actualCount)
                                    .arg(order->totalCount));

                    // 触发溢出报警
                    showOverflowAlert(orderNumber);
                }
            }
            
            // 无论计数是否有变化，都重新分类工单到正确的区域
            // 这确保了工单在正确的区域显示
            classifyWorkOrder(order);
        }

        // 如果有变化，更新表格显示
        if (hasChanges) {
            updateTable();
            LOG_INFO("定期同步完成，已更新表格显示");
        } else {
            LOG_INFO("定期同步完成，无变化");
        }

    } catch (const std::exception& e) {
        LOG_ERROR(QString("定期同步工单计数时发生异常: %1").arg(e.what()));
    }
}

void MainWindow::maybeStartAsyncSizeValidation()
{
    if (copyMachineLogPath.isEmpty() || workordersRootPath.isEmpty()) {
        LOG_WARNING("大小校验配置不完整(Path/CopyMachineLogPath 或 Path/WorkordersRoot 为空)，跳过");
        return;
    }
    if (gobRoute1.isEmpty() && gobRoute2.isEmpty()) {
        LOG_WARNING("大小校验配置不完整(Path/GobRoute1 与 Path/GobRoute2 均为空)，跳过");
        return;
    }

    bool expected = false;
    if (!sizeValidationRunning.compare_exchange_strong(expected, true)) {
        LOG_INFO("大小校验已在进行中，本轮跳过（单飞）");
        return;
    }

    QStringList orders;
    orders.reserve(workOrders.size());
    for (auto it = workOrders.begin(); it != workOrders.end(); ++it)
        orders << it.key();
    if (orders.isEmpty()) {
        sizeValidationRunning = false;
        return;
    }

    const QString wr = workordersRootPath;
    const QString cm = copyMachineLogPath;
    const QString g1 = gobRoute1;
    const QString g2 = gobRoute2;

    QtConcurrent::run([this, orders, wr, cm, g1, g2]() {
        QStringList errors;
        for (const QString& orderNumber : orders) {
            const QString copyLogPath = QDir(cm).filePath(orderNumber + QLatin1String(".txt"));
            if (!QFile::exists(copyLogPath)) {
                LOG_INFO(QString("大小校验跳过工单 %1: 拷贝机记录不存在 %2").arg(orderNumber).arg(copyLogPath));
                continue;
            }
            QFile f(copyLogPath);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                LOG_INFO(QString("大小校验跳过工单 %1: 无法打开拷贝机记录 %2").arg(orderNumber).arg(copyLogPath));
                continue;
            }
            QTextStream in(&f);
            QSet<qint64> distinct;
            int lineNum = 0;
            while (!in.atEnd()) {
                const QString raw = in.readLine();
                lineNum++;
                const QString line = raw.trimmed();
                if (line.isEmpty())
                    continue;
                const QStringList parts = line.split(QLatin1Char(';'), Qt::KeepEmptyParts);
                if (parts.size() < 5) {
                    LOG_WARNING(QString("大小校验跳过行(字段不足) 工单 %1 行 %2").arg(orderNumber).arg(lineNum));
                    continue;
                }
                qint64 v = 0;
                if (!parseField5AsCopySizeMiB(parts[4], v)) {
                    LOG_WARNING(QString("大小校验跳过行(第5字段无效或为0/非数字) 工单 %1 行 %2").arg(orderNumber).arg(lineNum));
                    continue;
                }
                distinct.insert(v);
            }
            f.close();

            if (distinct.isEmpty()) {
                LOG_INFO(QString("大小校验跳过工单 %1: 拷贝机记录无有效大小行").arg(orderNumber));
                continue;
            }
            if (distinct.size() > 1) {
                QList<qint64> vals;
                for (qint64 x : distinct)
                    vals.append(x);
                std::sort(vals.begin(), vals.end());
                QStringList valsStr;
                for (qint64 x : vals)
                    valsStr << QString::number(x);
                const QString errMsg = QStringLiteral("工单 [%1] 拷贝一致性校验失败！\n发现不同的拷贝大小数值，存在拷贝异常。\n数值详情：%2")
                                           .arg(orderNumber)
                                           .arg(valsStr.join(QLatin1String(", ")));
                LOG_WARNING(errMsg);
                errors << errMsg;
                continue;
            }

            const qint64 actualMiB = *distinct.begin();

            const QString gobListPath = buildWorkOrderGobListPath(wr, orderNumber);
            if (gobListPath.isEmpty() || !QFile::exists(gobListPath)) {
                LOG_INFO(QString("大小校验跳过工单 %1: Gob列表文件不存在 %2").arg(orderNumber).arg(gobListPath));
                continue;
            }
            QString firstLine;
            if (!readFirstNonEmptyGobLine(gobListPath, firstLine)) {
                LOG_INFO(QString("大小校验跳过工单 %1: Gob列表文件无有效行 %2").arg(orderNumber).arg(gobListPath));
                continue;
            }
            QString subfolder;
            QString baseNoExt;
            if (!parseGobLineToParts(firstLine, subfolder, baseNoExt)) {
                LOG_INFO(QString("大小校验跳过工单 %1: Gob名行格式错误").arg(orderNumber));
                continue;
            }

            const qint64 bytes = resolveGobBytesOnRoutes(g1, g2, subfolder, baseNoExt);
            if (bytes < 0) {
                LOG_ERROR(QString("大小校验工单 %1: 无法获取服务器源文件大小(静默重试已用尽)").arg(orderNumber));
                continue;
            }
            const qint64 expectedMiB = bytes / (1024LL * 1024LL);
            const qint64 diffMiB = actualMiB > expectedMiB ? (actualMiB - expectedMiB) : (expectedMiB - actualMiB);
            if (diffMiB > 2) {
                const QString errMsg = QStringLiteral("工单 [%1] 文件大小校验失败！\n服务器源文件计算值：%2\n拷贝机实际值：%3\n允许误差：2\n实际偏差：%4\n请检查源文件是否更新或拷贝异常。")
                                           .arg(orderNumber)
                                           .arg(expectedMiB)
                                           .arg(actualMiB)
                                           .arg(diffMiB);
                LOG_ERROR(errMsg);
                errors << errMsg;
            } else {
                LOG_INFO(QString("大小校验通过 工单 %1 实际=%2 期望=%3 偏差=%4(<=5)")
                             .arg(orderNumber)
                             .arg(actualMiB)
                             .arg(expectedMiB)
                             .arg(diffMiB));
            }
        }

        const QStringList errCopy = errors;
        QTimer::singleShot(0, this, [this, errCopy]() {
            sizeValidationRunning = false;
            onSizeValidationFinished(errCopy);
        });
    });
}

void MainWindow::onSizeValidationFinished(const QStringList& errorMessages)
{
    if (!errorMessages.isEmpty()) {
        const QString msg = errorMessages.join(QStringLiteral("\n\n"));
        QMessageBox::warning(this, QStringLiteral("大小校验异常"), msg);
    }
}

void MainWindow::convertTxtToLogInOrderFolder(const QString& orderNumber)
{
    try {
        // 手动构造网络路径，避免QDir::filePath()的问题
        QString orderPath;

        if (isNetworkPath(archivePath)) {
            // 网络路径：手动构造
            if (archivePath.startsWith("\\")) {
                orderPath = archivePath + "\\" + orderNumber;
            } else if (archivePath.startsWith("\\\\")) {
                orderPath = archivePath + "\\" + orderNumber;
            } else {
                orderPath = archivePath + "/" + orderNumber;
            }
        } else {
            // 本地路径：使用QDir::filePath()
            QDir archiveDir(archivePath);
            orderPath = archiveDir.filePath(orderNumber);
        }

        QDir orderDir(orderPath);
        if (!orderDir.exists()) {
            return; // 工单文件夹不存在，直接返回
        }

        // 需要检查的文件夹列表：根目录、completed、overlog
        QStringList foldersToCheck;
        foldersToCheck << orderPath; // 根目录

        // 添加completed文件夹
        QString completedPath;
        if (isNetworkPath(archivePath)) {
            if (archivePath.startsWith("\\")) {
                completedPath = orderPath + "\\completed";
            } else if (archivePath.startsWith("\\\\")) {
                completedPath = orderPath + "\\completed";
            } else {
                completedPath = orderPath + "/completed";
            }
        } else {
            completedPath = QDir(orderPath).filePath("completed");
        }
        foldersToCheck << completedPath;

        // 添加overlog文件夹
        QString overlogPath;
        if (isNetworkPath(archivePath)) {
            if (archivePath.startsWith("\\")) {
                overlogPath = orderPath + "\\overlog";
            } else if (archivePath.startsWith("\\\\")) {
                overlogPath = orderPath + "\\overlog";
            } else {
                overlogPath = orderPath + "/overlog";
            }
        } else {
            overlogPath = QDir(orderPath).filePath("overlog");
        }
        foldersToCheck << overlogPath;

        int totalConverted = 0;

        // 遍历每个文件夹
        for (const QString& folderPath : foldersToCheck) {
            QDir folderDir(folderPath);
            if (!folderDir.exists()) {
                continue; // 文件夹不存在，跳过
            }

            // 获取所有.txt文件
            QStringList filters;
            filters << "*.txt";
            QStringList txtFiles = folderDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot);

            for (const QString& txtFile : txtFiles) {
                // 检查是否是TimeCount*.txt文件，如果是则跳过
                if (txtFile.startsWith("TimeCount") && txtFile.endsWith(".txt")) {
                    LOG_INFO(QString("跳过TimeCount文件: %1").arg(txtFile));
                    continue;
                }

                QString txtFilePath = folderDir.filePath(txtFile);
                QString baseName = QFileInfo(txtFile).completeBaseName();
                QString logFilePath = folderDir.filePath(baseName + ".log");

                // 检查目标.log文件是否已存在
                if (QFile::exists(logFilePath)) {
                    LOG_WARNING(QString("目标.log文件已存在，跳过转换: %1").arg(logFilePath));
                    continue;
                }

                // 执行重命名
                if (QFile::rename(txtFilePath, logFilePath)) {
                    totalConverted++;
                    LOG_INFO(QString("成功转换txt文件为log文件: %1 -> %2")
                                .arg(txtFilePath)
                                .arg(logFilePath));
                } else {
                    LOG_ERROR(QString("转换txt文件失败: %1 -> %2")
                                 .arg(txtFilePath)
                                 .arg(logFilePath));
                }
            }
        }

        if (totalConverted > 0) {
            LOG_INFO(QString("工单 %1 转换完成，共转换 %2 个txt文件为log文件")
                        .arg(orderNumber)
                        .arg(totalConverted));
        }

    } catch (const std::exception& e) {
        LOG_ERROR(QString("转换工单文件夹txt文件时发生异常: %1, 工单: %2")
                     .arg(e.what())
                     .arg(orderNumber));
    }
}

QString MainWindow::getMaterialCodeFromSSDInfo(const QString& orderNumber, const QString& barcode)
{
    try {
        QString ssdInfoFilePath = QDir(ssdInfoPath).filePath(orderNumber + ".txt");
        QFile file(ssdInfoFilePath);

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            LOG_ERROR(QString("无法打开SSD信息文件: %1").arg(ssdInfoFilePath));
            return QString();
        }

        //LOG_INFO(QString("正在SSD信息文件中查找条码: %1, 文件路径: %2").arg(barcode).arg(ssdInfoFilePath));

        QTextStream in(&file);
        int lineNumber = 0;
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            lineNumber++;
            
            if (line.isEmpty()) continue;

            QStringList parts = line.split(";");
            LOG_INFO(QString("第%1行数据: %2, 分割后共%3部分").arg(lineNumber).arg(line).arg(parts.size()));
            
            // 查找条码在哪个位置
            int barcodeIndex = -1;
            for (int i = 0; i < parts.size(); ++i) {
                if (parts[i].trimmed() == barcode) {
                    barcodeIndex = i;
                    LOG_INFO(QString("在第%1行第%2个位置找到条码: %3").arg(lineNumber).arg(i + 1).arg(barcode));
                    break;
                }
            }
            
            if (barcodeIndex != -1) {
                // 条码后面就是料号
                if (barcodeIndex + 1 < parts.size()) {
                    QString materialCode = parts[barcodeIndex + 1].trimmed();
                    LOG_INFO(QString("找到条码 %1 对应的料号: %2").arg(barcode).arg(materialCode));
                    return materialCode;
                } else {
                    LOG_WARNING(QString("条码 %1 后面没有料号信息").arg(barcode));
                }
            }
        }

        LOG_WARNING(QString("在SSD信息文件中未找到条码 %1").arg(barcode));
        return QString();

    } catch (const std::exception& e) {
        LOG_ERROR(QString("读取SSD信息文件时发生异常: %1").arg(e.what()));
        return QString();
    }
}

bool MainWindow::loadOrderMaterialWhitelist(const QString& orderNumber)
{
    try {
        QString whitelistFilePath = QDir(archivePath).filePath(orderNumber + "/material_whitelist.json");
        QFile file(whitelistFilePath);

        if (!file.exists()) {
            LOG_INFO(QString("工单 %1 的料号白名单文件不存在").arg(orderNumber));
            return false;
        }

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            LOG_ERROR(QString("无法打开料号白名单文件: %1").arg(whitelistFilePath));
            return false;
        }

        QByteArray data = file.readAll();
        file.close();

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(data, &error);

        if (error.error != QJsonParseError::NoError) {
            LOG_ERROR(QString("解析料号白名单文件失败: %1").arg(error.errorString()));
            return false;
        }

        QJsonObject obj = doc.object();
        QJsonArray materialArray = obj["materialCodes"].toArray();

        QStringList materialCodes;
        for (const QJsonValue& value : materialArray) {
            materialCodes << value.toString();
        }

        orderMaterialWhitelist[orderNumber] = materialCodes;
        LOG_INFO(QString("成功加载工单 %1 的料号白名单: %2").arg(orderNumber).arg(materialCodes.join(", ")));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("加载料号白名单时发生异常: %1").arg(e.what()));
        return false;
    }
}

void MainWindow::saveOrderMaterialWhitelist(const QString& orderNumber, const QStringList& materialCodes)
{
    try {
        // 确保工单文件夹存在
        checkAndCreateOrderFolder(orderNumber);

        QString whitelistFilePath = QDir(archivePath).filePath(orderNumber + "/material_whitelist.json");

        QJsonObject obj;
        obj["orderNumber"] = orderNumber;
        obj["lastUpdated"] = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss");

        QJsonArray materialArray;
        for (const QString& materialCode : materialCodes) {
            materialArray.append(materialCode);
        }
        obj["materialCodes"] = materialArray;

        QJsonDocument doc(obj);

        QFile file(whitelistFilePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            LOG_ERROR(QString("无法创建料号白名单文件: %1").arg(whitelistFilePath));
            return;
        }

        file.write(doc.toJson());
        file.close();

        LOG_INFO(QString("成功保存工单 %1 的料号白名单: %2").arg(orderNumber).arg(materialCodes.join(", ")));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("保存料号白名单时发生异常: %1").arg(e.what()));
    }
}

bool MainWindow::loadOrderMaterialBlacklist(const QString& orderNumber)
{
    try {
        QString blacklistFilePath = QDir(archivePath).filePath(orderNumber + "/material_blacklist.json");
        
        QFile file(blacklistFilePath);
        if (!file.exists()) {
            LOG_INFO(QString("工单 %1 的料号黑名单文件不存在，创建空黑名单").arg(orderNumber));
            orderMaterialBlacklist[orderNumber] = QStringList();
            return true;
        }

        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            LOG_ERROR(QString("无法打开料号黑名单文件: %1").arg(blacklistFilePath));
            return false;
        }

        QByteArray data = file.readAll();
        file.close();

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(data, &error);
        if (error.error != QJsonParseError::NoError) {
            LOG_ERROR(QString("解析料号黑名单文件失败: %1").arg(error.errorString()));
            return false;
        }

        QJsonObject obj = doc.object();
        QJsonArray materialArray = obj["materialCodes"].toArray();

        QStringList materialCodes;
        for (const QJsonValue& value : materialArray) {
            materialCodes << value.toString();
        }

        orderMaterialBlacklist[orderNumber] = materialCodes;
        LOG_INFO(QString("成功加载工单 %1 的料号黑名单: %2").arg(orderNumber).arg(materialCodes.join(", ")));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("加载料号黑名单时发生异常: %1").arg(e.what()));
        return false;
    }
}

void MainWindow::saveOrderMaterialBlacklist(const QString& orderNumber, const QStringList& materialCodes)
{
    try {
        // 确保工单文件夹存在
        checkAndCreateOrderFolder(orderNumber);

        QString blacklistFilePath = QDir(archivePath).filePath(orderNumber + "/material_blacklist.json");

        QJsonObject obj;
        QJsonArray materialArray;
        for (const QString& materialCode : materialCodes) {
            materialArray.append(materialCode);
        }
        obj["materialCodes"] = materialArray;

        QJsonDocument doc(obj);

        QFile file(blacklistFilePath);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            LOG_ERROR(QString("无法创建料号黑名单文件: %1").arg(blacklistFilePath));
            return;
        }

        file.write(doc.toJson());
        file.close();

        LOG_INFO(QString("成功保存工单 %1 的料号黑名单: %2").arg(orderNumber).arg(materialCodes.join(", ")));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("保存料号黑名单时发生异常: %1").arg(e.what()));
    }
}

bool MainWindow::isOrderPendingConfirmation(const QString& orderNumber)
{
    QMutexLocker locker(&pendingOrderMutex);
    return pendingOrders.contains(orderNumber);
}

void MainWindow::addToPendingOrderQueue(const QString& orderNumber)
{
    QMutexLocker locker(&pendingOrderMutex);
    if (!pendingOrders.contains(orderNumber)) {
        pendingOrders.insert(orderNumber);
        pendingOrderQueue.enqueue(orderNumber);
        LOG_INFO(QString("工单 %1 已添加到等待确认队列").arg(orderNumber));
    }
}

void MainWindow::removeFromPendingOrderQueue(const QString& orderNumber)
{
    QMutexLocker locker(&pendingOrderMutex);
    pendingOrders.remove(orderNumber);
    // 从队列中移除（如果需要的话）
    QQueue<QString> newQueue;
    while (!pendingOrderQueue.isEmpty()) {
        QString order = pendingOrderQueue.dequeue();
        if (order != orderNumber) {
            newQueue.enqueue(order);
        }
    }
    pendingOrderQueue = newQueue;
    LOG_INFO(QString("工单 %1 已从等待确认队列中移除").arg(orderNumber));
}

void MainWindow::showMaterialCodeConfirmationDialog(const QString& orderNumber, const QString& barcode, const QString& newMaterialCode, const QString& filePath)
{
    QString currentMaterials = orderMaterialWhitelist[orderNumber].join(", ");
    QString message = QString("工单 %1 发现新料号：%2\n\n"
                             "SSD条码：%3\n"
                             "新料号：%4\n"
                             "当前工单已确认的料号：%5\n\n"
                             "是否确认添加此料号？")
                         .arg(orderNumber)
                         .arg(newMaterialCode)
                         .arg(barcode)
                         .arg(newMaterialCode)
                         .arg(currentMaterials.isEmpty() ? "无" : currentMaterials);

    // 创建全屏料号确认对话框
    FullScreenAlertDialog* confirmDialog = new FullScreenAlertDialog(
        FullScreenAlertDialog::MaterialConfirm,
        "料号确认",
        message,
        this
    );
    
    confirmDialog->setOrderNumber(orderNumber);
    confirmDialog->setAutoClose(0); // 不自动关闭，需要用户手动选择
    
    // 设置回调函数处理用户选择
    confirmDialog->setCallback([this, orderNumber, newMaterialCode, filePath, barcode](bool confirmed) {
        if (confirmed) {
            // 用户确认，添加料号到白名单
            QStringList materialCodes = orderMaterialWhitelist[orderNumber];
            if (!materialCodes.contains(newMaterialCode)) {
                materialCodes << newMaterialCode;
                saveOrderMaterialWhitelist(orderNumber, materialCodes);
                orderMaterialWhitelist[orderNumber] = materialCodes;
            }

            // 从等待确认列表中移除
            removeFromPendingOrderQueue(orderNumber);

            LOG_INFO(QString("用户确认添加料号 %1 到工单 %2 白名单").arg(newMaterialCode).arg(orderNumber));

            // 将缓存中的文件移动到正式归档路径
            moveFilesFromCacheToArchive(orderNumber, newMaterialCode);
            
            // 保留缓存目录结构，便于后续查看该工单出现过的新料号
            // 不调用 cleanupMaterialCache，让目录结构保留

            // 处理等待中的该工单文件
            processPendingOrders();
        } else {
            // 用户拒绝，添加料号到黑名单
            QStringList blacklistCodes = orderMaterialBlacklist[orderNumber];
            if (!blacklistCodes.contains(newMaterialCode)) {
                blacklistCodes << newMaterialCode;
                saveOrderMaterialBlacklist(orderNumber, blacklistCodes);
                orderMaterialBlacklist[orderNumber] = blacklistCodes;
            }
            
            // 从等待确认列表中移除
            removeFromPendingOrderQueue(orderNumber);
            
            // 被拒绝的料号文件保留在缓存中，不进行任何清理
            // 文件将永久保存在 E:/MaterialCache/工单号/料号/ 目录下
            LOG_INFO(QString("用户拒绝添加料号 %1 到工单 %2 白名单，已加入黑名单，文件永久保留在缓存中").arg(newMaterialCode).arg(orderNumber));
        }
    });
    
    // 连接关闭信号，确保对话框被正确清理
    connect(confirmDialog, &FullScreenAlertDialog::closed, [confirmDialog]() {
        confirmDialog->deleteLater();
    });
    
    connect(confirmDialog, &FullScreenAlertDialog::confirmed, [confirmDialog]() {
        confirmDialog->deleteLater();
    });
    
    connect(confirmDialog, &FullScreenAlertDialog::rejected, [confirmDialog]() {
        confirmDialog->deleteLater();
    });
}

void MainWindow::processPendingOrders()
{
    try {
        QMutexLocker locker(&pendingOrderMutex);

        if (pendingOrderQueue.isEmpty()) {
            return;
        }

        // 处理队列中的第一个工单
        QString orderNumber = pendingOrderQueue.dequeue();
        pendingOrders.remove(orderNumber);

        LOG_INFO(QString("开始处理等待确认的工单: %1").arg(orderNumber));

        // 料号确认后，文件已经从缓存移动到正式归档路径
        // 这里主要是清理等待状态，文件处理由moveFilesFromCacheToArchive完成

    } catch (const std::exception& e) {
        LOG_ERROR(QString("处理等待确认工单时发生异常: %1").arg(e.what()));
    }
}

bool MainWindow::validateMaterialCode(const QString& orderNumber, const QString& barcode, const QString& materialCode, const QString& filePath)
{
    try {
        // 1. 检查工单是否在等待确认列表中
        if (isOrderPendingConfirmation(orderNumber)) {
            LOG_INFO(QString("工单 %1 正在等待料号确认，跳过处理").arg(orderNumber));
            return false;
        }

        // 2. 使用文件内提供的料号（新方案），不再从SSDInfo检索
        if (materialCode.isEmpty()) {
            LOG_WARNING(QString("文件未提供料号信息，条码: %1，按旧格式处理：跳过").arg(barcode));
            return false;
        }

        // 3. 加载工单料号白名单
        if (!loadOrderMaterialWhitelist(orderNumber)) {
            // 首次处理该工单，直接添加料号到白名单
            QStringList materialCodes;
            materialCodes << materialCode;
            saveOrderMaterialWhitelist(orderNumber, materialCodes);
            orderMaterialWhitelist[orderNumber] = materialCodes;
            LOG_INFO(QString("工单 %1 首次处理，添加料号 %2 到白名单").arg(orderNumber).arg(materialCode));
            return true;
        }

        // 3.5. 加载工单料号黑名单
        if (!loadOrderMaterialBlacklist(orderNumber)) {
            LOG_ERROR(QString("加载工单 %1 料号黑名单失败").arg(orderNumber));
            return false;
        }

        // 4. 检查料号是否在黑名单中
        if (orderMaterialBlacklist[orderNumber].contains(materialCode)) {
            LOG_WARNING(QString("料号 %1 在黑名单中，直接拒绝").arg(materialCode));
            if (!filePath.isEmpty()) {
                handleErrorFileWithOrder(filePath, orderNumber, barcode, "料号在黑名单中");
            }
            return false;
        }

        // 5. 检查料号是否在白名单中
        if (orderMaterialWhitelist[orderNumber].contains(materialCode)) {
            LOG_INFO(QString("料号 %1 在白名单中，验证通过").arg(materialCode));
            return true;
        }

        // 6. 料号不在白名单中，需要用户确认
        LOG_WARNING(QString("工单 %1 发现新料号 %2，需要用户确认").arg(orderNumber).arg(materialCode));
        
        // 检查是否已经有该料号的确认对话框在等待中
        if (!isMaterialPendingConfirmation(orderNumber, materialCode)) {
            // 首次出现该料号，弹出确认对话框（非阻塞）
            showMaterialCodeConfirmationDialog(orderNumber, barcode, materialCode, filePath);
            addToPendingOrderQueue(orderNumber);
        } else {
            // 该料号已有确认对话框在等待中，跳过弹窗
            LOG_INFO(QString("工单 %1 料号 %2 已有确认对话框在等待中，跳过重复弹窗").arg(orderNumber).arg(materialCode));
        }
        
        // 移动文件到料号缓存目录
        if (!filePath.isEmpty()) {
            moveFileToMaterialCache(filePath, orderNumber, materialCode);
        }
        
        return false;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("料号验证时发生异常: %1").arg(e.what()));
        return false;
    }
}

void MainWindow::setupWorkOrderMonitoring()
{
    try {
        LOG_INFO("开始设置工单文件夹监控");

        int monitoredCount = 0;
        for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
            QString orderNumber = it.key();

            // 构造工单文件夹路径
            QString orderPath;
            if (isNetworkPath(archivePath)) {
                if (archivePath.startsWith("\\")) {
                    orderPath = archivePath + "\\" + orderNumber;
                } else if (archivePath.startsWith("\\\\")) {
                    orderPath = archivePath + "\\" + orderNumber;
                } else {
                    orderPath = archivePath + "/" + orderNumber;
                }
            } else {
                QDir archiveDir(archivePath);
                orderPath = archiveDir.filePath(orderNumber);
            }

            // 监控工单根目录
            if (QDir(orderPath).exists()) {
                if (fileWatcher->addPath(orderPath)) {
                    monitoredCount++;
                    LOG_INFO(QString("已添加工单路径到监控: %1").arg(orderPath));
                }

                // 监控completed子文件夹
                QString completedPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        completedPath = orderPath + "\\completed";
                    } else if (archivePath.startsWith("\\\\")) {
                        completedPath = orderPath + "\\completed";
                    } else {
                        completedPath = orderPath + "/completed";
                    }
                } else {
                    completedPath = QDir(orderPath).filePath("completed");
                }

                if (QDir(completedPath).exists()) {
                    if (fileWatcher->addPath(completedPath)) {
                        monitoredCount++;
                        LOG_INFO(QString("已添加completed路径到监控: %1").arg(completedPath));
                    }
                }

                // 监控overlog子文件夹
                QString overlogPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        overlogPath = orderPath + "\\overlog";
                    } else if (archivePath.startsWith("\\\\")) {
                        overlogPath = orderPath + "\\overlog";
                    } else {
                        overlogPath = orderPath + "/overlog";
                    }
                } else {
                    overlogPath = QDir(orderPath).filePath("overlog");
                }

                if (QDir(overlogPath).exists()) {
                    if (fileWatcher->addPath(overlogPath)) {
                        monitoredCount++;
                        LOG_INFO(QString("已添加overlog路径到监控: %1").arg(overlogPath));
                    }
                }
            }
        }

        LOG_INFO(QString("工单文件夹监控设置完成，共监控 %1 个路径").arg(monitoredCount));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("设置工单文件夹监控时发生异常: %1").arg(e.what()));
    }
}

bool MainWindow::processLogFile(const QString& filePath)
{
    try {
        LOG_INFO(QString("开始处理日志文件: %1").arg(filePath));

        // 检查文件名是否为空（仅包含扩展名）
        QFileInfo fileInfo(filePath);
        QString baseName = fileInfo.baseName();
        if (baseName.isEmpty()) {
            LOG_ERROR(QString("文件名为空（仅包含扩展名）: %1").arg(filePath));
            handleErrorFile(filePath, "", "", "文件名为空");
            return false;
        }

        // 检查工单配置对象
        if (!workorderSettings) {
            LOG_ERROR("工单配置对象未初始化");
            return false;
        }

        // 1. 检查文件是否存在
        if (!QFile::exists(filePath)) {
            LOG_WARNING(QString("文件不存在: %1").arg(filePath));
            return false;
        }

        // 2. 读取文件内容（带重试机制）
        QFile file(filePath);
        int retryCount = 0;
        const int maxRetries = 10;  // 最多重试10次
        const int retryDelay = 500; // 每次重试间隔500ms

        while (retryCount < maxRetries) {
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                break; // 成功打开文件
            }

            // 检查是否是文件被占用错误
            if (file.error() == QFile::ResourceError ||
                file.errorString().contains("另一个程序正在使用此文件") ||
                file.errorString().contains("进程无法访问")) {

                retryCount++;
                LOG_INFO(QString("文件被占用，等待重试 (%1/%2): %3")
                             .arg(retryCount)
                             .arg(maxRetries)
                             .arg(filePath));

                file.close(); // 关闭文件句柄
                QThread::msleep(retryDelay); // 等待文件释放
                continue;
            } else {
                // 其他错误，直接失败
                LOG_ERROR(QString("无法打开文件: %1, 错误: %2")
                              .arg(filePath)
                              .arg(file.errorString()));
                return false;
            }
        }

        // 检查是否达到最大重试次数
        if (retryCount >= maxRetries) {
            LOG_ERROR(QString("文件重试次数超限，处理失败: %1").arg(filePath));
            return false;
        }

        LOG_INFO(QString("文件打开成功 (重试次数: %1): %2").arg(retryCount).arg(filePath));

        QString content = QString::fromUtf8(file.readAll()).trimmed();
        file.close();

        // 3. 验证条码匹配
        LOG_INFO(QString("开始验证条码匹配: %1").arg(filePath));
        LOG_INFO(QString("文件内容: %1").arg(content));

        QString fileName = fileInfo.baseName();  // 获取不带扩展名的文件名
        LOG_INFO(QString("文件名(不含扩展名): %1").arg(fileName));

        if (!validateBarcode(filePath, content)) {
            LOG_WARNING(QString("条码不匹配 - 文件名: %1, 内容: %2").arg(fileName).arg(content));
            handleMismatchedFile(filePath);
            return false;
        }
        LOG_INFO("条码匹配验证通过");

        // 4. 解析文件内容
        QStringList parts = content.split("|");
        if (parts.size() < 2) {
            LOG_ERROR(QString("文件内容格式错误，字段不足: %1").arg(filePath));
            handleErrorFile(filePath, "", "", "内容格式错误");
            return false;
        }

        QString orderNumber = parts[0].trimmed();
        QString barcode = parts[1].trimmed();
        QString materialDescription = "";  // 料号描述，新格式支持
        
        // 检查工单号是否为空
        if (orderNumber.isEmpty()) {
            LOG_ERROR(QString("工单号为空: %1").arg(filePath));
            handleErrorFileWithOrder(filePath, "", barcode, "工单号为空");
            return false;
        }
        
        // 检查条码是否为空
        if (barcode.isEmpty()) {
            LOG_ERROR(QString("条码为空: %1").arg(filePath));
            handleErrorFileWithOrder(filePath, orderNumber, "", "条码为空");
            return false;
        }
        
        // 新格式支持：工单号|条码|料号描述
        if (parts.size() >= 3) {
            materialDescription = parts[2].trimmed();
            LOG_INFO(QString("检测到新格式文件 - 工单号: %1, 条码: %2, 料号描述: %3")
                        .arg(orderNumber)
                        .arg(barcode)
                        .arg(materialDescription));
            
            // 存储料号描述，为后续需求做准备
            materialDescriptions[barcode] = materialDescription;
        } else {
            LOG_INFO(QString("检测到旧格式文件 - 工单号: %1, 条码: %2")
                        .arg(orderNumber)
                        .arg(barcode));
        }

        // 4.5. 料号验证（改为使用文件内料号）
        if (!validateMaterialCode(orderNumber, barcode, materialDescription, filePath)) {
            LOG_WARNING(QString("料号验证失败，跳过处理: %1").arg(filePath));
            return false;
        }

        recordProcessedOrder(orderNumber);  // 记录处理过的工单号
        syncProcessedOrders();  // 尝试同步工单

        // 再次验证条码
        if (barcode != fileName) {
            LOG_WARNING(QString("条码不匹配 - 文件名: %1, 内容条码: %2").arg(fileName).arg(barcode));
            handleMismatchedFile(filePath);
            return false;
        }

        // 5. 记录处理的工单
       // recordProcessedOrder(orderNumber);
        LOG_INFO(QString("准备写入时间记录 - 工单: %1, 条码: %2")
                     .arg(orderNumber)
                     .arg(barcode));

        // 6. 检查重复文件（这个检查现在在moveLogFile中进行，这里只用于记录）
        int copyCount = 1;
        // 6.5 获取文件创建时间（在移动文件之前）
        QFileInfo fileInfo2(filePath);
        QString createTime = fileInfo2.birthTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
        LOG_INFO(QString("获取文件创建时间: %1").arg(createTime));

        // 7. 移动文件 (调整顺序：先移动文件)
        if (!moveLogFile(filePath, orderNumber, barcode, copyCount)) {
            LOG_ERROR(QString("移动文件失败: %1").arg(filePath));
            return false;
        }

        // 8. 更新工单计数 (调整顺序：更新计数)
        if (workOrders.contains(orderNumber)) {
            updateWorkOrderCount(orderNumber);
        }

        // 9. 写入时间记录 (调整顺序：最后写入记录)
        if (!writeTimeRecord(orderNumber, barcode, createTime, copyCount)) {
            LOG_WARNING(QString("时间记录写入失败: %1").arg(filePath));
        } else {
            LOG_INFO(QString("时间记录写入成功"));
        }

        LOG_INFO(QString("文件处理完成: %1, 工单: %2, 条码: %3")
                     .arg(filePath)
                     .arg(orderNumber)
                     .arg(barcode));

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("处理日志文件时发生异常: %1, 文件: %2")
                      .arg(e.what())
                      .arg(filePath));
        return false;
    }
}
bool MainWindow::moveLogFile(const QString& sourceFile, const QString& orderNumber, const QString& barcode, int& copyCount)
{
    try {
        LOG_INFO(QString("开始移动文件处理 - 源文件: %1, 工单号: %2, 条码: %3")
                     .arg(sourceFile)
                     .arg(orderNumber)
                     .arg(barcode));

        // 检查源文件是否存在
        if (!QFile::exists(sourceFile)) {
            LOG_ERROR(QString("源文件不存在: %1").arg(sourceFile));
            return false;
        }

        // 检查并创建工单文件夹
        LOG_INFO(QString("检查工单文件夹: %1").arg(orderNumber));
        checkAndCreateOrderFolder(orderNumber);

        // 手动构造网络路径，避免QDir::filePath()的问题
        QString orderPath;
        QString destFile;

        // 获取文件的基本名称（不含扩展名），确保目标文件总是.log扩展名
        QString baseName = QFileInfo(sourceFile).completeBaseName();
        QString targetFileName = baseName + ".log";

        if (isNetworkPath(archivePath)) {
            // 网络路径：手动构造
            if (archivePath.startsWith("\\")) {
                // 单个反斜杠开头的网络路径
                orderPath = archivePath + "\\" + orderNumber;
                destFile = orderPath + "\\" + targetFileName;
            } else if (archivePath.startsWith("\\\\")) {
                // 双反斜杠开头的网络路径
                orderPath = archivePath + "\\" + orderNumber;
                destFile = orderPath + "\\" + targetFileName;
            } else {
                // 正斜杠开头的网络路径
                orderPath = archivePath + "/" + orderNumber;
                destFile = orderPath + "/" + targetFileName;
            }
        } else {
            // 本地路径：使用QDir::filePath()
            QDir archiveDir(archivePath);
            orderPath = archiveDir.filePath(orderNumber);
            destFile = QDir(orderPath).filePath(targetFileName);
        }

        LOG_INFO(QString("目标文件路径: %1").arg(destFile));
        LOG_INFO(QString("工单文件夹路径: %1").arg(orderPath));

        // 检查是否存在同名文件（检查工单根目录、completed和overlog文件夹）
        QString existingFilePath;
        QString existingFileLocation;

        // 1. 检查工单根目录
        if (QFile::exists(destFile)) {
            existingFilePath = destFile;
            existingFileLocation = "工单根目录";
            LOG_INFO(QString("在工单根目录检测到重复文件: %1").arg(destFile));
        } else {
            // 2. 检查completed文件夹
            QString completedPath;
            if (isNetworkPath(archivePath)) {
                if (archivePath.startsWith("\\")) {
                    completedPath = orderPath + "\\completed\\" + targetFileName;
                } else if (archivePath.startsWith("\\\\")) {
                    completedPath = orderPath + "\\completed\\" + targetFileName;
                } else {
                    completedPath = orderPath + "/completed/" + targetFileName;
                }
            } else {
                completedPath = QDir(orderPath).filePath("completed/" + targetFileName);
            }

            if (QFile::exists(completedPath)) {
                existingFilePath = completedPath;
                existingFileLocation = "completed文件夹";
                LOG_INFO(QString("在completed文件夹检测到重复文件: %1").arg(completedPath));
            } else {
                // 3. 检查overlog文件夹
                QString overlogPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        overlogPath = orderPath + "\\overlog\\" + targetFileName;
                    } else if (archivePath.startsWith("\\\\")) {
                        overlogPath = orderPath + "\\overlog\\" + targetFileName;
                    } else {
                        overlogPath = orderPath + "/overlog/" + targetFileName;
                    }
                } else {
                    overlogPath = QDir(orderPath).filePath("overlog/" + targetFileName);
                }

                if (QFile::exists(overlogPath)) {
                    existingFilePath = overlogPath;
                    existingFileLocation = "overlog文件夹";
                    LOG_INFO(QString("在overlog文件夹检测到重复文件: %1").arg(overlogPath));
                }
            }
        }

        // 如果找到重复文件，处理重复文件
        if (!existingFilePath.isEmpty()) {

            // 确保重复文件目录存在
            QDir duplicateDir(duplicatePath);
            QString duplicateOrderPath = duplicateDir.filePath(orderNumber);
            LOG_INFO(QString("创建重复文件目录: %1").arg(duplicateOrderPath));

            if (isNetworkPath(duplicatePath)) {
                // 网络路径：尝试创建文件夹
                LOG_INFO(QString("尝试在网络路径创建重复文件目录: %1").arg(duplicateOrderPath));
                if (createNetworkFolder(duplicateOrderPath)) {
                    LOG_INFO(QString("成功在网络路径创建重复文件目录: %1").arg(duplicateOrderPath));
                } else {
                    // 如果创建失败，检查父目录是否可访问
                    QString parentPath = QFileInfo(duplicateOrderPath).path();
                    if (!QDir(parentPath).exists()) {
                        LOG_ERROR(QString("网络路径重复文件目录无法访问: %1").arg(parentPath));
                        return false;
                    }
                    LOG_INFO(QString("网络路径重复文件目录检查完成（创建失败但可访问）: %1").arg(duplicateOrderPath));
                }
            } else {
                // 本地路径：尝试创建
                if (!QDir().mkpath(duplicateOrderPath)) {
                    LOG_ERROR(QString("创建重复文件目录失败: %1").arg(duplicateOrderPath));
                    return false;
                }
                LOG_INFO(QString("已创建本地重复文件目录: %1").arg(duplicateOrderPath));
            }

            // 获取下一个重复序号（根据文件所在位置确定子文件夹）
            QString duplicateSubFolder = existingFileLocation;
            int duplicateNumber = getNextDuplicateNumber(barcode, orderNumber, duplicateSubFolder);
            copyCount = duplicateNumber; // 更新重复次数
            LOG_INFO(QString("获取到重复文件序号: %1 (位置: %2)").arg(duplicateNumber).arg(duplicateSubFolder));

            // 构造重复文件的新名称
            QString duplicateFileName = QString("%1+%2.log").arg(barcode).arg(duplicateNumber);
            QString duplicateFilePath = QDir(duplicateOrderPath).filePath(duplicateFileName);
            while (QFile::exists(duplicateFilePath)) {
                duplicateNumber++;
                copyCount = duplicateNumber;
                duplicateFileName = QString("%1+%2.log").arg(barcode).arg(duplicateNumber);
                duplicateFilePath = QDir(duplicateOrderPath).filePath(duplicateFileName);
            }
            LOG_INFO(QString("重复文件新路径: %1").arg(duplicateFilePath));

            // 移动已存在的文件到重复文件目录
            // Windows 上 QFile::rename 不能跨盘/跨共享移动；这里提供 copy+remove 兜底，避免“只建目录不落文件”
            {
                QFile existing(existingFilePath);
                if (existing.rename(duplicateFilePath)) {
                    // rename 成功
                } else {
                    const QString renameErr = existing.errorString();
                    LOG_WARNING(QString("rename移动重复文件失败，尝试copy+remove: %1 -> %2, 错误: %3")
                                    .arg(existingFilePath)
                                    .arg(duplicateFilePath)
                                    .arg(renameErr));

                    if (!QFile::copy(existingFilePath, duplicateFilePath)) {
                        QFile copyProbe(existingFilePath);
                        LOG_ERROR(QString("copy重复文件失败: %1 -> %2, 错误: %3")
                                      .arg(existingFilePath)
                                      .arg(duplicateFilePath)
                                      .arg(copyProbe.errorString()));
                        return false;
                    }

                    if (!QFile::remove(existingFilePath)) {
                        QFile removeProbe(existingFilePath);
                        LOG_ERROR(QString("copy成功但删除源重复文件失败: %1, 错误: %2")
                                      .arg(existingFilePath)
                                      .arg(removeProbe.errorString()));
                        // 不直接返回 false：避免阻塞新文件处理，但明确记录风险
                    }
                }
            }

            LOG_INFO(QString("已成功移动重复文件: %1 -> %2").arg(existingFilePath).arg(duplicateFilePath));

            // 根据旧文件位置确定新文件的目标位置
            if (existingFileLocation == "completed文件夹") {
                // 新文件移动到completed文件夹
                QString completedDirPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        completedDirPath = orderPath + "\\completed";
                    } else if (archivePath.startsWith("\\\\")) {
                        completedDirPath = orderPath + "\\completed";
                    } else {
                        completedDirPath = orderPath + "/completed";
                    }
                } else {
                    completedDirPath = QDir(orderPath).filePath("completed");
                }

                // 确保completed文件夹存在
                if (!QDir(completedDirPath).exists()) {
                    if (isNetworkPath(archivePath)) {
                        createNetworkFolder(completedDirPath);
                    } else {
                        QDir().mkpath(completedDirPath);
                    }
                }
                destFile = QDir(completedDirPath).filePath(QFileInfo(sourceFile).fileName());
                LOG_INFO(QString("新文件将移动到completed文件夹: %1").arg(destFile));
            } else if (existingFileLocation == "overlog文件夹") {
                // 新文件移动到overlog文件夹
                QString overlogDirPath;
                if (isNetworkPath(archivePath)) {
                    if (archivePath.startsWith("\\")) {
                        overlogDirPath = orderPath + "\\overlog";
                    } else if (archivePath.startsWith("\\\\")) {
                        overlogDirPath = orderPath + "\\overlog";
                    } else {
                        overlogDirPath = orderPath + "/overlog";
                    }
                } else {
                    overlogDirPath = QDir(orderPath).filePath("overlog");
                }

                // 确保overlog文件夹存在
                if (!QDir(overlogDirPath).exists()) {
                    if (isNetworkPath(archivePath)) {
                        createNetworkFolder(overlogDirPath);
                    } else {
                        QDir().mkpath(overlogDirPath);
                    }
                }
                destFile = QDir(overlogDirPath).filePath(QFileInfo(sourceFile).fileName());
                LOG_INFO(QString("新文件将移动到overlog文件夹: %1").arg(destFile));
            }
            // 如果旧文件在工单根目录，destFile已经是正确的，无需修改
        }

        // 检查是否需要移动到溢出文件夹（只有在没有找到重复文件时才检查）
        if (existingFilePath.isEmpty()) {
            // 实时统计工单文件夹下根目录+completed的log文件数（不包含overlog）
            int currentValidCount = countAllLogFilesInOrderFolder(orderNumber, false);
            LOG_INFO(QString("当前工单有效文件数（根目录+completed）: %1").arg(currentValidCount));

            if (workOrders.contains(orderNumber) && workOrders[orderNumber]) {
                int totalCount = workOrders[orderNumber]->totalCount;
                LOG_INFO(QString("工单总数: %1").arg(totalCount));

                // 基于实际文件夹状态判断是否需要放入overlog
                if (currentValidCount >= totalCount) {
                    // 创建溢出文件夹
                    QString overflowPath;
                    if (isNetworkPath(archivePath)) {
                        if (archivePath.startsWith("\\")) {
                            overflowPath = orderPath + "\\overlog";
                        } else if (archivePath.startsWith("\\\\")) {
                            overflowPath = orderPath + "\\overlog";
                        } else {
                            overflowPath = orderPath + "/overlog";
                        }
                    } else {
                        overflowPath = QDir(orderPath).filePath("overlog");
                    }

                    LOG_INFO(QString("创建溢出文件夹: %1").arg(overflowPath));

                    if (isNetworkPath(archivePath)) {
                        // 网络路径：尝试创建文件夹
                        LOG_INFO(QString("尝试在网络路径创建溢出文件夹: %1").arg(overflowPath));
                        if (createNetworkFolder(overflowPath)) {
                            LOG_INFO(QString("成功在网络路径创建溢出文件夹: %1").arg(overflowPath));
                        } else {
                            // 如果创建失败，检查父目录是否可访问
                            QString parentPath = QFileInfo(overflowPath).path();
                            if (!QDir(parentPath).exists()) {
                                LOG_ERROR(QString("网络路径溢出文件夹无法访问: %1").arg(parentPath));
                                return false;
                            }
                            LOG_INFO(QString("网络路径溢出文件夹检查完成（创建失败但可访问）: %1").arg(overflowPath));
                        }
                    } else {
                        // 本地路径：尝试创建
                        if (!QDir().mkpath(overflowPath)) {
                            LOG_ERROR(QString("创建溢出文件夹失败: %1").arg(overflowPath));
                            return false;
                        }
                        LOG_INFO(QString("已创建本地溢出文件夹: %1").arg(overflowPath));
                    }

                    // 修改目标路径为溢出文件夹
                    destFile = QDir(overflowPath).filePath(QFileInfo(sourceFile).fileName());
                    LOG_WARNING(QString("文件数超出限制，将移动到溢出文件夹: %1").arg(destFile));

                    // 使用非阻塞的溢出报警
                    showOverflowAlert(orderNumber);
                }
            } else {
                LOG_WARNING(QString("未找到工单对象或工单配置: %1").arg(orderNumber));
            }
        }

        // 移动新文件到目标位置
        LOG_INFO(QString("开始移动文件到目标位置: %1 -> %2").arg(sourceFile).arg(destFile));
        QFile file(sourceFile);
        if (!file.rename(destFile)) {
            LOG_ERROR(QString("移动文件失败: %1 -> %2, 错误: %3")
                          .arg(sourceFile)
                          .arg(destFile)
                          .arg(file.errorString()));
            return false;
        }

        LOG_INFO(QString("文件移动成功完成: %1 -> %2").arg(sourceFile).arg(destFile));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("移动文件时发生异常: %1, 源文件: %2").arg(e.what()).arg(sourceFile));
        return false;
    }
}
void MainWindow::onDirectoryChanged(const QString& path)
{
    try {
        LOG_INFO(QString("检测到目录变化: %1").arg(path));

        if (path == watchPath) {
            // 监控文件夹变化，处理新文件
            LOG_INFO(QString("监控目录变化，处理新文件: %1").arg(path));
            QTimer::singleShot(1000, this, [this, path]() {
                processDirectoryFiles(path);
            });
        } else {
            // 工单文件夹变化，更新工单计数
            LOG_INFO(QString("工单文件夹变化，更新计数: %1").arg(path));
            // 减少延迟时间，提高响应性
            QTimer::singleShot(200, this, [this, path]() {
                updateWorkOrderCountFromPath(path);
            });
        }

    } catch (const std::exception& e) {
        LOG_ERROR(QString("目录变化处理时发生异常: %1").arg(e.what()));
        isProcessing = false;
    }
}

void MainWindow::processDirectoryFiles(const QString& path)
{
    try {
        QStringList files = getLogFiles(path);

        LOG_INFO(QString("延迟处理发现 %1 个日志文件").arg(files.size()));

        // 如果文件数量较少，直接处理
        if (files.size() <= 10) {
            for (const QString& file : files) {
                QString filePath = QDir(path).filePath(file);
                LOG_INFO(QString("处理文件: %1").arg(filePath));

                if (QFile::exists(filePath)) {
                    // 确保文件扩展名为.log（如果是.txt则重命名）
                    QString processedPath = ensureLogExtension(filePath);

                    if (!isProcessing) {
                        LOG_INFO("直接处理文件");
                        isProcessing = true;
                        bool success = processLogFile(processedPath);
                        LOG_INFO(QString("文件处理%1: %2")
                                     .arg(success ? "成功" : "失败")
                                     .arg(processedPath));
                        isProcessing = false;
                    } else {
                        LOG_INFO("添加文件到队列");
                        addToProcessQueue(processedPath);
                    }
                } else {
                    LOG_WARNING(QString("文件不存在: %1").arg(filePath));
                }
            }
        } else {
            // 大量文件：全部添加到队列，避免界面卡死
            LOG_INFO(QString("发现大量文件(%1个)，全部添加到处理队列").arg(files.size()));

            for (const QString& file : files) {
                QString filePath = QDir(path).filePath(file);
                if (QFile::exists(filePath)) {
                    // 确保文件扩展名为.log（如果是.txt则重命名）
                    QString processedPath = ensureLogExtension(filePath);
                    addToProcessQueue(processedPath);
                }
            }

            LOG_INFO(QString("已将 %1 个文件添加到处理队列").arg(files.size()));
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("延迟处理目录文件时发生异常: %1").arg(e.what()));
        isProcessing = false;
    }
}

void MainWindow::onArchiveDirectoryChanged(const QString& path)
{
    try {
        if (path.startsWith(archivePath)) {
            QString orderNumber = QDir(path).dirName();
            if (workOrders.contains(orderNumber)) {
                updateWorkOrderCount(orderNumber);
                
                // 检查工单状态变化
                checkWorkOrderStatusChange(orderNumber);
            }
        }
    } catch (const std::exception& e) {
        qDebug() << "归档目录变化处理时发生异常:" << e.what();
    }
}
// 保存工单信息
void MainWindow::saveWorkOrders()
{
    try {
        // 如果没有工单，删除保存文件并返回
        if (workOrders.isEmpty()) {
            QFile file(getWorkOrdersFilePath());
            if (file.exists()) {
                file.remove();
                LOG_INFO("没有工单信息需要保存，已删除保存文件");
            }
            return;
        }

        QJsonArray ordersArray;

        // 遍历当前所有工单
        for (const auto& order : workOrders) {
            QJsonObject orderObj;
            orderObj["orderNumber"] = order->orderNumber;
            orderObj["totalCount"] = order->totalCount;
            ordersArray.append(orderObj);
        }

        QJsonDocument doc(ordersArray);

        // 保存到文件
        QFile file(getWorkOrdersFilePath());
        if (!file.open(QIODevice::WriteOnly)) {
            LOG_ERROR("无法打开工单保存文件");
            return;
        }

        file.write(doc.toJson());
        LOG_INFO(QString("已保存 %1 个工单信息").arg(workOrders.size()));

    } catch (const std::exception& e) {
        LOG_ERROR(QString("保存工单信息时发生错误: %1").arg(e.what()));
    }
}

void MainWindow::askToLoadSavedWorkOrders()
{
    if (workOrdersLoaded) {
        LOG_INFO("工单已经加载，跳过重复加载");
        return;
    }
    try {
        QFile file(getWorkOrdersFilePath());
        if (!file.exists()) {
            workOrdersLoaded = true;  // 标记为已加载（没有文件需要加载）
            // 工单加载完成后，执行启动自检
            QTimer::singleShot(100, this, &MainWindow::performInitialCheck);
            return;
        }

        if (!file.open(QIODevice::ReadOnly)) {
            LOG_ERROR("无法打开工单保存文件");
            workOrdersLoaded = true;  // 标记为已加载
            // 工单加载完成后，执行启动自检
            QTimer::singleShot(100, this, &MainWindow::performInitialCheck);
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();

        if (doc.isNull() || !doc.isArray() || doc.array().isEmpty()) {
            LOG_WARNING("工单保存文件无效或为空");
            file.remove();
            workOrdersLoaded = true;  // 标记为已加载
            // 工单加载完成后，执行启动自检
            QTimer::singleShot(100, this, &MainWindow::performInitialCheck);
            return;
        }

        QMessageBox::StandardButton reply = QMessageBox::question(
            this,
            "加载工单",
            QString("检测到上次保存的 %1 个工单信息，是否加载？").arg(doc.array().size()),
            QMessageBox::Yes | QMessageBox::No
            );

        if (reply == QMessageBox::Yes) {
            WorkOrderSelectDialog dialog(doc.array(), archivePath, this);
            if (dialog.exec() == QDialog::Accepted) {
                QStringList selectedOrders = dialog.getSelectedOrders();
                if (!selectedOrders.isEmpty()) {
                    loadSelectedWorkOrders(doc.array(), selectedOrders);
                    LOG_INFO(QString("成功加载 %1 个选定的工单").arg(selectedOrders.size()));
                }
            } else {
                file.remove();
                LOG_INFO("用户取消加载工单信息，已删除保存文件");
            }
        } else {
            file.remove();
            LOG_INFO("用户选择不加载工单信息，已删除保存文件");
        }

        workOrdersLoaded = true;  // 标记为已加载

    } catch (const std::exception& e) {
        LOG_ERROR(QString("加载保存的工单信息时发生错误：%1").arg(e.what()));
        workOrdersLoaded = true;  // 即使出错也标记为已尝试加载
    }

    // 工单加载完成后，执行启动自检
    QTimer::singleShot(100, this, &MainWindow::performInitialCheck);
}
void MainWindow::loadSelectedWorkOrders(const QJsonArray& allOrders, const QStringList& selectedOrders)
{
    try {
        // 不再清除现有工单，而是保留它们
        // qDeleteAll(workOrders);
        // workOrders.clear();

        // 只加载选中的工单
        for (const QJsonValue& value : allOrders) {
            QJsonObject orderObj = value.toObject();
            QString orderNumber = orderObj["orderNumber"].toString();

            if (selectedOrders.contains(orderNumber)) {
                // 如果工单已经存在，跳过
                if (workOrders.contains(orderNumber)) {
                    LOG_INFO(QString("工单 %1 已存在，跳过加载").arg(orderNumber));
                    continue;
                }

                int totalCount = orderObj["totalCount"].toInt();

                // 创建新工单
                WorkOrder* order = new WorkOrder(orderNumber, totalCount);

                // 统计实际完成数（基于实际文件夹状态）
                order->completedCount = countAllLogFilesInOrderFolder(orderNumber, true);

                workOrders[orderNumber] = order;
                suppressedAutoSyncOrders.remove(orderNumber);

                // 分类工单到相应区域
                classifyWorkOrder(order);

                LOG_INFO(QString("加载工单: %1, 总数: %2, 已完成: %3")
                             .arg(orderNumber)
                             .arg(totalCount)
                             .arg(order->completedCount));
            }
        }

        updateTable(); // 更新界面显示

    } catch (const std::exception& e) {
        LOG_ERROR(QString("加载选定工单时发生错误: %1").arg(e.what()));
    }
}
// 加载保存的工单信息
bool MainWindow::loadSavedWorkOrders()
{
    try {
        QFile file(getWorkOrdersFilePath());
        if (!file.exists()) {
            return false;
        }

        if (!file.open(QIODevice::ReadOnly)) {
            LOG_ERROR("无法打开工单保存文件");
            return false;
        }

        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        QJsonArray ordersArray = doc.array();

        // 清除现有工单
        qDeleteAll(workOrders);
        workOrders.clear();

        // 加载保存的工单
        for (const QJsonValue& value : ordersArray) {
            QJsonObject orderObj = value.toObject();
            QString orderNumber = orderObj["orderNumber"].toString();
            int totalCount = orderObj["totalCount"].toInt();

            // 创建新工单
            WorkOrder* order = new WorkOrder(orderNumber, totalCount);

            // 统计实际完成数（使用新的统计逻辑）
            order->completedCount = countAllLogFilesInOrderFolder(orderNumber);

            workOrders[orderNumber] = order;
            suppressedAutoSyncOrders.remove(orderNumber);
        }

        updateTable();
        LOG_INFO("已加载保存的工单信息");
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("加载工单信息时发生错误: %1").arg(e.what()));
        return false;
    }
}

// 询问是否加载保存的工单信息


// 获取工单保存文件路径
QString MainWindow::getWorkOrdersFilePath() const
{
    return QDir::current().filePath(WORK_ORDERS_FILENAME);
}

int MainWindow::readWorkOrderTotalFromNumFile(const QString& orderNumber) const
{
    try {
        if (workordersRootPath.isEmpty()) {
            LOG_WARNING("未配置工单根目录 Path/WorkordersRoot");
            return -1;
        }
        // 手动构造网络路径，避免QDir::filePath()的问题
        QString orderFolder;
        QString numFilePath;

        if (isNetworkPath(workordersRootPath)) {
            // 网络路径：手动构造
            if (workordersRootPath.startsWith("\\")) {
                // 单个反斜杠开头的网络路径
                orderFolder = workordersRootPath + "\\" + orderNumber;
                numFilePath = orderFolder + "\\NUM.txt";
            } else if (workordersRootPath.startsWith("\\\\")) {
                // 双反斜杠开头的网络路径
                orderFolder = workordersRootPath + "\\" + orderNumber;
                numFilePath = orderFolder + "\\NUM.txt";
            } else {
                // 正斜杠开头的网络路径
                orderFolder = workordersRootPath + "/" + orderNumber;
                numFilePath = orderFolder + "/NUM.txt";
            }
        } else {
            // 本地路径：使用QDir::filePath()
            QDir rootDir(workordersRootPath);
            orderFolder = rootDir.filePath(orderNumber);
            numFilePath = QDir(orderFolder).filePath("NUM.txt");
        }

        LOG_INFO(QString("NUM.txt路径构造: %1").arg(numFilePath));
        QFile numFile(numFilePath);
        if (!numFile.exists()) {
            LOG_WARNING(QString("未找到NUM.txt: %1").arg(numFilePath));
            return -1;
        }
        if (!numFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            LOG_ERROR(QString("无法打开NUM.txt: %1, 错误: %2").arg(numFilePath).arg(numFile.errorString()));
            return -1;
        }
        QTextStream in(&numFile);
        QString content = in.readAll().trimmed();
        numFile.close();
        bool ok = false;
        int total = content.toInt(&ok);
        if (!ok || total < 0) {
            LOG_WARNING(QString("NUM.txt内容无效: %1, 内容: %2").arg(numFilePath).arg(content));
            return -1;
        }
        return total;
    } catch (const std::exception& e) {
        LOG_ERROR(QString("读取NUM.txt时异常: %1").arg(e.what()));
        return -1;
    }
}

bool MainWindow::writeWorkOrderTotalToNumFile(const QString& orderNumber, int totalCount)
{
    try {
        if (workordersRootPath.isEmpty()) {
            LOG_WARNING("未配置工单根目录 Path/WorkordersRoot");
            return false;
        }
        
        // 手动构造网络路径，避免QDir::filePath()的问题
        QString orderFolder;
        QString numFilePath;

        if (isNetworkPath(workordersRootPath)) {
            // 网络路径：手动构造
            if (workordersRootPath.startsWith("\\")) {
                // 单个反斜杠开头的网络路径
                orderFolder = workordersRootPath + "\\" + orderNumber;
                numFilePath = orderFolder + "\\NUM.txt";
            } else if (workordersRootPath.startsWith("\\\\")) {
                // 双反斜杠开头的网络路径
                orderFolder = workordersRootPath + "\\" + orderNumber;
                numFilePath = orderFolder + "\\NUM.txt";
            } else {
                // 正斜杠开头的网络路径
                orderFolder = workordersRootPath + "/" + orderNumber;
                numFilePath = orderFolder + "/NUM.txt";
            }
        } else {
            // 本地路径：使用QDir::filePath()
            QDir rootDir(workordersRootPath);
            orderFolder = rootDir.filePath(orderNumber);
            numFilePath = QDir(orderFolder).filePath("NUM.txt");
        }

        LOG_INFO(QString("准备写入NUM.txt: %1, 总数: %2").arg(numFilePath).arg(totalCount));

        // 确保工单文件夹存在
        QDir dir;
        if (!dir.exists(orderFolder)) {
            LOG_INFO(QString("创建工单文件夹: %1").arg(orderFolder));
            if (!dir.mkpath(orderFolder)) {
                LOG_ERROR(QString("无法创建工单文件夹: %1").arg(orderFolder));
                return false;
            }
        }

        // 写入NUM.txt文件
        QFile numFile(numFilePath);
        if (!numFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
            LOG_ERROR(QString("无法打开NUM.txt进行写入: %1, 错误: %2").arg(numFilePath).arg(numFile.errorString()));
            return false;
        }

        QTextStream out(&numFile);
        out << totalCount;
        out.flush();
        numFile.close();

        // 验证写入是否成功
        int readBack = readWorkOrderTotalFromNumFile(orderNumber);
        if (readBack != totalCount) {
            LOG_ERROR(QString("NUM.txt写入验证失败 - 期望: %1, 实际: %2").arg(totalCount).arg(readBack));
            return false;
        }

        LOG_INFO(QString("NUM.txt写入成功 - 工单: %1, 总数: %2").arg(orderNumber).arg(totalCount));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("写入NUM.txt时异常: %1").arg(e.what()));
        return false;
    }
}
bool MainWindow::tryLockConfigFile(QFile& lockFile, int maxRetries)
{
    int retryCount = 0;
    while (retryCount < maxRetries) {
        if (lockFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            return true;
        }

        // 如果锁文件存在但超过10秒，认为是之前的程序异常退出，强制删除锁
        QFileInfo lockFileInfo(lockFile);
        if (lockFileInfo.exists() &&
            lockFileInfo.lastModified().secsTo(QDateTime::currentDateTime()) > 10) {
            lockFile.remove();
            LOG_WARNING("检测到过期的锁文件，已强制删除");
            continue;
        }

        QThread::msleep(100);  // 等待100ms后重试
        retryCount++;
    }
    return false;
}

void MainWindow::unlockConfigFile(QFile& lockFile)
{
    if (lockFile.isOpen()) {
        lockFile.close();
    }
    lockFile.remove();
}

bool MainWindow::updateWorkorderConfig(const QString& orderNumber, int totalCount)
{
    try {
        if (!workorderSettings) {
            LOG_ERROR("工单配置对象未初始化");
            return false;
        }

        // 创建锁文件
        QString lockFilePath = QFileInfo(workorderSettings->fileName()).path() + "/workorder.lock";
        QFile lockFile(lockFilePath);

        // 尝试获取文件锁
        if (!tryLockConfigFile(lockFile)) {
            LOG_ERROR(QString("无法获取配置文件锁，更新失败 - 工单: %1").arg(orderNumber));
            return false;
        }

        // 使用RAII方式确保锁文件被释放
        struct LockGuard {
            QFile& lockFile;
            MainWindow* window;
            LockGuard(QFile& f, MainWindow* w) : lockFile(f), window(w) {}
            ~LockGuard() { window->unlockConfigFile(lockFile); }
        } guard(lockFile, this);

        // 重新加载配置确保获取最新数据
        workorderSettings->sync();

        // 更新配置
        workorderSettings->setValue(QString("WorkOrders/%1").arg(orderNumber), totalCount);
        workorderSettings->sync();

        // 验证配置是否成功写入
        bool ok;
        int savedValue = workorderSettings->value(QString("WorkOrders/%1").arg(orderNumber), -1).toInt(&ok);
        if (!ok || savedValue != totalCount) {
            LOG_ERROR(QString("配置文件同步失败 - 工单: %1").arg(orderNumber));
            return false;
        }

        LOG_INFO(QString("已更新工单配置 - 工单: %1, 总数: %2").arg(orderNumber).arg(totalCount));
        return true;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("更新工单配置时发生异常: %1").arg(e.what()));
        return false;
    }
}
// 在析构函数中保存工单信息


void MainWindow::showProcessedOrders()
{
    try {
        QDialog dialog(this);
        dialog.setWindowTitle("本次启动处理过的工单列表");
        QVBoxLayout* layout = new QVBoxLayout(&dialog);

        QTableWidget* table = new QTableWidget(&dialog);
        table->setColumnCount(4);
        table->setHorizontalHeaderLabels({"工单号", "总数", "已完成", "剩余"});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);

        // 填充表格数据
        for (const QString& orderNumber : processedOrders) {
            int row = table->rowCount();
            table->insertRow(row);

            // 工单号（居中显示）
            QTableWidgetItem* item0 = new QTableWidgetItem(orderNumber);
            item0->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 0, item0);

            // 从配置文件获取总数
            int totalCount = readWorkOrderTotalFromNumFile(orderNumber);

            // 计算已完成数量（基于实际文件夹状态）
            int completedCount = countAllLogFilesInOrderFolder(orderNumber, true);

            // 计算剩余数量
            int remainingCount = totalCount > 0 ? totalCount - completedCount : 0;

            // 设置表格项（居中显示）
            QTableWidgetItem* item1 = new QTableWidgetItem(QString::number(totalCount));
            item1->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 1, item1);
            
            QTableWidgetItem* item2 = new QTableWidgetItem(QString::number(completedCount));
            item2->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 2, item2);
            
            QTableWidgetItem* item3 = new QTableWidgetItem(QString::number(remainingCount));
            item3->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, 3, item3);
        }

        // 自动调整列宽
        table->resizeColumnsToContents();

        layout->addWidget(table);

        // 添加关闭按钮
        QDialogButtonBox* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close);
        connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttonBox);

        dialog.resize(600, 400);
        dialog.exec();

    } catch (const std::exception& e) {
        LOG_ERROR(QString("显示处理过的工单列表时发生错误: %1").arg(e.what()));
    }
}

void MainWindow::syncProcessedOrders()
{
    try {
        bool tableUpdated = false;
        for (const QString& orderNumber : processedOrders) {
            if (suppressedAutoSyncOrders.contains(orderNumber))
                continue;
            // 检查工单是否已在主界面中
            if (!workOrders.contains(orderNumber)) {
                int totalCount = readWorkOrderTotalFromNumFile(orderNumber);
                if (totalCount > 0) {
                    // 创建新工单并添加到主界面
                    WorkOrder* order = new WorkOrder(orderNumber, totalCount);

                    // 统计实际完成数（基于实际文件夹状态）
                    order->completedCount = countAllLogFilesInOrderFolder(orderNumber, true);

                    workOrders.insert(orderNumber, order);
                    
                    // 分类工单到相应区域
                    classifyWorkOrder(order);
                    
                    tableUpdated = true;
                sendNewWorkOrderToCentral(orderNumber);
                    LOG_INFO(QString("自动同步添加工单: %1, 总数: %2").arg(orderNumber).arg(totalCount));
                } else {
                    // NUM.txt不存在或无效，显示通知
                    showNotification("NUM.txt缺失警告",
                                   QString("工单 %1 的NUM.txt文件不存在或无效！\n请检查工单根目录配置或手动添加工单。").arg(orderNumber),
                                   orderNumber);
                }
            }
        }

        // 只在有更新时才刷新表格
        if (tableUpdated) {
            updateTable();
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("同步处理过的工单时发生错误: %1").arg(e.what()));
    }
}
void MainWindow::setupWebSocket()
{
    m_webSocketClient = new WebSocketClient(this);

    connect(m_webSocketClient, &WebSocketClient::connectionStatusChanged,
            this, &MainWindow::onWebSocketStatusChanged);
    connect(m_webSocketClient, &WebSocketClient::connected,
            this, &MainWindow::onWebSocketConnected);
    connect(m_webSocketClient, &WebSocketClient::disconnected,
            this, &MainWindow::onWebSocketDisconnected);
    connect(m_webSocketClient, &WebSocketClient::error,
            this, &MainWindow::onWebSocketError);
}

void MainWindow::onConnectButtonClicked()
{
    if (!m_webSocketClient->isConnected()) {
        m_webSocketClient->connectToServer();
        m_connectButton->setEnabled(false);
    } else {
        m_webSocketClient->disconnectFromServer();
    }
}

void MainWindow::onWebSocketStatusChanged(const QString& status)
{
    m_connectButton->setText(status);
}

void MainWindow::onWebSocketConnected()
{
    m_connectButton->setEnabled(true);
    sendAllWorkOrdersToCentral();
}

void MainWindow::onWebSocketDisconnected()
{
    m_connectButton->setEnabled(true);
    m_connectButton->setText("连接中控");
    m_sentWorkOrdersAfterConnect.clear();
}

void MainWindow::onWebSocketError(const QString& error)
{
    m_connectButton->setEnabled(true);
    // 移除弹窗报错，只保留按钮文字更新（通过connectionStatusChanged信号已处理）
    // QMessageBox::warning(this, "连接错误", error);
    LOG_WARNING(QString("WebSocket连接错误: %1").arg(error));
}
QJsonObject MainWindow::createWorkOrdersJson()
{
    QJsonObject data;
    data["machineId"] = machineId;  // 假设您有 machineId 成员变量

    QJsonArray ordersArray;
    for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
        QJsonObject orderObj;
        orderObj["orderNumber"] = it.key();
        orderObj["totalCount"] = it.value()->totalCount;
        orderObj["completedCount"] = it.value()->completedCount;
        orderObj["failCount"] = it.value()->failedCount;
        orderObj["remainingCount"] = it.value()->totalCount - it.value()->completedCount;
        ordersArray.append(orderObj);
    }

    data["orders"] = ordersArray;
    return data;
}

QJsonObject MainWindow::createSingleWorkOrderJson(const QString& orderNumber) const
{
    QJsonObject data;
    data["machineId"] = machineId;

    QJsonArray ordersArray;
    if (workOrders.contains(orderNumber) && workOrders.value(orderNumber)) {
        WorkOrder* order = workOrders.value(orderNumber);
        QJsonObject orderObj;
        orderObj["orderNumber"] = orderNumber;
        orderObj["totalCount"] = order->totalCount;
        orderObj["completedCount"] = order->completedCount;
        orderObj["failCount"] = order->failedCount;
        orderObj["remainingCount"] = order->totalCount - order->completedCount;
        ordersArray.append(orderObj);
    }

    data["orders"] = ordersArray;
    return data;
}

void MainWindow::sendAllWorkOrdersToCentral()
{
    if (!m_webSocketClient || !m_webSocketClient->isConnected()) {
        return;
    }

    m_webSocketClient->sendWorkOrdersInfo(createWorkOrdersJson());
    m_sentWorkOrdersAfterConnect.clear();
    for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
        m_sentWorkOrdersAfterConnect.insert(it.key());
    }
    LOG_INFO(QString("已向中控发送全量工单，数量: %1").arg(m_sentWorkOrdersAfterConnect.size()));
}

void MainWindow::sendNewWorkOrderToCentral(const QString& orderNumber)
{
    if (orderNumber.isEmpty()) {
        return;
    }

    if (!m_webSocketClient || !m_webSocketClient->isConnected()) {
        return;
    }

    if (m_sentWorkOrdersAfterConnect.contains(orderNumber)) {
        return;
    }

    if (!workOrders.contains(orderNumber)) {
        return;
    }

    m_webSocketClient->sendWorkOrdersInfo(createSingleWorkOrderJson(orderNumber));
    m_sentWorkOrdersAfterConnect.insert(orderNumber);
    LOG_INFO(QString("已向中控发送新增工单: %1").arg(orderNumber));
}
/*void MainWindow::createActionButtons(int row)
{
    QWidget* widget = new QWidget();
    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(5, 0, 5, 0);
    layout->setSpacing(10);

    // 修改按钮
    QPushButton* editButton = new QPushButton("修改");
    editButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #FFA726;"  // 橙色
        "    color: white;"
        "    border: none;"
        "    border-radius: 3px;"
        "    min-width: 60px;"
        "    padding: 5px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #FB8C00;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #EF6C00;"
        "}"
    );

    // 删除按钮
    QPushButton* deleteButton = new QPushButton("删除");
    deleteButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #EF5350;"  // 红色
        "    color: white;"
        "    border: none;"
        "    border-radius: 3px;"
        "    min-width: 60px;"
        "    padding: 5px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #E53935;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #D32F2F;"
        "}"
    );

    // 失败按钮
    QPushButton* failButton = new QPushButton("失败+1");
    failButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #78909C;"  // 灰蓝色
        "    color: white;"
        "    border: none;"
        "    border-radius: 3px;"
        "    min-width: 60px;"
        "    padding: 5px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #607D8B;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #546E7A;"
        "}"
    );

    layout->addWidget(editButton);
    layout->addWidget(deleteButton);
    layout->addWidget(failButton);

    // ... 其他代码保持不变 ...
}*/
/*void MainWindow::setupUI()
{
    // ... 其他代码保持不变 ...

    // 添加工单按钮
    QPushButton* addButton = new QPushButton("添加工单", this);
    addButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    addButton->setMinimumHeight(40);
    addButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #4CAF50;"  // 绿色
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #45a049;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #3d8b40;"
        "}"
    );
    connect(addButton, &QPushButton::clicked, this, &MainWindow::onAddWorkOrder);

    // 快捷添加按钮
    QPushButton* quickAddButton = new QPushButton("快捷添加", this);
    quickAddButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quickAddButton->setMinimumHeight(40);
    quickAddButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #2196F3;"  // 蓝色
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #1976D2;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #1565C0;"
        "}"
    );
    connect(quickAddButton, &QPushButton::clicked, this, &MainWindow::onQuickAdd);

    // ... 其他代码保持不变 ...
}*/

// 添加新的槽函数
void MainWindow::onStartCopyClicked()
{
    CopyControlDialog dialog(this);
    dialog.exec();
}

// 辅助函数：检查是否为网络路径
bool MainWindow::isNetworkPath(const QString& path) const
{
    // 检查Windows网络路径格式 (\\server\share) 和Unix风格网络路径格式 (/server/share)
    // 注意：QSettings读取INI文件时可能会将 \\ 转换为单个 \ 或 /
    return path.startsWith("\\\\") || path.startsWith("//") ||
           path.startsWith("\\") ||  // 单个反斜杠开头（QSettings转换后的Windows网络路径）
           (path.startsWith("/") && path.contains("/") && path.count("/") >= 2);
}

// 辅助函数：检查网络路径可访问性
bool MainWindow::checkNetworkPathAccessibility(const QString& path) const
{
    if (!isNetworkPath(path)) {
        return true; // 本地路径总是可访问的
    }

    try {
        // 尝试访问网络路径
        QDir dir(path);
        if (dir.exists()) {
            return true;
        }

        // 如果路径不存在，尝试访问父目录
        QString parentPath = QFileInfo(path).path();
        if (parentPath.isEmpty()) {
            return false;
        }

        QDir parentDir(parentPath);
        return parentDir.exists();

    } catch (const std::exception& e) {
        LOG_ERROR(QString("检查网络路径可访问性时发生异常: %1, 路径: %2").arg(e.what()).arg(path));
        return false;
    }
}

// 辅助函数：创建网络路径文件夹
bool MainWindow::createNetworkFolder(const QString& folderPath)
{
    if (!isNetworkPath(folderPath)) {
        // 本地路径使用标准方法
        return QDir().mkpath(folderPath);
    }

    try {
        LOG_INFO(QString("尝试创建网络路径文件夹: %1").arg(folderPath));

        // 方法1：使用QDir::mkpath
        QDir dir;
        if (dir.mkpath(folderPath)) {
            LOG_INFO(QString("成功使用QDir::mkpath创建网络文件夹: %1").arg(folderPath));
            return true;
        }

        // 方法2：逐级创建路径
        QStringList pathParts = folderPath.split("/", Qt::SkipEmptyParts);
        if (pathParts.isEmpty()) {
            return false;
        }

        // 构建网络根路径
        QString currentPath;
        if (folderPath.startsWith("\\\\")) {
            // Windows网络路径（原始格式）
            currentPath = "\\\\";
            for (int i = 0; i < pathParts.size(); ++i) {
                if (i == 0) {
                    currentPath += pathParts[i];
                } else {
                    currentPath += "\\" + pathParts[i];
                }

                QDir currentDir(currentPath);
                if (!currentDir.exists()) {
                    if (!currentDir.mkpath(".")) {
                        LOG_WARNING(QString("无法创建网络路径部分: %1").arg(currentPath));
                        // 继续尝试，可能部分路径已存在
                    }
                }
            }
        } else if (folderPath.startsWith("\\")) {
            // 单个反斜杠开头的网络路径（QSettings转换后的Windows网络路径）
            currentPath = "\\\\";
            for (int i = 0; i < pathParts.size(); ++i) {
                if (i == 0) {
                    currentPath += pathParts[i];
                } else {
                    currentPath += "\\" + pathParts[i];
                }

                QDir currentDir(currentPath);
                if (!currentDir.exists()) {
                    if (!currentDir.mkpath(".")) {
                        LOG_WARNING(QString("无法创建网络路径部分: %1").arg(currentPath));
                        // 继续尝试，可能部分路径已存在
                    }
                }
            }
        } else if (folderPath.startsWith("/") && folderPath.contains("/") && folderPath.count("/") >= 2) {
            // Unix风格网络路径（可能是QSettings转换后的Windows网络路径）
            currentPath = "/";
            for (const QString& part : pathParts) {
                currentPath += part + "/";
                QDir currentDir(currentPath);
                if (!currentDir.exists()) {
                    if (!currentDir.mkpath(".")) {
                        LOG_WARNING(QString("无法创建网络路径部分: %1").arg(currentPath));
                    }
                }
            }
        } else {
            // 其他情况，使用标准方法
            return QDir().mkpath(folderPath);
        }

        // 最终检查
        QDir finalDir(folderPath);
        if (finalDir.exists()) {
            LOG_INFO(QString("成功逐级创建网络文件夹: %1").arg(folderPath));
            return true;
        }

        LOG_WARNING(QString("网络文件夹创建失败: %1").arg(folderPath));
        return false;

    } catch (const std::exception& e) {
        LOG_ERROR(QString("创建网络文件夹时发生异常: %1, 路径: %2").arg(e.what()).arg(folderPath));
        return false;
    }
}

// 完整的通知管理函数实现
bool MainWindow::shouldShowNotification(const QString& orderNumber)
{
    QDateTime currentTime = QDateTime::currentDateTime();

    // 检查是否在冷却期内
    if (lastNotificationTime.contains(orderNumber)) {
        QDateTime lastTime = lastNotificationTime[orderNumber];
        if (lastTime.secsTo(currentTime) < NOTIFICATION_COOLDOWN_MINUTES * 60) {
            LOG_INFO(QString("工单 %1 的通知在冷却期内，跳过弹窗").arg(orderNumber));
            return false;
        }
    }

    return true;
}

void MainWindow::recordNotification(const QString& orderNumber)
{
    QDateTime currentTime = QDateTime::currentDateTime();
    lastNotificationTime[orderNumber] = currentTime;
    LOG_INFO(QString("记录工单 %1 的通知时间").arg(orderNumber));
}

void MainWindow::showNotification(const QString& title, const QString& message, const QString& orderNumber)
{
    // 检查是否应该显示通知
    if (!shouldShowNotification(orderNumber)) {
        return;
    }

    // 记录通知
    recordNotification(orderNumber);

    // 添加到通知历史记录
    NotificationRecord record;
    record.timestamp = QDateTime::currentDateTime();
    record.title = title;
    record.message = message;
    record.orderNumber = orderNumber;

    notificationHistory.append(record);

    // 限制历史记录数量
    if (notificationHistory.size() > NOTIFICATION_HISTORY_LIMIT) {
        notificationHistory.removeFirst();
    }

    // 显示通知
    QMessageBox* notificationDialog = new QMessageBox(this);
    notificationDialog->setWindowTitle(title);
    notificationDialog->setText(message);
    notificationDialog->setIcon(QMessageBox::Warning);
    notificationDialog->setStandardButtons(QMessageBox::Ok);

    // 显示通知（不自动关闭，需要用户手动确认）
    notificationDialog->show();

    LOG_INFO(QString("显示通知 - 工单: %1, 标题: %2").arg(orderNumber).arg(title));
}

void MainWindow::cleanupExpiredNotifications()
{
    QDateTime currentTime = QDateTime::currentDateTime();
    QDateTime expireTime = currentTime.addDays(-7); // 7天前的通知过期

    // 清理过期的通知历史记录
    auto it = notificationHistory.begin();
    while (it != notificationHistory.end()) {
        if (it->timestamp < expireTime) {
            it = notificationHistory.erase(it);
        } else {
            ++it;
        }
    }

    // 清理过期的最后通知时间记录
    auto mapIt = lastNotificationTime.begin();
    while (mapIt != lastNotificationTime.end()) {
        if (mapIt.value() < expireTime) {
            mapIt = lastNotificationTime.erase(mapIt);
        } else {
            ++mapIt;
        }
    }

    // 清理过期的溢出报警时间记录
    auto overflowIt = lastOverflowAlertTime.begin();
    while (overflowIt != lastOverflowAlertTime.end()) {
        if (overflowIt.value() < expireTime) {
            overflowIt = lastOverflowAlertTime.erase(overflowIt);
        } else {
            ++overflowIt;
        }
    }

    LOG_INFO(QString("清理过期通知完成，当前历史记录数: %1").arg(notificationHistory.size()));
}

bool MainWindow::shouldShowOverflowAlert(const QString& orderNumber)
{
    QDateTime currentTime = QDateTime::currentDateTime();

    // 检查是否在冷却期内
    if (lastOverflowAlertTime.contains(orderNumber)) {
        QDateTime lastTime = lastOverflowAlertTime[orderNumber];
        if (lastTime.secsTo(currentTime) < OVERFLOW_ALERT_COOLDOWN_MINUTES * 60) {
            LOG_INFO(QString("工单 %1 的溢出报警在冷却期内，跳过弹窗").arg(orderNumber));
            return false;
        }
    }

    return true;
}

void MainWindow::recordOverflowAlert(const QString& orderNumber)
{
    QDateTime currentTime = QDateTime::currentDateTime();
    lastOverflowAlertTime[orderNumber] = currentTime;
    LOG_INFO(QString("记录工单 %1 的溢出报警时间").arg(orderNumber));
}

void MainWindow::showOverflowAlert(const QString& orderNumber)
{
    // 检查是否应该显示报警
    if (!shouldShowOverflowAlert(orderNumber)) {
        return;
    }

    // 记录报警
    recordOverflowAlert(orderNumber);

    // 使用单例管理器显示报警
    OverflowAlertManager::getInstance()->showOverflowAlert(orderNumber);
}

void MainWindow::addQuickAddButton()
{
    // 在UI中添加快捷添加按钮
    QPushButton* quickAddButton = new QPushButton("快捷添加", this);
    quickAddButton->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    quickAddButton->setMinimumHeight(40);
    quickAddButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #2196F3;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #1976D2;"
        "}"
        "QPushButton:pressed {"
        "    background-color: #1565C0;"
        "}"
    );
    connect(quickAddButton, &QPushButton::clicked, this, &MainWindow::onQuickAdd);

    // 添加到按钮布局中（需要在setupUI中调用）
}

void MainWindow::onQuickAdd()
{
    // 快捷添加功能：自动检测最近处理的工单并添加
    if (processedOrders.isEmpty()) {
        QMessageBox::information(this, "提示", "没有检测到最近处理的工单");
        return;
    }

    QStringList recentOrders = processedOrders.values();
    if (recentOrders.size() == 1) {
        // 只有一个工单，直接添加
        QString orderNumber = recentOrders.first();
        if (!workOrders.contains(orderNumber)) {
            int total = getWorkOrderTotal(orderNumber);
            if (total > 0) {
                WorkOrder* order = new WorkOrder(orderNumber, total);
                workOrders.insert(orderNumber, order);
                suppressedAutoSyncOrders.remove(orderNumber);

                // 分类工单到相应区域
                classifyWorkOrder(order);

                updateTable();
                sendNewWorkOrderToCentral(orderNumber);
                LOG_INFO(QString("快捷添加工单: %1, 总数: %2").arg(orderNumber).arg(total));
                QMessageBox::information(this, "成功", QString("已快捷添加工单: %1").arg(orderNumber));
            }
        }
    } else {
        // 多个工单，显示选择对话框
        QMessageBox::information(this, "提示", QString("检测到 %1 个最近处理的工单，请手动添加").arg(recentOrders.size()));
    }
}

void MainWindow::recordProcessedOrder(const QString& orderNumber)
{
    processedOrders.insert(orderNumber);
    startupProcessedOrders.insert(orderNumber);
    LOG_INFO(QString("记录处理的工单: %1").arg(orderNumber));
}

void MainWindow::checkAndMoveOverflowFiles()
{
    // 检查所有工单的溢出情况
    for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
        QString orderNumber = it.key();
        WorkOrder* order = it.value();

        int currentCount = countAllLogFilesInOrderFolder(orderNumber, true);
        if (currentCount > order->totalCount) {
            int overflowCount = currentCount - order->totalCount;
            LOG_WARNING(QString("工单 %1 检测到溢出: 当前 %2, 总数 %3, 溢出 %4")
                           .arg(orderNumber)
                           .arg(currentCount)
                           .arg(order->totalCount)
                           .arg(overflowCount));

            // 显示溢出通知
            showNotification("工单溢出检测",
                           QString("工单 %1 检测到溢出文件！\n当前: %2, 总数: %3, 溢出: %4")
                               .arg(orderNumber)
                               .arg(currentCount)
                               .arg(order->totalCount)
                               .arg(overflowCount),
                           orderNumber);
        }
    }
}

int MainWindow::getOverflowCount() const
{
    int totalOverflow = 0;
    for (auto it = workOrders.begin(); it != workOrders.end(); ++it) {
        QString orderNumber = it.key();
        WorkOrder* order = it.value();
        int currentCount = countAllLogFilesInOrderFolder(orderNumber, true);
        if (currentCount > order->totalCount) {
            totalOverflow += (currentCount - order->totalCount);
        }
    }
    return totalOverflow;
}

QString MainWindow::getOverflowPath() const
{
    return archivePath + "/overflow";
}

void MainWindow::updateQueueProgress()
{
    // 只有在批量处理时才显示进度
    if (isBatchProcessing && batchProgressDialog && totalFilesToProcess > 0) {
        int remainingFiles = fileQueue.size();
        int processedFiles = totalFilesToProcess - remainingFiles;

        // 更新进度对话框
        batchProgressDialog->setValue(processedFiles);
        batchProgressDialog->setLabelText(
            QString("正在处理文件... (%1/%2) 剩余队列: %3")
                .arg(processedFiles)
                .arg(totalFilesToProcess)
                .arg(remainingFiles)
        );

        // 如果队列为空，批量处理完成
        if (remainingFiles == 0) {
            batchProgressDialog->close();
            delete batchProgressDialog;
            batchProgressDialog = nullptr;
            isBatchProcessing = false;
            LOG_INFO(QString("批量文件处理完成，共处理 %1 个文件").arg(processedFiles));
        }
    }
}

// 双表格相关函数实现
void MainWindow::updateIncompleteTable()
{
    try {
        incompleteTableWidget->setRowCount(incompleteWorkOrders.size());
        int row = 0;

        for (auto it = incompleteWorkOrders.begin(); it != incompleteWorkOrders.end(); ++it) {
            updateIncompleteRow(row, it.value());
            createIncompleteActionButtons(row);
            row++;
        }
    } catch (const std::exception& e) {
        qDebug() << "更新未完成工单表格时发生异常:" << e.what();
    }
}

void MainWindow::updateCompletedTable()
{
    try {
        completedTableWidget->setRowCount(completedWorkOrders.size());
        int row = 0;

        for (auto it = completedWorkOrders.begin(); it != completedWorkOrders.end(); ++it) {
            updateCompletedRow(row, it.value());
            createCompletedActionButtons(row);
            row++;
        }
    } catch (const std::exception& e) {
        qDebug() << "更新已完成工单表格时发生异常:" << e.what();
    }
}

void MainWindow::updateIncompleteRow(int row, WorkOrder* order)
{
    try {
        // 始终基于工单文件夹的实际状态更新计数
        int actualCount = countAllLogFilesInOrderFolder(order->orderNumber, true);
        int oldCount = order->completedCount;
        order->completedCount = actualCount;

        // 检查是否溢出（新计数超过总数）
        if (actualCount > order->totalCount) {
            LOG_WARNING(QString("工单 %1 计数溢出：实际 %2 > 总数 %3")
                           .arg(order->orderNumber)
                           .arg(actualCount)
                           .arg(order->totalCount));
        }

        // 设置表格项（居中显示）
        QTableWidgetItem* item0 = new QTableWidgetItem(order->orderNumber);
        item0->setTextAlignment(Qt::AlignCenter);
        incompleteTableWidget->setItem(row, 0, item0);
        
        QTableWidgetItem* item1 = new QTableWidgetItem(QString::number(order->totalCount));
        item1->setTextAlignment(Qt::AlignCenter);
        incompleteTableWidget->setItem(row, 1, item1);
        
        QTableWidgetItem* item2 = new QTableWidgetItem(QString::number(actualCount));
        item2->setTextAlignment(Qt::AlignCenter);
        incompleteTableWidget->setItem(row, 2, item2);
        
        QTableWidgetItem* item3 = new QTableWidgetItem(QString::number(order->failedCount));
        item3->setTextAlignment(Qt::AlignCenter);
        incompleteTableWidget->setItem(row, 3, item3);
        
        QTableWidgetItem* item4 = new QTableWidgetItem(QString::number(order->totalCount - actualCount));
        item4->setTextAlignment(Qt::AlignCenter);
        incompleteTableWidget->setItem(row, 4, item4);

        // 根据完成状态设置行颜色和重新分类
        if (actualCount > order->totalCount) {
            // 超过总数，设置红色背景提示
            for (int col = 0; col < 5; col++) {
                QTableWidgetItem* item = incompleteTableWidget->item(row, col);
                if (item) {
                    item->setBackground(QColor(255, 200, 200));  // 浅红色背景
                }
            }
        } else if (actualCount == order->totalCount) {
            // 完全相等，设置绿色背景提示并重新分类到已完成区域
            for (int col = 0; col < 5; col++) {
                QTableWidgetItem* item = incompleteTableWidget->item(row, col);
                if (item) {
                    item->setBackground(QColor(200, 255, 200));  // 浅绿色背景
                }
            }
            // 重新分类工单到已完成区域
            QTimer::singleShot(100, [this, order]() {
                classifyWorkOrder(order);
            });
        } else {
            // 未完成，设置正常颜色
            for (int col = 0; col < 5; col++) {
                QTableWidgetItem* item = incompleteTableWidget->item(row, col);
                if (item) {
                    item->setBackground(QColor(255, 255, 255));  // 白色背景
                }
            }
        }
    } catch (const std::exception& e) {
        qDebug() << "更新未完成工单行时发生异常:" << e.what();
    }
}

void MainWindow::updateCompletedRow(int row, WorkOrder* order)
{
    try {
        // 始终基于工单文件夹的实际状态更新计数
        int actualCount = countAllLogFilesInOrderFolder(order->orderNumber, true);
        int oldCount = order->completedCount;
        order->completedCount = actualCount;

        // 设置表格项（居中显示）
        QTableWidgetItem* item0 = new QTableWidgetItem(order->orderNumber);
        item0->setTextAlignment(Qt::AlignCenter);
        completedTableWidget->setItem(row, 0, item0);
        
        QTableWidgetItem* item1 = new QTableWidgetItem(QString::number(order->totalCount));
        item1->setTextAlignment(Qt::AlignCenter);
        completedTableWidget->setItem(row, 1, item1);
        
        QTableWidgetItem* item2 = new QTableWidgetItem(QString::number(actualCount));
        item2->setTextAlignment(Qt::AlignCenter);
        completedTableWidget->setItem(row, 2, item2);
        
        QTableWidgetItem* item3 = new QTableWidgetItem(QString::number(order->failedCount));
        item3->setTextAlignment(Qt::AlignCenter);
        completedTableWidget->setItem(row, 3, item3);
        
        QTableWidgetItem* item4 = new QTableWidgetItem(QString::number(order->totalCount - actualCount));
        item4->setTextAlignment(Qt::AlignCenter);
        completedTableWidget->setItem(row, 4, item4);

        // 检查是否从已完成变为未完成或溢出（异常情况）
        if (actualCount != order->totalCount) {
            // 异常：已完成工单数量不等于总数，设置红色背景提示
            for (int col = 0; col < 5; col++) {
                QTableWidgetItem* item = completedTableWidget->item(row, col);
                if (item) {
                    item->setBackground(QColor(255, 200, 200));  // 浅红色背景
                }
            }
            
            if (actualCount < order->totalCount) {
                // 触发异常报警
                showAnomalyAlert(order);
            } else if (actualCount > order->totalCount) {
                // 触发溢出报警
                showOverflowAlert(order->orderNumber);
            }
            
            // 重新分类工单到未完成区域
            QTimer::singleShot(100, [this, order]() {
                classifyWorkOrder(order);
            });
        } else {
            // 正常完成状态，设置绿色背景
            for (int col = 0; col < 5; col++) {
                QTableWidgetItem* item = completedTableWidget->item(row, col);
                if (item) {
                    item->setBackground(QColor(200, 255, 200));  // 浅绿色背景
                }
            }
        }
    } catch (const std::exception& e) {
        qDebug() << "更新已完成工单行时发生异常:" << e.what();
    }
}

void MainWindow::createIncompleteActionButtons(int row)
{
    QWidget* widget = new QWidget();
    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(5, 0, 5, 0);
    layout->setSpacing(5);

    // 修改总量按钮
    QPushButton* modifyBtn = new QPushButton("修改总量");
    modifyBtn->setProperty("row", row);
    connect(modifyBtn, &QPushButton::clicked, [this, row]() {
        onModifyTotalCount(row, incompleteTableWidget);
    });

    // 停止计数按钮
    QPushButton* stopBtn = new QPushButton("停止计数");
    stopBtn->setProperty("row", row);
    stopBtn->setProperty("tableWidget", QVariant::fromValue(static_cast<QWidget*>(incompleteTableWidget)));
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStopCounting);

    // 删除工单按钮
    QPushButton* deleteBtn = new QPushButton("删除工单");
    deleteBtn->setProperty("row", row);
    connect(deleteBtn, &QPushButton::clicked, [this, row]() {
        onDeleteWorkOrder(row, incompleteTableWidget);
    });

    // 查看溢出按钮
    QPushButton* checkOverflowBtn = new QPushButton("查看溢出");
    checkOverflowBtn->setProperty("row", row);
    checkOverflowBtn->setProperty("tableWidget", QVariant::fromValue(static_cast<QWidget*>(incompleteTableWidget)));
    connect(checkOverflowBtn, &QPushButton::clicked, this, &MainWindow::onCheckOverflow);

    layout->addWidget(modifyBtn);
    layout->addWidget(stopBtn);
    layout->addWidget(deleteBtn);
    layout->addWidget(checkOverflowBtn);

    incompleteTableWidget->setCellWidget(row, 5, widget);
}

void MainWindow::createCompletedActionButtons(int row)
{
    QWidget* widget = new QWidget();
    QHBoxLayout* layout = new QHBoxLayout(widget);
    layout->setContentsMargins(5, 0, 5, 0);
    layout->setSpacing(5);

    // 修改总量按钮
    QPushButton* modifyBtn = new QPushButton("修改总量");
    modifyBtn->setProperty("row", row);
    connect(modifyBtn, &QPushButton::clicked, [this, row]() {
        onModifyTotalCount(row, completedTableWidget);
    });

    // 停止计数按钮
    QPushButton* stopBtn = new QPushButton("停止计数");
    stopBtn->setProperty("row", row);
    stopBtn->setProperty("tableWidget", QVariant::fromValue(static_cast<QWidget*>(completedTableWidget)));
    connect(stopBtn, &QPushButton::clicked, this, &MainWindow::onStopCounting);

    // 删除工单按钮
    QPushButton* deleteBtn = new QPushButton("删除工单");
    deleteBtn->setProperty("row", row);
    connect(deleteBtn, &QPushButton::clicked, [this, row]() {
        onDeleteWorkOrder(row, completedTableWidget);
    });

    // 查看溢出按钮
    QPushButton* checkOverflowBtn = new QPushButton("查看溢出");
    checkOverflowBtn->setProperty("row", row);
    checkOverflowBtn->setProperty("tableWidget", QVariant::fromValue(static_cast<QWidget*>(completedTableWidget)));
    connect(checkOverflowBtn, &QPushButton::clicked, this, &MainWindow::onCheckOverflow);

    layout->addWidget(modifyBtn);
    layout->addWidget(stopBtn);
    layout->addWidget(deleteBtn);
    layout->addWidget(checkOverflowBtn);

    completedTableWidget->setCellWidget(row, 5, widget);
}

// 双表格操作函数实现
void MainWindow::onModifyTotalCount(int row, QTableWidget* tableWidget)
{
    try {
        QString orderNumber = tableWidget->item(row, 0)->text();
        int currentTotal = tableWidget->item(row, 1)->text().toInt();
        
        bool ok;
        int newTotal = QInputDialog::getInt(this, "修改总量", 
                                          QString("工单 %1 当前总量: %2\n请输入新的总量:").arg(orderNumber).arg(currentTotal),
                                          currentTotal, 1, 999999, 1, &ok);
        
        if (ok && newTotal != currentTotal) {
            // 更新工单对象
            WorkOrder* order = workOrders.value(orderNumber);
            if (order) {
                order->totalCount = newTotal;
                
                // 更新配置文件
                updateWorkorderConfig(orderNumber, newTotal);
                
                // 更新NUM.txt文件
                if (!writeWorkOrderTotalToNumFile(orderNumber, newTotal)) {
                    // 创建全屏NUM.txt文件操作失败报警
                    FullScreenAlertDialog* errorDialog = new FullScreenAlertDialog(
                        FullScreenAlertDialog::FileOperationError,
                        "NUM.txt文件操作失败",
                        QString("NUM.txt文件更新失败！\n\n工单: %1\n新总量: %2\n\n请检查工单文件夹权限或网络连接。").arg(orderNumber).arg(newTotal),
                        this
                    );
                    
                    errorDialog->setOrderNumber(orderNumber);
                    errorDialog->setAutoClose(0); // 不自动关闭，需要用户手动确认
                    
                    // 连接关闭信号，确保对话框被正确清理
                    connect(errorDialog, &FullScreenAlertDialog::closed, [errorDialog]() {
                        errorDialog->deleteLater();
                    });
                    
                    connect(errorDialog, &FullScreenAlertDialog::confirmed, [errorDialog]() {
                        errorDialog->deleteLater();
                    });
                    
                    LOG_ERROR(QString("NUM.txt更新失败 - 工单: %1, 新总量: %2").arg(orderNumber).arg(newTotal));
                } else {
                    LOG_INFO(QString("NUM.txt更新成功 - 工单: %1, 新总量: %2").arg(orderNumber).arg(newTotal));
                }
                
                // 重新分类工单
                classifyWorkOrder(order);
                
                // 更新表格显示
                updateTable();
                
                LOG_INFO(QString("工单 %1 总量已修改: %2 -> %3").arg(orderNumber).arg(currentTotal).arg(newTotal));
            }
        }
    } catch (const std::exception& e) {
        qDebug() << "修改总量时发生异常:" << e.what();
    }
}

void MainWindow::onDeleteWorkOrder(int row, QTableWidget* tableWidget)
{
    try {
        QString orderNumber = tableWidget->item(row, 0)->text();
        
        int ret = QMessageBox::question(this, "确认删除", 
                                       QString("确定要删除工单 %1 吗？\n此操作不可撤销！").arg(orderNumber),
                                       QMessageBox::Yes | QMessageBox::No);
        
        if (ret == QMessageBox::Yes) {
            WorkOrder* order = workOrders.value(orderNumber);
            workOrders.remove(orderNumber);
            incompleteWorkOrders.remove(orderNumber);
            completedWorkOrders.remove(orderNumber);
            delete order;
            processedOrders.remove(orderNumber);
            startupProcessedOrders.remove(orderNumber);
            suppressedAutoSyncOrders.insert(orderNumber);

            // 更新表格显示
            updateTable();

            LOG_INFO(QString("工单 %1 已删除（已禁止自动同步加回，直至再次手动添加工单）").arg(orderNumber));
        }
    } catch (const std::exception& e) {
        qDebug() << "删除工单时发生异常:" << e.what();
    }
}

void MainWindow::onMarkAsFailed(int row, QTableWidget* tableWidget)
{
    try {
        QString orderNumber = tableWidget->item(row, 0)->text();
        
        int ret = QMessageBox::question(this, "确认标记失败", 
                                       QString("确定要将工单 %1 标记为失败吗？").arg(orderNumber),
                                       QMessageBox::Yes | QMessageBox::No);
        
        if (ret == QMessageBox::Yes) {
            WorkOrder* order = workOrders.value(orderNumber);
            if (order) {
                order->failedCount++;
                
                // 重新分类工单
                classifyWorkOrder(order);
                
                // 更新表格显示
                updateTable();
                
                LOG_INFO(QString("工单 %1 已标记为失败").arg(orderNumber));
            }
        }
    } catch (const std::exception& e) {
        qDebug() << "标记失败时发生异常:" << e.what();
    }
}

void MainWindow::onViewWorkOrderDetails(int row, QTableWidget* tableWidget)
{
    try {
        QString orderNumber = tableWidget->item(row, 0)->text();
        int totalCount = tableWidget->item(row, 1)->text().toInt();
        int completedCount = tableWidget->item(row, 2)->text().toInt();
        int failedCount = tableWidget->item(row, 3)->text().toInt();
        int remainingCount = tableWidget->item(row, 4)->text().toInt();
        
        QString details = QString("工单详情\n\n"
                                "工单号: %1\n"
                                "总数量: %2\n"
                                "已完成: %3\n"
                                "失败数量: %4\n"
                                "剩余数量: %5\n"
                                "完成率: %6%")
                            .arg(orderNumber)
                            .arg(totalCount)
                            .arg(completedCount)
                            .arg(failedCount)
                            .arg(remainingCount)
                            .arg(totalCount > 0 ? (completedCount * 100 / totalCount) : 0);
        
        QMessageBox::information(this, "工单详情", details);
    } catch (const std::exception& e) {
        qDebug() << "查看详情时发生异常:" << e.what();
    }
}

// 工单分类和迁移函数实现
void MainWindow::classifyWorkOrder(WorkOrder* order)
{
    try {
        if (!order) return;
        
        QString orderNumber = order->orderNumber;
        bool isCompleted = (order->completedCount == order->totalCount);  // 只有完全相等才算完成
        bool isOverflow = (order->completedCount > order->totalCount);    // 超过总数算溢出
        
        // 从两个分类容器中移除（如果存在）
        incompleteWorkOrders.remove(orderNumber);
        completedWorkOrders.remove(orderNumber);
        
        // 根据完成状态添加到相应容器
        if (isCompleted) {
            // 只有完全相等的工单才放到已完成区域
            completedWorkOrders.insert(orderNumber, order);
            LOG_INFO(QString("工单 %1 已分类到已完成区域").arg(orderNumber));
        } else {
            // 未完成或溢出的工单都放到未完成区域
            incompleteWorkOrders.insert(orderNumber, order);
            if (isOverflow) {
                LOG_INFO(QString("工单 %1 已分类到未完成区域（溢出）").arg(orderNumber));
            } else {
                LOG_INFO(QString("工单 %1 已分类到未完成区域").arg(orderNumber));
            }
        }
        
        // 更新表格显示
        updateTable();
        
    } catch (const std::exception& e) {
        qDebug() << "工单分类时发生异常:" << e.what();
    }
}

void MainWindow::moveToIncompleteSection(WorkOrder* order)
{
    try {
        if (!order) return;
        
        QString orderNumber = order->orderNumber;
        
        // 从已完成容器移除
        completedWorkOrders.remove(orderNumber);
        
        // 添加到未完成容器
        incompleteWorkOrders.insert(orderNumber, order);
        
        LOG_INFO(QString("工单 %1 已移动到未完成区域").arg(orderNumber));
        
        // 更新表格显示
        updateIncompleteTable();
        updateCompletedTable();
        
    } catch (const std::exception& e) {
        qDebug() << "移动工单到未完成区域时发生异常:" << e.what();
    }
}

void MainWindow::moveToCompletedSection(WorkOrder* order)
{
    try {
        if (!order) return;
        
        QString orderNumber = order->orderNumber;
        
        // 从未完成容器移除
        incompleteWorkOrders.remove(orderNumber);
        
        // 添加到已完成容器
        completedWorkOrders.insert(orderNumber, order);
        
        // 显示完成通知
        showCompletionNotification(order);
        
        LOG_INFO(QString("工单 %1 已移动到已完成区域").arg(orderNumber));
        
        // 更新表格显示
        updateIncompleteTable();
        updateCompletedTable();
        
    } catch (const std::exception& e) {
        qDebug() << "移动工单到已完成区域时发生异常:" << e.what();
    }
}

void MainWindow::showCompletionNotification(WorkOrder* order)
{
    try {
        if (!order) return;
        
        QString message = QString("工单 %1 已完成！\n\n"
                                "总数量: %2\n"
                                "已完成: %3\n"
                                "完成率: 100%")
                            .arg(order->orderNumber)
                            .arg(order->totalCount)
                            .arg(order->completedCount);
        
    //    QMessageBox::information(this, "工单完成", message);
        
        LOG_INFO(QString("工单 %1 完成通知已显示").arg(order->orderNumber));
        
    } catch (const std::exception& e) {
        qDebug() << "显示完成通知时发生异常:" << e.what();
    }
}

void MainWindow::showAnomalyAlert(WorkOrder* order)
{
    try {
        if (!order) return;
        
        // 暂时移除全屏报警，改为只在日志中记录
        LOG_WARNING(QString("工单状态异常 - 工单: %1, 当前状态: %2/%3, 该工单已完成后又新增了文件，请检查是否有重复文件或配置错误")
                   .arg(order->orderNumber)
                   .arg(order->completedCount)
                   .arg(order->totalCount));
        
        // TODO: 如需重新启用全屏报警，请取消以下注释并注释掉上面的LOG_WARNING
        /*
        QString message = QString("警告：工单 %1 状态异常！\n\n"
                                "该工单已完成后又新增了文件。\n"
                                "当前状态：%2/%3\n\n"
                                "请检查是否有重复文件或配置错误。")
                            .arg(order->orderNumber)
                            .arg(order->completedCount)
                            .arg(order->totalCount);
        
        // 创建全屏异常报警对话框
        FullScreenAlertDialog* alertDialog = new FullScreenAlertDialog(
            FullScreenAlertDialog::AnomalyAlert,
            "工单状态异常",
            message,
            this
        );
        
        alertDialog->setOrderNumber(order->orderNumber);
        alertDialog->setAutoClose(0); // 不自动关闭，需要用户手动确认
        
        // 连接关闭信号，确保对话框被正确清理
        connect(alertDialog, &FullScreenAlertDialog::closed, [alertDialog]() {
            alertDialog->deleteLater();
        });
        
        connect(alertDialog, &FullScreenAlertDialog::confirmed, [alertDialog]() {
            alertDialog->deleteLater();
        });
        
        LOG_WARNING(QString("显示全屏异常报警 - 工单: %1").arg(order->orderNumber));
        */
        
    } catch (const std::exception& e) {
        LOG_ERROR(QString("记录工单状态异常时发生异常: %1").arg(e.what()));
    }
}

void MainWindow::checkWorkOrderStatusChange(const QString& orderNumber)
{
    try {
        WorkOrder* order = workOrders.value(orderNumber);
        if (!order) return;
        
        // 获取当前实际计数
        int currentCount = countAllLogFilesInOrderFolder(orderNumber, true);
        int oldCount = order->completedCount;
        
        // 更新计数
        order->completedCount = currentCount;
        
        // 检查状态变化
        bool wasCompleted = (oldCount >= order->totalCount);
        bool isNowCompleted = (currentCount >= order->totalCount);
        
        if (!wasCompleted && isNowCompleted) {
            // 从未完成变为已完成
            LOG_INFO(QString("工单 %1 状态变化：未完成 -> 已完成").arg(orderNumber));
            moveToCompletedSection(order);
        } else if (wasCompleted && !isNowCompleted) {
            // 从已完成变为未完成（异常情况）
            LOG_WARNING(QString("工单 %1 状态变化：已完成 -> 未完成（异常）").arg(orderNumber));
            showAnomalyAlert(order);
            moveToIncompleteSection(order);
        }
        
    } catch (const std::exception& e) {
        qDebug() << "检查工单状态变化时发生异常:" << e.what();
    }
}

void MainWindow::setupAutoConnectTimer()
{
    try {
        // 创建自动连接定时器
        autoConnectTimer = new QTimer(this);
        autoConnectTimer->setInterval(600000); // 300秒 = 5分钟
        
        // 连接定时器信号到检查函数
        connect(autoConnectTimer, &QTimer::timeout, this, &MainWindow::checkAndAutoConnect);
        
        // 启动定时器
        autoConnectTimer->start();
        
        LOG_INFO("自动连接定时器已启动，每300秒检查一次连接状态");
        
    } catch (const std::exception& e) {
        LOG_ERROR(QString("设置自动连接定时器时发生异常: %1").arg(e.what()));
    }
}

void MainWindow::checkAndAutoConnect()
{
    try {
        // 检查WebSocket客户端是否已初始化
        if (!m_webSocketClient) {
            LOG_WARNING("WebSocket客户端未初始化，跳过自动连接检查");
            return;
        }
        
        // 检查是否已连接
        if (m_webSocketClient->isConnected()) {
            LOG_DEBUG("中控程序已连接，无需自动连接");
            return;
        }
        
        LOG_INFO("检测到中控程序未连接，尝试自动连接...");
        
        // 尝试自动连接（静默连接，不显示错误弹窗）
        // 注意：这里会触发WebSocketClient的信号机制，按钮文字会自动更新
        m_webSocketClient->connectToServer();
        
        // 记录自动连接尝试
        LOG_INFO("已尝试自动连接中控程序，按钮文字将通过信号机制自动更新");
        
    } catch (const std::exception& e) {
        LOG_ERROR(QString("自动连接检查时发生异常: %1").arg(e.what()));
    }
}

// 料号缓存管理函数实现
bool MainWindow::isMaterialPendingConfirmation(const QString& orderNumber, const QString& materialCode)
{
    QString cacheDir = getMaterialCachePath(orderNumber, materialCode);
    return QDir(cacheDir).exists();
}

bool MainWindow::moveFileToMaterialCache(const QString& filePath, const QString& orderNumber, const QString& materialCode)
{
    try {
        QString cacheDir = getMaterialCachePath(orderNumber, materialCode);
        
        // 确保缓存目录存在
        QDir().mkpath(cacheDir);
        
        // 构造目标文件路径
        QString fileName = QFileInfo(filePath).fileName();
        QString targetPath = QDir(cacheDir).filePath(fileName);
        
        // 移动文件到缓存目录
        if (QFile::rename(filePath, targetPath)) {
            LOG_INFO(QString("文件已移动到料号缓存: %1 -> %2").arg(filePath).arg(targetPath));
            return true;
        } else {
            LOG_ERROR(QString("移动文件到料号缓存失败: %1 -> %2").arg(filePath).arg(targetPath));
            return false;
        }
    } catch (const std::exception& e) {
        LOG_ERROR(QString("移动文件到料号缓存时发生异常: %1").arg(e.what()));
        return false;
    }
}

void MainWindow::moveFilesFromCacheToArchive(const QString& orderNumber, const QString& materialCode)
{
    try {
        QString cacheDir = getMaterialCachePath(orderNumber, materialCode);
        QDir cacheDirObj(cacheDir);
        
        if (!cacheDirObj.exists()) {
            LOG_WARNING(QString("料号缓存目录不存在: %1").arg(cacheDir));
            return;
        }
        
        // 获取缓存目录中的所有文件
        QStringList files = cacheDirObj.entryList(QDir::Files);
        
        LOG_INFO(QString("开始将料号 %1 的 %2 个文件从缓存移动到正式归档（保留缓存目录结构）").arg(materialCode).arg(files.size()));
        
        for (const QString& fileName : files) {
            QString sourcePath = cacheDirObj.filePath(fileName);
            
            // 构造目标路径（移动到工单文件夹）
            QString targetPath;
            if (isNetworkPath(archivePath)) {
                if (archivePath.startsWith("\\")) {
                    targetPath = archivePath + "\\" + orderNumber + "\\" + fileName;
                } else if (archivePath.startsWith("\\\\")) {
                    targetPath = archivePath + "\\" + orderNumber + "\\" + fileName;
                } else {
                    targetPath = archivePath + "/" + orderNumber + "/" + fileName;
                }
            } else {
                targetPath = QDir(archivePath).filePath(orderNumber + "/" + fileName);
            }
            
            // 确保目标目录存在
            QDir targetDir = QFileInfo(targetPath).dir();
            if (!targetDir.exists()) {
                if (isNetworkPath(archivePath)) {
                    createNetworkFolder(targetDir.absolutePath());
                } else {
                    targetDir.mkpath(".");
                }
            }
            
            // 移动文件
            if (QFile::rename(sourcePath, targetPath)) {
                LOG_INFO(QString("文件从缓存移动到正式归档: %1 -> %2").arg(sourcePath).arg(targetPath));
            } else {
                LOG_ERROR(QString("文件从缓存移动到正式归档失败: %1 -> %2").arg(sourcePath).arg(targetPath));
            }
        }
        
        LOG_INFO(QString("料号 %1 的文件归档完成，共处理 %2 个文件").arg(materialCode).arg(files.size()));
        
    } catch (const std::exception& e) {
        LOG_ERROR(QString("从缓存移动到正式归档时发生异常: %1").arg(e.what()));
    }
}

void MainWindow::cleanupMaterialCache(const QString& orderNumber, const QString& materialCode)
{
    try {
        QString cacheDir = getMaterialCachePath(orderNumber, materialCode);
        QDir cacheDirObj(cacheDir);
        
        if (cacheDirObj.exists()) {
            // 删除缓存目录（包括所有文件）
            // 注意：此函数现在主要用于特殊情况下的清理
            // 正常情况下，确认的料号会保留目录结构，拒绝的料号会保留文件和目录
            if (cacheDirObj.removeRecursively()) {
                LOG_INFO(QString("料号缓存目录已清理: %1").arg(cacheDir));
            } else {
                LOG_WARNING(QString("料号缓存目录清理失败: %1").arg(cacheDir));
            }
        }
        
        // 检查工单目录是否为空，如果为空则删除
        QString orderCacheDir = QDir(materialCachePath).filePath(orderNumber);
        QDir orderCacheDirObj(orderCacheDir);
        if (orderCacheDirObj.exists() && orderCacheDirObj.entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty()) {
            orderCacheDirObj.removeRecursively();
            LOG_INFO(QString("空的工单缓存目录已删除: %1").arg(orderCacheDir));
        }
        
    } catch (const std::exception& e) {
        LOG_ERROR(QString("清理料号缓存时发生异常: %1").arg(e.what()));
    }
}

QString MainWindow::getMaterialCachePath(const QString& orderNumber, const QString& materialCode)
{
    return QDir(materialCachePath).filePath(orderNumber + "/" + materialCode);
}

// OverflowAlertManager 实现
OverflowAlertManager* OverflowAlertManager::instance = nullptr;

OverflowAlertManager* OverflowAlertManager::getInstance()
{
    if (!instance) {
        instance = new OverflowAlertManager();
        // 初始化冷却检查定时器
        instance->cooldownTimer = new QTimer();
        instance->cooldownTimer->setInterval(60000); // 每分钟检查一次
        QObject::connect(instance->cooldownTimer, &QTimer::timeout, 
                        []() { OverflowAlertManager::getInstance()->checkCooldownExpired(); });
        instance->cooldownTimer->start();
    }
    return instance;
}

void OverflowAlertManager::showOverflowAlert(const QString& orderNumber)
{
    QDateTime now = QDateTime::currentDateTime();
    
    // 检查冷却时间是否已过
    if (lastAlertTime.contains(orderNumber)) {
        QDateTime lastTime = lastAlertTime[orderNumber];
        int minutesSinceLastAlert = lastTime.secsTo(now) / 60;
        
        if (minutesSinceLastAlert < cooldownMinutes) {
            LOG_INFO(QString("工单 %1 在冷却期内（%2/%3分钟），跳过重复报警").arg(orderNumber).arg(minutesSinceLastAlert).arg(cooldownMinutes));
            return;
        }
    }
    
    // 如果已有报警窗口，记录工单但不重复弹窗
    if (currentDialog) {
        overflowOrders.insert(orderNumber);
        lastAlertTime[orderNumber] = now;  // 更新最后弹窗时间
        LOG_WARNING(QString("工单 %1 溢出，但已有报警窗口显示中，记录到日志中。当前溢出工单: %2")
                   .arg(orderNumber)
                   .arg(QStringList(overflowOrders.begin(), overflowOrders.end()).join(", ")));
        return;
    }
    
    // 创建新的报警窗口
    currentDialog = new FullScreenAlertDialog(
        FullScreenAlertDialog::OverflowAlert,
        "工单溢出警告",
        "检测到工单数量溢出！\n\n"
        "超出文件已移至overlog文件夹\n"
        "请检查主界面红色标记的工单\n"
        "确认是否有重复文件或配置错误",
        nullptr  // 不指定父窗口，避免生命周期问题
    );
    
    currentDialog->setOrderNumber(orderNumber);
    overflowOrders.insert(orderNumber);
    lastAlertTime[orderNumber] = now;  // 记录弹窗时间
    
    // 连接关闭信号
    QObject::connect(currentDialog, &FullScreenAlertDialog::closed, [this]() {
        this->closeAlert();
    });
    
    QObject::connect(currentDialog, &FullScreenAlertDialog::confirmed, [this]() {
        this->closeAlert();
    });
    
    LOG_WARNING(QString("显示工单溢出报警 - 工单: %1").arg(orderNumber));
}

void OverflowAlertManager::closeAlert()
{
    if (currentDialog) {
        LOG_INFO(QString("关闭工单溢出报警窗口，已记录的溢出工单: %1")
                .arg(QStringList(overflowOrders.begin(), overflowOrders.end()).join(", ")));
        currentDialog->deleteLater();
        currentDialog = nullptr;
    }
    overflowOrders.clear();  // 清空当前溢出记录
    
    // 操作员确认后，清除所有工单的冷却时间，重新开始10分钟冷却
    QDateTime now = QDateTime::currentDateTime();
    for (auto it = lastAlertTime.begin(); it != lastAlertTime.end(); ++it) {
        it.value() = now;  // 重置为当前时间，重新开始冷却
    }
    LOG_INFO("操作员确认报警，重置所有工单的冷却时间");
}

void OverflowAlertManager::onOverflowResolved(const QString& orderNumber)
{
    if (overflowOrders.contains(orderNumber)) {
        overflowOrders.remove(orderNumber);
        LOG_INFO(QString("工单 %1 溢出问题已解决，从报警记录中移除").arg(orderNumber));
        
        // 如果所有工单都解决了，关闭报警窗口
        if (overflowOrders.isEmpty() && currentDialog) {
            closeAlert();
        }
    }
    
    // 问题解决时，清除该工单的冷却时间，允许立即重新弹窗
    if (lastAlertTime.contains(orderNumber)) {
        lastAlertTime.remove(orderNumber);
        LOG_INFO(QString("工单 %1 溢出问题已解决，清除冷却时间，允许立即重新弹窗").arg(orderNumber));
    }
}

void OverflowAlertManager::checkCooldownExpired()
{
    // 这个函数由定时器调用，用于检查是否有工单冷却时间过期
    // 实际的重新弹窗逻辑在showOverflowAlert中处理
    // 这里只是用于日志记录和监控
    QDateTime now = QDateTime::currentDateTime();
    
    for (auto it = lastAlertTime.begin(); it != lastAlertTime.end(); ++it) {
        QString orderNumber = it.key();
        QDateTime lastTime = it.value();
        int minutesSinceLastAlert = lastTime.secsTo(now) / 60;
        
        if (minutesSinceLastAlert >= cooldownMinutes) {
            LOG_DEBUG(QString("工单 %1 冷却时间已过期（%2分钟），可以重新弹窗").arg(orderNumber).arg(minutesSinceLastAlert));
        }
    }
}
