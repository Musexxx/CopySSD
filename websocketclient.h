#ifndef WEBSOCKETCLIENT_H
#define WEBSOCKETCLIENT_H

#include <QObject>
#include <QWebSocket>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

class WebSocketClient : public QObject
{
    Q_OBJECT
public:
    explicit WebSocketClient(QObject *parent = nullptr);
    ~WebSocketClient();

    void connectToServer();
    void disconnectFromServer();
    void sendWorkOrdersInfo(const QJsonObject& data);
    bool isConnected() const { return m_isConnected; }
    QString getConnectionStatus() const { return m_connectionStatus; }

signals:
    void connectionStatusChanged(const QString& status);
    void connected();
    void disconnected();
    void error(const QString& errorString);

private slots:
    void onConnected();
    void onDisconnected();
    void onError(QAbstractSocket::SocketError error);

private:
    void loadConfig();
    void updateConnectionStatus(const QString& status);

    QWebSocket* m_webSocket;
    QString m_serverIP;
    int m_serverPort;
    bool m_isConnected;
    QString m_connectionStatus;
};

#endif // WEBSOCKETCLIENT_H
