#include "image_frame.h"

#include <QDateTime>

bool decodeImageFrame(const QByteArray &payload, bool swapBytes, VideoFrame *out, QString *err)
{
    jcl::ImageHeader h{};
    if (!jcl::parseImageHeader(payload, &h)) {
        if (err) *err = QStringLiteral("0x05 载荷不足 8 字节");
        return false;
    }
    out->fmt = h.fmt;
    out->width = h.width;
    out->height = h.height;
    out->seq = h.seq;
    out->bytes = payload.size() - jcl::IMG_HDR_LEN;
    out->tsMs = QDateTime::currentMSecsSinceEpoch();

    const QByteArray body = payload.mid(jcl::IMG_HDR_LEN);

    if (h.fmt == jcl::IMG_FMT_JPEG) {
        QImage img;
        if (!img.loadFromData(body, "JPEG")) {
            if (err) *err = QStringLiteral("JPEG 解码失败（%1 B）").arg(body.size());
            return false;
        }
        out->image = img;
        return true;
    }

    if (h.fmt == jcl::IMG_FMT_RGB565) {
        const int w = h.width;
        const int ht = h.height;
        if (w <= 0 || ht <= 0 || body.size() < w * ht * 2) {
            if (err) *err = QStringLiteral("RGB565 尺寸非法 %1x%2 / %3 B").arg(w).arg(ht).arg(body.size());
            return false;
        }
        const int srcStride = body.size() / ht;      /* 板端按 MCU 对齐，行距可能 > w*2 */
        QImage img(w, ht, QImage::Format_RGB16);
        const uchar *base = reinterpret_cast<const uchar *>(body.constData());
        for (int y = 0; y < ht; y++) {
            const uchar *src = base + qsizetype(y) * srcStride;
            uchar *dst = img.scanLine(y);
            if (swapBytes) {
                for (int x = 0; x < w; x++) {
                    dst[2 * x]     = src[2 * x + 1];
                    dst[2 * x + 1] = src[2 * x];
                }
            } else {
                memcpy(dst, src, size_t(w) * 2);
            }
        }
        out->image = img;
        return true;
    }

    if (err) *err = QStringLiteral("未知图像格式 fmt=%1").arg(h.fmt);
    return false;
}
