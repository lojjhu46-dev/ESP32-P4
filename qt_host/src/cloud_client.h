#pragma once
/*
 * cloud_client.h —— 云签到 HTTP 客户端
 *
 * 只做三件事（云端不比对人脸）：
 *   · 上报 event=="match" 的签到事件（可带裁剪后的 JPEG，base64 放 JSON）
 *   · 上传/下载 JCDF 人脸库快照（字节级，0x06 导出/导入由 LinkSession 负责）
 *   · 心跳、健康检查
 *
 * 串行队列 + 超时重试；上报失败的事件落盘到 AppDataLocation/cloud_pending.jsonl，
 * 下次启动补发（服务器按 person+device 去重，重复发送是安全的）。
 */
#include <QByteArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QTimer>
#include <QUrl>

class CloudClient : public QObject
{
    Q_OBJECT
public:
    explicit CloudClient(QObject *parent = nullptr);

    void configure(const QUrl &base, const QString &token, const QString &device, bool withPhoto);
    /* 测试页使用独立落盘队列，避免测试事件补发到生产云 */
    void setSpoolName(const QString &name) { if (!name.isEmpty()) m_spoolName = name; }
    bool configured() const;
    QString baseUrl() const { return m_base.toString(); }
    QString deviceName() const { return m_device; }

    /* ev 已包含 ts/photo_b64 等字段；调用即入队并落盘。 */
    void sendEvent(const QJsonObject &ev);
    void pushSnapshot(const QByteArray &jcdf);
    void pullSnapshot();
    void heartbeat(int libN, const QString &fw, const QString &link);
    void probe();

    int pendingEvents() const;
    /* 服务器去重窗口（秒），来自 /api/health；0 = 还没探到，调用方用自己的默认 */
    int dedupWindowS() const { return m_dedupWindowS; }

signals:
    void stateChanged(const QString &text, bool isError);
    void eventSent(bool ok, int id, bool dup, const QString &msg);
    void snapshotPushed(bool ok, const QString &msg, int id);
    void snapshotPulled(bool ok, const QByteArray &data, const QString &msg);
    void probeFinished(bool ok, const QString &msg);

private:
    enum JobType { Event, Push, Pull, Heartbeat, Probe };
    struct Job {
        JobType type = Event;
        QJsonObject event;
        QByteArray blob;
        QString uid;
        int attempts = 0;
    };

    void enqueue(const Job &job);
    void pump();
    void handleReply(class QNetworkReply *reply, const Job &job);
    void retryLater(const Job &job, const QString &why);
    void failJob(const Job &job, const QString &msg);
    QNetworkRequest request(const QString &path, const QByteArray &ctype = QByteArray()) const;
    void emitState();

    QString spoolPath() const;
    void spoolAppend(const Job &job);
    void spoolRemove(const QString &uid);
    void loadSpool();

    QNetworkAccessManager *m_nam = nullptr;
    QUrl m_base;
    QString m_token;
    QString m_device;
    bool m_withPhoto = true;
    QQueue<Job> m_queue;
    bool m_busy = false;
    Job m_retryJob;
    QTimer *m_retry = nullptr;
    bool m_spoolLoaded = false;
    bool m_authFailed = false;
    QString m_spoolName = QStringLiteral("cloud_pending.jsonl");
    int m_dedupWindowS = 0;
};
