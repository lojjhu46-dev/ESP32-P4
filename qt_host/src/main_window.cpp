#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QHostInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include <utility>

#include "cloud_client.h"
#ifdef QT_HOST_PC_TEST
#include "pc_test_page.h"
#endif
#include "link_session.h"
#include "serial_transport.h"
#include "video_view.h"

/* ---------------- 小工具 ---------------- */

static QJsonObject cmdObj(const QString &c)
{
    QJsonObject o;
    o.insert(QStringLiteral("cmd"), c);
    return o;
}

static QJsonObject cmdObj(const QString &c, const QString &k, const QJsonValue &v)
{
    QJsonObject o = cmdObj(c);
    o.insert(k, v);
    return o;
}

static QLabel *dimLabel(const QString &t)
{
    auto *l = new QLabel(t);
    l->setProperty("cls", QStringLiteral("dim"));
    return l;
}

/* 窄面板下必须自动换行，否则长提示会被右边缘裁掉 */
static QLabel *hintLabel(const QString &t)
{
    auto *l = dimLabel(t);
    l->setWordWrap(true);
    return l;
}

static QWidget *formRow(const QString &label, QWidget *w, int labelWidth = 58)
{
    auto *box = new QWidget;
    auto *h = new QHBoxLayout(box);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto *l = dimLabel(label);
    l->setFixedWidth(labelWidth);
    h->addWidget(l);
    h->addWidget(w, 1);
    return box;
}

/* 套滚动区域：窄/矮窗口下换行后的提示不会被面板底边裁掉 */
static QWidget *scrollPage(QWidget *page)
{
    auto *sa = new QScrollArea;
    sa->setWidget(page);
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    return sa;
}

static QPushButton *button(const QString &text, const QString &cls = QString())
{
    auto *b = new QPushButton(text);
    if (!cls.isEmpty()) {
        b->setProperty("cls", cls);
    }
    return b;
}

/* ---------------- 构造 ---------------- */

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    m_link = new LinkSession(this);
    m_cloud = new CloudClient(this);

    setWindowTitle(QStringLiteral("JCLINK 上位机 · ESP32-P4 边缘人脸识别终端"));
    resize(1380, 860);

    buildUi();
    refreshPorts();
    updateIfaceWidgets();

    /* ---- 云签到：信号 + 启动配置（buildCloudTab 已从 QSettings 回填控件） ---- */
    connect(m_cloud, &CloudClient::stateChanged, this, [this](const QString &text, bool err) {
        m_cloudState->setText(text);
        m_cloudState->setStyleSheet(err ? QStringLiteral("color:#FF5C5C")
                                        : QStringLiteral("color:#8A93A3"));
    });
    connect(m_cloud, &CloudClient::eventSent, this,
            [this](bool ok, int id, bool dup, const QString &msg) {
                if (!ok) {
                    statusError(QStringLiteral("云签到上报失败：%1").arg(msg));
                    return;
                }
                appendLog(dup ? QStringLiteral("☁ 云端已有该签到（去重）：%1").arg(msg)
                              : QStringLiteral("☁ 签到已上报：%1 · #%2").arg(msg).arg(id));
                m_cloudLast->setText(QStringLiteral("最近上报：%1").arg(msg));
            });
    connect(m_cloud, &CloudClient::snapshotPushed, this,
            [this](bool ok, const QString &msg, int) {
                if (ok) {
                    appendLog(QStringLiteral("☁ 人脸库已上传：%1").arg(msg));
                    m_cloudState->setText(msg);
                    m_cloudState->setStyleSheet(QStringLiteral("color:#8A93A3"));
                } else {
                    statusError(QStringLiteral("云签到人脸库上传失败：%1").arg(msg));
                }
            });
    connect(m_cloud, &CloudClient::snapshotPulled, this,
            [this](bool ok, const QByteArray &data, const QString &msg) {
                if (!ok) {
                    statusError(QStringLiteral("云签到人脸库下载失败：%1").arg(msg));
                    return;
                }
                const QString path =
                    QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                        .filePath(QStringLiteral("facecheck_cloud.bin"));
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly)) {
                    statusError(QStringLiteral("云签到：写入临时文件失败 %1").arg(file.errorString()));
                    return;
                }
                file.write(data);
                file.close();
                m_cloudImportPath = path;
                appendLog(QStringLiteral("☁ %1，开始导入板端…").arg(msg));
                m_link->dbImport(path);
            });
    connect(m_cloud, &CloudClient::probeFinished, this, [this](bool ok, const QString &msg) {
        appendLog((ok ? QStringLiteral("☁ ") : QStringLiteral("☁ ✗ ")) + msg);
        m_cloudState->setText(msg);
        m_cloudState->setStyleSheet(ok ? QStringLiteral("color:#3DDC84")
                                       : QStringLiteral("color:#FF5C5C"));
    });
    applyCloudConfig(true);

    auto *cloudHb = new QTimer(this);
    connect(cloudHb, &QTimer::timeout, this, [this]() {
        if (!m_cloudOn->isChecked() || !m_cloud->configured()) return;
        m_cloud->heartbeat(m_lastLibN, QStringLiteral("p4-h264"),
                           m_link->isConnected() ? m_link->stats().link : QString());
    });
    cloudHb->start(60000);

    /* ---- 链路信号 ---- */
    connect(m_link, &LinkSession::connected, this, [this](const QString &desc) {
        m_sbDesc->setText(desc);
        m_connBtn->setText(QStringLiteral("断开"));
        appendLog(QStringLiteral("✓ 已连接：%1").arg(desc));
        setStreamUi(false);
        /* 连接即同步板端状态（与取景中枢「连上就同步」的做法一致） */
        cmd(cmdObj(QStringLiteral("mode")));
        cmd(cmdObj(QStringLiteral("cam")));
        cmd(cmdObj(QStringLiteral("stream")));
        cmd(cmdObj(QStringLiteral("link")));
        cmd(cmdObj(QStringLiteral("list")));
        if (m_mirrorChk && m_vflipChk) {
            QTimer::singleShot(800, this, [this]() {
                if (!m_link->isConnected()) return;
                QJsonObject o = cmdObj(QStringLiteral("cam"));
                o.insert(QStringLiteral("hflip"), m_mirrorChk->isChecked());
                o.insert(QStringLiteral("vflip"), m_vflipChk->isChecked());
                cmd(o);
            });
        }
        if (m_cloudOn->isChecked() && m_cloud->configured()) {
            QTimer::singleShot(1200, this, [this]() {
                if (m_link->isConnected()) {
                    m_cloud->heartbeat(m_lastLibN, QStringLiteral("p4-h264"),
                                       m_link->stats().link);
                }
            });
        }
    });

    connect(m_link, &LinkSession::disconnected, this, [this]() {
        m_sbDesc->setText(QStringLiteral("未连接"));
        m_connBtn->setText(QStringLiteral("连接"));
        m_linkBadge->setText(QStringLiteral("离线"));
        m_linkBadge->setProperty("state", QStringLiteral("off"));
        m_linkBadge->style()->polish(m_linkBadge);
        m_view->clear();
        appendLog(QStringLiteral("— 已断开"));
    });

    connect(m_link, &LinkSession::resultFrame, this, &MainWindow::onResultFrame);
    connect(m_link, &LinkSession::videoFrame, this, [this](const VideoFrame &f) {
        m_view->setFrame(f);
        if (!f.image.isNull()) {
            m_lastFrame = f.image;   /* QImage 隐式共享；签到抓拍只读这一帧 */
            m_lastFrameMs = QDateTime::currentMSecsSinceEpoch();
        }
    });
    connect(m_link, &LinkSession::logLine, this, [this](const QString &l) {
        m_sbLog->setText(l);
        /* 板端每秒刷十几行 imgpipe 统计/画像行，会把日志环形缓冲（2000 行）
           在两三分钟内冲干净，出问题时反而查不到证据 —— 这类周期行只进状态栏 */
        if (l.startsWith(QStringLiteral("imgpipe"))) {
            return;
        }
        appendLog(QStringLiteral("[板端] %1").arg(l));
    });
    connect(m_link, &LinkSession::sessionError, this, &MainWindow::statusError);
    connect(m_link, &LinkSession::decodeError, this, [this](const QString &e) {
        m_sbLog->setText(QStringLiteral("解码失败：%1").arg(e));
    });

    connect(m_link, &LinkSession::statsChanged, this, [this](const LinkSession::Stats &st) {
        m_sbTraffic->setText(QStringLiteral("RX %1 KB · TX %2 KB")
                                 .arg(st.rxBytes / 1024).arg(st.txBytes / 1024));
        m_sbFps->setText(QStringLiteral("%1 %2 fps · %3 KB/s")
                             .arg(st.lastFmt)
                             .arg(st.imgFps, 0, 'f', 1)
                             .arg(st.imgKbps, 0, 'f', 0));
        m_sbDrop->setText(QStringLiteral("丢帧 %1").arg(st.imgDrop));
        /* 角标带上实际格式：一眼看出"UI 选 JPEG 但板子在推 raw"这类不一致 */
        m_view->setStatsText(QStringLiteral("%1 %2 fps  %3 KB/s  丢%4  %5")
                                 .arg(st.lastFmt,
                                      QString::number(st.imgFps, 'f', 1),
                                      QString::number(st.imgKbps, 'f', 0))
                                 .arg(st.imgDrop)
                                 .arg(st.link));
        if (!st.link.isEmpty() && st.link != QLatin1String("-")) {
            m_linkBadge->setText(st.link);
            m_linkBadge->setProperty("state", st.link == QLatin1String("tcp") ? QStringLiteral("tcp")
                                                                              : QStringLiteral("usb"));
            m_linkBadge->style()->polish(m_linkBadge);
            m_sbLink->setText(QStringLiteral("链路 %1").arg(st.link));
        }
    });

    connect(m_link, &LinkSession::dbProgress, this, [this](int pct, const QString &text) {
        m_dbBar->setValue(pct);
        m_dbLabel->setText(text);
    });
    connect(m_link, &LinkSession::rawGrabDone, this,
            [this](bool ok, const QString &path, int bytes, int w, int h) {
                if (ok) {
                    appendLog(QStringLiteral("✓ raw 单帧已保存：%1（%2x%3, %4 B）")
                                  .arg(path).arg(w).arg(h).arg(bytes));
                    m_sbLog->setText(QStringLiteral("raw 单帧已保存：%1").arg(path));
                } else {
                    statusError(QStringLiteral("raw 单帧抓取失败（超时或未连接）：%1").arg(path));
                }
                /* 抓帧会把板端 stream 临时切到 raw 且关流 —— 若之前在看预览，用 UI 当前
                   的 fmt/fps/quality 自动恢复（否则会出现"UI 是 JPEG、板子还在推 raw"的假低帧率）。 */
                if (m_streamBeforeGrab && m_link->isConnected()) {
                    m_streamBeforeGrab = false;
                    QJsonObject o = cmdObj(QStringLiteral("stream"), QStringLiteral("value"),
                                           QStringLiteral("on"));
                    o.insert(QStringLiteral("fps"), m_fpsSpin->value());
                    o.insert(QStringLiteral("quality"), m_qSpin->value());
                    o.insert(QStringLiteral("fmt"), m_fmtBox->currentData().toString());
                    cmd(o);
                    appendLog(QStringLiteral("↺ 已恢复预览流（%1 %2fps）")
                                  .arg(m_fmtBox->currentData().toString())
                                  .arg(m_fpsSpin->value()));
                }
                m_streamBeforeGrab = false;
            });
    connect(m_link, &LinkSession::dbDone, this, [this](bool ok, const QString &msg) {
        m_dbBar->setValue(ok ? 100 : m_dbBar->value());
        m_dbLabel->setText((ok ? QStringLiteral("✓ ") : QStringLiteral("✗ ")) + msg);
        appendLog((ok ? QStringLiteral("✓ ") : QStringLiteral("✗ ")) + msg);
        m_exportBtn->setEnabled(true);
        m_importBtn->setEnabled(true);

        const bool wantUpload = m_dbUploadAfterExport;
        const bool wasAuto = m_dbAutoUp;
        m_dbAutoUp = false;
        m_dbUploadAfterExport = false;
        if (ok && wantUpload && !m_dbSyncPath.isEmpty()) {
            QFile file(m_dbSyncPath);
            if (file.open(QIODevice::ReadOnly)) {
                const QByteArray data = file.readAll();
                file.close();
                if (!data.isEmpty()) {
                    appendLog(QStringLiteral("☁ 人脸库已导出（%1 B），正在上传…").arg(data.size()));
                    m_cloudState->setText(QStringLiteral("正在上传人脸库…"));
                    m_cloud->pushSnapshot(data);
                    m_autoSyncRetry = false;
                } else {
                    statusError(QStringLiteral("云签到：导出文件为空"));
                }
            } else {
                statusError(QStringLiteral("云签到：读取导出文件失败"));
            }
        } else if (!ok && wantUpload) {
            appendLog(QStringLiteral("✗ 云签到：板端导出失败：%1").arg(msg));
            if (wasAuto && !m_autoSyncRetry) {
                m_autoSyncRetry = true;
                appendLog(QStringLiteral("⟳ 自动同步失败，3 秒后重试一次…"));
                QTimer::singleShot(3000, this, [this]() {
                    if (m_cloudOn->isChecked() && m_cloudAutoSync->isChecked()) {
                        pushFacebank(true);
                    }
                });
            } else if (wasAuto) {
                m_autoSyncRetry = false;
                appendLog(QStringLiteral("✗ 自动同步重试仍失败：%1 —— 名单变化未上传，"
                                         "可在「云」页手动点「上传板端人脸库到云端」").arg(msg));
            }
        }
        if (!m_cloudImportPath.isEmpty()) {
            QFile::remove(m_cloudImportPath);
            m_cloudImportPath.clear();
        }
    });
}

