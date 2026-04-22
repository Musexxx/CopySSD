#ifndef FULLSCREENALERTDIALOG_H
#define FULLSCREENALERTDIALOG_H

#include <QDialog>
#include <QTimer>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsEffect>
#include <QPropertyAnimation>
#include <QGraphicsOpacityEffect>

class FullScreenAlertDialog : public QDialog
{
    Q_OBJECT

public:
    enum AlertType {
        OverflowAlert,          // 工单溢出报警
        AnomalyAlert,          // 工单状态异常报警
        MaterialConfirm,       // 料号确认报警
        FileOperationError     // NUM.txt文件操作失败
    };

    explicit FullScreenAlertDialog(AlertType type, const QString& title, 
                                  const QString& message, QWidget* parent = nullptr);
    
    void setAutoClose(int seconds = 0); // 0表示不自动关闭
    void setOrderNumber(const QString& orderNumber);
    void setCallback(std::function<void(bool)> callback); // 用于确认/取消回调

signals:
    void confirmed();
    void rejected();
    void closed();

private slots:
    void onConfirmClicked();
    void onRejectClicked();
    void onCloseClicked();
    void onAutoClose();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;  // 窗口显示时确保焦点和事件正常
    void closeEvent(QCloseEvent* event) override;  // 窗口关闭时清理事件过滤器
    bool eventFilter(QObject* obj, QEvent* event) override;  // 事件过滤器，用于全局ESC键捕获（兜底机制）

private:
    void setupUI();
    void setupStyle();
    void setupAnimation();
    void applyAlertTypeStyle();
    void forceActivateWindow();  // 强制激活窗口，确保可以接收鼠标事件
    void installGlobalEventFilter();  // 安装全局事件过滤器，用于捕获ESC键
    void removeGlobalEventFilter();   // 移除全局事件过滤器
    
    AlertType m_type;
    QString m_orderNumber;
    QString m_title;
    QString m_message;
    QTimer* m_autoCloseTimer;
    int m_autoCloseSeconds;
    
    // UI组件
    QLabel* m_titleLabel;
    QLabel* m_messageLabel;
    QLabel* m_iconLabel;
    QPushButton* m_confirmButton;
    QPushButton* m_rejectButton;
    QPushButton* m_closeButton;
    
    // 回调函数
    std::function<void(bool)> m_callback;
    
    // 动画效果
    QPropertyAnimation* m_fadeInAnimation;
    QGraphicsOpacityEffect* m_opacityEffect;
    
    // 全局事件过滤器相关
    static QList<FullScreenAlertDialog*> s_activeDialogs;  // 静态列表，追踪所有活动的报警窗口
};

#endif // FULLSCREENALERTDIALOG_H
