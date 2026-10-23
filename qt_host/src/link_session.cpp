#include "link_session.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "serial_transport.h"
#include "tcp_transport.h"
#include "h264_player.h"

LinkSession::LinkSession(QObject *parent)
    : QObject(parent)
{
    m_statTimer = new QTimer(this);
    m_statTimer->setInterval(1000);
    connect(m_statTimer, &QTimer::timeout, this, &LinkSession::onStatTick);
    m_statTimer->start();

    m_pendTimer = new QTimer(this);
    m_pendTimer->setInterval(200);
    connect(m_pendTimer, &QTimer::timeout, this, &LinkSession::onPendingTick);
    m_pendTimer->start();

    m_winStartMs = QDateTime::currentMSecsSinceEpoch();
}

LinkSession::~LinkSession()
{
    close();
}

bool LinkSession::openTcp(const QString &host, quint16 port, QString *err)
{
    close();
    auto *tp = new TcpTransport(host, port, this);
    if (!tp->open(err)) {
        tp->deleteLater();
        return false;
    }
    m_iface = QStringLiteral("tcp");
    attach(tp);
    return true;
}

bool LinkSession::openSerial(const QString &port, QString *err)
{
    close();
    auto *tp = new SerialTransport(port, this);
    if (!tp->open(err)) {
        tp->deleteLater();
        return false;
    }
    m_iface = QStringLiteral("usb");
    attach(tp);
    return true;
}

void LinkSession::attach(ITransport *tp)
{
    m_tp = tp;
    connect(tp, &ITransport::bytesReceived, this, &LinkSession::onBytes);
    connect(tp, &ITransport::errorOccurred, this, [this](const QString &m) { emit sessionError(m); });
    connect(tp, &ITransport::closedByPeer, this, [this]() {
        emit sessionError(QStringLiteral("对端关闭了连接"));
        close();
    });
    m_parser.reset();

    /* 每次连接重建 0x07 解码器：上一次的 FFmpeg 读线程/管道状态不复用 */
    if (m_h264) {
        delete m_h264;
        m_h264 = nullptr;
    }
    m_h264 = new H264LivePlayer(this);
    connect(m_h264, &H264LivePlayer::frameReady, this, [this](const QImage &img) {
        VideoFrame vf;
        vf.image = img;
        vf.width = img.width();
        vf.height = img.height();
        vf.fmt = 2;                     /* 2 = H.264 解码帧 */
        vf.bytes = m_st.lastBytes;
        vf.tsMs = QDateTime::currentMSecsSinceEpoch();
        emit videoFrame(vf);
    });
    connect(m_h264, &H264LivePlayer::errorChanged, this, [this](const QString &m) {
        emit decodeError(QStringLiteral("H.264: %1").arg(m));
    });

    emit connected(tp->describe());
}

void LinkSession::close()
{
    if (!m_tp) {
        return;
    }
    ITransport *tp = m_tp;
    m_tp = nullptr;
    tp->disconnect(this);
    tp->close();
    tp->deleteLater();

    m_iface.clear();
    m_pending.clear();
    if (m_h264) {
        delete m_h264;
        m_h264 = nullptr;
    }
    if (m_db.pump) {
        m_db.pump->stop();
        m_db.pump->deleteLater();
        m_db.pump = nullptr;
    }
    m_db.busy = false;
    m_parser.reset();
    emit disconnected();
}

bool LinkSession::isConnected() const
{
    return m_tp && m_tp->isOpen();
}

void LinkSession::sendCmd(const QJsonObject &obj)
{
    if (!isConnected()) {
        emit sessionError(QStringLiteral("未连接，命令未发送"));
        return;
    }
    const QByteArray js = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    const QByteArray f = jcl::buildFrame(jcl::T_CMD, js, ++m_seq);
    if (m_tp->write(f) < 0) {
        emit sessionError(QStringLiteral("命令发送失败"));
        return;
    }
    m_st.txBytes += quint64(f.size());
}