void MainWindow::cmd(const QJsonObject &obj)
{
    m_link->sendCmd(obj);
}

void MainWindow::presetInterface(bool usb, const QString &serialPort, const QString &host,
                                 quint16 port, bool autoConnect)
{
    m_ifaceBox->setCurrentIndex(usb ? 1 : 0);      /* 触发 updateIfaceWidgets */
    if (usb) {
        QString want = serialPort;
        if (!want.isEmpty()) {
            /* 允许只给 COMx：在列表里按端口号找（列表里存的是 data=COMx） */
            const int idx = m_serialBox->findData(want, Qt::UserRole, Qt::MatchFixedString);
            if (idx >= 0) {
                m_serialBox->setCurrentIndex(idx);
            } else {
                refreshPorts();
                const int idx2 = m_serialBox->findData(want);
                if (idx2 >= 0) {
                    m_serialBox->setCurrentIndex(idx2);
                }
            }
        }
    } else {
        if (!host.isEmpty()) {
            m_hostEdit->setText(host);
        }
        if (port) {
            m_portSpin->setValue(port);
        }
    }
    if (autoConnect) {
        QTimer::singleShot(0, this, [this]() { onConnectClicked(); });
    }
}

/* ---------------- 界面搭建 ---------------- */

void MainWindow::buildUi()
{
    /* ===== 顶栏 ===== */
    auto *top = new QWidget;
    auto *topL = new QHBoxLayout(top);
    topL->setContentsMargins(10, 8, 10, 8);
    topL->setSpacing(8);

    m_ifaceBox = new QComboBox;
    m_ifaceBox->addItem(QStringLiteral("TCP（网口）"), QStringLiteral("tcp"));
    m_ifaceBox->addItem(QStringLiteral("USB（高速口）"), QStringLiteral("usb"));
    m_ifaceBox->setFixedWidth(130);

    m_hostEdit = new QLineEdit(QStringLiteral("192.168.33.2"));
    m_hostEdit->setFixedWidth(120);
    m_portSpin = new QSpinBox;
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(3333);
    m_portSpin->setFixedWidth(80);

    m_serialBox = new QComboBox;
    m_serialBox->setMinimumWidth(210);
    m_refreshPortsBtn = button(QStringLiteral("刷新"));

    m_connBtn = button(QStringLiteral("连接"), QStringLiteral("primary"));
    m_connBtn->setFixedWidth(84);

    m_linkBadge = new QLabel(QStringLiteral("离线"));
    m_linkBadge->setProperty("cls", QStringLiteral("badge"));
    m_linkBadge->setProperty("state", QStringLiteral("off"));
    m_linkBadge->setAlignment(Qt::AlignCenter);
    m_linkBadge->setMinimumWidth(84);

    m_linkModeBox = new QComboBox;
    m_linkModeBox->addItems({QStringLiteral("auto"), QStringLiteral("usb"), QStringLiteral("tcp")});
    m_linkModeBox->setFixedWidth(78);
    m_linkModeBox->setToolTip(QStringLiteral("板端 link 强制态：auto=TCP 连上抢占 / usb=强制 USB / tcp=强制网口"));

    m_shotBtn = button(QStringLiteral("截图 (S)"));
    m_zoomBox = new QComboBox;
    m_zoomBox->addItems({QStringLiteral("适应窗口"), QStringLiteral("1x"), QStringLiteral("2x"), QStringLiteral("3x")});
    m_zoomBox->setFixedWidth(96);

    topL->addWidget(new QLabel(QStringLiteral("接口")));
    topL->addWidget(m_ifaceBox);
    topL->addWidget(m_hostEdit);
    topL->addWidget(m_portSpin);
    topL->addWidget(m_serialBox);
    topL->addWidget(m_refreshPortsBtn);
    topL->addSpacing(6);
    topL->addWidget(m_connBtn);
    topL->addWidget(m_linkBadge);
    topL->addWidget(new QLabel(QStringLiteral("链路")));
    topL->addWidget(m_linkModeBox);
    topL->addStretch(1);
    topL->addWidget(m_shotBtn);
    topL->addWidget(new QLabel(QStringLiteral("缩放")));
    topL->addWidget(m_zoomBox);

    /* ===== 主体：左视频 + 右控制面板 ===== */
    m_view = new VideoView;

    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(2000);
    m_logView->setFixedHeight(132);
    m_logView->setProperty("cls", QStringLiteral("log"));

    auto *left = new QWidget;
    auto *leftL = new QVBoxLayout(left);
    leftL->setContentsMargins(10, 0, 6, 10);
    leftL->setSpacing(8);
    leftL->addWidget(m_view, 1);
    leftL->addWidget(dimLabel(QStringLiteral("板端通信 / 日志")));
    leftL->addWidget(m_logView);

    auto *tabs = new QTabWidget;
    tabs->addTab(scrollPage(buildStreamTab()), QStringLiteral("流"));
    tabs->addTab(scrollPage(buildRosterTab()), QStringLiteral("名单"));
    tabs->addTab(scrollPage(buildRecognizeTab()), QStringLiteral("识别"));
    tabs->addTab(scrollPage(buildCameraTab()), QStringLiteral("相机"));
    tabs->addTab(scrollPage(buildBackupTab()), QStringLiteral("备份"));
    tabs->addTab(scrollPage(buildCloudTab()), QStringLiteral("云"));
#ifdef QT_HOST_PC_TEST
    {
        auto *pcPage = new PcTestPage(m_link);
        connect(pcPage, &PcTestPage::testModeChanged, this,
                [this](bool on) { m_pcTestMode = on; });
        tabs->addTab(scrollPage(pcPage), QStringLiteral("PC测试"));
    }
#endif
    tabs->setMinimumWidth(330);
    tabs->setMaximumWidth(430);

    auto *split = new QSplitter(Qt::Horizontal);
    split->addWidget(left);
    split->addWidget(tabs);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);

    auto *central = new QWidget;
    auto *cl = new QVBoxLayout(central);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    cl->addWidget(top);
    cl->addWidget(split, 1);
    setCentralWidget(central);

    /* ===== 状态栏 ===== */
    m_sbDesc = new QLabel(QStringLiteral("未连接"));
    m_sbLink = new QLabel(QStringLiteral("链路 -"));
    m_sbTraffic = new QLabel(QStringLiteral("RX 0 KB · TX 0 KB"));
    m_sbFps = new QLabel(QStringLiteral("0 fps"));
    m_sbDrop = new QLabel(QStringLiteral("丢帧 0"));
    m_sbLog = new QLabel;
    m_sbLog->setMinimumWidth(320);
    for (QLabel *l : {m_sbDesc, m_sbLink, m_sbTraffic, m_sbFps, m_sbDrop}) {
        l->setProperty("cls", QStringLiteral("status"));
        statusBar()->addPermanentWidget(l);
    }
    statusBar()->addWidget(m_sbLog, 1);

    /* ===== 顶栏交互 ===== */
    connect(m_ifaceBox, &QComboBox::currentIndexChanged, this, [this]() { updateIfaceWidgets(); });
    connect(m_refreshPortsBtn, &QPushButton::clicked, this, [this]() { refreshPorts(); });
    connect(m_connBtn, &QPushButton::clicked, this, &MainWindow::onConnectClicked);
    connect(m_shotBtn, &QPushButton::clicked, this, &MainWindow::saveScreenshot);
    connect(m_zoomBox, &QComboBox::currentIndexChanged, this, [this](int i) { m_view->setZoom(i); });
    connect(m_linkModeBox, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_syncing || !m_link->isConnected()) return;
        cmd(cmdObj(QStringLiteral("link"), QStringLiteral("value"), m_linkModeBox->currentText()));
    });
    /* 快捷键 */
    auto *s = new QAction(this);
    s->setShortcut(QKeySequence(QStringLiteral("S")));
    connect(s, &QAction::triggered, this, &MainWindow::saveScreenshot);
    addAction(s);
    auto *r = new QAction(this);
    r->setShortcut(QKeySequence(QStringLiteral("R")));
    connect(r, &QAction::triggered, this, [this]() {
        if (m_link->isConnected()) { m_link->close(); }
        onConnectClicked();
    });
    addAction(r);
}

