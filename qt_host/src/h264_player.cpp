#include "h264_player.h"

#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

static QString findFfmpeg()
{
    const QString env = qEnvironmentVariable("FFMPEG_EXE");
    if (!env.isEmpty() && QFileInfo::exists(env)) {
        return env;
    }
    const QString inPath = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    if (!inPath.isEmpty()) {
        return inPath;
    }
    for (const char *c : { "D:/ffmpeg/bin/ffmpeg.exe", "C:/ffmpeg/bin/ffmpeg.exe" }) {
        if (QFileInfo::exists(QString::fromLatin1(c))) {
            return QString::fromLatin1(c);
        }
    }
    return {};
}

H264LivePlayer::H264LivePlayer(QObject *parent) : QObject(parent)
{
}

H264LivePlayer::~H264LivePlayer()
{
    stopProc();
}

void H264LivePlayer::ensureStarted(int width, int height)
{
    if (m_started) {
        return;
    }
    if (width <= 0 || height <= 0) {
        return;
    }
    const QString exe = findFfmpeg();
    if (exe.isEmpty()) {
        emit errorChanged(QStringLiteral("找不到 ffmpeg.exe（设 FFMPEG_EXE 或加入 PATH）"));
        return;
    }

    m_w = width;
    m_h = height;
    m_frameBytes = width * height * 3;
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, &H264LivePlayer::onReadyRead);
    connect(m_proc, &QProcess::readyReadStandardError, this, [this]() {
        m_err += m_proc->readAllStandardError();
        if (m_err.size() > 4096) {
            m_err = m_err.right(4096);
        }
    });
    connect(m_proc, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        emit errorChanged(QStringLiteral("ffmpeg 进程错误 %1: %2")
                              .arg(int(e)).arg(m_proc->errorString()));
    });
    connect(m_proc, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        qWarning() << "[h264] ffmpeg exited" << code << int(st)
                   << m_err.right(400).constData();
    });

    const QStringList args = {
        QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"), QStringLiteral("error"),
        /* ⚠️ 不要加 -fflags nobuffer：raw H.264 下它只解 2 帧就停（实测 199 帧只出 1.8MB） */
        QStringLiteral("-flags"), QStringLiteral("low_delay"),
        QStringLiteral("-f"), QStringLiteral("h264"),
        QStringLiteral("-i"), QStringLiteral("pipe:0"),
        QStringLiteral("-an"), QStringLiteral("-sn"),
        QStringLiteral("-f"), QStringLiteral("rawvideo"),
        QStringLiteral("-pix_fmt"), QStringLiteral("rgb24"),
        QStringLiteral("pipe:1"),
    };
    m_proc->start(exe, args);
    if (!m_proc->waitForStarted(3000)) {
        emit errorChanged(QStringLiteral("ffmpeg 启动失败: %1").arg(m_proc->errorString()));
        stopProc();
        return;
    }
    m_started = true;
    qInfo() << "[h264] ffmpeg decoder started" << width << "x" << height << exe;
}

void H264LivePlayer::feed(const QByteArray &nal, bool hasSpsPps, int width, int height)
{
    if (nal.isEmpty()) {
        return;
    }
    if (!m_started) {
        if (!hasSpsPps) {
            return;             /* 等第一个带 SPS/PPS 的 IDR */
        }
        ensureStarted(width, height);
    }
    if (m_proc && m_proc->state() == QProcess::Running) {
        m_proc->write(nal);
    }
}

void H264LivePlayer::onReadyRead()
{
    if (!m_proc) {
        return;
    }
    m_out += m_proc->readAllStandardOutput();
    while (m_frameBytes > 0 && m_out.size() >= m_frameBytes) {
        const QImage view(reinterpret_cast<const uchar *>(m_out.constData()),
                          m_w, m_h, m_w * 3, QImage::Format_RGB888);
        emit frameReady(view.copy());       /* 必须 copy：m_out 会被复用 */
        m_out.remove(0, m_frameBytes);
        if (++m_frames % 100 == 0) {
            qInfo() << "[h264] decoded frames" << m_frames;
        }
    }
    /* 如果解码器输出比预期慢，m_out 会短暂堆积；超过 2 帧就丢最旧的，保持低延迟 */
    if (m_frameBytes > 0 && m_out.size() > m_frameBytes * 2) {
        m_out.remove(0, m_out.size() - m_frameBytes);
    }
}

void H264LivePlayer::stopProc()
{
    m_started = false;
    if (!m_proc) {
        return;
    }
    m_proc->closeWriteChannel();
    if (!m_proc->waitForFinished(500)) {
        m_proc->kill();
        m_proc->waitForFinished(500);
    }
    m_proc->deleteLater();
    m_proc = nullptr;
}
