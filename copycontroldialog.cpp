#include "copycontroldialog.h"
#include <QApplication>
#include <QDebug>
#include <QStringConverter>

CopyControlDialog::CopyControlDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle("拷贝控制");
    setFixedSize(400, 200);

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // 工单号输入区域
    QHBoxLayout* inputLayout = new QHBoxLayout();
    QLabel* label = new QLabel("工单号:");
    orderNumberEdit = new QLineEdit();
    sendButton = new QPushButton("发送工单");

    inputLayout->addWidget(label);
    inputLayout->addWidget(orderNumberEdit);
    inputLayout->addWidget(sendButton);

    mainLayout->addLayout(inputLayout);

    // 工作模式选择区域
    QHBoxLayout* modeLayout = new QHBoxLayout();
    QLabel* modeLabel = new QLabel("工作模式:");
    workModeCombo = new QComboBox();
    workModeCombo->addItems({
        "快速拷贝",
        "快速拷贝+比对",
        "全盘拷贝",
        "全盘拷贝+比对",
        "制作镜像",
        "镜像拷贝"
    });
    workModeCombo->setCurrentIndex(5); // 默认 镜像拷贝
    setModeButton = new QPushButton("设置模式");
    modeLayout->addWidget(modeLabel);
    modeLayout->addWidget(workModeCombo);
    modeLayout->addWidget(setModeButton);
    mainLayout->addLayout(modeLayout);

    // 控制按钮区域
    QHBoxLayout* controlLayout = new QHBoxLayout();
    startButton = new QPushButton("开始");
    stopButton = new QPushButton("停止");

    controlLayout->addWidget(startButton);
    controlLayout->addWidget(stopButton);

    mainLayout->addLayout(controlLayout);

    // 连接信号槽
    connect(sendButton, &QPushButton::clicked, this, &CopyControlDialog::onSendOrderNumber);
    connect(startButton, &QPushButton::clicked, this, &CopyControlDialog::onStartCopy);
    connect(stopButton, &QPushButton::clicked, this, &CopyControlDialog::onStopCopy);
    connect(setModeButton, &QPushButton::clicked, this, &CopyControlDialog::onSetWorkMode);

    // 设置按钮样式
    startButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #4CAF50;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    padding: 8px 16px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #45a049;"
        "}"
        );

    stopButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #f44336;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    padding: 8px 16px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #da190b;"
        "}"
        );

    sendButton->setStyleSheet(
        "QPushButton {"
        "    background-color: #2196F3;"
        "    color: white;"
        "    border: none;"
        "    border-radius: 4px;"
        "    padding: 8px 16px;"
        "    font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "    background-color: #1976D2;"
        "}"
        );
    // 载入配置
    loadConfig();
    
    // 调试：列出所有窗口（仅在调试模式下）
    #ifdef QT_DEBUG
    listAllWindows();
    #endif
}

HWND CopyControlDialog::findVendorWindow()
{
    HWND hWnd = nullptr;
    
    // 方法1：使用配置的窗口类名查找
    if (!vendorWindowClass.isEmpty()) {
        hWnd = findVendorWindowByClass(vendorWindowClass);
        if (hWnd) {
            qDebug() << "找到厂商窗口 (类名):" << vendorWindowClass;
            return hWnd;
        }
    }
    
    // 方法2：使用配置的窗口标题查找
    if (!vendorWindowTitle.isEmpty()) {
        hWnd = findVendorWindowByTitle(vendorWindowTitle);
        if (hWnd) {
            qDebug() << "找到厂商窗口 (标题):" << vendorWindowTitle;
            return hWnd;
        }
    }
    
    // 方法3：尝试常见的窗口类名
    QStringList commonClassNames = {
        "SataCopyApp",
        "SataCopy",
        "CopyApp", 
        "CopyTool",
        "MainWindow",
        "Qt5QWindowIcon"
    };
    
    for (const QString& className : commonClassNames) {
        hWnd = findVendorWindowByClass(className);
        if (hWnd) {
            qDebug() << "找到厂商窗口 (常见类名):" << className;
            return hWnd;
        }
    }
    
    // 方法4：尝试部分标题匹配
    QStringList commonTitles = {
        "SataCopy",
        "Copy",
        "拷贝",
        "复制"
    };
    
    for (const QString& title : commonTitles) {
        hWnd = findVendorWindowByPartialTitle(title);
        if (hWnd) {
            qDebug() << "找到厂商窗口 (部分标题):" << title;
            return hWnd;
        }
    }
    
    qDebug() << "未找到厂商程序窗口";
    return nullptr;
}

HWND CopyControlDialog::findVendorWindowByClass(const QString& className)
{
    return FindWindowW(reinterpret_cast<LPCWSTR>(className.utf16()), nullptr);
}

HWND CopyControlDialog::findVendorWindowByTitle(const QString& title)
{
    return FindWindowW(nullptr, reinterpret_cast<LPCWSTR>(title.utf16()));
}

HWND CopyControlDialog::findVendorWindowByPartialTitle(const QString& partialTitle)
{
    // 使用EnumWindows枚举所有窗口，查找包含指定文本的窗口
    struct EnumData {
        QString partialTitle;
        HWND foundWindow;
    } enumData = {partialTitle, nullptr};
    
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        EnumData* data = reinterpret_cast<EnumData*>(lParam);
        
        // 获取窗口标题
        wchar_t windowTitle[256];
        if (GetWindowTextW(hWnd, windowTitle, 256) > 0) {
            QString title = QString::fromWCharArray(windowTitle);
            if (title.contains(data->partialTitle, Qt::CaseInsensitive)) {
                data->foundWindow = hWnd;
                return FALSE; // 停止枚举
            }
        }
        return TRUE; // 继续枚举
    }, reinterpret_cast<LPARAM>(&enumData));
    
    return enumData.foundWindow;
}