QWidget *MainWindow::buildStreamTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setSpacing(10);

    m_fpsS = new QSlider(Qt::Horizontal);
    m_fpsS->setRange(1, 30);
    m_fpsS->setValue(10);
    m_fpsSpin = new QSpinBox;
    m_fpsSpin->setRange(1, 30);
    m_fpsSpin->setValue(10);
    m_fpsSpin->setMinimumWidth(84);

    m_qS = new QSlider(Qt::Horizontal);
    m_qS->setRange(1, 100);
    m_qS->setValue(75);
    m_qSpin = new QSpinBox;
    m_qSpin->setRange(1, 100);
    m_qSpin->setValue(75);
    m_qSpin->setMinimumWidth(84);

    auto *fpsRow = new QWidget;
    auto *fh = new QHBoxLayout(fpsRow);
    fh->setContentsMargins(0, 0, 0, 0);
    fh->addWidget(m_fpsS, 1);
    fh->addWidget(m_fpsSpin);

    auto *qRow = new QWidget;
    auto *qh = new QHBoxLayout(qRow);
    qh->setContentsMargins(0, 0, 0, 0);
    qh->addWidget(m_qS, 1);
    qh->addWidget(m_qSpin);

    m_fmtBox = new QComboBox;
    m_fmtBox->addItem(QStringLiteral("JPEG（默认）"), QStringLiteral("jpeg"));
    m_fmtBox->addItem(QStringLiteral("raw RGB565（614KB/帧）"), QStringLiteral("raw"));
    m_srcBox = new QComboBox;
    m_srcBox->addItem(QStringLiteral("板载摄像头 (cam)"), QStringLiteral("cam"));
#ifdef QT_HOST_PC_TEST
    m_srcBox->addItem(QStringLiteral("PC 送帧 (pc)"), QStringLiteral("pc"));
#endif
    m_swapChk = new QCheckBox(QStringLiteral("raw 显示前交换字节序（颜色不对时勾选）"));

    /* H.264 编码参数：板端 esp_video 的 4 个控件只在 STREAMON 生效，
       参数变化会在下一帧重建编码器（画面短暂停顿）。 */
    m_brSpin = new QSpinBox;
    m_brSpin->setRange(25, 2500);
    m_brSpin->setSingleStep(25);
    m_brSpin->setValue(1000);
    m_brSpin->setSuffix(QStringLiteral(" kbps"));
    m_brSpin->setMinimumWidth(120);

    m_qpMinSpin = new QSpinBox;
    m_qpMinSpin->setRange(0, 51);
    m_qpMinSpin->setValue(30);
    m_qpMinSpin->setMinimumWidth(88);
    m_qpMaxSpin = new QSpinBox;
    m_qpMaxSpin->setRange(0, 51);
    m_qpMaxSpin->setValue(38);
    m_qpMaxSpin->setMinimumWidth(88);

    m_iPeriodSpin = new QSpinBox;
    m_iPeriodSpin->setRange(1, 120);
    m_iPeriodSpin->setValue(25);
    m_iPeriodSpin->setMinimumWidth(96);

    auto *qpRow = new QWidget;
    auto *qph = new QHBoxLayout(qpRow);
    qph->setContentsMargins(0, 0, 0, 0);
    qph->setSpacing(6);
    qph->addWidget(m_qpMinSpin);
    qph->addWidget(dimLabel(QStringLiteral("至")));
    qph->addWidget(m_qpMaxSpin);
    qph->addStretch(1);

    v->addWidget(formRow(QStringLiteral("帧率"), fpsRow));
    v->addWidget(formRow(QStringLiteral("质量"), qRow));
    v->addWidget(formRow(QStringLiteral("格式"), m_fmtBox));
    v->addWidget(formRow(QStringLiteral("图像源"), m_srcBox));
    v->addWidget(m_swapChk);

    v->addWidget(dimLabel(QStringLiteral("— H.264 编码（仅 YUV420 固件）—")));
    v->addWidget(formRow(QStringLiteral("码率"), m_brSpin));
    v->addWidget(formRow(QStringLiteral("QP 范围"), qpRow));
    v->addWidget(formRow(QStringLiteral("I 帧间隔"), m_iPeriodSpin));

    auto *apply = button(QStringLiteral("下发流参数"), QStringLiteral("primary"));
    v->addWidget(apply);
    auto *applyEnc = button(QStringLiteral("下发编码参数"), QStringLiteral("primary"));
    v->addWidget(applyEnc);
    /* ① 路线（2026-10-16）：raw 连续流只有 ~1.4MB/s（≈2.3fps），但单帧 0.5s 就能拿到，
       颜色标定/取证足够——一键"开流( raw )→取一帧→停流→存 PNG"。 */
    auto *grabRawBtn = button(QStringLiteral("抓一帧 raw → PNG"));
    v->addWidget(grabRawBtn);
    v->addStretch(1);
    v->addWidget(hintLabel(QStringLiteral("提示：raw 档 614KB/帧，连续推流实测 ~1.4MB/s（≈2.3fps）；\n"
                                          "「抓一帧 raw」会自动开流→取帧→停流（约 0.5s）。\n"
                                          "JPEG 档可稳定 ~8-11fps，日常预览用它。\n"
                                          "编码参数仅 H.264 固件生效；QP 下限必须小于上限（相等会关闭码控）。\n"
                                          "⚠️ 识别+推流同时运行时，改码率/QP 会重建编码器，可能因内部 RAM 不足停流；"
                                          "已实测触发过，需重启板子。")));

    /* 滑块 <-> 数字框联动 */
    connect(m_fpsS, &QSlider::valueChanged, m_fpsSpin, &QSpinBox::setValue);
    connect(m_fpsSpin, QOverload<int>::of(&QSpinBox::valueChanged), m_fpsS, &QSlider::setValue);
    connect(m_qS, &QSlider::valueChanged, m_qSpin, &QSpinBox::setValue);
    connect(m_qSpin, QOverload<int>::of(&QSpinBox::valueChanged), m_qS, &QSlider::setValue);

    connect(m_swapChk, &QCheckBox::toggled, this, [this](bool on) { m_link->setSwapRawBytes(on); });

    connect(apply, &QPushButton::clicked, this, [this]() {
        QJsonObject o = cmdObj(QStringLiteral("stream"), QStringLiteral("value"), QStringLiteral("on"));
        o.insert(QStringLiteral("fps"), m_fpsSpin->value());
        o.insert(QStringLiteral("quality"), m_qSpin->value());
        o.insert(QStringLiteral("fmt"), m_fmtBox->currentData().toString());
        cmd(o);
        cmd(cmdObj(QStringLiteral("src"), QStringLiteral("value"), m_srcBox->currentData().toString()));
    });

    connect(applyEnc, &QPushButton::clicked, this, [this]() {
        if (m_syncing || !m_link->isConnected()) {
            statusError(QStringLiteral("请先连接"));
            return;
        }
        if (m_qpMinSpin->value() >= m_qpMaxSpin->value()) {
            statusError(QStringLiteral("QP 下限必须小于上限（相等会关闭码控）"));
            return;
        }
        if (QMessageBox::warning(
                this, QStringLiteral("修改编码参数"),
                QStringLiteral("修改码率/QP 会重建 H.264 编码器。\n"
                               "识别+推流同时运行时内部 RAM 可能不足以重建，导致推流停止"
                               "（需重启板子；本机已实测触发过）。\n\n"
                               "建议改固件默认值，仍要下发吗？"),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        /* 不带 value：只改编码参数，不切换流的开关状态 */
        QJsonObject o = cmdObj(QStringLiteral("stream"));
        o.insert(QStringLiteral("bitrate"), m_brSpin->value() * 1000);
        o.insert(QStringLiteral("qp_min"), m_qpMinSpin->value());
        o.insert(QStringLiteral("qp_max"), m_qpMaxSpin->value());
        o.insert(QStringLiteral("i_period"), m_iPeriodSpin->value());
        appendLog(QStringLiteral("→ 编码参数：%1 kbps / QP%2-%3 / I=%4")
                      .arg(m_brSpin->value())
                      .arg(m_qpMinSpin->value())
                      .arg(m_qpMaxSpin->value())
                      .arg(m_iPeriodSpin->value()));
        cmd(o);
    });

    connect(grabRawBtn, &QPushButton::clicked, this, [this]() {
        if (!m_link->isConnected()) {
            statusError(QStringLiteral("请先连接"));
            return;
        }
        const QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("shots"));
        QDir().mkpath(dir);
        const QString path = QDir(dir).filePath(
            QStringLiteral("raw_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
        m_streamBeforeGrab = m_streamOn;      /* 抓完自动恢复预览（用 UI 当前的 fmt） */
        appendLog(QStringLiteral("→ 抓一帧 raw（自动 开流→取帧→停流）…"));
        m_link->startRawGrab(path);
    });

    connect(m_srcBox, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_syncing || !m_link->isConnected()) return;
        cmd(cmdObj(QStringLiteral("src"), QStringLiteral("value"), m_srcBox->currentData().toString()));
    });
    return page;
}

