#include "pc_test_page.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QPixmap>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "cloud_client.h"
#include "link_session.h"

namespace {

QWidget *row(const QString &label, QWidget *w, int labelWidth = 72)
{
    auto *box = new QWidget;
    auto *h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto *l = new QLabel(label);
    l->setProperty("cls", QStringLiteral("dim"));
    l->setFixedWidth(labelWidth);
    h->addWidget(l);
    h->addWidget(w, 1);
    return box;
}

QPushButton *mkButton(const QString &text, const QString &cls = QString())
{
    auto *b = new QPushButton(text);
    if (!cls.isEmpty()) b->setProperty("cls", cls);
    return b;
}

const char *kDefaultFace =
    "D:/esp32project/ESP32-P4/esp-dl/examples/human_face_recognition/faces/bill2.jpg";

}  // namespace

PcTestPage::PcTestPage(LinkSession *link, QWidget *parent)
    : QWidget(parent), m_link(link)
{
    m_cloud = new CloudClient(this);
    m_cloud->setSpoolName(QStringLiteral("cloud_pending_pctest.jsonl"));
    buildUi();
    loadSettings();
    applyCloud();

    connect(m_link, &LinkSession::resultFrame, this, &PcTestPage::onResult);
    connect(m_link, &LinkSession::dbDone, this, &PcTestPage::onDbDone);
    connect(m_cloud, &CloudClient::stateChanged, this, [this](const QString &s, bool err) {
        if (err) setStatus(s, true);
    });
    connect(m_cloud, &CloudClient::eventSent, this,
            [this](bool ok, int id, bool dup, const QString &msg) {
                if (ok) {
                    log(QStringLiteral("☁ 测试事件已上报 #%1%2：%3")
                            .arg(id).arg(dup ? QStringLiteral("（重复）") : QString(), msg));
                    setStatus(QStringLiteral("测试事件已上报 #%1").arg(id));
                } else {
                    setStatus(QStringLiteral("测试事件上报失败：%1").arg(msg), true);
                }
            });
    connect(m_cloud, &CloudClient::snapshotPushed, this,
            [this](bool ok, const QString &msg, int) {
                if (ok) {
                    log(QStringLiteral("☁ 测试库快照已上传：%1").arg(msg));
                    setStatus(QStringLiteral("测试库快照已上传"));
                } else {
                    setStatus(QStringLiteral("测试库快照上传失败：%1").arg(msg), true);
                }
            });
    connect(m_cloud, &CloudClient::probeFinished, this, [this](bool ok, const QString &msg) {
        log((ok ? QStringLiteral("☁ ") : QStringLiteral("☁ ✗ ")) + msg);
        setStatus(msg, !ok);
    });
}

