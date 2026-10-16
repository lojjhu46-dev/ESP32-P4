#pragma once
/*
 * image_frame.h —— 0x05 图像帧 → QImage
 *
 * 载荷：8 字节头 {fmt u8, flags u8, width u16, height u16, seq u16} + 数据
 *   fmt=0 JPEG（Qt 内置插件解码）
 *   fmt=1 RGB565 裸帧（stride 可能 > width*2，行距 = 数据长度/高度）
 */
#include <QImage>
#include <QString>

#include "jclink.h"

struct VideoFrame {
    QImage image;
    int    fmt = 0;
    int    width = 0;
    int    height = 0;
    int    seq = 0;
    int    bytes = 0;          /* 图像数据字节数（不含 8B 头） */
    qint64 tsMs = 0;
    bool   valid() const { return !image.isNull(); }
};

bool decodeImageFrame(const QByteArray &payload, bool swapBytes, VideoFrame *out, QString *err);
