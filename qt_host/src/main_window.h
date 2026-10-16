#pragma once
/*
 * main_window.h —— 主窗口（深色专业工具风）
 *   左：视频区（0x05 实时画面 + 检测框叠加 + 截图）
 *   右：控制面板 Tab（流 / 名单 / 识别 / 相机 / 备份）
 *   顶：接口选择（TCP / USB）+ 连接 + 链路徽标 + 流开关 + 截图 + 缩放
 *   底：状态栏（链路 / 流量 / 帧率 / 丢帧 / 板端日志）
 */
#include <QJsonObject>
#include <QMainWindow>

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
    QPushButton *m_streamBtn = nullptr;
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
    QCheckBox *m_aeChk = nullptr;
    QCheckBox *m_awbChk = nullptr;
    QCheckBox *m_nightChk = nullptr;
    QCheckBox *m_faceNightChk = nullptr;

    /* 备份 */
    QPushButton *m_exportBtn = nullptr;
    QPushButton *m_importBtn = nullptr;
    QProgressBar *m_dbBar = nullptr;
    QLabel *m_dbLabel = nullptr;

    QPlainTextEdit *m_logView = nullptr;

    bool m_syncing = false;      /* 结果帧回填控件时抑制回发命令 */
    bool m_streamOn = false;
    int  m_gainMax = 192, m_expMax = 994;
};
