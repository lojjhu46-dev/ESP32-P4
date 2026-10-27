#pragma once
/*
 * main_window.h —— 主窗口（深色专业工具风）
 *   左：视频区（0x05 实时画面 + 检测框叠加 + 截图）
 *   右：控制面板 Tab（流 / 名单 / 识别 / 相机 / 备份）
 *   顶：接口选择（TCP / USB）+ 连接 + 链路徽标 + 流开关 + 截图 + 缩放
 *   底：状态栏（链路 / 流量 / 帧率 / 丢帧 / 板端日志）
 */
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>

class CloudClient;
class LinkSession;
class VideoView;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

    /* 命令行预置接口（2026-10-16）：qt_host.exe --serial COM5 / --tcp 192.168.33.2:3333
     * 传 autoConnect=true 则启动后立即连（省去手点接口/端口/连接三步）。 */
    void presetInterface(bool usb, const QString &serialPort, const QString &host,
                         quint16 port, bool autoConnect);

private:
    void buildUi();
    QWidget *buildStreamTab();
    QWidget *buildRosterTab();
    QWidget *buildRecognizeTab();
    QWidget *buildCameraTab();
    QWidget *buildBackupTab();
    QWidget *buildCloudTab();

    void applyCloudConfig(bool quiet = false);
    void onCloudMatch(const QJsonObject &obj);
    QByteArray cropFaceJpeg(const QString &name) const;
    void pushFacebank(bool autoSync);
    void pullFacebank();

    void refreshPorts();
    void updateIfaceWidgets();
    void onConnectClicked();
    void onResultFrame(const QJsonObject &obj);
    void syncFromResult(const QJsonObject &obj);
    void appendLog(const QString &line);
    void saveScreenshot();
    void setStreamUi(bool on);
    void statusError(const QString &msg);

    /* 便捷：发命令（定义在 .cpp：LinkSession 在此仅有前向声明） */
    void cmd(const QJsonObject &obj);

    LinkSession *m_link = nullptr;
    VideoView   *m_view = nullptr;

    /* 顶栏 */
    QComboBox   *m_ifaceBox = nullptr;
    QComboBox   *m_serialBox = nullptr;
    QLineEdit   *m_hostEdit = nullptr;
    QSpinBox    *m_portSpin = nullptr;
    QPushButton *m_connBtn = nullptr;
    QPushButton *m_refreshPortsBtn = nullptr;
    QLabel      *m_linkBadge = nullptr;
    QComboBox   *m_linkModeBox = nullptr;
    QPushButton *m_shotBtn = nullptr;
    QComboBox   *m_zoomBox = nullptr;

    /* 状态栏 */
    QLabel *m_sbDesc = nullptr;
    QLabel *m_sbLink = nullptr;
    QLabel *m_sbTraffic = nullptr;
    QLabel *m_sbFps = nullptr;
    QLabel *m_sbDrop = nullptr;
    QLabel *m_sbLog = nullptr;

    /* 流 */
    QSlider *m_fpsS = nullptr;
    QSpinBox *m_fpsSpin = nullptr;
    QSlider *m_qS = nullptr;
    QSpinBox *m_qSpin = nullptr;
    QComboBox *m_fmtBox = nullptr;
    QComboBox *m_srcBox = nullptr;
    QCheckBox *m_swapChk = nullptr;
    /* H.264 编码参数（仅 YUV420 固件；字段与 app_h264enc_cmd_cb 对齐） */
    QSpinBox *m_brSpin = nullptr;       /* kbps，下发时 ×1000 */
    QSpinBox *m_qpMinSpin = nullptr;
    QSpinBox *m_qpMaxSpin = nullptr;
    QSpinBox *m_iPeriodSpin = nullptr;

    /* 名单 */
    QTableWidget *m_roster = nullptr;
    QLineEdit *m_regName = nullptr;
    QComboBox *m_regShots = nullptr;

    /* 识别 */
    QCheckBox *m_detChk = nullptr;
    QCheckBox *m_matchChk = nullptr;
    QDoubleSpinBox *m_loS = nullptr;
    QDoubleSpinBox *m_hiS = nullptr;
    QDoubleSpinBox *m_fastS = nullptr;
    QSpinBox *m_dedupSpin = nullptr;
    QSpinBox *m_multiSpin = nullptr;

    /* 相机 */
    QSlider *m_gainS = nullptr;
    QSlider *m_expS = nullptr;
    QLabel  *m_gainLbl = nullptr;
    QLabel  *m_expLbl = nullptr;
    QComboBox *m_camFpsBox = nullptr;
    QComboBox *m_dnBox = nullptr;
    QCheckBox *m_mirrorChk = nullptr;
    QCheckBox *m_vflipChk = nullptr;
    QCheckBox *m_aeChk = nullptr;
    QCheckBox *m_awbChk = nullptr;
    QCheckBox *m_nightChk = nullptr;
    QCheckBox *m_faceNightChk = nullptr;

    /* 备份 */
    QPushButton *m_exportBtn = nullptr;
    QPushButton *m_importBtn = nullptr;
    QProgressBar *m_dbBar = nullptr;
    QLabel *m_dbLabel = nullptr;

    /* 云签到 */
    CloudClient *m_cloud = nullptr;
    QCheckBox   *m_cloudOn = nullptr;
    QLineEdit   *m_cloudUrl = nullptr;
    QLineEdit   *m_cloudToken = nullptr;
    QLineEdit   *m_cloudDevice = nullptr;
    QCheckBox   *m_cloudPhoto = nullptr;
    QCheckBox   *m_cloudAutoSync = nullptr;
    QLabel      *m_cloudState = nullptr;
    QPushButton *m_cloudTestBtn = nullptr;
    QPushButton *m_cloudPushBtn = nullptr;
    QPushButton *m_cloudPullBtn = nullptr;
    QLabel      *m_cloudLast = nullptr;
    QImage       m_lastFrame;
    QJsonArray   m_lastBoxes;
    qint64       m_lastFrameMs = 0;
    qint64       m_lastBoxesMs = 0;
    QHash<QString, qint64> m_cloudSeen;
    bool         m_dbAutoUp = false;
    bool         m_dbUploadAfterExport = false;
    bool         m_pcTestMode = false;      /* PC 测试页激活时阻断生产云上报 */
    QString      m_dbSyncPath;
    QString      m_cloudImportPath;
    int          m_lastLibN = 0;
    int          m_cloudHeartbeatFailures = 0;

    QPlainTextEdit *m_logView = nullptr;

    bool m_syncing = false;      /* 结果帧回填控件时抑制回发命令 */
    bool m_streamOn = false;
    bool m_streamBeforeGrab = false;   /* 抓 raw 前流是否开着（抓完自动恢复预览） */
    int  m_gainMax = 192, m_expMax = 994;
};