void PcTestPage::buildUi()
{
    auto *v = new QVBoxLayout(this);
    v->setSpacing(8);

    auto *intro = new QLabel(QStringLiteral(
        "仅测试固件（APP_FACEAI_PC_TEST=y）可用：进入测试库后板端切到 /storage/ftest.*，"
        "生产 face.db / personA·personB 不受影响；退出或重启即回生产库。"));
    intro->setWordWrap(true);
    intro->setProperty("cls", QStringLiteral("dim"));
    v->addWidget(intro);

    m_url = new QLineEdit;
    m_url->setPlaceholderText(QStringLiteral("http://127.0.0.1:18080"));
    m_token = new QLineEdit;
    m_token->setEchoMode(QLineEdit::Password);
    m_token->setPlaceholderText(QStringLiteral("测试云 Token（可留空只做本地识别）"));
    m_device = new QLineEdit;
    m_device->setPlaceholderText(QStringLiteral("测试设备名（如 pc-qt-test）"));
    m_testBtn = mkButton(QStringLiteral("保存并测试测试云"));
    v->addWidget(row(QStringLiteral("测试云"), m_url));
    v->addWidget(row(QStringLiteral("Token"), m_token));
    v->addWidget(row(QStringLiteral("设备名"), m_device));
    v->addWidget(m_testBtn);

    m_enterBtn = mkButton(QStringLiteral("进入测试库（reset）"), QStringLiteral("primary"));
    m_exitBtn = mkButton(QStringLiteral("退出测试库 / 回摄像头"));
    m_resetBtn = mkButton(QStringLiteral("清空测试库"));
    auto *libRow = new QHBoxLayout;
    libRow->addWidget(m_enterBtn);
    libRow->addWidget(m_exitBtn);
    libRow->addWidget(m_resetBtn);
    v->addLayout(libRow);
    m_status = new QLabel(QStringLiteral("未进入测试库"));
    m_status->setWordWrap(true);
    m_status->setProperty("cls", QStringLiteral("dim"));
    v->addWidget(m_status);

    m_facePath = new QLineEdit;
    m_browseBtn = mkButton(QStringLiteral("浏览"));
    auto *imgRow = new QHBoxLayout;
    imgRow->addWidget(m_facePath, 1);
    imgRow->addWidget(m_browseBtn);
    v->addLayout(imgRow);
    m_preview = new QLabel(QStringLiteral("未选择图片"));
    m_preview->setFixedHeight(150);
    m_preview->setAlignment(Qt::AlignCenter);
    m_preview->setProperty("cls", QStringLiteral("dim"));
    v->addWidget(m_preview);

    m_name = new QLineEdit;
    m_name->setPlaceholderText(QStringLiteral("测试人名（如 cloudtest）"));
    m_shots = new QSpinBox;
    m_shots->setRange(1, 5);
    m_shots->setValue(1);
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(m_name, 1);
    nameRow->addWidget(new QLabel(QStringLiteral("帧")));
    nameRow->addWidget(m_shots);
    v->addLayout(nameRow);

    m_regBtn = mkButton(QStringLiteral("注册当前图片"), QStringLiteral("primary"));
    m_recBtn = mkButton(QStringLiteral("反复送图识别"), QStringLiteral("primary"));
    m_snapBtn = mkButton(QStringLiteral("上传测试库快照到测试云"));
    v->addWidget(m_regBtn);
    v->addWidget(m_recBtn);
    v->addWidget(m_snapBtn);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(300);
    m_log->setFixedHeight(150);
    m_log->setProperty("cls", QStringLiteral("log"));
    v->addWidget(m_log);
    v->addStretch(1);

    m_tick = new QTimer(this);
    connect(m_tick, &QTimer::timeout, this, &PcTestPage::tick);

    connect(m_browseBtn, &QPushButton::clicked, this, &PcTestPage::browseImage);
    connect(m_enterBtn, &QPushButton::clicked, this, &PcTestPage::enterTestLib);
    connect(m_exitBtn, &QPushButton::clicked, this, &PcTestPage::exitTestLib);
    connect(m_resetBtn, &QPushButton::clicked, this, &PcTestPage::resetTestLib);
    connect(m_regBtn, &QPushButton::clicked, this, &PcTestPage::registerFace);
    connect(m_recBtn, &QPushButton::clicked, this, &PcTestPage::recognizeFace);
    connect(m_snapBtn, &QPushButton::clicked, this, &PcTestPage::uploadSnapshot);
    connect(m_testBtn, &QPushButton::clicked, this, &PcTestPage::testCloud);
    connect(m_url, &QLineEdit::editingFinished, this, &PcTestPage::applyCloud);
    connect(m_token, &QLineEdit::editingFinished, this, &PcTestPage::applyCloud);
    connect(m_device, &QLineEdit::editingFinished, this, &PcTestPage::applyCloud);
    connect(m_facePath, &QLineEdit::editingFinished, this, [this]() {
        loadImage(m_facePath->text().trimmed());
    });
}

void PcTestPage::loadSettings()
{
    QSettings s;
    m_url->setText(s.value(QStringLiteral("pctest/url"),
                           QStringLiteral("http://127.0.0.1:18080")).toString());
    m_token->setText(s.value(QStringLiteral("pctest/token")).toString());
    m_device->setText(s.value(QStringLiteral("pctest/device"),
                              QStringLiteral("pc-qt-test")).toString());
    m_name->setText(s.value(QStringLiteral("pctest/name"),
                            QStringLiteral("cloudtest")).toString());
    m_shots->setValue(s.value(QStringLiteral("pctest/shots"), 1).toInt());
    m_facePath->setText(s.value(QStringLiteral("pctest/face"),
                                QString::fromLatin1(kDefaultFace)).toString());
    loadImage(m_facePath->text().trimmed());
}

void PcTestPage::saveSettings()
{
    QSettings s;
    s.setValue(QStringLiteral("pctest/url"), m_url->text().trimmed());
    s.setValue(QStringLiteral("pctest/token"), m_token->text().trimmed());
    s.setValue(QStringLiteral("pctest/device"), m_device->text().trimmed());
    s.setValue(QStringLiteral("pctest/name"), m_name->text().trimmed());
    s.setValue(QStringLiteral("pctest/shots"), m_shots->value());
    s.setValue(QStringLiteral("pctest/face"), m_facePath->text().trimmed());
}

