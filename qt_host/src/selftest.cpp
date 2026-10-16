#include "selftest.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QTimer>

#include <chrono>
#include <cstdlib>
#include <thread>

#include "link_session.h"

static QJsonObject cmd(const char *c, const char *k = nullptr, const QJsonValue &v = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("cmd"), QString::fromLatin1(c));
    if (k) {
        o.insert(QString::fromLatin1(k), v);
    }
    return o;
}

int runSelfTest(const QStringList &args)
{
    QTextStream out(stdout);

    QString  iface = QStringLiteral("tcp");
    QString  host = QStringLiteral("192.168.33.2");
    quint16  port = 3333;
    QString  serialPort;
    double   seconds = 8.0;
    int      minImages = 20;
    QString  fmt = QStringLiteral("jpeg");
    int      fps = 10;
    int      quality = 75;
    bool     swap = false;
    QString  dumpPath;
    QString  grabRawPath;

    for (int i = 0; i < args.size(); i++) {
        const QString a = args.at(i);
        auto next = [&]() -> QString { return (i + 1 < args.size()) ? args.at(++i) : QString(); };
        if (a == QLatin1String("--tcp")) {
            iface = QStringLiteral("tcp");
            const QString v = next();
            if (!v.isEmpty()) {
                host = v.section(QLatin1Char(':'), 0, 0);
                const QString ps = v.section(QLatin1Char(':'), 1, 1);
                if (!ps.isEmpty()) port = quint16(ps.toUInt());
            }
        } else if (a == QLatin1String("--serial")) {
            iface = QStringLiteral("usb");
            serialPort = next();
        } else if (a == QLatin1String("--seconds")) {
            seconds = next().toDouble();
        } else if (a == QLatin1String("--images")) {
            minImages = next().toInt();
        } else if (a == QLatin1String("--fmt")) {
            fmt = next();
        } else if (a == QLatin1String("--fps")) {
            fps = next().toInt();
        } else if (a == QLatin1String("--quality")) {
            quality = next().toInt();
        } else if (a == QLatin1String("--swap")) {
            swap = true;
        } else if (a == QLatin1String("--dump")) {
            dumpPath = next();
        } else if (a == QLatin1String("--grab-raw")) {
            grabRawPath = next();
        }
    }

    LinkSession s;
    s.setSwapRawBytes(swap);

    int    images = 0, results = 0, logs = 0, decErrs = 0, blocks = 0;
    quint64 imgBytes = 0;
    int    maxSeq = -1;
    qint64 firstMs = 0, lastMs = 0;
    QString boardLink = QStringLiteral("-");
    QString lastAck;
    QImage  lastImage;
    int     lastW = 0, lastH = 0;

    QObject::connect(&s, &LinkSession::videoFrame, [&](const VideoFrame &f) {
        images++;
        imgBytes += quint64(f.bytes);
        maxSeq = qMax(maxSeq, f.seq);
        lastImage = f.image;
        lastW = f.width;
        lastH = f.height;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (images == 1) firstMs = now;
        lastMs = now;
    });
    QObject::connect(&s, &LinkSession::resultFrame, [&](const QJsonObject &o) {
        results++;
        if (o.contains(QStringLiteral("link"))) boardLink = o.value(QStringLiteral("link")).toString();
        const QString ack = o.value(QStringLiteral("ack")).toString();
        if (!ack.isEmpty()) {
            lastAck = QStringLiteral("%1: %2").arg(ack, o.value(QStringLiteral("msg")).toString());
            out << "  ack " << lastAck << "\n";
        }
    });
    QObject::connect(&s, &LinkSession::logLine, [&](const QString &) { logs++; });
    QObject::connect(&s, &LinkSession::decodeError, [&](const QString &e) {
        decErrs++;
        if (decErrs <= 3) out << "  decode error: " << e << "\n";
    });
    QObject::connect(&s, &LinkSession::sessionError, [&](const QString &e) {
        out << "  session: " << e << "\n";
    });
    QObject::connect(&s, &LinkSession::statsChanged, [&](const LinkSession::Stats &st) {
        blocks = int(st.rxBlock);
    });

    out << "=== qt_host selftest ===\n";
    out << "iface=" << iface << " " << (iface == QLatin1String("usb") ? serialPort : QStringLiteral("%1:%2").arg(host).arg(port))
        << "  fmt=" << fmt << " fps=" << fps << " q=" << quality
        << " seconds=" << seconds << " minImages=" << minImages << "\n";

    QString err;
    const bool ok = (iface == QLatin1String("usb")) ? s.openSerial(serialPort, &err)
                                                    : s.openTcp(host, port, &err);
    if (!ok) {
        out << "!! open failed: " << err << "\n";
        return 2;
    }
    out << "connected: " << s.describe() << "\n";

    s.sendCmd(cmd("link"));
    s.sendCmd(cmd("list"));

    /* ① 路线专项：raw 按需单帧抓取（脚本化验证；顺带可当命令行工具用） */
    if (!grabRawPath.isEmpty()) {
        QEventLoop loop;
        bool ok = false;
        int bytes = 0, w = 0, h = 0;
        QObject::connect(&s, &LinkSession::rawGrabDone,
                         [&](bool o, const QString &, int b, int ww, int hh) {
                             ok = o;
                             bytes = b;
                             w = ww;
                             h = hh;
                             loop.quit();
                         });
        s.startRawGrab(grabRawPath, 8000);
        QTimer::singleShot(9000, &loop, &QEventLoop::quit);
        loop.exec();
        out << QStringLiteral("grab-raw: ok=%1 %2x%3 data=%4B path=%5\n")
                   .arg(ok ? 1 : 0).arg(w).arg(h).arg(bytes).arg(grabRawPath);
        out << (ok ? QStringLiteral("SELFTEST PASS") : QStringLiteral("SELFTEST FAIL")) << "\n";
        out.flush();
        s.close();
        return ok ? 0 : 1;
    }

    QTimer::singleShot(250, [&]() {
        s.sendCmd(cmd("stream", "value", QStringLiteral("on")));
        QJsonObject o = cmd("stream");
        o.insert(QStringLiteral("value"), QStringLiteral("on"));
        o.insert(QStringLiteral("fps"), fps);
        o.insert(QStringLiteral("quality"), quality);
        o.insert(QStringLiteral("fmt"), fmt);
        s.sendCmd(o);
    });

    QTimer::singleShot(int(seconds * 1000.0), [&]() {
        const double dt = double(lastMs - firstMs) / 1000.0;
        const double eff = dt > 0.05 ? double(images - 1) / dt : 0.0;
        const double kbps = dt > 0.05 ? double(imgBytes) / 1024.0 / dt : 0.0;
        out << "--- result ---\n";
        out << QStringLiteral("images=%1  effective_fps=%2  KB/s=%3  img_bytes=%4\n")
                   .arg(images).arg(eff, 0, 'f', 2).arg(kbps, 0, 'f', 0).arg(imgBytes);
        out << QStringLiteral("results=%1  logs=%2  decodeErr=%3  blocks=%4  maxSeq=%5\n")
                   .arg(results).arg(logs).arg(decErrs).arg(blocks).arg(maxSeq);
        out << QStringLiteral("board_link=%1  last_ack=%2\n").arg(boardLink, lastAck);
        out << QStringLiteral("txBytes=%1  rxBytes=%2\n")
                   .arg(s.stats().txBytes).arg(s.stats().rxBytes);

        if (!lastImage.isNull()) {
            out << QStringLiteral("last frame: %1x%2 QImage=%3x%4\n")
                       .arg(lastW).arg(lastH).arg(lastImage.width()).arg(lastImage.height());
        }
        if (!dumpPath.isEmpty()) {
            if (!lastImage.isNull() && lastImage.save(dumpPath, "PNG")) {
                out << QStringLiteral("dumped last frame -> %1\n").arg(dumpPath);
            } else {
                out << QStringLiteral("!! dump failed: %1\n").arg(dumpPath);
            }
        }

        const bool pass = (images >= minImages) && (decErrs == 0);
        out << (pass ? QStringLiteral("SELFTEST PASS") : QStringLiteral("SELFTEST FAIL")) << "\n";
        out.flush();

        s.sendCmd(cmd("stream", "value", QStringLiteral("off")));
        QTimer::singleShot(300, [&]() {
            s.close();
            QCoreApplication::exit(pass ? 0 : 1);
        });
    });

    /* 硬看门狗（2026-10-16 实测踩坑）：自动化验收绝不允许悬挂——无论事件循环、
       串口驱动还是关闭路径出什么问题，到点直接 _Exit(4)。
       正常路径在 seconds 内自己退出，看不到这行。（曾因阻塞读不可取消把 COM 口
       连同 usbser.sys 一起卡死，进程挂 759s 不退、端口一直 ACCESS_DENIED。） */
    std::thread([seconds]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(int((seconds + 15.0) * 1000.0)));
        QTextStream out(stdout);
        out << "!! selftest hard timeout -> exit(4)\n";
        out.flush();
        std::_Exit(4);
    }).detach();

    return QCoreApplication::exec();
}
