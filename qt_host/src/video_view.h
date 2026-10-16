#pragma once
/*
 * video_view.h —— 视频预览控件：等比 letterbox 显示 0x05 帧 + 检测框/名字叠加 + 缩放 + 截图
 *
 * ⚠️ 坐标系：板端结果帧的 boxes/kp 是**原图坐标**（esp-dl 的 detector 已把结果映射回输入帧，
 *    见 app_faceai handle_frame 的 img 直接是整帧），摄像头链路下与 0x05 画面同为 640x480，
 *    因此叠加框可直接按显示矩形等比换算，无需额外缩放。
 */
#include <QImage>
#include <QJsonObject>
#include <QWidget>

#include "image_frame.h"

class VideoView : public QWidget
{
    Q_OBJECT
public:
    explicit VideoView(QWidget *parent = nullptr);

    void setFrame(const VideoFrame &f);
    void setResult(const QJsonObject &obj) { m_result = obj; update(); }
    void setStatsText(const QString &s) { m_stats = s; update(); }
    void setZoom(int z);                       /* 0=适应窗口, 1/2/3 = 倍数 */
    int  zoom() const { return m_zoom; }
    QImage snapshot() const;                   /* 含叠加层的截图 */
    void clear();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QRect  targetRect() const;
    void   drawOverlay(QPainter &p, const QRect &dst) const;

    VideoFrame  m_frame;
    QJsonObject m_result;
    QString     m_stats;
    int         m_zoom = 0;
};