void LinkSession::sendCmdWait(const QJsonObject &obj, AckCb cb, int timeoutMs)
{
    if (!isConnected()) {
        if (cb) cb(false, QJsonObject{});
        emit sessionError(QStringLiteral("未连接"));
        return;
    }
    Pending p;
    p.cmd = obj.value(QStringLiteral("cmd")).toString();
    p.deadlineMs = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    p.cb = std::move(cb);
    m_pending.append(p);
    sendCmd(obj);
}

void LinkSession::sendJpeg(const QByteArray &jpeg)
{
    if (!isConnected()) {
        emit sessionError(QStringLiteral("未连接，图像未发送"));
        return;
    }
    const QByteArray f = jcl::buildFrame(jcl::T_JPEG, jpeg, ++m_seq);
    if (m_tp->write(f) < 0) {
        emit sessionError(QStringLiteral("图像发送失败"));
        return;
    }
    m_st.txBytes += quint64(f.size());
}

void LinkSession::onBytes(const QByteArray &data)
{
    m_st.rxBytes += quint64(data.size());
    const QVector<jcl::Frame> frames = m_parser.feed(data);
    for (const jcl::Frame &f : frames) {
        handleFrame(f);
    }
}

void LinkSession::handleFrame(const jcl::Frame &f)
{
    m_st.rxFrames++;
    switch (f.type) {
    case jcl::T_RESULT: {
        m_st.rxResult++;
        QJsonParseError perr{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.payload, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            return;
        }
        const QJsonObject obj = doc.object();
        if (obj.contains(QStringLiteral("link"))) {
            m_st.link = obj.value(QStringLiteral("link")).toString();
        }
        const QString ack = obj.value(QStringLiteral("ack")).toString();
        if (!ack.isEmpty()) {
            for (int i = 0; i < m_pending.size(); i++) {
                if (m_pending.at(i).cmd == ack) {
                    const AckCb cb = m_pending.at(i).cb;
                    m_pending.remove(i);
                    if (cb) cb(true, obj);
                    break;
                }
            }
        }
        emit resultFrame(obj);
        break;
    }

    case jcl::T_IMAGE: {
        m_st.rxImage++;
        m_winBytes += quint64(f.payload.size());
        m_winFrames++;
        VideoFrame vf;
        QString err;
        if (decodeImageFrame(f.payload, m_swapRaw, &vf, &err)) {
            m_st.lastRaw = (vf.fmt == 1);
            m_st.lastFmt = (vf.fmt == 1) ? QStringLiteral("raw") : QStringLiteral("jpeg");
            m_st.lastBytes = vf.bytes;
            emit videoFrame(vf);
            /* ① 路线：raw 按需单帧抓取（见 startRawGrab）——收到第一张 raw 帧即落盘并停流 */
            if (!m_grab.path.isEmpty() && vf.fmt == 1) {
                const QString path = m_grab.path;
                const int bytes = vf.bytes, w = vf.width, h = vf.height;
                m_grab.path.clear();
                if (m_grab.timer) {
                    m_grab.timer->stop();
                }
                QJsonObject off;
                off.insert(QStringLiteral("cmd"), QStringLiteral("stream"));
                off.insert(QStringLiteral("value"), QStringLiteral("off"));
                sendCmd(off);
                const bool ok = vf.image.save(path, "PNG");
                emit rawGrabDone(ok, path, bytes, w, h);
            }
        } else {
            m_st.imgDrop++;
            emit decodeError(err);
        }
        break;
    }

    case jcl::T_H264: {
        /* 0x07：20B 头 + Annex-B AU。头见 app_usb_link.h：
           codec u8 | flags u8 | stream_id u16 | pts_us u32 | frame_seq u32 |
           width u16 | height u16 | nal_len u32 */
        const QByteArray &p = f.payload;
        if (p.size() < 20) {
            m_st.imgDrop++;
            emit decodeError(QStringLiteral("0x07 载荷不足 20 字节"));
            break;
        }
        const auto *b = reinterpret_cast<const quint8 *>(p.constData());
        const quint8 codec = b[0];
        const quint8 flags = b[1];
        const quint16 w = quint16(b[12] | (b[13] << 8));
        const quint16 h = quint16(b[14] | (b[15] << 8));
        const quint32 nalLen = quint32(b[16]) | (quint32(b[17]) << 8) |
                               (quint32(b[18]) << 16) | (quint32(b[19]) << 24);
        if (codec != 1 || nalLen == 0 || qint64(20) + qint64(nalLen) > qint64(p.size())) {
            m_st.imgDrop++;
            emit decodeError(QStringLiteral("0x07 头非法（codec=%1 nal_len=%2 size=%3）")
                                 .arg(codec).arg(nalLen).arg(p.size()));
            break;
        }
        m_st.rxH264++;
        m_st.lastFmt = QStringLiteral("h264");
        m_st.lastBytes = int(nalLen);
        m_winBytes += quint64(nalLen);
        m_winFrames++;
        if (m_h264) {
            /* bit1 = has_sps_pps；解码器要等第一个带参数集的关键帧才开始 */
            m_h264->feed(p.mid(20, int(nalLen)), (flags & 0x02) != 0, w, h);
        }
        break;
    }

    case jcl::T_LOG: {
        m_st.rxLog++;
        m_st.lastLog = QString::fromUtf8(f.payload).trimmed();
        emit logLine(m_st.lastLog);
        break;
    }

    case jcl::T_BLOCK: {
        m_st.rxBlock++;
        if (!m_db.busy || m_db.isImport) {
            break;
        }
        quint32 off = 0;
        QByteArray chunk;
        if (!jcl::parseBlockHeader(f.payload, &off, &chunk)) {
            break;
        }
        if (quint64(off) + quint64(chunk.size()) > quint64(m_db.data.size())) {
            dbFinish(false, QStringLiteral("分片越界 off=%1 len=%2").arg(off).arg(chunk.size()));
            break;
        }
        memcpy(m_db.data.data() + off, chunk.constData(), size_t(chunk.size()));
        m_db.got += quint32(chunk.size());
        const int pct = m_db.expectSize ? int(quint64(m_db.got) * 100u / m_db.expectSize) : 0;
        emit dbProgress(pct, QStringLiteral("接收 %1%  (%2 / %3 B)")
                                 .arg(pct).arg(m_db.got).arg(m_db.expectSize));
        if (m_db.got >= m_db.expectSize) {
            const quint32 crc = jcl::crc32(m_db.data);
            if (crc != m_db.expectCrc) {
                dbFinish(false, QStringLiteral("CRC 不符（板端 %1 / 本地 %2）")
                                    .arg(m_db.expectCrc, 8, 16, QChar('0'))
                                    .arg(crc, 8, 16, QChar('0')));
                break;
            }
            QFile file(m_db.path);
            if (!file.open(QIODevice::WriteOnly)) {
                dbFinish(false, QStringLiteral("写文件失败：%1").arg(file.errorString()));
                break;
            }
            file.write(m_db.data);
            file.close();
            dbFinish(true, QStringLiteral("导出成功：%1（%2 B）")
                              .arg(m_db.path).arg(m_db.data.size()));
        }
        break;
    }

    default:
        break;
    }
}