void PcTestPage::applyCloud()
{
    saveSettings();
    m_cloud->setSpoolName(QStringLiteral("cloud_pending_pctest.jsonl"));
    m_cloud->configure(QUrl(m_url->text().trimmed()), m_token->text().trimmed(),
                       m_device->text().trimmed(), true);
}

void PcTestPage::testCloud()
{
    applyCloud();
    setStatus(QStringLiteral("正在连接测试云…"));
    m_cloud->probe();
}

void PcTestPage::browseImage()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择测试人脸图"), QFileInfo(m_facePath->text()).absolutePath(),
        QStringLiteral("JPEG (*.jpg *.jpeg)"));
    if (path.isEmpty()) return;
    m_facePath->setText(path);
    loadImage(path);
}

void PcTestPage::loadImage(const QString &path)
{
    m_image.clear();
    if (path.isEmpty()) {
        m_preview->setText(QStringLiteral("未选择图片"));
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        m_preview->setText(QStringLiteral("读图失败"));
        return;
    }
    m_image = f.readAll();
    if (!m_image.startsWith(QByteArray("\xff\xd8\xff"))) {
        m_image.clear();
        m_preview->setText(QStringLiteral("不是 JPEG"));
        return;
    }
    QImage img;
    if (img.loadFromData(m_image)) {
        m_preview->setPixmap(QPixmap::fromImage(img).scaled(
            260, 150, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        m_preview->setText(QStringLiteral("JPEG 解码失败"));
    }
    log(QStringLiteral("图片：%1（%2 B）").arg(path).arg(m_image.size()));
}

bool PcTestPage::ensureTestMode()
{
    if (!m_link->isConnected()) {
        setStatus(QStringLiteral("请先连接板端"), true);
        return false;
    }
    if (!m_testMode) {
        setStatus(QStringLiteral("请先点「进入测试库」"), true);
        return false;
    }
    return true;
}

void PcTestPage::enterTestLib()
{
    if (!m_link->isConnected()) {
        setStatus(QStringLiteral("请先连接板端"), true);
        return;
    }
    setStatus(QStringLiteral("正在切换测试库…"));
    m_link->sendCmdWait(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("testlib")},
                                    {QStringLiteral("value"), QStringLiteral("on")},
                                    {QStringLiteral("reset"), true}},
                        [this](bool ok, const QJsonObject &reply) {
                            if (!ok) {
                                setStatus(QStringLiteral("testlib 无应答（是否测试固件？）"), true);
                                return;
                            }
                            if (reply.value(QStringLiteral("event")).toString()
                                    == QLatin1String("error")) {
                                setStatus(QStringLiteral("testlib 失败：%1")
                                              .arg(reply.value(QStringLiteral("msg")).toString()),
                                          true);
                                return;
                            }
                            if (reply.value(QStringLiteral("pctest")).toInt() != 1) {
                                setStatus(QStringLiteral("当前不是测试固件（APP_FACEAI_PC_TEST=y）"),
                                          true);
                                return;
                            }
                            m_testMode = true;
                            emit testModeChanged(true);
                            m_link->sendCmd(QJsonObject{{QStringLiteral("cmd"),
                                                         QStringLiteral("src")},
                                                        {QStringLiteral("value"),
                                                         QStringLiteral("pc")}});
                            m_link->sendCmd(QJsonObject{
                                {QStringLiteral("cmd"), QStringLiteral("mode")},
                                {QStringLiteral("thr_lo"), 0.61},
                                {QStringLiteral("thr_hi"), 0.61},
                                {QStringLiteral("fast_thr"), 0.80},
                                {QStringLiteral("dedup_ms"), 0}});
                            log(QStringLiteral("✓ 已进入测试库：%1")
                                    .arg(reply.value(QStringLiteral("msg")).toString()));
                            setStatus(QStringLiteral("测试库已就绪（db=%1）")
                                          .arg(reply.value(QStringLiteral("db")).toString()));
                        },
                        6000);
}

void PcTestPage::exitTestLib()
{
    m_tick->stop();
    m_registering = m_recognizing = false;
    if (m_link->isConnected()) {
        m_link->sendCmd(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("src")},
                                    {QStringLiteral("value"), QStringLiteral("cam")}});
        m_link->sendCmd(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("testlib")},
                                    {QStringLiteral("value"), QStringLiteral("off")}});
    }
    m_testMode = false;
    emit testModeChanged(false);
    log(QStringLiteral("已退出测试库，切回摄像头/生产库"));
    setStatus(QStringLiteral("未进入测试库"));
}

