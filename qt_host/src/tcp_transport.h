#pragma once
#include "transport.h"

#include <QTcpSocket>

class TcpTransport : public ITransport
{
    Q_OBJECT
public:
    TcpTransport(QString host, quint16 port, QObject *parent = nullptr);

    bool open(QString *err) override;
    void close() override;
    bool isOpen() const override { return m_sock && m_sock->state() == QAbstractSocket::ConnectedState; }
    qint64 write(const QByteArray &data) override;
    QString describe() const override;

private:
    QString     m_host;
    quint16     m_port = 3333;
    QTcpSocket *m_sock = nullptr;
};
