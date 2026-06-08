#include "fullscreenalertdialog.h"
#include <QApplication>
#include <QScreen>
#include <QFont>
#include <QPalette>
#include <QStyle>
#include <QKeyEvent>
#include <QShowEvent>
#include <QCloseEvent>
#include <QDebug>
#include <QTimer>
#include <QWindow>
#include <QEventLoop>
#include <QFocusEvent>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

// 静态成员变量定义
QList<FullScreenAlertDialog*> FullScreenAlertDialog::s_activeDialogs;

FullScreenAlertDialog::FullScreenAlertDialog(AlertType type, const QString& title, 
                                            const QString& message, QWidget* parent)
    : QDialog(parent)
    , m_type(type)
    , m_title(title)
    , m_message(message)
    , m_autoCloseTimer(nullptr)
    , m_autoCloseSeconds(0)
    , m_titleLabel(nullptr)
    , m_messageLabel(nullptr)
    , m_iconLabel(nullptr)
    , m_confirmButton(nullptr)
    , m_rejectButton(nullptr)
    , m_closeButton(nullptr)
    , m_fadeInAnimation(nullptr)
    , m_opacityEffect(nullptr)
{
    // 设置为全屏无边框窗口
    // 溢出报警和料号确认使用模态窗口，确保窗口获得焦点且按钮可以点击
    // 其他类型保持非模态，避免不必要的阻塞
    if (type == OverflowAlert || type == MaterialConfirm) {
        // 模态窗口：溢出报警和料号确认
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Dialog);
        setModal(true);  // 设置为模态，阻塞整个应用直到用户响应
        setWindowModality(Qt::ApplicationModal);  // 阻塞整个应用程序
    } else {
        // 非模态窗口：其他报警类型
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool | Qt::MSWindowsFixedSizeDialogHint);
        setModal(false);
        setAttribute(Qt::WA_ShowModal, false);
    }
    
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose, true);
    // 确保窗口可以接收鼠标和键盘事件
    setAttribute(Qt::WA_AcceptTouchEvents, false);
    setMouseTracking(true);  // 启用鼠标跟踪，确保按钮hover效果正常
    
    // 获取屏幕尺寸并设置全屏
    QScreen* screen = QApplication::primaryScreen();
    if (screen) {
        setGeometry(screen->geometry());
    } else {
        // 备用方案
        setGeometry(0, 0, 1920, 1080);
    }
    
    setupUI();
    setupStyle();
    // 关闭淡入动画以降低资源占用
    // setupAnimation();
    
    // 安装全局事件过滤器，用于捕获ESC键（即使窗口失去焦点也能响应）
    installGlobalEventFilter();
    
    // 显示窗口
    if (type == OverflowAlert || type == MaterialConfirm) {
        // 模态窗口：先不显示，等待外部调用open()或exec()
        // 这样可以确保在设置完所有属性后再显示，获得更可靠的焦点
        // 但为了保持兼容性，仍然在构造函数中显示
        show();
        
        // 立即强制激活窗口（使用多种方法确保可靠性）
        QTimer::singleShot(0, this, [this]() {
            raise();
            activateWindow();
            setFocus();
            
#ifdef Q_OS_WIN
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (hwnd) {
                SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
                BringWindowToTop(hwnd);
            }
#endif
            QApplication::processEvents();
        });
    } else {
        // 非模态窗口：正常显示
        show();
        // 强制激活窗口（使用多种方法确保可靠性）
        forceActivateWindow();
    }
}

void FullScreenAlertDialog::setAutoClose(int seconds)
{
    m_autoCloseSeconds = seconds;
    if (seconds > 0) {
        if (!m_autoCloseTimer) {
            m_autoCloseTimer = new QTimer(this);
            connect(m_autoCloseTimer, &QTimer::timeout, this, &FullScreenAlertDialog::onAutoClose);
        }
        m_autoCloseTimer->setInterval(seconds * 1000);
        m_autoCloseTimer->start();
    }
}

void FullScreenAlertDialog::setOrderNumber(const QString& orderNumber)
{
    m_orderNumber = orderNumber;
}

