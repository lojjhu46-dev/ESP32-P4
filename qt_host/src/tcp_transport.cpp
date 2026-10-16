#include "tcp_transport.h"

TcpTransport::TcpTransport(QString host, quint16 port, QObject *parent)
    : ITransport(parent), m_host(std::move(host)), m_port(port)
{
    m_sock = new QTcpSocket(this);
    connect(m_sock, &QTcpSocket::readyRead, this, [this]() {
        emit bytesReceived(m_sock->readAll());
    });
    connect(m_sock, &QTcpSocket::disconnected, this, [this]() { emit closedByPeer(); });
    connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        emit errorOccurred(m_sock->errorString());
    });
}

bool TcpTransport::open(QString *err)
{
    m_sock->connectToHost(m_host, m_port);
    if (!m_sock->waitForConnected(3000)) {
        if (err) *err = QStringLiteral("TCP 连接 %1:%2 失败：%3")
                            .arg(m_host).arg(m_port).arg(m_sock->errorString());
        return false;
    }
    /* 低延迟 + 不合并小包：0x02 结果帧很小，延迟优先 */
    m_sock->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    return true;
}

void TcpTransport::close()
{
    if (m_sock) {
        m_sock->abort();            /* 立即关，不等对端 */
    }
}

qint64 TcpTransport::write(const QByteArray &data)
{
    if (!isOpen()) {
        return -1;
    }
    qint64 n = m_sock->write(data);
    m_sock->flush();
    return n;
}

QString TcpTransport::describe() const
{
    return QStringLiteral("TCP %1:%2").arg(m_host).arg(m_port);
}
