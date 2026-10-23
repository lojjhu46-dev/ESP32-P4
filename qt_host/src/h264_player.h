#pragma once
/*
 * h264_player.h —— 0x07 H.264 活流 → QImage
 *
 * 用 QProcess 调 ffmpeg.exe 解码（而不是 QMediaPlayer）：
 *   NAL 写 stdin -> ffmpeg -f h264 -i pipe:0 -> rawvideo rgb24 stdout -> QImage。
 * QMediaPlayer 的 FFmpeg 后端会把非 seekable 的活管道卡在 Buffering，
 * 随后 demux seek 失败（实测 "Demuxing failed -1"）；子进程 + 裸 RGB 管道没有这个问题。
 *
 * 需要 ffmpeg.exe：优先 $FFMPEG_EXE，其次 PATH，最后 D:/ffmpeg/bin/ffmpeg.exe。
 */
#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>

class QProcess;

class H264LivePlayer : public QObject
{
    Q_OBJECT
public:
    explicit H264LivePlayer(QObject *parent = nullptr);
    ~H264LivePlayer() override;

    /* hasSpsPps：0x07 flags bit1。首个带参数集的关键帧到达时才启动 ffmpeg。 */
    void feed(const QByteArray &nal, bool hasSpsPps, int width, int height);

signals:
    void frameReady(const QImage &image);
    void errorChanged(const QString &msg);

private:
    void ensureStarted(int width, int height);
    void onReadyRead();
    void stopProc();

    QProcess   *m_proc = nullptr;
    QByteArray  m_out;
    QByteArray  m_err;
    int         m_w = 0;
    int         m_h = 0;
    int         m_frameBytes = 0;
    quint32     m_frames = 0;
    bool        m_started = false;
};
