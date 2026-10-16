#pragma once
#include <QStringList>

/* 无界面自检：连接（TCP/USB）→ 开流 → 收 N 秒 → 打印统计 → 退出码 0/1
   用法：qt_host.exe --selftest [--tcp 192.168.33.2:3333 | --serial COM10]
                      [--seconds 8] [--images 20] [--fps 10] [--quality 75]
                      [--fmt jpeg|raw] [--swap] */
int runSelfTest(const QStringList &args);
