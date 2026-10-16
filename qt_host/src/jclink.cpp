#include "jclink.h"

namespace jcl {

quint16 crc16Ccitt(const quint8 *data, int len, quint16 crc)
{
    while (len-- > 0) {
        crc ^= (quint16)(*data++) << 8;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 0x8000) ? quint16((crc << 1) ^ 0x1021) : quint16(crc << 1);
        }
    }
    return crc;
}

quint32 crc32(const QByteArray &data)
{
    static quint32 table[256];
    static bool inited = false;
    if (!inited) {
        for (quint32 i = 0; i < 256; i++) {
            quint32 c = i;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        inited = true;
    }
    quint32 c = 0xFFFFFFFFu;
    const quint8 *p = reinterpret_cast<const quint8 *>(data.constData());
    for (int i = 0; i < data.size(); i++) {
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

QByteArray buildFrame(quint8 type, const QByteArray &payload, quint8 seq)
{
    QByteArray f;
    f.reserve(8 + payload.size() + 2);
    f.append('J');
    f.append('C');
    f.append(char(type));
    f.append(char(seq));
    quint32 len = quint32(payload.size());
    f.append(char(len & 0xFF));
    f.append(char((len >> 8) & 0xFF));
    f.append(char((len >> 16) & 0xFF));
    f.append(char((len >> 24) & 0xFF));
    f.append(payload);
    quint16 crc = crc16Ccitt(reinterpret_cast<const quint8 *>(payload.constData()),
                             payload.size(), crc16Ccitt(reinterpret_cast<const quint8 *>(&f[2]), 6));
    f.append(char(crc & 0xFF));
    f.append(char((crc >> 8) & 0xFF));
    return f;
}

QVector<Frame> FrameParser::feed(const QByteArray &data)
{
    m_buf.append(data);
    QVector<Frame> out;
    int pos = 0;
    const int n = m_buf.size();
    const quint8 *base = reinterpret_cast<const quint8 *>(m_buf.constData());

    while (n - pos >= 8) {
        if (base[pos] != 'J' || base[pos + 1] != 'C') {
            pos++;
            continue;
        }
        const quint8 type = base[pos + 2];
        const quint8 seq  = base[pos + 3];
        const quint32 len = quint32(base[pos + 4])
                          | (quint32(base[pos + 5]) << 8)
                          | (quint32(base[pos + 6]) << 16)
                          | (quint32(base[pos + 7]) << 24);
        if (len == 0 || len > MAX_PAYLOAD) {
            pos += 2;                       /* 非法长度：滑 2 字节重找 magic */
            continue;
        }
        if (quint32(n - pos) < 8u + len + 2u) {
            break;                          /* 帧还没收全 */
        }
        const quint16 crc = crc16Ccitt(base + pos + 2, 6 + int(len));
        const quint16 rx  = quint16(base[pos + 8 + len]) | quint16(quint16(base[pos + 9 + len]) << 8);
        if (crc == rx) {
            Frame f;
            f.type = type;
            f.seq  = seq;
            f.payload = QByteArray(reinterpret_cast<const char *>(base + pos + 8), int(len));
            out.append(f);
            pos += 8 + int(len) + 2;
        } else {
            pos += 2;                       /* CRC 错：重同步 */
        }
    }
    if (pos > 0) {
        m_buf.remove(0, pos);
    }
    return out;
}

bool parseImageHeader(const QByteArray &payload, ImageHeader *out)
{
    if (payload.size() < IMG_HDR_LEN) {
        return false;
    }
    const quint8 *p = reinterpret_cast<const quint8 *>(payload.constData());
    out->fmt    = p[0];
    out->flags  = p[1];
    out->width  = int(quint16(p[2]) | quint16(quint16(p[3]) << 8));
    out->height = int(quint16(p[4]) | quint16(quint16(p[5]) << 8));
    out->seq    = int(quint16(p[6]) | quint16(quint16(p[7]) << 8));
    return true;
}

bool parseBlockHeader(const QByteArray &payload, quint32 *offset, QByteArray *data)
{
    if (payload.size() < 5) {              /* 4B 偏移 + 至少 1B 数据（板端不允许空载荷） */
        return false;
    }
    const quint8 *p = reinterpret_cast<const quint8 *>(payload.constData());
    *offset = quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
    *data = payload.mid(4);
    return true;
}

} // namespace jcl
