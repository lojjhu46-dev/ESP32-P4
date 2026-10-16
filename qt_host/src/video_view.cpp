#include "video_view.h"

#include <QJsonArray>
#include <QPainter>
#include <QPaintEvent>

#include <cmath>

VideoView::VideoView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(480, 360);
    setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
}

void VideoView::setZoom(int z)
{
    m_zoom = qBound(0, z, 3);
    update();
}

void VideoView::clear()
{
    m_frame = VideoFrame{};
    m_result = QJsonObject{};
    update();
}

void VideoView::setFrame(const VideoFrame &f)
{
    m_frame = f;
    update();
}

QRect VideoView::targetRect() const
{
    if (m_frame.image.isNull()) {
        return rect();
    }
    const int iw = m_frame.image.width();
    const int ih = m_frame.image.height();
    if (iw <= 0 || ih <= 0) {
        return rect();
    }
    if (m_zoom > 0) {
        return QRect((width() - iw * m_zoom) / 2, (height() - ih * m_zoom) / 2,
                     iw * m_zoom, ih * m_zoom);
    }
    const double sx = double(width()) / iw;
    const double sy = double(height()) / ih;
    const double s = qMin(sx, sy);
    const int w = int(iw * s);
    const int h = int(ih * s);
    return QRect((width() - w) / 2, (height() - h) / 2, w, h);
}

void VideoView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0x0B, 0x0D, 0x11));

    if (m_frame.image.isNull()) {
        p.setPen(QColor(0x8A, 0x93, 0xA3));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("未收到画面\n\n· 点「连接」后打开「流」开关\n· USB 需插板子的 High speed USB 口并选对 COM 口"));
        return;
    }

    const QRect dst = targetRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform, m_zoom == 0);
    p.drawImage(dst, m_frame.image);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);

    if (m_zoom == 0) {
        p.setPen(QPen(QColor(0x2A, 0x30, 0x3A), 1));
        p.drawRect(dst.adjusted(0, 0, -1, -1));
    }

    drawOverlay(p, dst);

    /* 右上角统计角标 */
    if (!m_stats.isEmpty()) {
        const QFontMetrics fm(font());
        const int tw = fm.horizontalAdvance(m_stats) + 16;
        const QRect badge(dst.right() - tw - 8, dst.top() + 8, tw, fm.height() + 10);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(badge, 4, 4);
        p.setPen(QColor(0xE6, 0xEA, 0xF0));
        p.drawText(badge, Qt::AlignCenter, m_stats);
    }
}

void VideoView::drawOverlay(QPainter &p, const QRect &dst) const
{
    const int iw = m_frame.image.width();
    const int ih = m_frame.image.height();
    if (iw <= 0 || ih <= 0) {
        return;
    }
    const double sx = double(dst.width()) / iw;
    const double sy = double(dst.height()) / ih;
    auto mapX = [&](double x) { return dst.left() + x * sx; };
    auto mapY = [&](double y) { return dst.top() + y * sy; };

    const QJsonArray boxes = m_result.value(QStringLiteral("boxes")).toArray();
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const QJsonValue &v : boxes) {
        const QJsonArray b = v.toArray();
        if (b.size() < 4) {
            continue;
        }
        const double x0 = b.at(0).toDouble(), y0 = b.at(1).toDouble();
        const double x1 = b.at(2).toDouble(), y1 = b.at(3).toDouble();
        const QString name = b.size() > 4 && b.at(4).isString() ? b.at(4).toString() : QString();
        const double sim = b.size() > 5 ? b.at(5).toDouble() : 0.0;
        const bool hit = !name.isEmpty();

        const QRectF r(mapX(x0), mapY(y0), mapX(x1) - mapX(x0), mapY(y1) - mapY(y0));
        p.setPen(QPen(hit ? QColor(0x3D, 0xDC, 0x84) : QColor(0xF2, 0xB8, 0x4B), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);

        if (hit) {
            const QString label = QStringLiteral("%1  %2").arg(name).arg(sim, 0, 'f', 2);
            const QFontMetrics fm(p.font());
            const QRect lr(int(r.left()), int(r.top()) - fm.height() - 6,
                           fm.horizontalAdvance(label) + 12, fm.height() + 6);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x1E, 0x88, 0xA8, 220));
            p.drawRoundedRect(lr, 3, 3);
            p.setPen(QColor(0xE6, 0xEA, 0xF0));
            p.drawText(lr, Qt::AlignCenter, label);
        }
    }

    /* 关键点（若板端开了 DIAG_KP：10 个整数 = 5 个点） */
    const QJsonArray kp = m_result.value(QStringLiteral("kp")).toArray();
    if (kp.size() >= 10) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xFF, 0x5C, 0x5C));
        for (int i = 0; i + 1 < kp.size(); i += 2) {
            const double x = mapX(kp.at(i).toDouble());
            const double y = mapY(kp.at(i + 1).toDouble());
            p.drawEllipse(QPointF(x, y), 2.5, 2.5);
        }
    }
    p.setRenderHint(QPainter::Antialiasing, false);
}

QImage VideoView::snapshot() const
{
    if (m_frame.image.isNull()) {
        return QImage();
    }
    /* 按画面原始分辨率出图 + 叠加层（便于取证/测量） */
    QImage out = m_frame.image.convertToFormat(QImage::Format_RGB888);
    QPainter p(&out);
    drawOverlay(p, QRect(0, 0, out.width(), out.height()));
    p.end();
    return out;
}