void LinkSession::onStatTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double dt = double(now - m_winStartMs) / 1000.0;
    if (dt > 0.05) {
        m_st.imgFps = double(m_winFrames) / dt;
        m_st.imgKbps = double(m_winBytes) / 1024.0 / dt;
    }
    m_winFrames = 0;
    m_winBytes = 0;
    m_winStartMs = now;
    emit statsChanged(m_st);
}

void LinkSession::onPendingTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = m_pending.size() - 1; i >= 0; i--) {
        if (m_pending.at(i).deadlineMs <= now) {
            const AckCb cb = m_pending.at(i).cb;
            m_pending.remove(i);
            if (cb) cb(false, QJsonObject{});
        }
    }
}

void LinkSession::dbFinish(bool ok, const QString &msg)
{
    if (m_db.pump) {
        m_db.pump->stop();
    }
    m_db.busy = false;
    m_db.data.clear();
    m_db.got = 0;
    emit dbDone(ok, msg);
}

void LinkSession::dbExport(const QString &path)
{
    if (m_db.busy) {
        emit dbDone(false, QStringLiteral("已有备份任务在跑"));
        return;
    }
    m_db = DbJob{};
    m_db.busy = true;
    m_db.isImport = false;
    m_db.path = path;

    QJsonObject req;
    req.insert(QStringLiteral("cmd"), QStringLiteral("db_export"));
    sendCmdWait(req, [this](bool ok, const QJsonObject &reply) {
        if (!ok) {
            dbFinish(false, QStringLiteral("db_export 无应答（超时）"));
            return;
        }
        if (reply.value(QStringLiteral("event")).toString() == QStringLiteral("error")) {
            dbFinish(false, QStringLiteral("板端拒绝：%1").arg(reply.value(QStringLiteral("msg")).toString()));
            return;
        }
        const QJsonObject dbx = reply.value(QStringLiteral("dbx")).toObject();
        m_db.expectSize = quint32(dbx.value(QStringLiteral("size")).toDouble());
        m_db.expectCrc  = quint32(dbx.value(QStringLiteral("crc")).toDouble());
        if (m_db.expectSize == 0 || m_db.expectSize > (4u * 1024u * 1024u)) {
            dbFinish(false, QStringLiteral("板端返回的 size 非法：%1").arg(m_db.expectSize));
            return;
        }
        m_db.data = QByteArray(int(m_db.expectSize), '\0');
        m_db.got = 0;
        emit dbProgress(0, QStringLiteral("等待分片…共 %1 B / %2 片")
                               .arg(m_db.expectSize)
                               .arg(dbx.value(QStringLiteral("parts")).toInt()));
    }, 5000);
}

