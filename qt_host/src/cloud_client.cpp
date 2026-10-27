#include "cloud_client.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {

quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool init = false;
    if (!init) {
        for (quint32 i = 0; i < 256; i++) {
            quint32 c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        init = true;
    }
    quint32 c = 0xFFFFFFFFu;
    for (int i = 0; i < data.size(); i++) {
        c = table[(c ^ quint8(data.at(i))) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

QString jsonString(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toString();
}

int jsonInt(const QJsonObject &obj, const char *key, int fallback = -1)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    return v.isDouble() ? v.toInt() : fallback;
}

}  // namespace

CloudClient::CloudClient(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, [this]() {
        m_queue.prepend(m_retryJob);
        pump();
    });
}

void CloudClient::configure(const QUrl &base, const QString &token, const QString &device,
                            bool withPhoto)
{
    QString text = base.toString().trimmed();
    if (text.isEmpty()) {
        m_base = QUrl();
    } else if (!text.contains(QStringLiteral("://"))) {
        m_base = QUrl(QStringLiteral("http://") + text);
    } else {
        m_base = base;
    }
    if (m_base.isValid()) {
        QString path = m_base.path();
        while (path.endsWith(QLatin1Char('/'))) path.chop(1);
        m_base.setPath(path);
    }
    m_token = token.trimmed();
    m_device = device.trimmed().isEmpty() ? QStringLiteral("qt_host") : device.trimmed();
    m_withPhoto = withPhoto;
    m_authFailed = false;
    m_spoolLoaded = false;
    m_dedupWindowS = 0;         /* 换服务器要重新探活才肯用它的窗口 */
    m_retry->stop();
    loadSpool();
    pump();
}

bool CloudClient::configured() const
{
    return m_base.isValid() && !m_base.host().isEmpty() && !m_token.isEmpty();
}

int CloudClient::pendingEvents() const
{
    int n = m_busy ? 1 : 0;
    for (const Job &job : m_queue) {
        if (job.type == Event) n++;
    }
    return n;
}

QString CloudClient::spoolPath() const
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty()) dir = QDir::tempPath();
    QDir().mkpath(dir);
    return QDir(dir).filePath(m_spoolName);
}

void CloudClient::spoolAppend(const Job &job)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("uid"), job.uid);
    obj.insert(QStringLiteral("event"), job.event);
    QFile file(spoolPath());
    if (!file.open(QIODevice::Append)) {
        emit stateChanged(QStringLiteral("待发队列落盘失败：%1").arg(file.errorString()), true);
        return;
    }
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    file.write("\n");
}

void CloudClient::spoolRemove(const QString &uid)
{
    if (uid.isEmpty()) return;
    QFile in(spoolPath());
    if (!in.open(QIODevice::ReadOnly)) return;
    QList<QByteArray> keep;
    bool changed = false;
    while (!in.atEnd()) {
        const QByteArray line = in.readLine();
        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (obj.value(QStringLiteral("uid")).toString() == uid) {
            changed = true;
            continue;
        }
        if (!line.trimmed().isEmpty()) keep.append(line);
    }
    in.close();
    if (!changed) return;
    QSaveFile out(spoolPath());
    if (!out.open(QIODevice::WriteOnly)) return;
    for (const QByteArray &line : keep) {
        out.write(line);
        if (!line.endsWith('\n')) out.write("\n");
    }
    out.commit();
}

void CloudClient::loadSpool()
{
    if (m_spoolLoaded) return;
    m_spoolLoaded = true;
    QFile file(spoolPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    int loaded = 0;
    while (!file.atEnd()) {
        const QJsonObject obj = QJsonDocument::fromJson(file.readLine()).object();
        const QString uid = obj.value(QStringLiteral("uid")).toString();
        const QJsonObject ev = obj.value(QStringLiteral("event")).toObject();
        if (uid.isEmpty() || ev.isEmpty()) continue;
        Job job;
        job.type = Event;
        job.uid = uid;
        job.event = ev;
        m_queue.enqueue(job);
        loaded++;
    }
    if (loaded > 0) {
        emit stateChanged(QStringLiteral("补发上次未上报的 %1 条签到").arg(loaded), false);
    }
}

void CloudClient::sendEvent(const QJsonObject &ev)
{
    if (!configured()) return;
    QJsonObject payload = ev;
    if (!m_withPhoto) payload.remove(QStringLiteral("photo_b64"));
    Job job;
    job.type = Event;
    job.event = payload;
    job.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);

    int events = 0;
    for (const Job &queued : m_queue) {
        if (queued.type == Event) events++;
    }
    if (events >= 200) {
        for (int i = 0; i < m_queue.size(); i++) {
            if (m_queue.at(i).type == Event) {
                spoolRemove(m_queue.at(i).uid);
                m_queue.removeAt(i);
                emit stateChanged(QStringLiteral("待发队列已满，丢弃最旧一条"), true);
                break;
            }
        }
    }
    spoolAppend(job);
    enqueue(job);
}

