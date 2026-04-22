#ifndef COPYCONTROLDIALOG_H
#define COPYCONTROLDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <QFile>
#include <QTextStream>
#include <windows.h>
#include <QStringConverter>
class CopyControlDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CopyControlDialog(QWidget *parent = nullptr);

private slots:
    void onSendOrderNumber();
    void onStartCopy();
    void onStopCopy();
    void onSetWorkMode();

private:
    QLineEdit* orderNumberEdit;
    QPushButton* sendButton;
    QPushButton* startButton;
    QPushButton* stopButton;
    QPushButton* setModeButton;
    QComboBox* workModeCombo;

    // 配置
    QString orderNumberFilePath; // 覆盖写入工单号的文件路径

    // Windows消息定义
    static const UINT WM_COPY_ORDER_NUMBER = WM_USER + 1001; // 通知读取工单号文件
    static const UINT WM_COPY_START = WM_USER + 1002;
    static const UINT WM_COPY_STOP = WM_USER + 1003;
    static const UINT WM_COPY_WORK_MODE = WM_USER + 1004;

    // 厂商窗口类名（可配置）
    QString vendorWindowClass;
    QString vendorWindowTitle;

    // 查找厂商程序窗口
    HWND findVendorWindow();
    // 尝试多种方式查找窗口
    HWND findVendorWindowByClass(const QString& className);
    HWND findVendorWindowByTitle(const QString& title);
    HWND findVendorWindowByPartialTitle(const QString& partialTitle);
    // 发送消息到厂商程序
    bool postMessageToVendor(UINT message, WPARAM wParam = 0, LPARAM lParam = 0);
    // 将工单号写入文件（覆盖写入）
    bool writeOrderNumberToFile(const QString& orderNumber);
    // 初始化配置
    void loadConfig();
    // 调试功能：列出所有窗口信息
    void listAllWindows();
};

#endif // COPYCONTROLDIALOG_H
