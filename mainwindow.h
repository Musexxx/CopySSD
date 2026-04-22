#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include <QSettings>
#include <QMainWindow>
#include <QTableWidget>
#include <QFileSystemWatcher>
#include <QMap>
#include <QQueue>
#include <QTimer>
#include <QSet>
#include "workorder.h"
#include "logger.h"
#include <QMessageBox>
#include <QProgressDialog>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include "websocketclient.h"
#include <QVBoxLayout>
#include <atomic>
// 前向声明
class CopyControlDialog;
class FullScreenAlertDialog;

// 工单溢出报警管理器（单例模式）
class OverflowAlertManager {
public:
    static OverflowAlertManager* getInstance();
    void showOverflowAlert(const QString& orderNumber);
    void closeAlert();
    void onOverflowResolved(const QString& orderNumber);
    bool isAlertShowing() const { return currentDialog != nullptr; }
    
private:
    OverflowAlertManager() = default;
    static OverflowAlertManager* instance;
    FullScreenAlertDialog* currentDialog = nullptr;
    QSet<QString> overflowOrders;  // 记录所有溢出的工单（用于冷却机制）
    QMap<QString, QDateTime> lastAlertTime;  // 记录每个工单的最后弹窗时间
    QTimer* cooldownTimer;  // 冷却检查定时器
    int cooldownMinutes = 10;  // 冷却时间（分钟）
    
    void checkCooldownExpired();  // 检查冷却时间是否过期
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onAddWorkOrder();
    void onStopCounting();
    void onModifyTotalCount(int row, QTableWidget* tableWidget);  // 修改总量（支持双表格）
    void onDeleteWorkOrder(int row, QTableWidget* tableWidget);   // 删除工单（支持双表格）
    void onMarkAsFailed(int row, QTableWidget* tableWidget);      // 标记失败（支持双表格）
    void onViewWorkOrderDetails(int row, QTableWidget* tableWidget);  // 查看详情（支持双表格）
    void onDirectoryChanged(const QString& path);
    void processDirectoryFiles(const QString& path);  // 新增：延迟处理目录文件
    void onArchiveDirectoryChanged(const QString& path);
    void processNextFile();  // 新增：处理队列中的下一个文件
    void onCheckOverflow();  // 添加新的槽函数声明
    void onRefresh();
    void onStartCopyClicked();  // 新增
private:
    void setupUI();
    void loadConfig();
    void updateTable();
    void updateRow(int row, WorkOrder* order);
    void createActionButtons(int row);
    bool processLogFile(const QString& filePath);
    bool moveLogFile(const QString& sourceFile, const QString& orderNumber, const QString& barcode, int& copyCount);
    int countLogFiles(const QString& orderNumber);
    int countAllLogFilesInOrderFolder(const QString& orderNumber, bool includeOverlog = true) const;  // 新增：统计工单文件夹下所有log文件
    int  getWorkOrderTotal(const QString& orderNumber);
    int  readWorkOrderTotalFromNumFile(const QString& orderNumber) const; // 从NUM.txt读取总数
    bool writeWorkOrderTotalToNumFile(const QString& orderNumber, int totalCount); // 写入NUM.txt总数
    void updateWorkOrderCount(const QString& orderNumber);
    void updateWorkOrderCountFromPath(const QString& path);
    void syncAllWorkOrderCounts(); // 同步所有工单计数
    void maybeStartAsyncSizeValidation(); // 达到周期时异步触发大小校验（单飞）
    void setupWorkOrderMonitoring(); // 设置工单文件夹监控
    void checkAndCreateOrderFolder(const QString& orderNumber);
    void performInitialCheck();
    void processExistingFiles();
    bool processAllFiles();
    bool processAllFilesOptimized();  // 优化版本：异步处理大量文件
    void addToProcessQueue(const QString& filePath);  // 新增：添加文件到队列
    QStringList getLogFiles(const QString& dirPath);  // 获取日志文件列表（支持.txt和.log）
    QString ensureLogExtension(const QString& filePath);  // 确保文件扩展名为.log
    void convertTxtToLogInOrderFolder(const QString& orderNumber);  // 转换工单文件夹下的txt文件为log文件（避开TimeCount*.txt）
    
    // 料号确认相关函数
    bool validateMaterialCode(const QString& orderNumber, const QString& barcode, const QString& materialCode, const QString& filePath = "");
    QString getMaterialCodeFromSSDInfo(const QString& orderNumber, const QString& barcode);
    bool loadOrderMaterialWhitelist(const QString& orderNumber);
    void saveOrderMaterialWhitelist(const QString& orderNumber, const QStringList& materialCodes);
    bool loadOrderMaterialBlacklist(const QString& orderNumber);
    void saveOrderMaterialBlacklist(const QString& orderNumber, const QStringList& materialCodes);
    void showMaterialCodeConfirmationDialog(const QString& orderNumber, const QString& barcode, const QString& newMaterialCode, const QString& filePath = "");
    void processPendingOrders();
    bool isOrderPendingConfirmation(const QString& orderNumber);
    void addToPendingOrderQueue(const QString& orderNumber);
    void removeFromPendingOrderQueue(const QString& orderNumber);
    void handleErrorFileWithOrder(const QString& filePath, const QString& orderNumber, const QString& barcode, const QString& errorType);
    
