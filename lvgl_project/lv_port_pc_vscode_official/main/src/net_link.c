/*
 * net_link.c —— JCLINK 客户端实现（非阻塞 TCP，主循环轮询）
 */
#include "net_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <time.h>

#define RETRY_PERIOD_MS 1000
#define CONNECT_TIMEOUT_MS 2000

static struct {
    int      fd;
    char     host[64];
    int      port;
    uint8_t *rx;                 /* 帧解析累积缓冲 */
    size_t   rx_len;
    uint8_t *tx;                 /* 发不出去时暂存（命令帧很小） */
    size_t   tx_len, tx_off;
    uint32_t seq;
    bool     want_connect;
    int64_t  next_retry_ms;
    int64_t  connect_deadline_ms;
    bool     connecting;
    char     detail[96];

    nl_result_cb_t on_result;
    nl_image_cb_t  on_image;
    nl_log_cb_t    on_log;
    nl_state_cb_t  on_state;
    void *arg;
} s_nl = { .fd = -1 };

/* ---------------- 时间 / crc ---------------- */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint16_t crc16_ccitt(const uint8_t *d, size_t n, uint16_t crc)
{
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static void set_state(bool connected, const char *detail)
{
    snprintf(s_nl.detail, sizeof(s_nl.detail), "%s", detail ? detail : "");
    if (s_nl.on_state) {
        s_nl.on_state(connected, s_nl.detail, s_nl.arg);
    }
}

/* ---------------- 连接管理 ---------------- */
static void close_socket(void)
{
    if (s_nl.fd >= 0) {
        close(s_nl.fd);
        s_nl.fd = -1;
    }
    s_nl.rx_len = s_nl.tx_len = s_nl.tx_off = 0;
    s_nl.connecting = false;
}

static void try_connect(void)
{
    if (!s_nl.want_connect) {
        return;
    }
    if (s_nl.fd >= 0 && !s_nl.connecting) {
        return;                     /* 已经连上了 */
    }
    int64_t ms = now_ms();
    if (!s_nl.connecting) {
        if (ms < s_nl.next_retry_ms) {
            return;
        }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            s_nl.next_retry_ms = ms + RETRY_PERIOD_MS;
            return;
        }
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons((uint16_t)s_nl.port);
        if (inet_pton(AF_INET, s_nl.host, &a.sin_addr) != 1) {
            close(fd);
            s_nl.next_retry_ms = ms + RETRY_PERIOD_MS * 5;
            set_state(false, "bad host");
            return;
        }
        int r = connect(fd, (struct sockaddr *)&a, sizeof(a));
        if (r == 0) {
            s_nl.fd = fd;
            s_nl.next_retry_ms = 0;
            set_state(true, "connected");
            return;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            s_nl.next_retry_ms = ms + RETRY_PERIOD_MS;
            set_state(false, "connect failed");
            return;
        }
        s_nl.fd = fd;               /* 连接中 */
        s_nl.connecting = true;
        s_nl.connect_deadline_ms = ms + CONNECT_TIMEOUT_MS;
        set_state(false, "connecting...");
        return;
    }

    /* 等待连接完成 */
    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_ZERO(&ef);
    FD_SET(s_nl.fd, &wf);
    FD_SET(s_nl.fd, &ef);
    struct timeval tv = { 0, 0 };
    int r = select(s_nl.fd + 1, NULL, &wf, &ef, &tv);
    if (r <= 0) {
        if (ms > s_nl.connect_deadline_ms) {
            close_socket();
            s_nl.next_retry_ms = ms + RETRY_PERIOD_MS;
            set_state(false, "connect timeout");
        }
        return;
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (getsockopt(s_nl.fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        close_socket();
        s_nl.next_retry_ms = ms + RETRY_PERIOD_MS;
        set_state(false, "connect refused");
        return;
    }
    s_nl.connecting = false;
    s_nl.next_retry_ms = 0;
    set_state(true, "connected");
}

/* ---------------- 发送 ---------------- */
static int flush_tx(void)
{
    while (s_nl.fd >= 0 && s_nl.tx_off < s_nl.tx_len) {
        ssize_t n = send(s_nl.fd, s_nl.tx + s_nl.tx_off, s_nl.tx_len - s_nl.tx_off, MSG_NOSIGNAL);
        if (n > 0) {
            s_nl.tx_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return 0;               /* 稍后再发 */
        }
        close_socket();
        set_state(false, "send error");
        return -1;
    }
    s_nl.tx_len = s_nl.tx_off = 0;
    return 0;
}

int net_link_send_cmd(const char *json)
{
    if (s_nl.fd < 0 || s_nl.connecting) {
        return -1;
    }
    size_t len = strlen(json);
    if (len == 0 || len > NET_LINK_MAX_PAYLOAD) {
        return -1;
    }
    if (s_nl.tx_len - s_nl.tx_off + len + 10 > NET_LINK_RX_BUF) {
        return -1;
    }
    /* 先把残留发掉，避免帧序错乱 */
    flush_tx();
    if (s_nl.fd < 0) {
        return -1;
    }
    s_nl.tx_len = s_nl.tx_off = 0;

    uint8_t *f = s_nl.tx;
    s_nl.seq = (s_nl.seq % 255) + 1;
    f[0] = 'J';
    f[1] = 'C';
    f[2] = NL_TYPE_CMD;
    f[3] = (uint8_t)s_nl.seq;
    uint32_t l32 = (uint32_t)len;
    f[4] = (uint8_t)(l32 & 0xFF);
    f[5] = (uint8_t)((l32 >> 8) & 0xFF);
    f[6] = (uint8_t)((l32 >> 16) & 0xFF);
    f[7] = (uint8_t)((l32 >> 24) & 0xFF);
    memcpy(f + 8, json, len);
    uint16_t crc = crc16_ccitt(f + 2, 6 + len, 0xFFFF);
    f[8 + len] = (uint8_t)(crc & 0xFF);
    f[9 + len] = (uint8_t)((crc >> 8) & 0xFF);
    s_nl.tx_len = 10 + len;
    s_nl.tx_off = 0;
    return flush_tx();
}

/* ---------------- 接收 / 解析 ---------------- */
static void dispatch(uint8_t type, const uint8_t *payload, uint32_t len)
{
    switch (type) {
    case NL_TYPE_RESULT: {
        char *js = (char *)payload;      /* payload 后面留了 1 字节 \0 */
        js[len] = '\0';
        if (s_nl.on_result) s_nl.on_result(js, s_nl.arg);
        break;
    }
    case NL_TYPE_IMAGE: {
        if (len < 8) break;
        nl_image_t img;
        img.fmt    = payload[0];
        img.width  = (int)(payload[2] | (payload[3] << 8));
        img.height = (int)(payload[4] | (payload[5] << 8));
        img.seq    = (uint16_t)(payload[6] | (payload[7] << 8));
        img.data   = payload + 8;
        img.len    = len - 8;
        if (s_nl.on_image) s_nl.on_image(&img, s_nl.arg);
        break;
    }
    case NL_TYPE_LOG: {
        char *ln = (char *)payload;
        ln[len] = '\0';
        if (s_nl.on_log) s_nl.on_log(ln, s_nl.arg);
        break;
    }
    default:
        break;
    }
}

static void parse_rx(void)
{
    size_t off = 0;
    while (s_nl.rx_len - off >= 8) {
        uint8_t *p = s_nl.rx + off;
        if (p[0] != 'J' || p[1] != 'C') {         /* 丢到下一个 magic */
            uint8_t *m = memchr(s_nl.rx + off + 1, 'J', s_nl.rx_len - off - 1);
            if (!m) {
                off = s_nl.rx_len > 1 ? s_nl.rx_len - 1 : s_nl.rx_len;
                break;
            }
            off = (size_t)(m - s_nl.rx);
            continue;
        }
        uint8_t type = p[2];
        uint32_t len = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        if (len == 0 || len > NET_LINK_MAX_PAYLOAD) {
            off += 2;
            continue;
        }
        /* 需要多留 1 字节给 \0（缓冲有富余，见 init） */
        if (s_nl.rx_len - off < 10u + len) {
            break;
        }
        uint16_t crc = (uint16_t)(p[8 + len] | (p[9 + len] << 8));
        if (crc16_ccitt(p + 2, 6 + len, 0xFFFF) != crc) {
            off += 2;                            /* CRC 错，滑窗 */
            continue;
        }
        dispatch(type, p + 8, len);
        off += 10u + len;
    }
    if (off > 0) {
        memmove(s_nl.rx, s_nl.rx + off, s_nl.rx_len - off);
        s_nl.rx_len -= off;
    }
}

static void do_recv(void)
{
    while (s_nl.fd >= 0) {
        size_t space = NET_LINK_RX_BUF - s_nl.rx_len - 1;
        if (space < 4096) {
            /* 缓冲满（正常不会，除非帧异常）：尝试解析腾出空间 */
            parse_rx();
            space = NET_LINK_RX_BUF - s_nl.rx_len - 1;
            if (space < 4096) {
                s_nl.rx_len = 0;
                space = NET_LINK_RX_BUF - 1;
            }
        }
        ssize_t n = recv(s_nl.fd, s_nl.rx + s_nl.rx_len, space, 0);
        if (n > 0) {
            s_nl.rx_len += (size_t)n;
            parse_rx();
            continue;
        }
        if (n == 0) {
            close_socket();
            set_state(false, "closed by board");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        close_socket();
        set_state(false, "recv error");
        return;
    }
}

/* ---------------- 对外 API ---------------- */
void net_link_set_callbacks(nl_result_cb_t on_result, nl_image_cb_t on_image,
                            nl_log_cb_t on_log, nl_state_cb_t on_state, void *arg)
{
    s_nl.on_result = on_result;
    s_nl.on_image = on_image;
    s_nl.on_log = on_log;
    s_nl.on_state = on_state;
    s_nl.arg = arg;
}

void net_link_start(const char *host, int port)
{
    if (!s_nl.rx) {
        s_nl.rx = (uint8_t *)malloc(NET_LINK_RX_BUF);
    }
    if (!s_nl.tx) {
        s_nl.tx = (uint8_t *)malloc(NET_LINK_MAX_PAYLOAD);
    }
    snprintf(s_nl.host, sizeof(s_nl.host), "%s", host ? host : "192.168.33.2");
    s_nl.port = port > 0 ? port : 3333;
    s_nl.want_connect = true;
    s_nl.next_retry_ms = 0;
    close_socket();
    snprintf(s_nl.detail, sizeof(s_nl.detail), "%s:%d", s_nl.host, s_nl.port);
    set_state(false, s_nl.detail);
}

void net_link_stop(void)
{
    s_nl.want_connect = false;
    close_socket();
    set_state(false, "stopped");
}

bool net_link_connected(void)
{
    return s_nl.fd >= 0 && !s_nl.connecting;
}

const char *net_link_detail(void)
{
    return s_nl.detail;
}

void net_link_tick(void)
{
    if (!s_nl.rx) {
        return;
    }
    try_connect();
    if (s_nl.fd >= 0 && !s_nl.connecting) {
        flush_tx();
        do_recv();
    }
}