void FullScreenAlertDialog::setCallback(std::function<void(bool)> callback)
{
    m_callback = callback;
}

void FullScreenAlertDialog::setupUI()
{
    // 主布局
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    
    // 创建半透明背景
    QWidget* backgroundWidget = new QWidget(this);
    backgroundWidget->setStyleSheet("background-color: rgba(0, 0, 0, 0.7);");
    backgroundWidget->setAttribute(Qt::WA_StyledBackground, true);
    backgroundWidget->setAttribute(Qt::WA_TransparentForMouseEvents, false);  // 确保背景可以接收鼠标事件传递给子控件
    mainLayout->addWidget(backgroundWidget);
    
    // 内容区域布局
    QVBoxLayout* contentLayout = new QVBoxLayout(backgroundWidget);
    contentLayout->setAlignment(Qt::AlignCenter);
    contentLayout->setSpacing(30);
    
    // 图标标签
    m_iconLabel = new QLabel(this);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setFixedSize(120, 120);
    contentLayout->addWidget(m_iconLabel);
    
    // 标题标签
    m_titleLabel = new QLabel(m_title, this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setWordWrap(true);
    contentLayout->addWidget(m_titleLabel);
    
    // 消息标签
    m_messageLabel = new QLabel(m_message, this);
    m_messageLabel->setAlignment(Qt::AlignCenter);
    m_messageLabel->setWordWrap(true);
    contentLayout->addWidget(m_messageLabel);
    
    // 按钮布局
    QHBoxLayout* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(20);
    buttonLayout->setAlignment(Qt::AlignCenter);
    
    // 根据报警类型创建不同的按钮
    switch (m_type) {
    case OverflowAlert:
    case AnomalyAlert:
    case FileOperationError:
        // 只有确认按钮
        m_confirmButton = new QPushButton("确认", this);
        m_confirmButton->setFixedSize(150, 50);
        buttonLayout->addWidget(m_confirmButton);
        break;
        
    case MaterialConfirm:
        // 确认和拒绝按钮（料号确认必须处理，不提供关闭选项）
        m_confirmButton = new QPushButton("确认添加", this);
        m_rejectButton = new QPushButton("拒绝料号", this);
        m_confirmButton->setFixedSize(150, 50);
        m_rejectButton->setFixedSize(150, 50);
        buttonLayout->addWidget(m_confirmButton);
        buttonLayout->addWidget(m_rejectButton);
        break;
    }
    
    // 关闭按钮（除料号确认外的所有类型都有）
    if (m_type != MaterialConfirm) {
        m_closeButton = new QPushButton("关闭", this);
        m_closeButton->setFixedSize(150, 50);
        buttonLayout->addWidget(m_closeButton);
    }
    
    contentLayout->addLayout(buttonLayout);
    
    // 连接信号槽
    // 确保按钮可以接收鼠标事件
    if (m_confirmButton) {
        m_confirmButton->setEnabled(true);
        m_confirmButton->setMouseTracking(true);  // 启用鼠标跟踪，确保hover效果
        m_confirmButton->setAttribute(Qt::WA_AcceptTouchEvents, false);  // 禁用触摸，使用鼠标
        m_confirmButton->setAttribute(Qt::WA_Hover, true);  // 启用hover效果
        connect(m_confirmButton, &QPushButton::clicked, this, &FullScreenAlertDialog::onConfirmClicked, Qt::UniqueConnection);
    }
    if (m_rejectButton) {
        m_rejectButton->setEnabled(true);
        m_rejectButton->setMouseTracking(true);
        m_rejectButton->setAttribute(Qt::WA_AcceptTouchEvents, false);
        m_rejectButton->setAttribute(Qt::WA_Hover, true);
        connect(m_rejectButton, &QPushButton::clicked, this, &FullScreenAlertDialog::onRejectClicked, Qt::UniqueConnection);
    }
    if (m_closeButton) {
        m_closeButton->setEnabled(true);
        m_closeButton->setMouseTracking(true);
        m_closeButton->setAttribute(Qt::WA_AcceptTouchEvents, false);
        m_closeButton->setAttribute(Qt::WA_Hover, true);
        connect(m_closeButton, &QPushButton::clicked, this, &FullScreenAlertDialog::onCloseClicked, Qt::UniqueConnection);
    }
}

void FullScreenAlertDialog::setupStyle()
{
    // 设置字体（静态缓存，避免重复创建）
    static QFont titleFont("Microsoft YaHei", 28, QFont::Bold);
    static QFont messageFont("Microsoft YaHei", 18, QFont::Normal);
    static QFont buttonFont("Microsoft YaHei", 14, QFont::Normal);
    
    m_titleLabel->setFont(titleFont);
    m_messageLabel->setFont(messageFont);
    
    if (m_confirmButton) m_confirmButton->setFont(buttonFont);
    if (m_rejectButton) m_rejectButton->setFont(buttonFont);
    if (m_closeButton) m_closeButton->setFont(buttonFont);
    
    applyAlertTypeStyle();
}

void FullScreenAlertDialog::applyAlertTypeStyle()
{
    QString titleColor, messageColor, iconText, buttonStyle;
    
    switch (m_type) {
    case OverflowAlert:
        titleColor = "#FF6B6B";      // 红色
        messageColor = "#FFFFFF";
        iconText = "⚠️";
        buttonStyle = "QPushButton { background-color: #FF6B6B; color: white; border: none; border-radius: 8px; }"
                     "QPushButton:hover { background-color: #FF5252; }";
        break;
        
    case AnomalyAlert:
        titleColor = "#FF9800";      // 橙色
        messageColor = "#FFFFFF";
        iconText = "🚨";
        buttonStyle = "QPushButton { background-color: #FF9800; color: white; border: none; border-radius: 8px; }"
                     "QPushButton:hover { background-color: #F57C00; }";
        break;
        
    case MaterialConfirm:
        titleColor = "#2196F3";      // 蓝色
        messageColor = "#FFFFFF";
        iconText = "❓";
        buttonStyle = "QPushButton { background-color: #2196F3; color: white; border: none; border-radius: 8px; }"
                     "QPushButton:hover { background-color: #1976D2; }";
        break;
        
    case FileOperationError:
        titleColor = "#F44336";      // 深红色
        messageColor = "#FFFFFF";
        iconText = "❌";
        buttonStyle = "QPushButton { background-color: #F44336; color: white; border: none; border-radius: 8px; }"
                     "QPushButton:hover { background-color: #D32F2F; }";
        break;
    }
    
    // 设置标题样式
    m_titleLabel->setStyleSheet(QString("color: %1;").arg(titleColor));
    
    // 设置消息样式
    m_messageLabel->setStyleSheet(QString("color: %1; padding: 20px;").arg(messageColor));
    
    // 设置图标
    m_iconLabel->setText(iconText);
    m_iconLabel->setStyleSheet(QString("color: %1; font-size: 80px;").arg(titleColor));
    
    // 设置按钮样式
    if (m_confirmButton) m_confirmButton->setStyleSheet(buttonStyle);
    if (m_rejectButton) {
        m_rejectButton->setStyleSheet("QPushButton { background-color: #757575; color: white; border: none; border-radius: 8px; }"
                                     "QPushButton:hover { background-color: #616161; }");
    }
    if (m_closeButton) {
        m_closeButton->setStyleSheet("QPushButton { background-color: #9E9E9E; color: white; border: none; border-radius: 8px; }"
                                    "QPushButton:hover { background-color: #757575; }");
    }
}

void FullScreenAlertDialog::setupAnimation()
{
    // 已禁用动画（以减少资源占用）
}

void FullScreenAlertDialog::onConfirmClicked()
{
    if (m_callback) {
        m_callback(true);
    }
    emit confirmed();
    accept();
}

void FullScreenAlertDialog::onRejectClicked()
{
    if (m_callback) {
        m_callback(false);
    }
    emit rejected();
    reject();
}

void FullScreenAlertDialog::onCloseClicked()
{
    // 移除全局事件过滤器
    removeGlobalEventFilter();
    
    emit closed();
    close();
}

void FullScreenAlertDialog::onAutoClose()
{
    // 移除全局事件过滤器
    removeGlobalEventFilter();
    
    if (m_autoCloseTimer) {
        m_autoCloseTimer->stop();
    }
    close();
}

void FullScreenAlertDialog::forceActivateWindow()
{
    // 立即激活
    raise();
    activateWindow();
    setFocus();
    
    // 强制处理事件循环，确保窗口完全显示
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    
#ifdef Q_OS_WIN
    // 使用Windows API强制激活窗口（最可靠的方法）
    HWND hwnd = reinterpret_cast<HWND>(winId());
    if (hwnd) {
        // 方法1: SetForegroundWindow - 将窗口置于前台
        SetForegroundWindow(hwnd);
        
        // 方法2: ShowWindow - 确保窗口显示
        ShowWindow(hwnd, SW_SHOW);
        ShowWindow(hwnd, SW_RESTORE);
        
        // 方法3: SetActiveWindow - 激活窗口
        SetActiveWindow(hwnd);
        
        // 方法4: SetFocus - 设置焦点到窗口
        SetFocus(hwnd);
        
        // 方法5: BringWindowToTop - 将窗口置于最前
        BringWindowToTop(hwnd);
    }
#endif
    
    // 再次处理事件循环
    QApplication::processEvents();
    
    // 延迟再次激活（双重保险）
    QTimer::singleShot(50, this, [this]() {
        raise();
        activateWindow();
        setFocus();
        
        // 确保所有按钮都已启用并且可以接收事件
        if (m_confirmButton) {
            m_confirmButton->setEnabled(true);
            m_confirmButton->update();  // 强制更新按钮状态
        }
        if (m_rejectButton) {
            m_rejectButton->setEnabled(true);
            m_rejectButton->update();
        }
        if (m_closeButton) {
            m_closeButton->setEnabled(true);
            m_closeButton->update();
        }
        
        // 再次处理事件循环
        QApplication::processEvents();
    });
    
    // 第三次激活（三重保险）
    QTimer::singleShot(200, this, [this]() {
        raise();
        activateWindow();
        setFocus();
        
#ifdef Q_OS_WIN
        HWND hwnd = reinterpret_cast<HWND>(winId());
        if (hwnd) {
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
            SetFocus(hwnd);
        }
#endif
        
        QApplication::processEvents();
    });
}

void FullScreenAlertDialog::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    
    if (m_type == OverflowAlert || m_type == MaterialConfirm) {
        // 模态窗口：确保窗口获得焦点并可以接收鼠标事件
        // 使用多重延迟确保窗口完全显示后再激活
        QTimer::singleShot(0, this, [this]() {
            raise();
            activateWindow();
            setFocus();
            
#ifdef Q_OS_WIN
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (hwnd) {
                SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
                BringWindowToTop(hwnd);
            }
#endif
            QApplication::processEvents();
        });
        
        // 第二次激活（50ms后），确保窗口完全显示
        QTimer::singleShot(50, this, [this]() {
            raise();
            activateWindow();
            setFocus();
            
            // 确保所有按钮都已启用并可以接收事件
            if (m_confirmButton) {
                m_confirmButton->setEnabled(true);
                m_confirmButton->update();
            }
            if (m_rejectButton) {
                m_rejectButton->setEnabled(true);
                m_rejectButton->update();
            }
            if (m_closeButton) {
                m_closeButton->setEnabled(true);
                m_closeButton->update();
            }
            
#ifdef Q_OS_WIN
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (hwnd) {
                SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
            }
#endif
            QApplication::processEvents();
        });
        
        // 第三次激活（200ms后），最终保险
        QTimer::singleShot(200, this, [this]() {
            raise();
            activateWindow();
            setFocus();
            
#ifdef Q_OS_WIN
            HWND hwnd = reinterpret_cast<HWND>(winId());
            if (hwnd) {
                SetForegroundWindow(hwnd);
                SetActiveWindow(hwnd);
                SetFocus(hwnd);
            }
#endif
            QApplication::processEvents();
        });
    } else {
        // 非模态窗口：使用完整的强制激活逻辑
        QTimer::singleShot(10, this, [this]() {
            forceActivateWindow();
        });
    }
}