void LinkSession::startRawGrab(const QString &pngPath, int timeoutMs)
{
    if (pngPath.isEmpty() || !isConnected()) {
        emit rawGrabDone(false, pngPath, 0, 0, 0);
        return;
    }
    if (rawGrabBusy()) {
        emit rawGrabDone(false, pngPath, 0, 0, 0);      /* 已有抓取在跑 */
        return;
    }
    m_grab.path = pngPath;
    if (!m_grab.timer) {
        m_grab.timer = new QTimer(this);
        m_grab.timer->setSingleShot(true);
        connect(m_grab.timer, &QTimer::timeout, this, [this]() {
            if (m_grab.path.isEmpty()) {
                return;
            }
            const QString p = m_grab.path;
            m_grab.path.clear();
            QJsonObject off;
            off.insert(QStringLiteral("cmd"), QStringLiteral("stream"));
            off.insert(QStringLiteral("value"), QStringLiteral("off"));
            sendCmd(off);
            emit rawGrabDone(false, p, 0, 0, 0);        /* 超时：流已停，库/画面不受影响 */
        });
    }
    m_grab.timer->start(timeoutMs);

    QJsonObject on;
    on.insert(QStringLiteral("cmd"), QStringLiteral("stream"));
    on.insert(QStringLiteral("value"), QStringLiteral("on"));
    on.insert(QStringLiteral("fps"), 5);                 /* raw 单帧 ~0.43s，5fps 足够且不压链路 */
    on.insert(QStringLiteral("fmt"), QStringLiteral("raw"));
    sendCmd(on);
}