QWidget *MainWindow::buildRosterTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setSpacing(8);

    m_roster = new QTableWidget(0, 2);
    m_roster->setHorizontalHeaderLabels({QStringLiteral("名字"), QStringLiteral("特征数")});
    m_roster->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_roster->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_roster->verticalHeader()->setVisible(false);
    m_roster->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_roster->setEditTriggers(QAbstractItemView::NoEditTriggers);
    v->addWidget(m_roster, 1);

    auto *h = new QHBoxLayout;
    m_regName = new QLineEdit;
    m_regName->setPlaceholderText(QStringLiteral("新成员名字（ASCII）"));
    m_regShots = new QComboBox;
    m_regShots->addItems({QStringLiteral("1 帧"), QStringLiteral("3 帧"), QStringLiteral("5 帧")});
    m_regShots->setCurrentIndex(1);
    m_regShots->setMinimumWidth(84);
    auto *regBtn = button(QStringLiteral("注册"), QStringLiteral("primary"));
    h->addWidget(m_regName, 1);
    h->addWidget(m_regShots);
    h->addWidget(regBtn);
    v->addLayout(h);

    auto *h2 = new QHBoxLayout;
    auto *listBtn = button(QStringLiteral("刷新名单"));
    auto *delBtn = button(QStringLiteral("删除选中"));
    auto *clearBtn = button(QStringLiteral("清空库"), QStringLiteral("danger"));
    h2->addWidget(listBtn);
    h2->addWidget(delBtn);
    h2->addWidget(clearBtn);
    v->addLayout(h2);
    v->addWidget(hintLabel(QStringLiteral("注册：点「注册」后请站到镜头前（可多帧）；\n"
                                          "删除/清空会同时清掉该名字的全部特征。")));

    connect(listBtn, &QPushButton::clicked, this, [this]() { cmd(cmdObj(QStringLiteral("list"))); });
    connect(regBtn, &QPushButton::clicked, this, [this]() {
        const QString n = m_regName->text().trimmed();
        if (n.isEmpty()) { statusError(QStringLiteral("请填名字")); return; }
        const int shots = m_regShots->currentIndex() == 0 ? 1
                        : (m_regShots->currentIndex() == 1 ? 3 : 5);
        QJsonObject o = cmdObj(QStringLiteral("register"));
        o.insert(QStringLiteral("id"), n);
        o.insert(QStringLiteral("shots"), shots);
        cmd(o);
        appendLog(QStringLiteral("→ 注册 %1（%2 帧）").arg(n).arg(shots));
    });
    connect(delBtn, &QPushButton::clicked, this, [this]() {
        const int r = m_roster->currentRow();
        if (r < 0) { statusError(QStringLiteral("先选中一行")); return; }
        const QString n = m_roster->item(r, 0)->text();
        if (QMessageBox::question(this, QStringLiteral("删除"),
                                  QStringLiteral("删除 %1 的全部特征？").arg(n))
            != QMessageBox::Yes) {
            return;
        }
        cmd(cmdObj(QStringLiteral("delete"), QStringLiteral("id"), n));
    });
    connect(clearBtn, &QPushButton::clicked, this, [this]() {
        if (QMessageBox::warning(this, QStringLiteral("清空特征库"),
                                 QStringLiteral("将删除全部成员及其特征，且不可撤销。\n建议先「备份」页导出一份。\n\n继续？"),
                                 QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        cmd(cmdObj(QStringLiteral("clear")));
    });
    return page;
}

QWidget *MainWindow::buildRecognizeTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setSpacing(10);

    m_diagLabel = hintLabel(QStringLiteral("实时投票：等待结果帧…"));
    m_diagLabel->setObjectName(QStringLiteral("diagVote"));   /* UIA 定位用 */
    v->addWidget(m_diagLabel);

    m_detChk = new QCheckBox(QStringLiteral("启用检测"));
    m_detChk->setChecked(true);
    m_matchChk = new QCheckBox(QStringLiteral("启用比对（1:N）"));
    m_matchChk->setChecked(true);

    m_loS = new QDoubleSpinBox;
    m_loS->setRange(0.0, 1.0);
    m_loS->setSingleStep(0.01);
    m_loS->setDecimals(2);
    m_loS->setValue(0.61);
    m_hiS = new QDoubleSpinBox;
    m_hiS->setRange(0.0, 1.0);
    m_hiS->setSingleStep(0.01);
    m_hiS->setDecimals(2);
    m_hiS->setValue(0.61);
    m_fastS = new QDoubleSpinBox;
    m_fastS->setRange(0.0, 1.0);
    m_fastS->setSingleStep(0.01);
    m_fastS->setDecimals(2);
    m_fastS->setValue(0.80);

    m_dedupSpin = new QSpinBox;
    m_dedupSpin->setRange(0, 60000);
    m_dedupSpin->setSingleStep(500);
    m_dedupSpin->setValue(5000);
    m_dedupSpin->setMinimumWidth(100);
    m_multiSpin = new QSpinBox;
    m_multiSpin->setRange(1, 3);
    m_multiSpin->setValue(2);
    m_multiSpin->setMinimumWidth(100);

    v->addWidget(m_detChk);
    v->addWidget(m_matchChk);
    v->addWidget(formRow(QStringLiteral("thr_lo"), m_loS));
    v->addWidget(formRow(QStringLiteral("thr_hi"), m_hiS));
    v->addWidget(formRow(QStringLiteral("快速通道"), m_fastS));
    v->addWidget(formRow(QStringLiteral("去重 ms"), m_dedupSpin));
    v->addWidget(formRow(QStringLiteral("多人脸数"), m_multiSpin));

    auto *apply = button(QStringLiteral("下发识别参数"), QStringLiteral("primary"));
    v->addWidget(apply);
    v->addStretch(1);
    v->addWidget(hintLabel(QStringLiteral("⚠️ esp-dl 特征提取在 P4 上非确定性：绝对阈值跨开机不可移植，\n"
                                          "每 boot 建议用 thr_calib 相对标定；快速通道可绕开投票抖动。")));

    connect(apply, &QPushButton::clicked, this, [this]() {
        QJsonObject o = cmdObj(QStringLiteral("mode"));
        o.insert(QStringLiteral("detect"), m_detChk->isChecked());
        o.insert(QStringLiteral("match"), m_matchChk->isChecked());
        o.insert(QStringLiteral("thr_lo"), m_loS->value());
        o.insert(QStringLiteral("thr_hi"), m_hiS->value());
        o.insert(QStringLiteral("fast_thr"), m_fastS->value());
        o.insert(QStringLiteral("dedup_ms"), m_dedupSpin->value());
        o.insert(QStringLiteral("multi"), m_multiSpin->value());
        cmd(o);
    });
    return page;
}

