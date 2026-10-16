/*
 * main.cpp —— qt_host 入口：深色专业工具风主题
 *   ESP32-P4 边缘人脸识别终端上位机（JCLINK over TCP / USB CDC）
 */
#include <QApplication>
#include <QFont>
#include <QNetworkProxy>

#include "main_window.h"
#include "selftest.h"

static const char *kQss = R"QSS(
QWidget { background: #14161A; color: #E6EAF0; font-size: 12px; }
QMainWindow, QDialog, QMessageBox { background: #16191E; }
QToolTip { background: #1C1F26; color: #E6EAF0; border: 1px solid #2A303A; padding: 4px; }
QLabel[cls="dim"] { color: #8A93A3; }
QLabel[cls="badge"] { border-radius: 4px; padding: 3px 10px; font-weight: 600; }
QLabel[cls="badge"][state="off"] { background: #2A303A; color: #8A93A3; }
QLabel[cls="badge"][state="usb"] { background: #1E88A8; color: #EAF7FB; }
QLabel[cls="badge"][state="tcp"] { background: #2E7D5B; color: #EAFBEF; }
QLabel[cls="status"] { color: #8A93A3; padding: 0 8px; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #1C1F26; border: 1px solid #2A303A; border-radius: 4px; padding: 3px 6px;
    selection-background-color: #1E88A8; selection-color: #EAF7FB;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus, QComboBox:focus { border: 1px solid #35C2E8; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #1C1F26; border: 1px solid #2A303A; selection-background-color: #1E88A8; }
QPushButton { background: #1C1F26; border: 1px solid #2A303A; border-radius: 4px; padding: 5px 12px; }
QPushButton:hover { background: #22262F; border-color: #38404C; }
QPushButton:pressed { background: #171A20; }
QPushButton:disabled { color: #5A6270; border-color: #22262F; }
QPushButton[cls="primary"] { background: #1E88A8; border-color: #35C2E8; color: #EAF7FB; font-weight: 600; }
QPushButton[cls="primary"]:hover { background: #2499BC; }
QPushButton[cls="danger"] { background: #7A2E2E; border-color: #FF5C5C; color: #FFE9E9; }
QPushButton[cls="danger"]:hover { background: #8E3535; }
QTabWidget::pane { border: 1px solid #2A303A; border-radius: 4px; top: -1px; }
QTabBar::tab { background: #1C1F26; color: #8A93A3; padding: 6px 13px;
    border: 1px solid #2A303A; border-bottom: none;
    border-top-left-radius: 4px; border-top-right-radius: 4px; }
QTabBar::tab:selected { background: #22262F; color: #35C2E8; }
QSlider::groove:horizontal { height: 4px; background: #2A303A; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #1E88A8; border-radius: 2px; }
QSlider::handle:horizontal { width: 12px; margin: -5px 0; background: #35C2E8; border-radius: 6px; }
QTableWidget { background: #1C1F26; gridline-color: #2A303A; border: 1px solid #2A303A; }
QHeaderView::section { background: #22262F; color: #8A93A3; border: none; padding: 4px; }
QTableWidget::item:selected { background: #1E88A8; color: #EAF7FB; }
QPlainTextEdit[cls="log"] { background: #101216; border: 1px solid #2A303A; color: #B9C2CF;
    font-family: Consolas, "Cascadia Mono", monospace; font-size: 11px; }
QProgressBar { background: #1C1F26; border: 1px solid #2A303A; border-radius: 4px; text-align: center; }
QProgressBar::chunk { background: #35C2E8; border-radius: 3px; }
QSplitter::handle { background: #14161A; width: 8px; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #38404C; border-radius: 3px; background: #1C1F26; }
QCheckBox::indicator:checked { background: #35C2E8; border-color: #35C2E8; }
QStatusBar { background: #1C1F26; border-top: 1px solid #2A303A; }
QStatusBar::item { border: none; }
)QSS";

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qt_host"));
    app.setOrganizationName(QStringLiteral("jc_p4_app"));

    QFont f(QStringLiteral("Microsoft YaHei UI"));
    f.setPointSize(9);
    app.setFont(f);
    app.setStyleSheet(QString::fromUtf8(kQss));

    /* 板子在直连局域网里：**必须禁用系统代理**，否则 QTcpSocket 会走系统代理设置，
       直连 192.168.33.2 时报 "The proxy type is invalid for this operation"
       （2026-10-16 实测踩到）。 */
    QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    /* 无界面自检（脚本化验收用）：qt_host.exe --selftest [--tcp|--serial ...] */
    if (app.arguments().contains(QStringLiteral("--selftest"))) {
        return runSelfTest(app.arguments());
    }

    MainWindow w;
    w.show();
    return app.exec();
}
