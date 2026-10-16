#include "serial_transport.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <chrono>
#include <setupapi.h>

/* 端口设备类 GUID {4d36e978-e325-11ce-bfc1-08002be10318}
   （不用 <devguid.h> 的 GUID_DEVCLASS_PORTS：MinGW 下需要 INITGUID 才能取到定义） */
static const GUID kPortsClassGuid =
    {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};

QVector<SerialPortInfo> enumerateSerialPorts()
{
    QVector<SerialPortInfo> list;
    HDEVINFO devs = SetupDiGetClassDevsW(&kPortsClassGuid, nullptr, nullptr, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        return list;
    }

    static const QRegularExpression rePort(QStringLiteral("\\((COM\\d+)\\)"));
    SP_DEVINFO_DATA info{};
    info.cbSize = sizeof(info);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &info); i++) {
        wchar_t friendly[256] = {0};
        SetupDiGetDeviceRegistryPropertyW(devs, &info, SPDRP_FRIENDLYNAME, nullptr,
                                          reinterpret_cast<PBYTE>(friendly), sizeof(friendly), nullptr);
        wchar_t instId[512] = {0};
        SetupDiGetDeviceInstanceIdW(devs, &info, instId, 512, nullptr);

        const QString fr = QString::fromWCharArray(friendly);
        const QString id = QString::fromWCharArray(instId);
        const auto m = rePort.match(fr);
        if (!m.hasMatch()) {
            continue;
        }

        SerialPortInfo p;
        p.port = m.captured(1);
        p.friendly = fr;
        /* CH340(VID_1A86) 是烧录口：打开它会拉 DTR/RTS 复位板子，标记为非本板 */
        const bool isCh340 = id.contains(QStringLiteral("VID_1A86"), Qt::CaseInsensitive)
                          || fr.contains(QStringLiteral("CH340"), Qt::CaseInsensitive);
        p.likelyBoard = !isCh340
                     && (id.contains(QStringLiteral("VID_303A"), Qt::CaseInsensitive)
                         || fr.contains(QStringLiteral("Espressif"), Qt::CaseInsensitive)
                         || fr.contains(QStringLiteral("USB 串行设备"))
                         || fr.contains(QStringLiteral("USB Serial Device"), Qt::CaseInsensitive));
        list.append(p);
    }
    SetupDiDestroyDeviceInfoList(devs);

    std::stable_sort(list.begin(), list.end(), [](const SerialPortInfo &a, const SerialPortInfo &b) {
        return a.likelyBoard && !b.likelyBoard;
    });
    return list;
}

