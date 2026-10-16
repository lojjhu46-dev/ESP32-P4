#include "main_window.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
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
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QTabWidget>
#include <QVBoxLayout>

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

    setWindowTitle(QStringLiteral("JCLINK 上位机 · ESP32-P4 边缘人脸识别终端"));
    resize(1380, 860);

    buildUi();
    refreshPorts();
    updateIfaceWidgets();

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
    connect(m_link, &LinkSession::videoFrame, this, [this](const VideoFrame &f) { m_view->setFrame(f); });
    connect(m_link, &LinkSession::logLine, this, [this](const QString &l) {
        m_sbLog->setText(l);
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
                             .arg(st.lastRaw ? QStringLiteral("raw") : QStringLiteral("jpeg"))
                             .arg(st.imgFps, 0, 'f', 1)
                             .arg(st.imgKbps, 0, 'f', 0));
        m_sbDrop->setText(QStringLiteral("丢帧 %1").arg(st.imgDrop));
        /* 角标带上实际格式：一眼看出"UI 选 JPEG 但板子在推 raw"这类不一致 */
        m_view->setStatsText(QStringLiteral("%1 %2 fps  %3 KB/s  丢%4  %5")
                                 .arg(st.lastRaw ? QStringLiteral("raw") : QStringLiteral("jpeg"),
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

    m_streamBtn = button(QStringLiteral("开始流"), QStringLiteral("primary"));
    m_streamBtn->setFixedWidth(90);
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
    topL->addWidget(m_streamBtn);
    topL->addWidget(m_shotBtn);
    topL->addWidget(new QLabel(QStringLiteral("缩放")));
    topL->addWidget(m_zoomBox);

    /* ===== 主体：左视频 + 右控制面板 ===== */
    m_view = new VideoView;

    m_logView = new QPlainTextEdit;
    m_logView->setReadOnly(true);
    m_logView->setMaximumBlockCount(500);
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
    tabs->addTab(buildStreamTab(), QStringLiteral("流"));
    tabs->addTab(buildRosterTab(), QStringLiteral("名单"));
    tabs->addTab(buildRecognizeTab(), QStringLiteral("识别"));
    tabs->addTab(buildCameraTab(), QStringLiteral("相机"));
    tabs->addTab(buildBackupTab(), QStringLiteral("备份"));
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
    connect(m_streamBtn, &QPushButton::clicked, this, [this]() {
        if (!m_link->isConnected()) { statusError(QStringLiteral("请先连接")); return; }
        const bool want = !m_streamOn;
        QJsonObject o = cmdObj(QStringLiteral("stream"), QStringLiteral("value"),
                               want ? QStringLiteral("on") : QStringLiteral("off"));
        if (want) {
            /* ⚠️ 必须带 fmt/fps/quality：板端的 fmt 可能停在 raw（例如刚用过"抓一帧 raw"），
               只发 stream on 会继续推 raw 614KB/帧 ≈2.3fps，看起来就是"预览只有两三帧"。
               2026-10-16 实测踩到：UI 显示 JPEG、实际在推 raw。 */
            o.insert(QStringLiteral("fps"), m_fpsSpin->value());
            o.insert(QStringLiteral("quality"), m_qSpin->value());
            o.insert(QStringLiteral("fmt"), m_fmtBox->currentData().toString());
        }
        cmd(o);
        appendLog(QStringLiteral("→ %1（%2 %3fps q%4）")
                      .arg(want ? QStringLiteral("开始流") : QStringLiteral("停止流"),
                           m_fmtBox->currentData().toString())
                      .arg(m_fpsSpin->value())
                      .arg(m_qSpin->value()));
    });

    /* 快捷键 */
    auto *s = new QAction(this);
    s->setShortcut(QKeySequence(QStringLiteral("S")));
    connect(s, &QAction::triggered, this, &MainWindow::saveScreenshot);
    addAction(s);
    auto *sp = new QAction(this);
    sp->setShortcut(QKeySequence(Qt::Key_Space));
    connect(sp, &QAction::triggered, this, [this]() { m_streamBtn->click(); });
    addAction(sp);
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
    m_fpsSpin->setFixedWidth(64);

    m_qS = new QSlider(Qt::Horizontal);
    m_qS->setRange(1, 100);
    m_qS->setValue(75);
    m_qSpin = new QSpinBox;
    m_qSpin->setRange(1, 100);
    m_qSpin->setValue(75);
    m_qSpin->setFixedWidth(64);

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
    m_srcBox->addItem(QStringLiteral("PC 送帧 (pc)"), QStringLiteral("pc"));
    m_swapChk = new QCheckBox(QStringLiteral("raw 显示前交换字节序（颜色不对时勾选）"));

    v->addWidget(formRow(QStringLiteral("帧率"), fpsRow));
    v->addWidget(formRow(QStringLiteral("质量"), qRow));
    v->addWidget(formRow(QStringLiteral("格式"), m_fmtBox));
    v->addWidget(formRow(QStringLiteral("图像源"), m_srcBox));
    v->addWidget(m_swapChk);

    auto *apply = button(QStringLiteral("下发流参数"), QStringLiteral("primary"));
    v->addWidget(apply);
    /* ① 路线（2026-10-16）：raw 连续流只有 ~1.4MB/s（≈2.3fps），但单帧 0.5s 就能拿到，
       颜色标定/取证足够——一键"开流( raw )→取一帧→停流→存 PNG"。 */
    auto *grabRawBtn = button(QStringLiteral("抓一帧 raw → PNG"));
    v->addWidget(grabRawBtn);
    v->addStretch(1);
    v->addWidget(dimLabel(QStringLiteral("提示：raw 档 614KB/帧，连续推流实测 ~1.4MB/s（≈2.3fps）；\n"
                                         "「抓一帧 raw」会自动开流→取帧→停流（约 0.5s）。\n"
                                         "JPEG 档可稳定 ~8-11fps，日常预览用它。")));

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
    m_regShots->setFixedWidth(70);
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
    v->addWidget(dimLabel(QStringLiteral("注册：点「注册」后请站到镜头前（可多帧）；\n"
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
    m_dedupSpin->setFixedWidth(90);
    m_multiSpin = new QSpinBox;
    m_multiSpin->setRange(1, 3);
    m_multiSpin->setValue(2);
    m_multiSpin->setFixedWidth(90);

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
    v->addWidget(dimLabel(QStringLiteral("⚠️ esp-dl 特征提取在 P4 上非确定性：绝对阈值跨开机不可移植，\n"
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

    m_mirrorChk = new QCheckBox(QStringLiteral("镜像（mirror）"));
    m_aeChk = new QCheckBox(QStringLiteral("自动曝光 AE"));
    m_awbChk = new QCheckBox(QStringLiteral("自动白平衡 AWB"));
    m_nightChk = new QCheckBox(QStringLiteral("夜视阶梯（暗度降帧+降噪）"));
    m_faceNightChk = new QCheckBox(QStringLiteral("低光照停人脸（夜视预览优先）"));
    m_mirrorChk->setChecked(true);
    m_aeChk->setChecked(true);
    m_awbChk->setChecked(true);
    m_nightChk->setChecked(true);
    m_faceNightChk->setChecked(true);

    v->addWidget(formRow(QStringLiteral("增益"), gRow));
    v->addWidget(formRow(QStringLiteral("曝光"), eRow));
    v->addWidget(formRow(QStringLiteral("帧率"), m_camFpsBox));
    v->addWidget(formRow(QStringLiteral("降噪"), m_dnBox));
    v->addWidget(m_mirrorChk);
    v->addWidget(m_aeChk);
    v->addWidget(m_awbChk);
    v->addWidget(m_nightChk);
    v->addWidget(m_faceNightChk);

    auto *apply = button(QStringLiteral("下发相机参数"), QStringLiteral("primary"));
    v->addWidget(apply);
    v->addStretch(1);
    v->addWidget(dimLabel(QStringLiteral("⚠️ 手动改帧率会让 AE/夜视阶梯让位；恢复顺序：先 fps 再 night/ae。")));

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
    connect(apply, &QPushButton::clicked, this, [this]() {
        QJsonObject o = cmdObj(QStringLiteral("cam"));
        o.insert(QStringLiteral("mirror"), m_mirrorChk->isChecked());
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
    m_dbLabel = dimLabel(QStringLiteral("空闲（导入前板端会自动把现有库备份到 face_bak.db / names.bak）"));

    v->addWidget(m_exportBtn);
    v->addWidget(m_importBtn);
    v->addWidget(m_dbBar);
    v->addWidget(m_dbLabel);
    v->addStretch(1);
    v->addWidget(dimLabel(QStringLiteral("备份内容 = 名表 + face.db（id 与名字映射逐位一致，可跨设备迁移）；\n"
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

    const QString ev = obj.value(QStringLiteral("event")).toString();
    if (ev == QLatin1String("registered") || ev == QLatin1String("deleted")
        || ev == QLatin1String("list") || ev == QLatin1String("error")) {
        const QString ack = obj.value(QStringLiteral("ack")).toString();
        const QString msg = obj.value(QStringLiteral("msg")).toString();
        if (!msg.isEmpty()) {
            appendLog(QStringLiteral("← %1: %2").arg(ack.isEmpty() ? ev : ack, msg));
        }
        /* 名单变化后自动刷新一次 */
        if (ev == QLatin1String("registered") || ev == QLatin1String("deleted")) {
            cmd(cmdObj(QStringLiteral("list")));
        }
    }
    syncFromResult(obj);
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
    if (obj.contains(QStringLiteral("mirror"))) m_mirrorChk->setChecked(obj.value(QStringLiteral("mirror")).toInt() != 0);
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
    if (ack == QLatin1String("link") && !msg.isEmpty()) {
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

    m_syncing = false;
}

void MainWindow::setStreamUi(bool on)
{
    m_streamOn = on;
    m_streamBtn->setText(on ? QStringLiteral("停止流") : QStringLiteral("开始流"));
    m_streamBtn->setProperty("cls", on ? QStringLiteral("danger") : QStringLiteral("primary"));
    m_streamBtn->style()->polish(m_streamBtn);
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
