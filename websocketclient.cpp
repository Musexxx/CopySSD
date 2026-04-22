#include "websocketclient.h"
#include "logger.h"

WebSocketClient::WebSocketClient(QObject *parent)
    : QObject(parent)
    , m_webSocket(new QWebSocket())
    , m_isConnected(false)
    , m_connectionStatus("未连接")
{
    loadConfig();

    connect(m_webSocket, &QWebSocket::connected, this, &WebSocketClient::onConnected);
    connect(m_webSocket, &QWebSocket::disconnected, this, &WebSocketClient::onDisconnected);
    connect(m_webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            this, &WebSocketClient::onError);
}

WebSocketClient::~WebSocketClient()
{
    if (m_webSocket) {
        m_webSocket->close();
        delete m_webSocket;
    }
}

void WebSocketClient::loadConfig()
{
    QSettings settings("config.ini", QSettings::IniFormat);
    m_serverIP = settings.value("CentralServer/IP", "localhost").toString();
    m_serverPort = settings.value("CentralServer/Port", 8080).toInt();
    LOG_INFO(QString("已加载中控服务器配置 - IP: %1, Port: %2").arg(m_serverIP).arg(m_serverPort));
}

void WebSocketClient::connectToServer()
{
    if (m_isConnected) {
        LOG_WARNING("已经连接到服务器");
        return;
    }

    QUrl url(QString("ws://%1:%2").arg(m_serverIP).arg(m_serverPort));
    m_webSocket->open(url);
    updateConnectionStatus("正在连接...");
    LOG_INFO(QString("正在连接到服务器: %1").arg(url.toString()));
}

void WebSocketClient::disconnectFromServer()
{
    if (!m_isConnected) {
        LOG_WARNING("未连接到服务器");
        return;
    }

    m_webSocket->close();
    LOG_INFO("正在断开服务器连接");
}

void WebSocketClient::sendWorkOrdersInfo(const QJsonObject& data)
{
    if (!m_isConnected) {
        LOG_WARNING("未连接到服务器，无法发送数据");
        return;
    }

    QJsonDocument doc(data);
    QString message = doc.toJson(QJsonDocument::Compact);
    m_webSocket->sendTextMessage(message);
    LOG_INFO("已发送工单信息到中控服务器");
}

void WebSocketClient::onConnected()
{
    m_isConnected = true;
    updateConnectionStatus("已连接");
    emit connected();
    LOG_INFO("已连接到中控服务器");
}

void WebSocketClient::onDisconnected()
{
    m_isConnected = false;
    updateConnectionStatus("未连接");
    emit disconnected();
    LOG_INFO("已断开与中控服务器的连接");
}

void WebSocketClient::onError(QAbstractSocket::SocketError error)
{
    QString errorString = m_webSocket->errorString();
    updateConnectionStatus(QString("连接错误: %1").arg(errorString));
    emit this->error(errorString);
    LOG_ERROR(QString("WebSocket错误: %1").arg(errorString));
}

void WebSocketClient::updateConnectionStatus(const QString& status)
{
    m_connectionStatus = status;
    emit connectionStatusChanged(status);
}