void CloudClient::pushSnapshot(const QByteArray &jcdf)
{
    if (!configured() || jcdf.isEmpty()) return;
    Job job;
    job.type = Push;
    job.blob = jcdf;
    enqueue(job);
}

void CloudClient::pullSnapshot()
{
    if (!configured()) return;
    Job job;
    job.type = Pull;
    enqueue(job);
}

void CloudClient::heartbeat(int libN, const QString &fw, const QString &link)
{
    if (!configured()) return;
    QJsonObject ev;
    ev.insert(QStringLiteral("device"), m_device);
    ev.insert(QStringLiteral("lib_n"), libN);
    ev.insert(QStringLiteral("fw"), fw);
    ev.insert(QStringLiteral("link"), link);
    Job job;
    job.type = Heartbeat;
    job.event = ev;
    enqueue(job);
}

void CloudClient::probe()
{
    if (!configured()) {
        emit probeFinished(false, QStringLiteral("请先填写服务器地址与 Token"));
        return;
    }
    Job job;
    job.type = Probe;
    enqueue(job);
}

void CloudClient::enqueue(const Job &job)
{
    m_queue.enqueue(job);
    emit stateChanged(QStringLiteral("云签到待发 %1 条").arg(m_queue.size()), false);
    pump();
}

QNetworkRequest CloudClient::request(const QString &path, const QByteArray &ctype) const
{
    QUrl url = m_base;
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qt_host/facecheck"));
    req.setRawHeader("X-Token", m_token.toUtf8());
    if (!ctype.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromLatin1(ctype));
    }
    req.setTransferTimeout(10000);
    return req;
}

void CloudClient::pump()
{
    if (m_busy || m_authFailed || m_queue.isEmpty() || !configured()) return;
    const Job job = m_queue.dequeue();
    m_busy = true;
    QNetworkReply *reply = nullptr;
    switch (job.type) {
    case Event: {
        QJsonDocument doc(job.event);
        reply = m_nam->post(request(QStringLiteral("/api/events"),
                                    QByteArrayLiteral("application/json")),
                            doc.toJson(QJsonDocument::Compact));
        break;
    }
    case Push: {
        QNetworkRequest req = request(QStringLiteral("/api/facebank"),
                                      QByteArrayLiteral("application/octet-stream"));
        req.setRawHeader("X-Device", m_device.toUtf8());
        req.setRawHeader("X-CRC32", QByteArray::number(qint64(crc32(job.blob))));
        reply = m_nam->post(req, job.blob);
        break;
    }
    case Pull:
        reply = m_nam->get(request(QStringLiteral("/api/facebank/latest")));
        break;
    case Heartbeat: {
        QJsonDocument doc(job.event);
        reply = m_nam->post(request(QStringLiteral("/api/heartbeat"),
                                    QByteArrayLiteral("application/json")),
                            doc.toJson(QJsonDocument::Compact));
        break;
    }
    case Probe:
        reply = m_nam->get(request(QStringLiteral("/api/health")));
        break;
    }
    if (!reply) {
        m_busy = false;
        retryLater(job, QStringLiteral("请求创建失败"));
        return;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, job]() {
        handleReply(reply, job);
    });
}