void LinkSession::dbImport(const QString &path)
{
    if (m_db.busy) {
        emit dbDone(false, QStringLiteral("已有备份任务在跑"));
        return;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        emit dbDone(false, QStringLiteral("打不开文件：%1").arg(file.errorString()));
        return;
    }
    const QByteArray data = file.readAll();
    file.close();
    if (data.size() < 32) {
        emit dbDone(false, QStringLiteral("文件太小，不像备份文件（%1 B）").arg(data.size()));
        return;
    }

    m_db = DbJob{};
    m_db.busy = true;
    m_db.isImport = true;
    m_db.path = path;
    m_db.data = data;
    m_db.expectSize = quint32(data.size());
    m_db.totalChunks = (data.size() + 16384 - 1) / 16384;

    const quint32 crc = jcl::crc32(data);
    QJsonObject req;
    req.insert(QStringLiteral("cmd"), QStringLiteral("db_import"));
    req.insert(QStringLiteral("size"), double(data.size()));
    req.insert(QStringLiteral("crc"), double(crc));
    sendCmdWait(req, [this](bool ok, const QJsonObject &reply) {
        if (!ok) {
            dbFinish(false, QStringLiteral("db_import 无应答（超时）"));
            return;
        }
        if (reply.value(QStringLiteral("event")).toString() == QStringLiteral("error")) {
            dbFinish(false, QStringLiteral("板端拒绝：%1").arg(reply.value(QStringLiteral("msg")).toString()));
            return;
        }
        m_db.pump = new QTimer(this);
        m_db.pump->setInterval(1);
        connect(m_db.pump, &QTimer::timeout, this, &LinkSession::pumpImport);
        m_db.pump->start();
    }, 5000);
}

void LinkSession::pumpImport()
{
    if (!m_db.busy || !m_db.isImport || !isConnected()) {
        if (m_db.pump) m_db.pump->stop();
        return;
    }
    const int kChunk = 16384;                 /* 与板端 JCL_BLOB_CHUNK_DEF 同档 */
    for (int k = 0; k < 4 && m_db.sentChunks < m_db.totalChunks; k++) {
        const int off = m_db.sentChunks * kChunk;
        const int n = qMin(kChunk, m_db.data.size() - off);
        QByteArray payload;
        payload.reserve(4 + n);
        payload.append(char(off & 0xFF));
        payload.append(char((off >> 8) & 0xFF));
        payload.append(char((off >> 16) & 0xFF));
        payload.append(char((off >> 24) & 0xFF));
        payload.append(m_db.data.constData() + off, n);
        const QByteArray f = jcl::buildFrame(jcl::T_BLOCK, payload, ++m_seq);
        if (m_tp->write(f) < 0) {
            dbFinish(false, QStringLiteral("分片发送失败（off=%1）").arg(off));
            return;
        }
        m_st.txBytes += quint64(f.size());
        m_db.sentChunks++;
    }
    const int pct = m_db.totalChunks ? int(quint64(m_db.sentChunks) * 100u / quint64(m_db.totalChunks)) : 0;
    emit dbProgress(pct, QStringLiteral("发送 %1%  (%2 / %3 片)")
                             .arg(pct).arg(m_db.sentChunks).arg(m_db.totalChunks));

    if (m_db.sentChunks >= m_db.totalChunks) {
        m_db.pump->stop();
        QJsonObject req;
        req.insert(QStringLiteral("cmd"), QStringLiteral("db_import_end"));
        sendCmdWait(req, [this](bool ok, const QJsonObject &reply) {
            if (!ok) {
                dbFinish(false, QStringLiteral("db_import_end 无应答（超时）"));
                return;
            }
            if (reply.value(QStringLiteral("event")).toString() == QStringLiteral("error")) {
                dbFinish(false, QStringLiteral("导入被拒：%1").arg(reply.value(QStringLiteral("msg")).toString()));
                return;
            }
            dbFinish(true, QStringLiteral("导入成功：%1").arg(reply.value(QStringLiteral("msg")).toString()));
        }, 30000);
    }
}