    // 料号缓存管理函数
    bool isMaterialPendingConfirmation(const QString& orderNumber, const QString& materialCode);
    bool moveFileToMaterialCache(const QString& filePath, const QString& orderNumber, const QString& materialCode);
    void moveFilesFromCacheToArchive(const QString& orderNumber, const QString& materialCode);
    void cleanupMaterialCache(const QString& orderNumber, const QString& materialCode);
    QString getMaterialCachePath(const QString& orderNumber, const QString& materialCode);
    
    // 双表格相关函数
    void setupDualTableUI();  // 设置双表格UI
    QVBoxLayout* createDualTableLayout();  // 创建双表格布局
    void classifyWorkOrder(WorkOrder* order);  // 工单分类
    void moveToIncompleteSection(WorkOrder* order);  // 移动到未完成区域
    void moveToCompletedSection(WorkOrder* order);   // 移动到已完成区域
    void updateIncompleteTable();  // 更新未完成工单表格
    void updateCompletedTable();   // 更新已完成工单表格
    void updateIncompleteRow(int row, WorkOrder* order);  // 更新未完成工单行
    void updateCompletedRow(int row, WorkOrder* order);   // 更新已完成工单行
    void createIncompleteActionButtons(int row);  // 创建未完成工单操作按钮
    void createCompletedActionButtons(int row);   // 创建已完成工单操作按钮
    void checkWorkOrderStatusChange(const QString& orderNumber);  // 检查工单状态变化
    void showCompletionNotification(WorkOrder* order);  // 显示完成通知
    void showAnomalyAlert(WorkOrder* order);  // 显示异常报警
    
    void initLogger();
    void handleMismatchedFile(const QString& filePath);
    void handleErrorFile(const QString& filePath, const QString& orderNumber, const QString& barcode, const QString& errorType);
    bool validateBarcode(const QString& filePath, const QString& fileContent);
    void createDefaultConfig();
    QTableWidget* tableWidget;  // 保留原有表格，用于兼容性
    QTableWidget* incompleteTableWidget;  // 未完成工单表格
    QTableWidget* completedTableWidget;   // 已完成工单表格
    QFileSystemWatcher* fileWatcher;
    QMap<QString, WorkOrder*> workOrders;  // 保留原有数据结构，用于兼容性
    QMap<QString, WorkOrder*> incompleteWorkOrders;  // 未完成工单
    QMap<QString, WorkOrder*> completedWorkOrders;   // 已完成工单
    QQueue<QString> fileQueue;        // 新增：文件处理队列
    QTimer* processTimer;             // 新增：处理定时器
    QTimer* syncTimer;                // 新增：同步定时器
    QTimer* autoConnectTimer;         // 新增：自动连接定时器
    bool isProcessing;                // 新增：处理状态标志
    
    // 批量处理相关
    QProgressDialog* batchProgressDialog;  // 批量处理进度对话框
    int totalFilesToProcess;               // 总文件数
    int processedFilesCount;               // 已处理文件数
    bool isBatchProcessing;                // 是否正在批量处理

    QString machineId;
    QString watchPath;
    QString archivePath;
    QString duplicatePath;  // 用于存储重复文件的路径
    QString workordersRootPath;  // 工单根目录（每个工单文件夹下含 NUM.txt）
    QString ssdInfoPath;  // SSD信息文件路径
    QString errorPath;  // 错误状态文件路径
    QString materialCachePath;  // 料号确认缓存路径
    QString gobRoute1;            // 源文件服务器根路径1
    QString gobRoute2;            // 源文件服务器根路径2
    QString copyMachineLogPath;   // 拷贝机记录根目录（工单号.txt）
    int syncValidationInterval = 20; // Settings/SyncValidationInterval，与 syncTimer 周期配合
    int syncCounter = 0;          // 同步计数，达到间隔触发大小校验
    std::atomic<bool> sizeValidationRunning{false}; // 大小校验单飞

    // 料号确认相关
    QMap<QString, QStringList> orderMaterialWhitelist;  // 工单料号白名单 <工单号, 料号列表>
    QMap<QString, QStringList> orderMaterialBlacklist;  // 工单料号黑名单 <工单号, 料号列表>
    QSet<QString> pendingOrders;  // 等待确认的工单号
    QQueue<QString> pendingOrderQueue;  // 等待确认的工单队列
    QTimer* pendingOrderTimer;  // 处理等待确认工单的定时器
    QMutex pendingOrderMutex;  // 保护等待确认工单的互斥锁
    
    // 料号描述存储（新格式支持）
    QMap<QString, QString> materialDescriptions;  // 条码对应的料号描述 <条码, 料号描述>
    
    int getNextDuplicateNumber(const QString& barcode, const QString& orderNumber, const QString& subFolder = "");  // 获取下一个重复序号
    bool writeTimeRecord(const QString& orderNumber, const QString& barcode,
                         const QString& createTime, int copyCount);
    void saveWorkOrders();
    bool loadSavedWorkOrders();
    void askToLoadSavedWorkOrders();
    QString getWorkOrdersFilePath() const;
    bool workOrdersLoaded = false;