void PcTestPage::resetTestLib()
{
    if (!m_testMode) {
        enterTestLib();
        return;
    }
    m_tick->stop();
    m_registering = m_recognizing = false;
    m_link->sendCmdWait(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("testlib")},
                                    {QStringLiteral("value"), QStringLiteral("on")},
                                    {QStringLiteral("reset"), true}},
                        [this](bool ok, const QJsonObject &r) {
                            setStatus(ok ? r.value(QStringLiteral("msg")).toString()
                                         : QStringLiteral("清空测试库失败"),
                                      !ok);
                            log(QStringLiteral("测试库 reset：%1")
                                    .arg(r.value(QStringLiteral("msg")).toString()));
                        },
                        6000);
}

void PcTestPage::sendImage()
{
    if (!m_link->isConnected() || m_image.isEmpty()) return;
    m_link->sendJpeg(m_image);
    m_sendCount++;
    if (m_sendCount == 1 || m_sendCount % 5 == 0) {
        log(QStringLiteral("→ 送图 #%1").arg(m_sendCount));
    }
}

void PcTestPage::tick()
{
    if (!m_testMode || (!m_registering && !m_recognizing)) {
        m_tick->stop();
        return;
    }
    if (m_sendLeft <= 0) {
        m_tick->stop();
        if (m_recognizing && !m_matchUploaded) {
            setStatus(QStringLiteral("送图次数用尽，仍未 match"), true);
            log(QStringLiteral("✗ 识别送图结束，未 match"));
        }
        return;
    }
    sendImage();
    m_sendLeft--;
}

void PcTestPage::registerFace()
{
    if (!ensureTestMode()) return;
    if (m_image.isEmpty()) {
        setStatus(QStringLiteral("先选择一张 JPEG"), true);
        return;
    }
    const QString name = m_name->text().trimmed();
    if (name.isEmpty()) {
        setStatus(QStringLiteral("先填测试人名"), true);
        return;
    }
    saveSettings();
    QJsonObject o{{QStringLiteral("cmd"), QStringLiteral("register")},
                  {QStringLiteral("id"), name},
                  {QStringLiteral("shots"), m_shots->value()}};
    m_link->sendCmdWait(o, [this, name](bool ok, const QJsonObject &r) {
        if (!ok || r.value(QStringLiteral("event")).toString() == QLatin1String("error")) {
            setStatus(QStringLiteral("注册命令失败：%1")
                          .arg(r.value(QStringLiteral("msg")).toString()),
                      true);
            return;
        }
        log(QStringLiteral("→ 注册 %1：%2")
                .arg(name, r.value(QStringLiteral("msg")).toString()));
        m_registering = true;
        m_recognizing = false;
        m_matchUploaded = false;
        m_sendLeft = 10;
        m_sendCount = 0;
        sendImage();
        m_tick->start(500);
    }, 5000);
}

void PcTestPage::recognizeFace()
{
    if (!ensureTestMode()) return;
    if (m_image.isEmpty()) {
        setStatus(QStringLiteral("先选择一张 JPEG"), true);
        return;
    }
    m_registering = false;
    m_recognizing = true;
    m_matchUploaded = false;
    m_sendLeft = 15;
    m_sendCount = 0;
    sendImage();
    m_tick->start(400);
    log(QStringLiteral("→ 开始反复送图识别（15 帧）"));
    setStatus(QStringLiteral("识别中…"));
}

void PcTestPage::onResult(const QJsonObject &obj)
{
    if (!m_testMode) return;
    const QString ev = obj.value(QStringLiteral("event")).toString();
    const QString nm = obj.value(QStringLiteral("match")).toString();
    if (ev == QLatin1String("registered")) {
        m_registering = false;
        m_tick->stop();
        log(QStringLiteral("✓ 注册完成：%1")
                .arg(obj.value(QStringLiteral("msg")).toString()));
        setStatus(QStringLiteral("注册完成，可开始识别"));
        if (!m_token->text().trimmed().isEmpty()) {
            QTimer::singleShot(900, this, [this]() { uploadSnapshot(); });
        }
    } else if (ev == QLatin1String("enrolling")) {
        log(QStringLiteral("  采集中：%1").arg(obj.value(QStringLiteral("msg")).toString()));
    } else if (ev == QLatin1String("match")) {
        log(QStringLiteral("✓ match=%1 sim=%2 faces=%3")
                .arg(nm)
                .arg(obj.value(QStringLiteral("sim")).toDouble(), 0, 'f', 3)
                .arg(obj.value(QStringLiteral("faces")).toInt()));
        if (m_recognizing && !m_matchUploaded && !nm.isEmpty()) {
            m_recognizing = false;
            m_tick->stop();
            uploadMatch(obj);
        }
    } else if (ev == QLatin1String("error")) {
        if (m_registering || m_recognizing) {
            m_registering = m_recognizing = false;
            m_tick->stop();
            setStatus(QStringLiteral("板端错误：%1")
                          .arg(obj.value(QStringLiteral("msg")).toString()),
                      true);
        }
    }
}

