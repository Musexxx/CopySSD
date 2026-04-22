#ifndef WORKORDERSELECTDIALOG_H
#define WORKORDERSELECTDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QPushButton>
#include <QJsonArray>

class WorkOrderSelectDialog : public QDialog
{
    Q_OBJECT

public:
    WorkOrderSelectDialog(const QJsonArray& orders,const QString& archivePath,QWidget* parent);
    QStringList getSelectedOrders() const;

private slots:
    void onSelectAll();
    void onDeselectAll();

private:
    void setupUI();
    QListWidget* listWidget;
    QPushButton* selectAllBtn;
    QPushButton* deselectAllBtn;
    QString archivePath;  // 添加归档路径成员变量
};

#endif