    const QString WORK_ORDERS_FILENAME = "saved_workorders.json";
    void loadSelectedWorkOrders(const QJsonArray& allOrders, const QStringList& selectedOrders);
    //QSet<QString> startupProcessedOrders;  // 存储启动时处理的工单
  //  void addQuickAddButton();  // 添加快捷添加按钮
 //   void onQuickAdd();  // 快捷添加按钮点击处理
  //  void recordProcessedOrder(const QString& orderNumber);  // 记录处理的工单
    QString mismatchPath;  // 存储条码不匹配文件的路径
    QString workorderConfigPath;  // 工单配置文件路径
    QSettings* workorderSettings; // 工单配置对象
    QMessageBox* overflowDialog = nullptr;
    bool updateWorkorderConfig(const QString& orderNumber, int totalCount);
    bool tryLockConfigFile(QFile& lockFile, int maxRetries = 50);
    void unlockConfigFile(QFile& lockFile);//文件锁，用于多台机器更新workorder.ini
private slots:
    void onTableItemDoubleClicked(QTableWidgetItem* item);
    void onTableCustomContextMenuRequested(const QPoint& pos);
private:
    void moveRowToTop(int row);
    void moveRowToBottom(int row);//移动行事件
    int  countFilesInOrderFolder(const QString& orderNumber);
    QSet<QString> processedOrders;  // 存储本次启动以来处理过的工单号
    /// 用户从主界面删除的工单：不再由 syncProcessedOrders 自动加回（直至用户再次手动/加载添加工单）
    QSet<QString> suppressedAutoSyncOrders;
    QPushButton* processedOrdersButton;
    // 添加新方法
    void showProcessedOrders();  // 显示处理过的工单表
    void syncProcessedOrders();  // 同步处理过的工单
//以下是websocket类
    WebSocketClient* m_webSocketClient;
    QPushButton* m_connectButton;

    void setupWebSocket();
    QJsonObject createWorkOrdersJson();
    QJsonObject createSingleWorkOrderJson(const QString& orderNumber) const;
    void sendAllWorkOrdersToCentral();
    void sendNewWorkOrderToCentral(const QString& orderNumber);
    void setupAutoConnectTimer();     // 设置自动连接定时器
    void checkAndAutoConnect();       // 检查并自动连接中控程序
    QSet<QString> m_sentWorkOrdersAfterConnect;  // 记录本次连接后已发送的工单

private slots:
    void onSizeValidationFinished(const QStringList& errorMessages);
    void onConnectButtonClicked();
    void onWebSocketStatusChanged(const QString& status);
    void onWebSocketConnected();
    void onWebSocketDisconnected();
    void onWebSocketError(const QString& error);
    void updateQueueProgress();  // 更新队列处理进度

private:
    bool isNetworkPath(const QString& path) const;  // 检查是否为网络路径
    bool checkNetworkPathAccessibility(const QString& path) const;  // 检查网络路径可访问性
    bool createNetworkFolder(const QString& folderPath);  // 创建网络路径文件夹
    
    // 通知管理系统
    struct NotificationRecord {
        QDateTime timestamp;
        QString title;
        QString message;
        QString orderNumber;
    };
    
    QList<NotificationRecord> notificationHistory; // 通知历史记录
    QMap<QString, QDateTime> lastNotificationTime; // 记录每个工单的最后通知时间
    QMap<QString, QDateTime> lastOverflowAlertTime; // 记录每个工单的最后溢出报警时间
    static const int NOTIFICATION_COOLDOWN_MINUTES = 5; // 5分钟冷却时间
    static const int OVERFLOW_ALERT_COOLDOWN_MINUTES = 5; // 溢出报警5分钟冷却时间
    static const int NOTIFICATION_HISTORY_LIMIT = 100; // 通知历史记录限制
    
    // 通知管理函数
    bool shouldShowNotification(const QString& orderNumber);
    void recordNotification(const QString& orderNumber);
    void showNotification(const QString& title, const QString& message, const QString& orderNumber);
    void cleanupExpiredNotifications(); // 清理过期通知
    
    // 溢出报警管理函数
    bool shouldShowOverflowAlert(const QString& orderNumber);
    void recordOverflowAlert(const QString& orderNumber);
    void showOverflowAlert(const QString& orderNumber); // 非阻塞的溢出报警
    
    // 快捷添加功能
    void addQuickAddButton(); // 添加快捷添加按钮
    void onQuickAdd(); // 快捷添加按钮点击处理
    
    // 工单管理增强功能
    void recordProcessedOrder(const QString& orderNumber); // 记录处理的工单
    QSet<QString> startupProcessedOrders; // 存储启动时处理的工单
    
    // 文件处理增强功能
    void checkAndMoveOverflowFiles(); // 检查并移动溢出文件
    int getOverflowCount() const; // 获取超出总数的数量
    QString getOverflowPath() const; // 获取溢出文件夹路径
};

#endif