QWidget *MainWindow::buildCameraTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setSpacing(9);

    m_gainS = new QSlider(Qt::Horizontal);
    m_gainS->setRange(0, m_gainMax);
    m_expS = new QSlider(Qt::Horizontal);
    m_expS->setRange(0, m_expMax);
    m_gainLbl = dimLabel(QStringLiteral("0 / %1").arg(m_gainMax));
    m_gainLbl->setFixedWidth(78);
    m_expLbl = dimLabel(QStringLiteral("0 / %1").arg(m_expMax));
    m_expLbl->setFixedWidth(78);

    auto *gRow = new QWidget;
    auto *gh = new QHBoxLayout(gRow);
    gh->setContentsMargins(0, 0, 0, 0);
    gh->addWidget(m_gainS, 1);
    gh->addWidget(m_gainLbl);
    auto *eRow = new QWidget;
    auto *eh = new QHBoxLayout(eRow);
    eh->setContentsMargins(0, 0, 0, 0);
    eh->addWidget(m_expS, 1);
    eh->addWidget(m_expLbl);

    m_camFpsBox = new QComboBox;
    for (int f : {5, 10, 15, 20, 25, 30, 50}) {
        m_camFpsBox->addItem(QStringLiteral("%1 fps").arg(f), f);
    }
    m_dnBox = new QComboBox;
    m_dnBox->addItem(QStringLiteral("关"), 0);
    m_dnBox->addItem(QStringLiteral("低"), 1);
    m_dnBox->addItem(QStringLiteral("中"), 2);
    m_dnBox->addItem(QStringLiteral("高"), 3);

    /* YUV420/H.264 固件里 H.264 与送检帧都来自 ISP：镜像要发 hflip（ISP HFLIP），
       不能发 imgpipe 的 mirror（那只影响 RGB565 送检，会造成视频/送检反向）。 */
    m_mirrorChk = new QCheckBox(QStringLiteral("镜像（ISP HFLIP，H.264/送检同向）"));
    m_mirrorChk->setChecked(QSettings().value(QStringLiteral("cam/hflip"), true).toBool());
    m_vflipChk = new QCheckBox(QStringLiteral("垂直翻转（相机倒装 180° 校正）"));
    m_vflipChk->setChecked(QSettings().value(QStringLiteral("cam/vflip"), false).toBool());
    m_aeChk = new QCheckBox(QStringLiteral("自动曝光 AE"));
    m_awbChk = new QCheckBox(QStringLiteral("自动白平衡 AWB"));
    m_nightChk = new QCheckBox(QStringLiteral("夜视阶梯（暗度降帧+降噪）"));
    m_faceNightChk = new QCheckBox(QStringLiteral("低光照停人脸（夜视预览优先）"));
    m_aeChk->setChecked(true);
    m_awbChk->setChecked(true);
    m_nightChk->setChecked(true);
    m_faceNightChk->setChecked(true);

    v->addWidget(formRow(QStringLiteral("增益"), gRow));
    v->addWidget(formRow(QStringLiteral("曝光"), eRow));
    v->addWidget(formRow(QStringLiteral("帧率"), m_camFpsBox));
    v->addWidget(formRow(QStringLiteral("降噪"), m_dnBox));
    v->addWidget(m_mirrorChk);
    v->addWidget(m_vflipChk);
    v->addWidget(m_aeChk);
    v->addWidget(m_awbChk);
    v->addWidget(m_nightChk);
    v->addWidget(m_faceNightChk);

    auto *apply = button(QStringLiteral("下发相机参数"), QStringLiteral("primary"));
    v->addWidget(apply);
    v->addStretch(1);
    v->addWidget(hintLabel(QStringLiteral(
        "勾选/取消会立即下发（无需再点按钮）；帧率/降噪仍走「下发相机参数」。\n"
        "镜像 = ISP HFLIP（H.264 与送检帧同向）；倒装相机再勾「垂直翻转」合成 180°。\n"
        "YUV420 固件固定 WB，AWB 可能无法保持开启；画面发黑/噪点多先补正面光。")));

    /* 滑块拖动结束才下发（避免命令洪泛） */
    connect(m_gainS, &QSlider::valueChanged, this, [this](int v) {
        m_gainLbl->setText(QStringLiteral("%1 / %2").arg(v).arg(m_gainMax));
    });
    connect(m_expS, &QSlider::valueChanged, this, [this](int v) {
        m_expLbl->setText(QStringLiteral("%1 / %2").arg(v).arg(m_expMax));
    });
    connect(m_gainS, &QSlider::sliderReleased, this, [this]() {
        if (m_syncing || !m_link->isConnected()) return;
        cmd(cmdObj(QStringLiteral("cam"), QStringLiteral("gain"), m_gainS->value()));
    });
    connect(m_expS, &QSlider::sliderReleased, this, [this]() {
        if (m_syncing || !m_link->isConnected()) return;
        cmd(cmdObj(QStringLiteral("cam"), QStringLiteral("exp"), m_expS->value()));
    });
    auto sendCamToggle = [this](const char *key, bool on, const QString &label) {
        if (m_syncing || !m_link->isConnected()) return;
        QJsonObject o = cmdObj(QStringLiteral("cam"));
        o.insert(QLatin1String(key), on);
        cmd(o);
        appendLog(QStringLiteral("相机 %1：%2").arg(label, on ? QStringLiteral("开")
                                                             : QStringLiteral("关")));
    };
    connect(m_mirrorChk, &QCheckBox::toggled, this, [this, sendCamToggle](bool on) {
        QSettings().setValue(QStringLiteral("cam/hflip"), on);
        sendCamToggle("hflip", on, QStringLiteral("水平翻转"));
    });
    connect(m_vflipChk, &QCheckBox::toggled, this, [this, sendCamToggle](bool on) {
        QSettings().setValue(QStringLiteral("cam/vflip"), on);
        sendCamToggle("vflip", on, QStringLiteral("垂直翻转"));
    });
    connect(m_aeChk, &QCheckBox::toggled, this, [sendCamToggle](bool on) {
        sendCamToggle("ae", on, QStringLiteral("自动曝光"));
    });
    connect(m_awbChk, &QCheckBox::toggled, this, [sendCamToggle](bool on) {
        sendCamToggle("awb", on, QStringLiteral("自动白平衡"));
    });
    connect(m_nightChk, &QCheckBox::toggled, this, [sendCamToggle](bool on) {
        sendCamToggle("night", on, QStringLiteral("夜视阶梯"));
    });
    connect(m_faceNightChk, &QCheckBox::toggled, this, [sendCamToggle](bool on) {
        sendCamToggle("face_night", on, QStringLiteral("低光照停人脸"));
    });
    connect(apply, &QPushButton::clicked, this, [this]() {
        QJsonObject o = cmdObj(QStringLiteral("cam"));
        o.insert(QStringLiteral("hflip"), m_mirrorChk->isChecked());
        o.insert(QStringLiteral("vflip"), m_vflipChk->isChecked());
        o.insert(QStringLiteral("ae"), m_aeChk->isChecked());
        o.insert(QStringLiteral("awb"), m_awbChk->isChecked());
        o.insert(QStringLiteral("night"), m_nightChk->isChecked());
        o.insert(QStringLiteral("face_night"), m_faceNightChk->isChecked());
        o.insert(QStringLiteral("dn"), m_dnBox->currentData().toInt());
        o.insert(QStringLiteral("fps"), m_camFpsBox->currentData().toInt());
        cmd(o);
        /* 恢复顺序：先 fps 再 night/ae（见 HANDOVER 纪律） */
        cmd(cmdObj(QStringLiteral("cam"), QStringLiteral("night"), m_nightChk->isChecked()));
        cmd(cmdObj(QStringLiteral("cam"), QStringLiteral("ae"), m_aeChk->isChecked()));
    });
    return page;
}

QWidget *MainWindow::buildBackupTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setSpacing(10);

    m_exportBtn = button(QStringLiteral("导出特征库到 .bin"), QStringLiteral("primary"));
    m_importBtn = button(QStringLiteral("从 .bin 恢复（覆盖式）"));
    m_dbBar = new QProgressBar;
    m_dbBar->setRange(0, 100);
    m_dbBar->setValue(0);
    m_dbLabel = hintLabel(QStringLiteral("空闲（导入前板端会自动把现有库备份到 face_bak.db / names.bak）"));

    v->addWidget(m_exportBtn);
    v->addWidget(m_importBtn);
    v->addWidget(m_dbBar);
    v->addWidget(m_dbLabel);
    v->addStretch(1);
    v->addWidget(hintLabel(QStringLiteral("备份内容 = 名表 + face.db（id 与名字映射逐位一致，可跨设备迁移）；\n"
                                          "传输走 JCLINK 0x06 分片，整包 CRC32 校验，失败则库不变。")));

    connect(m_exportBtn, &QPushButton::clicked, this, [this]() {
        if (!m_link->isConnected()) { statusError(QStringLiteral("请先连接")); return; }
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("导出特征库"),
            QDir(QCoreApplication::applicationDirPath()).filePath(
                QStringLiteral("face_db_%1.bin").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))),
            QStringLiteral("备份文件 (*.bin)"));
        if (path.isEmpty()) return;
        m_exportBtn->setEnabled(false);
        m_importBtn->setEnabled(false);
        m_dbBar->setValue(0);
        m_link->dbExport(path);
    });

    connect(m_importBtn, &QPushButton::clicked, this, [this]() {
        if (!m_link->isConnected()) { statusError(QStringLiteral("请先连接")); return; }
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("选择备份文件"), QString(),
                                                         QStringLiteral("备份文件 (*.bin)"));
        if (path.isEmpty()) return;
        if (QMessageBox::warning(this, QStringLiteral("恢复特征库"),
                                 QStringLiteral("将【覆盖】板端现有全部成员与特征。\n"
                                                "板端会自动把当前库备份为 face_bak.db / names.bak。\n\n继续？"),
                                 QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
            return;
        }
        m_exportBtn->setEnabled(false);
        m_importBtn->setEnabled(false);
        m_dbBar->setValue(0);
        m_link->dbImport(path);
    });
    return page;
}

