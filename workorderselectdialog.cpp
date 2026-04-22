#include "workorderselectdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDir>
#include "workorderselectdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QDir>

WorkOrderSelectDialog::WorkOrderSelectDialog(const QJsonArray& orders,
                                             const QString& archivePath,
                                             QWidget* parent)
    : QDialog(parent), archivePath(archivePath)  // 通过初始化列表设置成员变量
{
    setWindowTitle("选择要加载的工单");
    setupUI();

    // 删除这行，因为我们已经通过构造函数参数获得了archivePath
    // QString archivePath = "D:/Archive";  // 删除这行

    // 添加工单到列表
    for (const QJsonValue& value : orders) {
        QJsonObject orderObj = value.toObject();
        QString orderNumber = orderObj["orderNumber"].toString();
        int totalCount = orderObj["totalCount"].toInt();

        // 使用成员变量 this->archivePath 或直接使用 archivePath
        QString orderPath = this->archivePath + "/" + orderNumber;
        QDir dir(orderPath);
        int completedCount = 0;
        if (dir.exists()) {
            QStringList filters;
            filters << "*.log";
            
            // 统计根目录下的log文件
            int mainCount = dir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
            completedCount += mainCount;
            
            // 统计completed文件夹下的log文件
            QDir completedDir(orderPath + "/completed");
            if (completedDir.exists()) {
                int completedDirCount = completedDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
                completedCount += completedDirCount;
            }
            
            // 统计overlog文件夹下的log文件
            QDir overlogDir(orderPath + "/overlog");
            if (overlogDir.exists()) {
                int overlogCount = overlogDir.entryList(filters, QDir::Files | QDir::NoDotAndDotDot).count();
                completedCount += overlogCount;
            }
        }

        // 计算剩余数量
        int remainingCount = totalCount - completedCount;

        // 创建显示文本
        QString displayText = QString("%1 (总数: %2, 已完成: %3, 剩余: %4)")
                                  .arg(orderNumber)
                                  .arg(totalCount)
                                  .arg(completedCount)
                                  .arg(remainingCount);

        QListWidgetItem* item = new QListWidgetItem(displayText);
        item->setData(Qt::UserRole, orderNumber);
        item->setCheckState(Qt::Checked);  // 默认选中
        listWidget->addItem(item);
    }
}

void WorkOrderSelectDialog::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // 添加说明标签
    QLabel* label = new QLabel("请选择要加载的工单:", this);
    mainLayout->addWidget(label);

    // 创建列表控件
    listWidget = new QListWidget(this);
    mainLayout->addWidget(listWidget);

    // 创建全选/取消全选按钮
    QHBoxLayout* buttonLayout = new QHBoxLayout;
    selectAllBtn = new QPushButton("全选", this);
    deselectAllBtn = new QPushButton("取消全选", this);

    connect(selectAllBtn, &QPushButton::clicked, this, &WorkOrderSelectDialog::onSelectAll);
    connect(deselectAllBtn, &QPushButton::clicked, this, &WorkOrderSelectDialog::onDeselectAll);

    buttonLayout->addWidget(selectAllBtn);
    buttonLayout->addWidget(deselectAllBtn);
    mainLayout->addLayout(buttonLayout);

    // 添加确定和取消按钮
    QDialogButtonBox* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
        Qt::Horizontal,
        this
        );
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(buttonBox);

    resize(400, 300);
}

void WorkOrderSelectDialog::onSelectAll()
{
    for (int i = 0; i < listWidget->count(); ++i) {
        listWidget->item(i)->setCheckState(Qt::Checked);
    }
}

void WorkOrderSelectDialog::onDeselectAll()
{
    for (int i = 0; i < listWidget->count(); ++i) {
        listWidget->item(i)->setCheckState(Qt::Unchecked);
    }
}

QStringList WorkOrderSelectDialog::getSelectedOrders() const
{
    QStringList selected;
    for (int i = 0; i < listWidget->count(); ++i) {
        QListWidgetItem* item = listWidget->item(i);
        if (item->checkState() == Qt::Checked) {
            selected << item->data(Qt::UserRole).toString();
        }
    }
    return selected;
}
