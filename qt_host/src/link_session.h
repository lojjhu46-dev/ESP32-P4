#pragma once
/*
 * link_session.h —— 连接管理：TCP / USB 两种承载之上的同一套 JCLINK 会话
 *   · 收帧分发（0x02 结果 / 0x04 日志 / 0x05 图像 / 0x06 备份块）
 *   · 发命令 + 按 ack 精确匹配等应答（踩过的坑：ack 必须按命令名匹配）
 *   · 统计（fps / KB-s / 丢帧 / 当前链路）
 *   · F10 特征库备份导出 / 导入（0x06 分片收与发）
 */
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <QVector>

#include <functional>

#include "image_frame.h"
#include "transport.h"

class H264LivePlayer;

class LinkSession : public QObject
{
    Q_OBJECT
public:
    struct Stats {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
        quint32 rxFrames = 0, rxImage = 0, rxResult = 0, rxLog = 0, rxBlock = 0, rxH264 = 0;
        double  imgFps = 0.0;
        double  imgKbps = 0.0;
        quint32 imgDrop = 0;             /* 解码失败帧数 */
        /* 实际收到的图像格式/大小（2026-10-16）：板端 stream 的 fmt 可能与 UI 选择不一致
         * （例如"抓一帧 raw"把板端切到 raw 后没切回来），显示出来才能一眼看出。 */
        bool    lastRaw = false;
        QString lastFmt = QStringLiteral("jpeg");   /* jpeg / raw / h264 */
        int     lastBytes = 0;
        QString link = QStringLiteral("-");   /* 板端结果帧里的 link 字段（usb-cdc / tcp） */
        QString lastLog;
    };

    explicit LinkSession(QObject *parent = nullptr);
    ~LinkSession() override;

    bool openTcp(const QString &host, quint16 port, QString *err);
    bool openSerial(const QString &port, QString *err);
    void close();
    bool isConnected() const;
    QString describe() const { return m_tp ? m_tp->describe() : QStringLiteral("未连接"); }
    QString interfaceName() const { return m_iface; }

    const Stats &stats() const { return m_st; }
    bool swapRawBytes() const { return m_swapRaw; }
    void setSwapRawBytes(bool on) { m_swapRaw = on; }

    void sendCmd(const QJsonObject &obj);
    using AckCb = std::function<void(bool, const QJsonObject &)>;
    void sendCmdWait(const QJsonObject &obj, AckCb cb, int timeoutMs = 3000);
    void sendJpeg(const QByteArray &jpeg);

    void dbExport(const QString &path);
    void dbImport(const QString &path);
    bool dbBusy() const { return m_db.busy; }

    /* ---- ① 路线：raw 按需单帧抓取（2026-10-16）----
     * 连续 raw 只有 ~1.4MB/s（≈2.3fps，受 CDC 512B FIFO + 主机节奏限制，见 HANDOVER
     * 第廿五节），但**单帧 0.5s 就能拿到**，颜色标定/取证足够。本方法自动：
     *   stream on(fmt=raw, fps=5) → 等第一张 raw 帧 → 存 PNG → stream off。
     * 异步：结果经 rawGrabDone 信号回（ok=false 表示超时或未连接）。 */
    void startRawGrab(const QString &pngPath, int timeoutMs = 6000);
    bool rawGrabBusy() const { return !m_grab.path.isEmpty(); }

signals:
    void connected(const QString &desc);
    void disconnected();
    void resultFrame(const QJsonObject &obj);
    void videoFrame(const VideoFrame &frame);
    void logLine(const QString &line);
    void statsChanged(const LinkSession::Stats &st);
    void sessionError(const QString &msg);
    void decodeError(const QString &msg);
    void dbProgress(int percent, const QString &text);
    void dbDone(bool ok, const QString &msg);
    void rawGrabDone(bool ok, const QString &path, int bytes, int width, int height);

private:
    void attach(ITransport *tp);
    void onBytes(const QByteArray &data);
    void handleFrame(const jcl::Frame &f);
    void onStatTick();
    void onPendingTick();
    void pumpImport();
    void dbFinish(bool ok, const QString &msg);
    void dbArmWatchdog(int idleMs);
    void dbResetJob();

    ITransport      *m_tp = nullptr;
    H264LivePlayer  *m_h264 = nullptr;     /* 0x07 活流解码（随连接创建/销毁） */
    QString          m_iface;              /* "tcp" / "usb" */
    jcl::FrameParser m_parser;
    quint8           m_seq = 0;
    Stats            m_st;
    bool             m_swapRaw = false;
    QTimer          *m_statTimer = nullptr;
    QTimer          *m_pendTimer = nullptr;
    quint64          m_winBytes = 0;
    quint32          m_winFrames = 0;
    qint64           m_winStartMs = 0;

    struct Pending {
        QString cmd;
        qint64  deadlineMs = 0;
        AckCb   cb;
    };
    QVector<Pending> m_pending;

    struct DbJob {
        bool       busy = false;
        bool       isImport = false;
        QString    path;
        QByteArray data;                  /* 导出=接收缓冲；导入=待发送数据 */
        quint32    expectSize = 0, expectCrc = 0, got = 0;
        int        sentChunks = 0, totalChunks = 0;
        QTimer    *pump = nullptr;
        QTimer    *watchdog = nullptr;    /* 分片停住即判失败，否则 busy 永久卡死 */
    };
    DbJob m_db;

    struct GrabJob {
        QString path;              /* 非空 = 抓取进行中（兼作忙标志） */
        QTimer *timer = nullptr;
    };
    GrabJob m_grab;
};