void CloudClient::handleReply(QNetworkReply *reply, const Job &job)
{
    m_busy = false;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body = reply->readAll();
    const QByteArray crcHeader = reply->rawHeader("X-CRC32");
    const QString netErr = reply->errorString();
    const bool ok2xx = status >= 200 && status < 300;
    reply->deleteLater();

    QJsonObject js;
    if (!body.isEmpty() && body.at(0) == '{') {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) js = doc.object();
    }
    const QString errMsg = js.value(QStringLiteral("error")).toString();
    const QString detail = errMsg.isEmpty()
                               ? (status > 0 ? QStringLiteral("HTTP %1").arg(status) : netErr)
                               : errMsg;

    if (job.type == Event) {
        if (ok2xx) {
            spoolRemove(job.uid);
            const int id = jsonInt(js, "id");
            const bool dup = js.value(QStringLiteral("dup")).toBool();
            emit eventSent(true, id, dup,
                           QStringLiteral("%1 (%2)")
                               .arg(jsonString(job.event, "person"))
                               .arg(job.event.value(QStringLiteral("sim")).toDouble(), 0, 'f', 3));
            emit stateChanged(QStringLiteral("云签到待发 %1 条").arg(m_queue.size()), false);
            pump();
            return;
        }
        if (status == 401 || status == 403) {
            m_authFailed = true;
            emit stateChanged(QStringLiteral("云签到 Token 无效（待发事件已保留）"), true);
            emit eventSent(false, -1, false, QStringLiteral("Token 无效"));
            return;
        }
        if (status >= 400 && status < 500 && status != 408 && status != 429) {
            spoolRemove(job.uid);
            emit stateChanged(QStringLiteral("云签到事件被拒：%1").arg(detail), true);
            emit eventSent(false, -1, false, detail);
            pump();
            return;
        }
        retryLater(job, detail);
        return;
    }

    if (job.type == Push) {
        if (ok2xx) {
            const int id = jsonInt(js, "id");
            emit snapshotPushed(true,
                                QStringLiteral("云端版本 #%1 · %2 人 / %3 特征")
                                    .arg(id)
                                    .arg(jsonInt(js, "persons", 0))
                                    .arg(jsonInt(js, "feats", 0)),
                                id);
            emit stateChanged(QStringLiteral("云签到待发 %1 条").arg(m_queue.size()), false);
            pump();
            return;
        }
        if (status == 401 || status == 403) {
            m_authFailed = true;
            emit stateChanged(QStringLiteral("云签到 Token 无效"), true);
            emit snapshotPushed(false, QStringLiteral("Token 无效"), -1);
            return;
        }
        if (status >= 400 && status < 500 && status != 408 && status != 429) {
            emit snapshotPushed(false, detail, -1);
            pump();
            return;
        }
        retryLater(job, detail);
        return;
    }

    if (job.type == Pull) {
        if (ok2xx && !body.isEmpty()) {
            if (!crcHeader.isEmpty() && crcHeader.toUInt() != crc32(body)) {
                emit snapshotPulled(false, QByteArray(), QStringLiteral("云端库 CRC 校验失败"));
                pump();
                return;
            }
            emit snapshotPulled(true, body,
                                QStringLiteral("云端库 %1 KB · CRC %2")
                                    .arg(body.size() / 1024)
                                    .arg(QString::number(crc32(body), 16)));
            emit stateChanged(QStringLiteral("云签到待发 %1 条").arg(m_queue.size()), false);
            pump();
            return;
        }
        if (status == 401 || status == 403) {
            m_authFailed = true;
            emit stateChanged(QStringLiteral("云签到 Token 无效"), true);
            emit snapshotPulled(false, QByteArray(), QStringLiteral("Token 无效"));
            return;
        }
        if (status == 404) {
            emit snapshotPulled(false, QByteArray(), QStringLiteral("云端还没有人脸库快照"));
            pump();
            return;
        }
        if (status >= 400 && status < 500 && status != 408 && status != 429) {
            emit snapshotPulled(false, QByteArray(), detail);
            pump();
            return;
        }
        retryLater(job, detail);
        return;
    }

    if (job.type == Probe) {
        if (ok2xx) {
            const int win = js.value(QStringLiteral("dedup_window_s")).toInt();
            if (win > 0) {
                m_dedupWindowS = win;
            }
            emit probeFinished(true,
                               QStringLiteral("连接正常（服务版本 %1）")
                                   .arg(js.value(QStringLiteral("version")).toString()));
        } else {
            if (status == 401 || status == 403) m_authFailed = true;
            emit probeFinished(false, detail);
        }
        pump();
        return;
    }

    // Heartbeat: 失败不阻塞后续事件；401 则暂停队列。
    if (status == 401 || status == 403) {
        m_authFailed = true;
        emit stateChanged(QStringLiteral("云签到 Token 无效"), true);
    } else if (!ok2xx && status != 0) {
        emit stateChanged(QStringLiteral("心跳失败：%1").arg(detail), true);
    }
    pump();
}

void CloudClient::retryLater(const Job &job, const QString &why)
{
    Job next = job;
    next.attempts = job.attempts + 1;
    const int maxAttempts = (next.type == Event) ? 999 : 3;
    if (next.attempts >= maxAttempts) {
        failJob(next, why);
        pump();
        return;
    }
    static const int delays[] = {2000, 5000, 15000, 60000, 300000};
    const int idx = qMin(next.attempts - 1, 4);
    const int delay = delays[idx];
    m_retryJob = next;
    emit stateChanged(QStringLiteral("%1，%2 秒后重试（待发 %3）")
                         .arg(why)
                         .arg(delay / 1000)
                         .arg(m_queue.size() + 1),
                     true);
    m_retry->start(delay);
}

void CloudClient::failJob(const Job &job, const QString &msg)
{
    switch (job.type) {
    case Event:
        emit eventSent(false, -1, false, msg);
        emit stateChanged(QStringLiteral("云签到事件未送达，已保留待下次补发：%1").arg(msg), true);
        break;
    case Push:
        emit snapshotPushed(false, msg, -1);
        break;
    case Pull:
        emit snapshotPulled(false, QByteArray(), msg);
        break;
    case Probe:
        emit probeFinished(false, msg);
        break;
    case Heartbeat:
        emit stateChanged(QStringLiteral("心跳失败：%1").arg(msg), true);
        break;
    }
}
