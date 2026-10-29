#pragma once
/*
 * jclink.h —— JCLINK v1 协议（与板端 main/app_usb_link.c、pc_host/protocol.py 同源）
 *
 *   帧： 'J' 'C' | type(1) | seq(1) | len(u32 LE) | payload | crc16(2 LE)
 *        crc16 = CRC-16/CCITT-FALSE(init 0xFFFF)，范围 = type+seq+len+payload
 *
 * type：0x01 JPEG(PC→板) 0x02 结果JSON 0x03 命令JSON 0x04 日志 0x05 图像 0x06 块数据
 * 详见 docs/library/usb_link_protocol.md
 */
#include <QByteArray>
#include <QVector>
#include <QtGlobal>

namespace jcl {

constexpr quint8 T_JPEG   = 0x01;
constexpr quint8 T_RESULT = 0x02;
constexpr quint8 T_CMD    = 0x03;
constexpr quint8 T_LOG    = 0x04;
constexpr quint8 T_IMAGE  = 0x05;
constexpr quint8 T_BLOCK  = 0x06;
constexpr quint8 T_H264   = 0x07;   /* 板端 app_h264enc：20B 头 + Annex-B NAL */

constexpr quint32 MAX_PAYLOAD = 1024u * 1024u;   /* 与板端 JCL_MAX_PAYLOAD 一致 */

quint16 crc16Ccitt(const quint8 *data, int len, quint16 crc = 0xFFFF);
quint32 crc32(const QByteArray &data);           /* 标准 CRC-32/ISO-HDLC（= zlib.crc32、板端 esp_rom_crc32_le） */

QByteArray buildFrame(quint8 type, const QByteArray &payload, quint8 seq);

struct Frame {
    quint8     type = 0;
    quint8     seq = 0;
    QByteArray payload;
};

/* 增量解析器：喂入任意分片，吐出完整帧；CRC 不符则丢弃并重同步（滑 2 字节找 magic） */
class FrameParser
{
public:
    QVector<Frame> feed(const QByteArray &data);
    void reset() { m_buf.clear(); }
    int  buffered() const { return m_buf.size(); }

private:
    QByteArray m_buf;
};

/* ---- 0x05 图像帧载荷：8 字节头 + 数据 ---- */
constexpr int IMG_HDR_LEN   = 8;
constexpr int IMG_FMT_JPEG  = 0;
constexpr int IMG_FMT_RGB565 = 1;

struct ImageHeader {
    int fmt = 0, flags = 0, width = 0, height = 0, seq = 0;
};
bool parseImageHeader(const QByteArray &payload, ImageHeader *out);

/* ---- 0x06 块分片：offset(u32 LE) + data（F10 备份导出/导入用）---- */
bool parseBlockHeader(const QByteArray &payload, quint32 *offset, QByteArray *data);

} // namespace jcl