QWidget *MainWindow::buildCloudTab()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setSpacing(10);

    m_cloudOn = new QCheckBox(QStringLiteral("启用云签到（识别到 match 时上报）"));
    m_cloudUrl = new QLineEdit;
    m_cloudUrl->setPlaceholderText(QStringLiteral("http://<服务器地址>:8080"));
    m_cloudToken = new QLineEdit;
    m_cloudToken->setEchoMode(QLineEdit::Password);
    m_cloudToken->setPlaceholderText(QStringLiteral("服务器 install.sh 生成的 Token"));
    m_cloudDevice = new QLineEdit;
    m_cloudDevice->setPlaceholderText(QStringLiteral("设备名（默认本机名）"));
    m_cloudPhoto = new QCheckBox(QStringLiteral("签到抓拍（按人脸框裁剪 JPEG 上传）"));
    m_cloudAutoSync = new QCheckBox(QStringLiteral("注册/删除后自动同步人脸库"));
    m_cloudPhoto->setChecked(true);
    m_cloudAutoSync->setChecked(true);

    {
        QSettings s;
        m_cloudOn->setChecked(s.value(QStringLiteral("cloud/enabled"), false).toBool());
        m_cloudUrl->setText(s.value(QStringLiteral("cloud/url"),
                                    QString()).toString());
        m_cloudToken->setText(s.value(QStringLiteral("cloud/token")).toString());
        QString name = s.value(QStringLiteral("cloud/device"), QHostInfo::localHostName()).toString();
        if (name.trimmed().isEmpty()) name = QStringLiteral("qt_host");
        m_cloudDevice->setText(name);
        m_cloudPhoto->setChecked(s.value(QStringLiteral("cloud/photo"), true).toBool());
        m_cloudAutoSync->setChecked(s.value(QStringLiteral("cloud/autoSync"), true).toBool());
    }

    m_cloudTestBtn = button(QStringLiteral("保存配置并测试连接"), QStringLiteral("primary"));
    m_cloudPushBtn = button(QStringLiteral("上传板端人脸库到云端"));
    m_cloudPullBtn = button(QStringLiteral("从云端同步人脸库到板端"));
    auto *openWeb = button(QStringLiteral("打开网页后台"));
    m_cloudState = hintLabel(QStringLiteral("未启用：填写服务器与 Token 后勾选"));
    m_cloudLast = hintLabel(QStringLiteral("最近上报：—"));

    v->addWidget(m_cloudOn);
    v->addWidget(formRow(QStringLiteral("服务器"), m_cloudUrl));
    v->addWidget(formRow(QStringLiteral("Token"), m_cloudToken));
    v->addWidget(formRow(QStringLiteral("设备名"), m_cloudDevice));
    v->addWidget(m_cloudPhoto);
    v->addWidget(m_cloudAutoSync);
    v->addWidget(m_cloudTestBtn);
    v->addWidget(m_cloudPushBtn);
    v->addWidget(m_cloudPullBtn);
    v->addWidget(openWeb);
    v->addWidget(m_cloudState);
    v->addWidget(m_cloudLast);
    v->addStretch(1);
    v->addWidget(hintLabel(QStringLiteral(
        "云端只保存签到事件与人脸库快照，不做特征比对。\n"
        "Token 在服务器执行 install.sh 时生成；公网部署请用安全组限制来源 IP。\n"
        "抓拍仅在勾选时上传，云端默认保留 90 天；自动同步失败会落盘待下次补发。")));

    connect(m_cloudOn, &QCheckBox::toggled, this, [this](bool) { applyCloudConfig(); });
    connect(m_cloudPhoto, &QCheckBox::toggled, this, [this](bool) { applyCloudConfig(); });
    connect(m_cloudAutoSync, &QCheckBox::toggled, this, [this](bool) { applyCloudConfig(); });
    connect(m_cloudUrl, &QLineEdit::editingFinished, this, [this]() { applyCloudConfig(); });
    connect(m_cloudToken, &QLineEdit::editingFinished, this, [this]() { applyCloudConfig(); });
    connect(m_cloudDevice, &QLineEdit::editingFinished, this, [this]() { applyCloudConfig(); });
    connect(m_cloudTestBtn, &QPushButton::clicked, this, [this]() {
        applyCloudConfig();
        m_cloudState->setText(QStringLiteral("正在测试连接…"));
        m_cloud->probe();
    });
    connect(m_cloudPushBtn, &QPushButton::clicked, this, [this]() { pushFacebank(false); });
    connect(m_cloudPullBtn, &QPushButton::clicked, this, [this]() { pullFacebank(); });
    connect(openWeb, &QPushButton::clicked, this, [this]() {
        QString url = m_cloudUrl->text().trimmed();
        if (url.isEmpty()) { statusError(QStringLiteral("请先填写服务器地址")); return; }
        if (!url.contains(QStringLiteral("://"))) url.prepend(QStringLiteral("http://"));
        QDesktopServices::openUrl(QUrl(url));
    });
    return page;
}

/* ---------------- 云签到 ---------------- */

void MainWindow::applyCloudConfig(bool quiet)
{
    if (!m_cloudOn) return;
    QSettings s;
    s.setValue(QStringLiteral("cloud/enabled"), m_cloudOn->isChecked());
    s.setValue(QStringLiteral("cloud/url"), m_cloudUrl->text().trimmed());
    s.setValue(QStringLiteral("cloud/token"), m_cloudToken->text().trimmed());
    s.setValue(QStringLiteral("cloud/device"), m_cloudDevice->text().trimmed());
    s.setValue(QStringLiteral("cloud/photo"), m_cloudPhoto->isChecked());
    s.setValue(QStringLiteral("cloud/autoSync"), m_cloudAutoSync->isChecked());

    m_cloud->configure(QUrl(m_cloudUrl->text().trimmed()), m_cloudToken->text().trimmed(),
                       m_cloudDevice->text().trimmed(), m_cloudPhoto->isChecked());
    if (!quiet) {
        m_cloudState->setText(m_cloudOn->isChecked()
                                  ? QStringLiteral("配置已保存")
                                  : QStringLiteral("未启用云签到"));
        m_cloudState->setStyleSheet(QStringLiteral("color:#8A93A3"));
    }
    if (m_cloudOn->isChecked() && m_cloud->configured() && m_link->isConnected()) {
        m_cloud->heartbeat(m_lastLibN, QStringLiteral("p4-h264"), m_link->stats().link);
    }
}

void MainWindow::onCloudMatch(const QJsonObject &obj)
{
    if (!m_cloudOn || !m_cloudOn->isChecked()) return;
    if (!m_cloud->configured()) {
        m_cloudState->setText(QStringLiteral("未配置服务器/Token，签到未上报"));
        m_cloudState->setStyleSheet(QStringLiteral("color:#FF5C5C"));
        return;
    }
    const QString name = obj.value(QStringLiteral("match")).toString().trimmed();
    if (name.isEmpty()) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    /* 本地静默与服务器窗口对齐（服务器仍是权威）：没探到之前沿用 5s，
       上限 30 分钟，避免服务器配错把签到全压掉 */
    const int serverWinS = m_cloud->dedupWindowS();
    const qint64 seenMs = serverWinS > 0 ? qMin(serverWinS, 1800) * 1000LL : 5000;
    const auto it = m_cloudSeen.constFind(name);
    if (it != m_cloudSeen.constEnd() && now - it.value() < seenMs) return;
    m_cloudSeen.insert(name, now);
    if (m_cloudSeen.size() > 200) {
        const qint64 keepMs = qMax(3600000LL, seenMs * 2);
        for (auto i = m_cloudSeen.begin(); i != m_cloudSeen.end();) {
            if (now - i.value() > keepMs) i = m_cloudSeen.erase(i);
            else ++i;
        }
    }

    QJsonObject ev;
    ev.insert(QStringLiteral("event"), QStringLiteral("match"));
    ev.insert(QStringLiteral("device"), m_cloud->deviceName());
    ev.insert(QStringLiteral("person"), name);
    static const char *const keys[] = {"sim", "sim_raw", "stable", "faces",
                                       "src", "link", "fps", "infer_ms", "lib_n"};
    for (const char *key : keys) {
        const QLatin1String k(key);
        if (obj.contains(k)) ev.insert(k, obj.value(k));
    }
    ev.insert(QStringLiteral("ts"),
              QDateTime::currentDateTime().toString(Qt::ISODateWithMs));

    QString photoNote;
    if (m_cloudPhoto->isChecked()) {
        const QByteArray jpeg = cropFaceJpeg(name);
        if (!jpeg.isEmpty()) {
            ev.insert(QStringLiteral("photo_b64"), QString::fromLatin1(jpeg.toBase64()));
            photoNote = QStringLiteral(" + 抓拍");
        }
    }
    m_cloud->sendEvent(ev);
    appendLog(QStringLiteral("☁ 上报签到：%1（sim %2）%3")
                  .arg(name)
                  .arg(obj.value(QStringLiteral("sim")).toDouble(), 0, 'f', 3)
                  .arg(photoNote));
    m_cloudLast->setText(QStringLiteral("最近上报：%1 · %2")
                             .arg(name, QDateTime::currentDateTime().toString("HH:mm:ss")));
}

