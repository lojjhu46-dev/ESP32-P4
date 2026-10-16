#pragma once
/*
 * serial_transport.h —— USB CDC 串口传输（Win32 API 自实现）
 *
 * ⚠️ 本机 Qt 6.12 未安装 Qt6SerialPort 模块，故直接用 Win32：
 *   CreateFile("\\\\.\\COMx") + SetupComm(1MB) + SetCommTimeouts + 独立读线程(64KB 块)
 * —— 比 QSerialPort 更能给到大缓冲（raw 614KB/帧 @10fps ≈ 6MB/s 靠这个）。
 */
#include "transport.h"

#include <atomic>
#include <thread>
#include <windows.h>

class SerialTransport : public ITransport
{
    Q_OBJECT
public:
    explicit SerialTransport(QString port, QObject *parent = nullptr);
    ~SerialTransport() override;

    bool open(QString *err) override;
    void close() override;
    bool isOpen() const override { return m_h != INVALID_HANDLE_VALUE; }
    qint64 write(const QByteArray &data) override;
    QString describe() const override;

private:
    void readLoop();

    QString           m_port;
    HANDLE            m_h = INVALID_HANDLE_VALUE;
    HANDLE            m_evtRead = nullptr;    /* 重叠读完成事件 */
    HANDLE            m_evtWrite = nullptr;   /* 重叠写完成事件 */
    std::atomic<bool> m_running{false};
    std::thread       m_reader;
};