void FullScreenAlertDialog::closeEvent(QCloseEvent* event)
{
    // 窗口关闭时，确保移除全局事件过滤器
    removeGlobalEventFilter();
    
    // 调用基类的关闭事件处理
    QDialog::closeEvent(event);
}

void FullScreenAlertDialog::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Enter:
    case Qt::Key_Return:
        if (m_confirmButton && m_confirmButton->isVisible()) {
            onConfirmClicked();
        }
        break;
    case Qt::Key_Escape:
        // ESC键：根据类型决定行为
        // 料号确认窗口：接受料号（与全局事件过滤器行为一致）
        // 其他类型：关闭窗口
        if (m_type == MaterialConfirm) {
            onConfirmClicked();
        } else {
            onCloseClicked();
        }
        break;
    default:
        QDialog::keyPressEvent(event);
        break;
    }
}

void FullScreenAlertDialog::installGlobalEventFilter()
{
    // 将当前窗口添加到活动列表
    if (!s_activeDialogs.contains(this)) {
        s_activeDialogs.append(this);
    }
    
    // 安装事件过滤器到QApplication（如果还没有安装）
    // 注意：我们只需要一个事件过滤器，当第一个窗口创建时安装
    if (s_activeDialogs.size() == 1) {
        QApplication::instance()->installEventFilter(this);
        qDebug() << "已安装全局ESC键事件过滤器（兜底机制）";
    }
}

