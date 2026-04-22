#ifndef WORKORDER_H
#define WORKORDER_H

#include <QString>

class WorkOrder {
public:
    QString orderNumber;
    int totalCount;
    int completedCount;
    int failedCount;
    bool isCountingStopped;

    WorkOrder(const QString& order, int total)
        : orderNumber(order)
        , totalCount(total)
        , completedCount(0)
        , failedCount(0)
        , isCountingStopped(false) {}

    int getRemainingCount() const {
        return totalCount - completedCount - failedCount;
    }
};

#endif