bool CopyControlDialog::postMessageToVendor(UINT message, WPARAM wParam, LPARAM lParam)
{
    HWND vendorWindow = findVendorWindow();

    if (vendorWindow == nullptr) {
        QString errorMsg = "未找到厂商程序窗口，请确保厂商程序已启动。\n\n";
        errorMsg += "可能的解决方案：\n";
        errorMsg += "1. 确保厂商程序已启动\n";
        errorMsg += "2. 在config.ini中配置正确的窗口类名或标题：\n";
        errorMsg += "   [Vendor]\n";
        errorMsg += "   WindowClass=你的窗口类名\n";
        errorMsg += "   WindowTitle=你的窗口标题\n";
        errorMsg += "3. 查看程序日志中的窗口信息";
        
        QMessageBox::warning(this, "错误", errorMsg);
        return false;
    }

    // 使用PostMessage异步发送
    if (PostMessage(vendorWindow, message, wParam, lParam)) {
        qDebug() << "消息发送成功，消息ID:" << message;
        return true;
    }
    
    QString errorMsg = QString("消息发送失败 (消息ID: %1)\n\n").arg(message);
    errorMsg += "可能的原因：\n";
    errorMsg += "1. 厂商程序不支持该消息\n";
    errorMsg += "2. 厂商程序版本不兼容\n";
    errorMsg += "3. 权限不足";
    
    QMessageBox::warning(this, "错误", errorMsg);
    return false;
}

void CopyControlDialog::onSendOrderNumber()
{
    QString orderNumber = orderNumberEdit->text().trimmed();

    if (orderNumber.isEmpty()) {
        QMessageBox::warning(this, "警告", "请输入工单号");
        return;
    }
    // 1) 覆盖写入文件
    if (!writeOrderNumberToFile(orderNumber)) {
        return;
    }
    // 2) 通知厂商程序读取（不再传递字符串地址）
    postMessageToVendor(WM_COPY_ORDER_NUMBER, 0, 0);
}

void CopyControlDialog::onStartCopy()
{
    postMessageToVendor(WM_COPY_START);
}

void CopyControlDialog::onStopCopy()
{
    postMessageToVendor(WM_COPY_STOP);
}

void CopyControlDialog::onSetWorkMode()
{
    // 根据厂商定义的枚举映射
    // QUICK_COPY=0, QUICK_COPY_COMPARE=1, FULL_COPY=2, FULLL_COPY_COMPARE=3, MAKE_MAP=6, MAP_COPY=7
    int idx = workModeCombo->currentIndex();
    int mode = 7; // 默认镜像拷贝
    switch (idx) {
    case 0: mode = 0; break; // 快速拷贝
    case 1: mode = 1; break; // 快速拷贝+比对
    case 2: mode = 2; break; // 全盘拷贝
    case 3: mode = 3; break; // 全盘拷贝+比对
    case 4: mode = 6; break; // 制作镜像
    case 5: mode = 7; break; // 镜像拷贝
    }
    postMessageToVendor(WM_COPY_WORK_MODE, 0, (LPARAM)mode);
}

bool CopyControlDialog::writeOrderNumberToFile(const QString& orderNumber)
{
    QString path = orderNumberFilePath.isEmpty() ? QStringLiteral("E:/OrderNumber.txt") : orderNumberFilePath;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, "错误", QString("无法写入工单号文件: %1").arg(path));
        return false;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << orderNumber;
    out.flush();
    f.close();
    return true;
}

void CopyControlDialog::loadConfig()
{
    QSettings settings("config.ini", QSettings::IniFormat);
    // 允许在配置中自定义工单号文件路径（可选）
    orderNumberFilePath = settings.value("Vendor/OrderNumberFile", "E:/OrderNumber.txt").toString();
    
    // 加载厂商窗口配置
    vendorWindowClass = settings.value("Vendor/WindowClass", "").toString();
    vendorWindowTitle = settings.value("Vendor/WindowTitle", "").toString();
    
    qDebug() << "厂商窗口配置加载:";
    qDebug() << "  窗口类名:" << vendorWindowClass;
    qDebug() << "  窗口标题:" << vendorWindowTitle;
}

void CopyControlDialog::listAllWindows()
{
    qDebug() << "=== 所有窗口信息 ===";
    
    struct EnumData {
        int count;
    } enumData = {0};
    
    EnumWindows([](HWND hWnd, LPARAM lParam) -> BOOL {
        EnumData* data = reinterpret_cast<EnumData*>(lParam);
        
        // 获取窗口类名
        wchar_t className[256];
        GetClassNameW(hWnd, className, 256);
        QString classStr = QString::fromWCharArray(className);
        
        // 获取窗口标题
        wchar_t windowTitle[256];
        GetWindowTextW(hWnd, windowTitle, 256);
        QString titleStr = QString::fromWCharArray(windowTitle);
        
        // 只显示有标题的窗口
        if (!titleStr.isEmpty()) {
            data->count++;
            qDebug() << QString("窗口 %1: 类名='%2', 标题='%3'")
                        .arg(data->count)
                        .arg(classStr)
                        .arg(titleStr);
        }
        
        return TRUE; // 继续枚举
    }, reinterpret_cast<LPARAM>(&enumData));
    
    qDebug() << QString("总共找到 %1 个有标题的窗口").arg(enumData.count);
    qDebug() << "=== 窗口信息结束 ===";
}