void FullScreenAlertDialog::removeGlobalEventFilter()
{
    // 从活动列表中移除
    s_activeDialogs.removeAll(this);
    
    // 如果所有窗口都已关闭，移除事件过滤器
    if (s_activeDialogs.isEmpty()) {
        QApplication::instance()->removeEventFilter(this);
        qDebug() << "已移除全局ESC键事件过滤器";
    }
}

bool FullScreenAlertDialog::eventFilter(QObject* obj, QEvent* event)
{
    // 全局事件过滤器：捕获ESC键，即使窗口失去焦点也能响应
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
        
        // 检查是否是ESC键
        if (keyEvent->key() == Qt::Key_Escape) {
            // 查找当前最顶层的活动报警窗口
            FullScreenAlertDialog* topDialog = nullptr;
            
            // 优先处理模态窗口（OverflowAlert和MaterialConfirm）
            for (FullScreenAlertDialog* dialog : s_activeDialogs) {
                if (dialog && dialog->isVisible()) {
                    if (dialog->m_type == OverflowAlert || dialog->m_type == MaterialConfirm) {
                        topDialog = dialog;
                        break;  // 模态窗口优先
                    }
                    if (!topDialog) {
                        topDialog = dialog;  // 记录第一个可见窗口
                    }
                }
            }
            
            // 如果找到活动窗口，强制关闭它
            if (topDialog) {
                qDebug() << "全局ESC键捕获：强制关闭报警窗口（兜底机制生效）";
                
                // 根据窗口类型选择关闭方式
                if (topDialog->m_type == MaterialConfirm) {
                    // 料号确认窗口：按接受处理（相当于用户点击"确认添加"按钮）
                    if (topDialog->m_confirmButton && topDialog->m_confirmButton->isVisible()) {
                        topDialog->onConfirmClicked();
                    } else {
                        // 如果没有确认按钮，直接关闭并触发确认回调
                        topDialog->removeGlobalEventFilter();
                        topDialog->accept();
                        topDialog->close();
                    }
                } else {
                    // 其他窗口类型：按关闭处理
                    if (topDialog->m_closeButton && topDialog->m_closeButton->isVisible()) {
                        topDialog->onCloseClicked();
                    } else {
                        // 如果没有关闭按钮，直接关闭
                        topDialog->removeGlobalEventFilter();
                        topDialog->close();
                    }
                }
                
                // 消费这个事件，防止传递给其他窗口
                return true;
            }
        }
    }
    
    // 其他事件继续传递给默认处理
    return QDialog::eventFilter(obj, event);
}