QByteArray MainWindow::cropFaceJpeg(const QString &name) const
{
    if (m_lastFrame.isNull()) return QByteArray();

    QRect best;
    QRect fallback;
    int bestArea = 0;
    int fallbackArea = 0;
    for (const QJsonValue &value : m_lastBoxes) {
        const QJsonArray box = value.toArray();
        if (box.size() < 4) continue;
        int x1 = box.at(0).toInt();
        int y1 = box.at(1).toInt();
        int x2 = box.at(2).toInt();
        int y2 = box.at(3).toInt();
        if (x2 < x1) std::swap(x1, x2);
        if (y2 < y1) std::swap(y1, y2);
        const QRect rect(x1, y1, x2 - x1, y2 - y1);
        const int area = rect.width() * rect.height();
        if (area <= 0) continue;
        if (area > fallbackArea) { fallbackArea = area; fallback = rect; }
        if (box.size() >= 5 && box.at(4).toString() == name && area > bestArea) {
            bestArea = area;
            best = rect;
        }
    }
    QRect box = bestArea > 0 ? best : fallback;

    const QImage frame = m_lastFrame;
    QImage face;
    if (box.isEmpty()) {
        face = frame;
    } else {
        /* 0x02 的框是 640x480 坐标；解码帧通常也是 640x480，按比例兜底 */
        const double sx = frame.width() / 640.0;
        const double sy = frame.height() / 480.0;
        QRect scaled(qRound(box.x() * sx), qRound(box.y() * sy),
                     qRound(box.width() * sx), qRound(box.height() * sy));
        const int mx = qMax(8, scaled.width() * 3 / 10);
        const int my = qMax(8, scaled.height() * 3 / 10);
        const QRect crop = scaled.adjusted(-mx, -my, mx, my).intersected(frame.rect());
        if (crop.width() < 8 || crop.height() < 8) return QByteArray();
        face = frame.copy(crop);
    }
    if (face.width() > 640) {
        face = face.scaledToWidth(640, Qt::SmoothTransformation);
    }
    QByteArray out;
    QBuffer buffer(&out);
    if (!buffer.open(QIODevice::WriteOnly)) return QByteArray();
    if (!face.save(&buffer, "JPEG", 85)) return QByteArray();
    return out;
}

void MainWindow::pushFacebank(bool autoSync)
{
    if (!m_cloudOn || !m_cloudOn->isChecked()) {
        if (!autoSync) statusError(QStringLiteral("请先启用云签到"));
        return;
    }
    if (!m_link->isConnected()) {
        if (!autoSync) statusError(QStringLiteral("请先连接板端"));
        return;
    }
    if (!m_cloud->configured()) {
        if (!autoSync) statusError(QStringLiteral("请先填写服务器与 Token"));
        return;
    }
    if (m_link->dbBusy()) {
        if (autoSync) {
            QTimer::singleShot(1500, this, [this]() {
                if (m_cloudAutoSync->isChecked()) pushFacebank(true);
            });
        } else {
            statusError(QStringLiteral("板端库任务忙，请稍后再试"));
        }
        return;
    }

    m_dbAutoUp = autoSync;
    m_dbUploadAfterExport = true;
    m_dbSyncPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                       .filePath(QStringLiteral("facecheck_sync.bin"));
    QFile::remove(m_dbSyncPath);
    appendLog(autoSync ? QStringLiteral("☁ 名单已变化，自动同步人脸库…")
                       : QStringLiteral("☁ 从板端导出人脸库…"));
    m_dbLabel->setText(QStringLiteral("正在从板端导出人脸库…"));
    m_exportBtn->setEnabled(false);
    m_importBtn->setEnabled(false);
    m_link->dbExport(m_dbSyncPath);
}

void MainWindow::pullFacebank()
{
    if (!m_cloudOn || !m_cloudOn->isChecked()) {
        statusError(QStringLiteral("请先启用云签到"));
        return;
    }
    if (!m_link->isConnected()) {
        statusError(QStringLiteral("请先连接板端"));
        return;
    }
    if (!m_cloud->configured()) {
        statusError(QStringLiteral("请先填写服务器与 Token"));
        return;
    }
    if (m_link->dbBusy()) {
        statusError(QStringLiteral("板端库任务忙，请稍后再试"));
        return;
    }
    if (QMessageBox::warning(
            this, QStringLiteral("从云端同步人脸库"),
            QStringLiteral("将【覆盖】板端现有名单与特征。\n"
                           "板端会自动把当前库备份为 face_bak.db / names.bak。\n\n继续？"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    m_exportBtn->setEnabled(false);
    m_importBtn->setEnabled(false);
    m_dbLabel->setText(QStringLiteral("正在从云端下载人脸库…"));
    appendLog(QStringLiteral("☁ 正在从云端下载人脸库…"));
    m_cloud->pullSnapshot();
}

/* ---------------- 交互逻辑 ---------------- */

void MainWindow::refreshPorts()
{
    const QString keep = m_serialBox->currentData().toString();
    m_serialBox->clear();
    const QVector<SerialPortInfo> ports = enumerateSerialPorts();
    for (const SerialPortInfo &p : ports) {
        m_serialBox->addItem(p.friendly.isEmpty() ? p.port
                                                  : QStringLiteral("%1").arg(p.friendly),
                             p.port);
        if (!p.likelyBoard) {
            m_serialBox->setItemData(m_serialBox->count() - 1,
                                     QStringLiteral("疑似非本板（CH340 等）：打开会拉 DTR/RTS 复位板子"),
                                     Qt::ToolTipRole);
        }
    }
    if (m_serialBox->count() == 0) {
        m_serialBox->addItem(QStringLiteral("（未发现串口）"), QString());
    }
    if (!keep.isEmpty()) {
        const int idx = m_serialBox->findData(keep);
        if (idx >= 0) m_serialBox->setCurrentIndex(idx);
    }
    /* 默认优先选疑似本板的 USB 口 */
    for (int i = 0; i < ports.size(); i++) {
        if (ports.at(i).likelyBoard) { m_serialBox->setCurrentIndex(i); break; }
    }
}

void MainWindow::updateIfaceWidgets()
{
    const bool usb = m_ifaceBox->currentData().toString() == QLatin1String("usb");
    m_hostEdit->setVisible(!usb);
    m_portSpin->setVisible(!usb);
    m_serialBox->setVisible(usb);
    m_refreshPortsBtn->setVisible(usb);
}

void MainWindow::onConnectClicked()
{
    if (m_link->isConnected()) {
        m_link->close();
        return;
    }
    QString err;
    const bool usb = m_ifaceBox->currentData().toString() == QLatin1String("usb");
    bool ok;
    if (usb) {
        const QString port = m_serialBox->currentData().toString();
        if (port.isEmpty()) { statusError(QStringLiteral("没有可用串口（板子的 HS USB 插了吗？）")); return; }
        appendLog(QStringLiteral("连接 USB %1 …").arg(port));
        ok = m_link->openSerial(port, &err);
    } else {
        appendLog(QStringLiteral("连接 TCP %1:%2 …").arg(m_hostEdit->text()).arg(m_portSpin->value()));
        ok = m_link->openTcp(m_hostEdit->text().trimmed(), quint16(m_portSpin->value()), &err);
    }
    if (!ok) {
        statusError(err);
        appendLog(QStringLiteral("✗ %1").arg(err));
    }
}

void MainWindow::statusError(const QString &msg)
{
    m_sbLog->setText(QStringLiteral("⚠ %1").arg(msg));
    appendLog(QStringLiteral("⚠ %1").arg(msg));
}

void MainWindow::appendLog(const QString &line)
{
    m_logView->appendPlainText(QStringLiteral("[%1] %2")
                                   .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), line));
}

void MainWindow::onResultFrame(const QJsonObject &obj)
{
    m_view->setResult(obj);
    updateVoteDiag(obj);

    if (obj.contains(QStringLiteral("boxes"))) {
        m_lastBoxes = obj.value(QStringLiteral("boxes")).toArray();
        m_lastBoxesMs = QDateTime::currentMSecsSinceEpoch();
    }
    if (obj.contains(QStringLiteral("lib_n"))) {
        m_lastLibN = obj.value(QStringLiteral("lib_n")).toInt();
    }

    const QString ev = obj.value(QStringLiteral("event")).toString();
    if (ev == QLatin1String("registered") || ev == QLatin1String("deleted")
        || ev == QLatin1String("list") || ev == QLatin1String("error")) {
        const QString ack = obj.value(QStringLiteral("ack")).toString();
        const QString msg = obj.value(QStringLiteral("msg")).toString();
        if (!msg.isEmpty()) {
            appendLog(QStringLiteral("← %1: %2").arg(ack.isEmpty() ? ev : ack, msg));
        }
        /* 名单变化后自动刷新一次；云签到开启时把新库同步到云端 */
        if (ev == QLatin1String("registered") || ev == QLatin1String("deleted")) {
            cmd(cmdObj(QStringLiteral("list")));
            if (m_cloudOn->isChecked() && m_cloudAutoSync->isChecked() && !m_pcTestMode) {
                m_autoSyncRetry = false;   /* 新的名单变化：重试额度重置 */
                /* 等在途结果/落盘稳定后再导出，避免与注册收尾抢 storage */
                QTimer::singleShot(900, this, [this]() { pushFacebank(true); });
            }
        }
    }
    if (ev == QLatin1String("match") && m_cloudOn->isChecked() && !m_pcTestMode) {
        onCloudMatch(obj);
    }
    syncFromResult(obj);
}

/* 板端每个比对帧带 votes/need/win（票仓按"人"归并后的票数）。
   灰区里"raw 不够高 + 票数凑不满"会让板端一直停在 tracking 不报名字——
   没有这行诊断就只能靠猜，所以把它显示出来并在卡住时告警。 */
void MainWindow::updateVoteDiag(const QJsonObject &obj)
{
    const QString ev = obj.value(QStringLiteral("event")).toString();
    if (!obj.contains(QStringLiteral("votes"))) {
        return;
    }
    const int votes = obj.value(QStringLiteral("votes")).toInt();
    const int need = obj.value(QStringLiteral("need")).toInt();
    const int win = obj.value(QStringLiteral("win")).toInt();
    const int rid = obj.value(QStringLiteral("rid")).toInt();
    const double raw = obj.value(QStringLiteral("sim_raw")).toDouble();
    const double sim = obj.value(QStringLiteral("sim")).toDouble();
    const int stable = obj.value(QStringLiteral("stable")).toInt();
    const QString name = obj.value(QStringLiteral("match")).toString();

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_diagLabel && now >= m_diagNextMs) {
        m_diagNextMs = now + 500;
        m_diagLabel->setText(QStringLiteral(
            "实时投票：结论 %1%2 · sim %3 · raw %4 · 票数 %5/%6 · 窗口 %7 帧 · top1槽 %8 · stable %9")
            .arg(ev,
                 name.isEmpty() ? QString() : QStringLiteral("（%1）").arg(name))
            .arg(sim, 0, 'f', 3).arg(raw, 0, 'f', 3)
            .arg(votes).arg(need).arg(win).arg(rid).arg(stable));
    }
    m_sbLog->setText(QStringLiteral("票 %1/%2 · raw %3").arg(votes).arg(need).arg(raw, 0, 'f', 3));

    if (ev == QLatin1String("tracking") && win >= 5) {
        if (m_stuckSinceMs == 0) {
            m_stuckSinceMs = now;
        } else if (now - m_stuckSinceMs > 5000 && now - m_lastStuckLogMs > 30000) {
            m_lastStuckLogMs = now;
            appendLog(QStringLiteral("⚠ 识别卡在投票：raw %1 票数 %2/%3（窗口已满仍未确认）")
                          .arg(raw, 0, 'f', 3).arg(votes).arg(need));
        }
    } else {
        m_stuckSinceMs = 0;
    }
}