SerialTransport::SerialTransport(QString port, QObject *parent)
    : ITransport(parent), m_port(std::move(port))
{
    /* 手动复位事件：重叠 I/O 的完成通知；读写各一个，避免互相覆盖 */
    m_evtRead = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_evtWrite = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

SerialTransport::~SerialTransport()
{
    close();
    if (m_evtRead) {
        CloseHandle(m_evtRead);
        m_evtRead = nullptr;
    }
    if (m_evtWrite) {
        CloseHandle(m_evtWrite);
        m_evtWrite = nullptr;
    }
}

bool SerialTransport::open(QString *err)
{
    close();

    const QString path = QStringLiteral("\\\\.\\%1").arg(m_port);
    const std::wstring wpath = path.toStdWString();
    /* ⚠️ 必须 FILE_FLAG_OVERLAPPED：否则读线程阻塞在 ReadFile 里时无法取消，
       一旦上层异常退出就会把 COM 口连同 usbser.sys 一起卡死
       （2026-10-16 实测：进程被杀后端口一直 ERROR_ACCESS_DENIED，只能复位板子重枚举）。 */
    m_h = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (m_h == INVALID_HANDLE_VALUE) {
        if (err) {
            *err = QStringLiteral("打开 %1 失败（Win32 错误码 %2，端口被占用或不存在？）")
                       .arg(m_port).arg(GetLastError());
        }
        return false;
    }

    /* 1MB 驱动收发缓冲：raw 614KB/帧 时，缓冲小了会卡在 WriteFile */
    SetupComm(m_h, 1 << 20, 1 << 20);
    PurgeComm(m_h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(m_h, &dcb)) {
        if (err) *err = QStringLiteral("GetCommState 失败（错误码 %1）").arg(GetLastError());
        close();
        return false;
    }
    dcb.BaudRate = 921600;          /* USB CDC 忽略波特率，但 DCB 必须给合法值 */
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;   /* 原生 USB CDC 无副作用；CH340 会复位板子（UI 已提示） */
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX  = FALSE;
    if (!SetCommState(m_h, &dcb)) {
        if (err) *err = QStringLiteral("SetCommState 失败（错误码 %1）").arg(GetLastError());
        close();
        return false;
    }

    /* 重叠 I/O 下 CommTimeouts 不生效（读取的等待由 WaitForSingleObject 控制），
       这里仍设一份"有数据就返回"的配置以供非重叠路径兜底。 */
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout        = 5;
    to.ReadTotalTimeoutMultiplier = 0;
    to.ReadTotalTimeoutConstant   = 0;
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 3000;
    SetCommTimeouts(m_h, &to);

    m_running = true;
    m_reader = std::thread(&SerialTransport::readLoop, this);
    return true;
}

void SerialTransport::close()
{
    m_running = false;
    if (m_h != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_h, nullptr);           /* 取消在途读写 → 读线程 ≤100ms 内返回 */
        if (m_reader.joinable()) {
            m_reader.join();                /* 读线程的等待最长 100ms，不会长挂 */
        }
        CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    } else if (m_reader.joinable()) {
        m_reader.join();
    }
}

void SerialTransport::readLoop()
{
    const HANDLE h = m_h;                   /* 线程启动时取一次，避免与 close() 竞争 */
    QByteArray buf(64 * 1024, Qt::Uninitialized);

    while (m_running.load()) {
        OVERLAPPED ov{};
        ov.hEvent = m_evtRead;
        ResetEvent(ov.hEvent);

        DWORD got = 0;
        if (!ReadFile(h, buf.data(), DWORD(buf.size()), &got, &ov)) {
            const DWORD e = GetLastError();
            if (e != ERROR_IO_PENDING) {
                if (m_running.load()) {
                    emit errorOccurred(QStringLiteral("串口读失败（错误码 %1）").arg(e));
                }
                break;
            }
            /* 等完成，最多 100ms —— 这样 close() 的 CancelIoEx 能立刻让线程退出 */
            const DWORD w = WaitForSingleObject(ov.hEvent, 100);
            if (w == WAIT_TIMEOUT) {
                CancelIoEx(h, &ov);
                WaitForSingleObject(ov.hEvent, 100);
                continue;                   /* 超时属正常（无数据），下轮再等 */
            }
            if (!GetOverlappedResult(h, &ov, &got, FALSE)) {
                if (!m_running.load()) {
                    break;
                }
                continue;                   /* 偶发错误不杀线程 */
            }
        }

        if (got > 0) {
            emit bytesReceived(QByteArray(buf.constData(), int(got)));
        }
    }
}

qint64 SerialTransport::write(const QByteArray &data)
{
    if (m_h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    OVERLAPPED ov{};
    ov.hEvent = m_evtWrite;
    ResetEvent(ov.hEvent);

    DWORD written = 0;
    if (!WriteFile(m_h, data.constData(), DWORD(data.size()), &written, &ov)) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            emit errorOccurred(QStringLiteral("串口写失败（错误码 %1）").arg(e));
            return -1;
        }
        if (WaitForSingleObject(ov.hEvent, 3000) != WAIT_OBJECT_0) {
            CancelIoEx(m_h, &ov);
            emit errorOccurred(QStringLiteral("串口写超时（3s）"));
            return -1;
        }
        if (!GetOverlappedResult(m_h, &ov, &written, FALSE)) {
            emit errorOccurred(QStringLiteral("串口写未完成（错误码 %1）").arg(GetLastError()));
            return -1;
        }
    }
    return qint64(written);
}

QString SerialTransport::describe() const
{
    return QStringLiteral("USB %1").arg(m_port);
}