void PcTestPage::uploadMatch(const QJsonObject &obj)
{
    applyCloud();
    if (m_token->text().trimmed().isEmpty()) {
        log(QStringLiteral("未填测试云 Token：仅完成本地识别"));
        setStatus(QStringLiteral("match（未配置测试云，未上报）"));
        return;
    }
    if (m_image.isEmpty()) return;
    m_matchUploaded = true;

    QJsonObject ev;
    ev.insert(QStringLiteral("event"), QStringLiteral("match"));
    ev.insert(QStringLiteral("device"), m_device->text().trimmed());
    ev.insert(QStringLiteral("person"), obj.value(QStringLiteral("match")).toString());
    ev.insert(QStringLiteral("sim"), obj.value(QStringLiteral("sim")));
    ev.insert(QStringLiteral("sim_raw"), obj.value(QStringLiteral("sim_raw")));
    ev.insert(QStringLiteral("stable"), obj.value(QStringLiteral("stable")));
    ev.insert(QStringLiteral("faces"), obj.value(QStringLiteral("faces")));
    ev.insert(QStringLiteral("src"), QStringLiteral("pc"));
    ev.insert(QStringLiteral("link"), m_link->stats().link);
    ev.insert(QStringLiteral("fps"), obj.value(QStringLiteral("fps")));
    ev.insert(QStringLiteral("infer_ms"), obj.value(QStringLiteral("infer_ms")));
    ev.insert(QStringLiteral("lib_n"), obj.value(QStringLiteral("lib_n")));
    ev.insert(QStringLiteral("ts"), QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    ev.insert(QStringLiteral("photo_b64"), QString::fromLatin1(m_image.toBase64()));
    m_cloud->sendEvent(ev);
    log(QStringLiteral("☁ 用送出的原图上报测试签到（%1 B）").arg(m_image.size()));
}

void PcTestPage::uploadSnapshot()
{
    if (!m_testMode || !m_link->isConnected()) return;
    if (m_link->dbBusy()) {
        setStatus(QStringLiteral("板端库任务忙，稍后再试"), true);
        return;
    }
    applyCloud();
    if (m_token->text().trimmed().isEmpty()) {
        log(QStringLiteral("未填测试云 Token：跳过快照上传"));
        return;
    }
    m_exportPath = tempBin();
    QFile::remove(m_exportPath);
    log(QStringLiteral("→ 导出测试库快照…"));
    setStatus(QStringLiteral("导出测试库快照…"));
    m_link->dbExport(m_exportPath);
}

QString PcTestPage::tempBin() const
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
        .filePath(QStringLiteral("facecheck_pctest.bin"));
}

void PcTestPage::onDbDone(bool ok, const QString &msg)
{
    if (m_exportPath.isEmpty()) return;
    const QString path = m_exportPath;
    m_exportPath.clear();
    if (!ok) {
        setStatus(QStringLiteral("测试库导出失败：%1").arg(msg), true);
        return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setStatus(QStringLiteral("读测试库快照失败"), true);
        return;
    }
    const QByteArray data = f.readAll();
    f.close();
    QFile::remove(path);
    if (data.isEmpty()) {
        setStatus(QStringLiteral("测试库快照为空"), true);
        return;
    }
    log(QStringLiteral("☁ 上传测试库快照（%1 B）").arg(data.size()));
    m_cloud->pushSnapshot(data);
}

void PcTestPage::log(const QString &s)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), s));
}

void PcTestPage::setStatus(const QString &s, bool err)
{
    m_status->setText(s);
    m_status->setStyleSheet(err ? QStringLiteral("color:#FF5C5C")
                                : QStringLiteral("color:#8A93A3"));
}