void MainWindow::syncFromResult(const QJsonObject &obj)
{
    /* 命令 ack 里带阈值/相机状态：回填控件（避免与控制面板显示不一致） */
    m_syncing = true;

    const QString ack = obj.value(QStringLiteral("ack")).toString();
    const QString msg = obj.value(QStringLiteral("msg")).toString();

    if (ack == QLatin1String("mode") || ack == QLatin1String("start")) {
        if (obj.contains(QStringLiteral("thr_lo"))) m_loS->setValue(obj.value(QStringLiteral("thr_lo")).toDouble());
    }
    if (ack == QLatin1String("cam") && obj.contains(QStringLiteral("gain_max"))) {
        m_gainMax = obj.value(QStringLiteral("gain_max")).toInt();
        m_expMax = obj.value(QStringLiteral("exp_max")).toInt();
        m_gainS->setRange(0, m_gainMax);
        m_expS->setRange(0, m_expMax);
    }
    if (obj.contains(QStringLiteral("gain")) && obj.value(QStringLiteral("gain")).isDouble()) {
        const int g = obj.value(QStringLiteral("gain")).toInt();
        if (m_gainS->value() != g && !m_gainS->isSliderDown()) m_gainS->setValue(g);
        m_gainLbl->setText(QStringLiteral("%1 / %2").arg(g).arg(m_gainMax));
    }
    if (obj.contains(QStringLiteral("exp"))) {
        const int e = obj.value(QStringLiteral("exp")).toInt();
        if (m_expS->value() != e && !m_expS->isSliderDown()) m_expS->setValue(e);
        m_expLbl->setText(QStringLiteral("%1 / %2").arg(e).arg(m_expMax));
    }
    /* 不回填 mirror：板端 mirror 是 imgpipe 的 hflip，YUV420 下固定 false；
       相机页的“镜像”是 ISP HFLIP，状态由 QSettings + 下发维护。 */
    if (obj.contains(QStringLiteral("ae"))) m_aeChk->setChecked(obj.value(QStringLiteral("ae")).toInt() != 0);
    if (obj.contains(QStringLiteral("awb"))) m_awbChk->setChecked(obj.value(QStringLiteral("awb")).toInt() != 0);
    if (obj.contains(QStringLiteral("night"))) m_nightChk->setChecked(obj.value(QStringLiteral("night")).toInt() != 0);
    if (obj.contains(QStringLiteral("face_night"))) m_faceNightChk->setChecked(obj.value(QStringLiteral("face_night")).toInt() != 0);
    if (obj.contains(QStringLiteral("dn"))) m_dnBox->setCurrentIndex(qBound(0, obj.value(QStringLiteral("dn")).toInt(), 3));
    if (obj.contains(QStringLiteral("cam_fps"))) {
        const int idx = m_camFpsBox->findData(obj.value(QStringLiteral("cam_fps")).toInt());
        if (idx >= 0) m_camFpsBox->setCurrentIndex(idx);
    }

    /* 名单表格 */
    if (obj.contains(QStringLiteral("lib"))) {
        const QJsonArray lib = obj.value(QStringLiteral("lib")).toArray();
        const QJsonArray feats = obj.value(QStringLiteral("lib_feats")).toArray();
        m_roster->setRowCount(lib.size());
        for (int i = 0; i < lib.size(); i++) {
            m_roster->setItem(i, 0, new QTableWidgetItem(lib.at(i).toString()));
            m_roster->setItem(i, 1, new QTableWidgetItem(
                                        QString::number(i < feats.size() ? feats.at(i).toInt() : 0)));
        }
        if (!lib.isEmpty()) {
            appendLog(QStringLiteral("名单：%1 人 / %2 条特征")
                          .arg(lib.size()).arg(obj.value(QStringLiteral("lib_n")).toInt()));
        }
    }

    /* link 强制态回填 + 流开关状态 */
    if (ack == QLatin1String("link") && !msg.isEmpty() && msg != QLatin1String("h264-only")) {
        const int eq = msg.indexOf(QStringLiteral("link="));
        if (eq >= 0) {
            const QString v = msg.mid(eq + 5).section(QLatin1Char(' '), 0, 0);
            const int idx = m_linkModeBox->findText(v);
            if (idx >= 0) m_linkModeBox->setCurrentIndex(idx);
        }
        appendLog(QStringLiteral("链路：%1").arg(msg));
    }
    if (ack == QLatin1String("stream") && !msg.isEmpty()) {
        const bool on = msg.startsWith(QStringLiteral("stream=on")) || msg.contains(QStringLiteral("on"));
        setStreamUi(on && msg.startsWith(QStringLiteral("stream=on")));
    }
    /* H.264 固件的 stream ack：无 msg，字段为 bitrate/qp_min/qp_max/i_period */
    if (ack == QLatin1String("stream") && obj.contains(QStringLiteral("bitrate"))) {
        const int br = obj.value(QStringLiteral("bitrate")).toInt();
        m_brSpin->setValue(qBound(25, br / 1000, 2500));
        m_qpMinSpin->setValue(obj.value(QStringLiteral("qp_min")).toInt());
        m_qpMaxSpin->setValue(obj.value(QStringLiteral("qp_max")).toInt());
        m_iPeriodSpin->setValue(obj.value(QStringLiteral("i_period")).toInt());
        m_streamOn = obj.value(QStringLiteral("value")).toBool();
        appendLog(QStringLiteral("编码：%1 kbps / QP%2-%3 / I=%4")
                      .arg(m_brSpin->value())
                      .arg(m_qpMinSpin->value())
                      .arg(m_qpMaxSpin->value())
                      .arg(m_iPeriodSpin->value()));
    }

    m_syncing = false;
}

void MainWindow::setStreamUi(bool on)
{
    m_streamOn = on;
}

void MainWindow::saveScreenshot()
{
    const QImage img = m_view->snapshot();
    if (img.isNull()) {
        statusError(QStringLiteral("当前没有画面可截图"));
        return;
    }
    const QString dir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("shots"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(
        QStringLiteral("shot_%1.png").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")));
    if (img.save(path, "PNG")) {
        appendLog(QStringLiteral("✓ 截图已保存：%1").arg(path));
        m_sbLog->setText(QStringLiteral("截图已保存：%1").arg(path));
    } else {
        statusError(QStringLiteral("截图保存失败：%1").arg(path));
    }
}
