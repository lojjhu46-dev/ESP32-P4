#pragma once
/* PC 送图测试页（仅 QT_HOST_PC_TEST=ON 编译）。
 *
 * 配合测试固件 APP_FACEAI_PC_TEST=y：
 *   testlib on reset -> 切到 /storage/ftest.*
 *   src=pc -> 0x01 送图 -> 板端识别 -> match
 *   Qt 保存送出的原图，match 时直接作为 photo_b64 发测试云。
 * 生产固件/生产云不参与。
 */
#include <QByteArray>
#include <QJsonObject>
#include <QWidget>

class CloudClient;
class LinkSession;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;

class PcTestPage : public QWidget
{
    Q_OBJECT
public:
    explicit PcTestPage(LinkSession *link, QWidget *parent = nullptr);

signals:
    void testModeChanged(bool on);

private:
    void buildUi();
    void loadSettings();
    void saveSettings();
    void applyCloud();
    void enterTestLib();
    void exitTestLib();
    void resetTestLib();
    void browseImage();
    void loadImage(const QString &path);
    void registerFace();
    void recognizeFace();
    void tick();
    void sendImage();
    void onResult(const QJsonObject &obj);
    void onDbDone(bool ok, const QString &msg);
    void uploadSnapshot();
    void uploadMatch(const QJsonObject &obj);
    void testCloud();
    void log(const QString &s);
    void setStatus(const QString &s, bool err = false);
    bool ensureTestMode();
    QString tempBin() const;

    LinkSession *m_link = nullptr;
    CloudClient *m_cloud = nullptr;

    QLineEdit *m_url = nullptr;
    QLineEdit *m_token = nullptr;
    QLineEdit *m_device = nullptr;
    QLineEdit *m_facePath = nullptr;
    QLineEdit *m_name = nullptr;
    QSpinBox  *m_shots = nullptr;
    QPushButton *m_enterBtn = nullptr;
    QPushButton *m_exitBtn = nullptr;
    QPushButton *m_resetBtn = nullptr;
    QPushButton *m_browseBtn = nullptr;
    QPushButton *m_regBtn = nullptr;
    QPushButton *m_recBtn = nullptr;
    QPushButton *m_snapBtn = nullptr;
    QPushButton *m_testBtn = nullptr;
    QLabel *m_preview = nullptr;
    QLabel *m_status = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTimer *m_tick = nullptr;

    QByteArray m_image;
    bool m_testMode = false;
    bool m_registering = false;
    bool m_recognizing = false;
    bool m_matchUploaded = false;
    int m_sendLeft = 0;
    int m_sendCount = 0;
    QString m_exportPath;
};
