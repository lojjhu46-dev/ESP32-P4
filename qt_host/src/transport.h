#pragma once
/*
 * transport.h —— 传输抽象：TCP 与 USB 串口是同一套 JCLINK 的两种物理承载。
 * 上层（LinkSession / UI）只依赖本接口，切换接口 = 换实现 + 重连。
 */
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

class ITransport : public QObject
{
    Q_OBJECT
public:
    explicit ITransport(QObject *parent = nullptr) : QObject(parent) {}
    ~ITransport() override = default;

    virtual bool open(QString *err) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual qint64 write(const QByteArray &data) = 0;
    virtual QString describe() const = 0;

signals:
    void bytesReceived(const QByteArray &data);
    void closedByPeer();
    void errorOccurred(const QString &msg);
};

/* 串口枚举（Win32 SetupAPI）：给出端口名、友好名与"是否疑似本板 USB" */
struct SerialPortInfo {
    QString port;        /* "COM10" */
    QString friendly;    /* "USB 串行设备 (COM10)" */
    bool    likelyBoard = false;   /* VID_303A（Espressif）/ 名字含 USB 串行设备，且不是 CH340 */
};

QVector<SerialPortInfo> enumerateSerialPorts();
