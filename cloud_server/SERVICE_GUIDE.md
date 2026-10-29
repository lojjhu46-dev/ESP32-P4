# FaceCheck 云签到服务 · 部署与使用说明

> 版本：v1（2026-10-24） · 运行环境：Linux + systemd，纯 Python 3.6 标准库（服务器具体发行版与规格不在本仓库记录）
> 本文档同时适用于生产实例（8080）和独立测试实例（18080）。

---

## 1. 服务定位

人脸**比对在 ESP32-P4 板端完成**；云端**不做 ML、不做特征比对**，只负责：

- 接收 Qt 上位机转发的签到事件（`event=match`），可带一张 JPEG 抓拍；
- 保存板端 `db_export` 的 JCDF 人脸库**字节级快照**，供设备备份/下发；
- 提供 Web 后台、CSV 导出、人员名单、设备心跳与照片查看。

```
P4 板端（检测 + MFN 特征 + 1:N）
   └─ 0x02 event=match ──> Qt 上位机「云」页 ──HTTP POST──> 云端 :8080
   └─ /storage/face.db + names.json ──0x06──> Qt ──POST──> 云端快照
```

> 云端不合并特征、不重排 `face.db` 的 id；多设备共库前需同一人实测相似度
> （esp-dl 特征跨 boot/跨设备存在非确定性）。

---

## 2. 当前部署信息

| 项 | 生产实例 | 测试实例 |
|---|---|---|
| 公网地址 | `http://<CLOUD_IP>:8080` | 仅 `127.0.0.1:18080`（SSH 隧道访问） |
| 服务 | `facecheck.service` | `facecheck-test.service` |
| 程序目录 | `/opt/facecheck` | `/opt/facecheck_test` |
| 数据目录 | `/var/lib/facecheck` | `/var/lib/facecheck_test` |
| 配置/Token | `/etc/facecheck.json` | `/etc/facecheck_test.json` |
| 绑定 | `0.0.0.0:8080` | `127.0.0.1:18080`（不开放防火墙） |
| 时区 | Asia/Shanghai | Asia/Shanghai |

- 生产库当前人员：`personA`、`personB`；快照为板端 45259 B 的生产库。
- 测试实例是**完全独立**的 SQLite/照片/Token；测试数据不进入生产库。
- 云服务器为小规格单机（内存紧张、默认无 swap）；服务常驻 RSS 约 20–25 MB。具体规格与登录账号见本机私有速查 `docs/private/`（不入库）。

---

## 3. 目录与数据

```
/opt/facecheck/                 # 程序（app.py、db.py、facebank.py、web/）
/var/lib/facecheck/
  facecheck.db                  # SQLite：events / facebank_snapshots / persons / devices
  photos/YYYY/MM/DD/*.jpg       # 签到抓拍（默认保留 90 天）
/etc/facecheck.json             # 配置：token、cookie_secret、data_dir、端口等（0640 root:facecheck）
```

SQLite 使用 WAL；每个请求独立连接 + `busy_timeout=5000`。人脸库快照保存最近 5 个/设备。

---

## 4. 部署与升级

### 4.1 生产实例（首次/升级）

```bash
# Windows 侧打包（仓库根目录）
tar czf _dev/facecheck.tgz --exclude='__pycache__' --exclude='*.pyc' cloud_server
scp _dev/facecheck.tgz <部署账号>@<CLOUD_IP>:/tmp/
ssh <部署账号>@<CLOUD_IP> "tar xzf /tmp/facecheck.tgz -C /opt && bash /opt/cloud_server/install.sh"
```

安装脚本会：
- 创建 `facecheck` 用户与 `/var/lib/facecheck`（0750）；
- 首次生成 `/etc/facecheck.json` 并打印一次 Token（重复安装保留旧 Token）；
- 写入/重启 `facecheck.service`（`MemoryMax=256M`、`ProtectSystem=strict`）；
- 生产实例尝试放行 firewalld 8080（安全组仍需在阿里云控制台放行）。

### 4.2 独立测试实例（推荐用于 PC 送图/回归）

```bash
ssh <部署账号>@<CLOUD_IP> "tar xzf /tmp/facecheck.tgz -C /opt && \
  bash /opt/cloud_server/install.sh --test"
```

- 生成 `facecheck-test.service`、`/etc/facecheck_test.json`、`/var/lib/facecheck_test`；
- 只绑 `127.0.0.1:18080`，不改防火墙、不影响生产 8080；
- PC 访问：

```bash
ssh -L 18080:127.0.0.1:18080 <部署账号>@<CLOUD_IP> -N
# 浏览器/Qt 访问 http://127.0.0.1:18080
```

### 4.3 升级注意

- 只覆盖程序目录，不要覆盖 `/var/lib/facecheck` 与 `/etc/facecheck.json`；
- `install.sh` 可重复执行，不会重新生成已有 Token；
- 升级后 `systemctl restart facecheck`，用 `/api/health` 验证。

---

## 5. 配置与安全

`/etc/facecheck.json` 关键项：

| 键 | 默认 | 说明 |
|---|---|---|
| `host` / `port` | 0.0.0.0 / 8080 | 测试实例为 127.0.0.1 / 18080 |
| `token` | 随机 32 字符 | API `X-Token` / Web 登录 |
| `cookie_secret` | 随机 64 hex | Web 会话签名 |
| `data_dir` | `/var/lib/facecheck` | SQLite + 照片 |
| `dedup_window_s` | 生产 1800 / 测试 10 | 同人+设备滚动去重窗口；命中则事件与抓拍都不落库，被压制的尝试不延长窗口 |
| `snapshots_keep` | 5 | 每设备保留快照版本数 |
| `photo_keep_days` | 90 | 照片保留天数（janitor 每日清理） |
| `max_body_bytes` | 8 MB | 请求体上限 |
| `photo_max_bytes` | 2 MB | 单张 JPEG 上限 |
| `session_hours` | 12 | Web 登录有效期 |
| `max_concurrent` | 8 | 并发请求上限 |

**安全要点**

- 当前是 **HTTP + Token**；公网只建议在阿里云 **SWAS/安全组** 放行 8080，来源限制为办公网出口 IP。
- Token 存在 `/etc/facecheck.json` 与 Qt QSettings；不要提交到 Git、不要贴到聊天/日志。
- 出口 IP 会随网络切换变化（如 `<EGRESS_IP_A>` → `<EGRESS_IP_B>`）；公网 8080 超时先核对当前出口 IP，再改 SWAS 防火墙。
- 照片属于个人敏感数据；保留期默认 90 天，按需调整 `photo_keep_days`。
- 建议定期轮换 Token：修改配置 → `systemctl restart facecheck` → 同步更新 Qt「云」页。

---

## 6. Web 后台

打开 `http://<公网IP>:8080/`（测试实例走隧道后 `http://127.0.0.1:18080/`），用 Token 登录。

- **签到记录**：按起止时间/姓名/设备筛选；查看抓拍；导出 CSV（UTF-8 BOM，Excel 可直接打开）。
- **人员**：由板端快照解析的人员与有效特征数；未知姓名事件会标「未入库」。
- **人脸库**：快照版本、CRC32、人数/特征数；下载最新/指定版本 `.bin`；也可上传 `.bin`。
- **设备**：Qt 心跳（设备名、lib_n、最近在线）。
- 每 15 秒自动刷新签到记录；照片弹窗需登录。

---

## 7. HTTP API

鉴权：`X-Token: <TOKEN>` 或 `Authorization: Bearer <TOKEN>`；Web 登录后使用 HttpOnly cookie。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 无鉴权；`{ok,version,time,db_ok,rss_kb}` |
| POST | `/api/login` / `/api/logout` | Token 换/清 cookie |
| POST | `/api/events` | 签到事件；只接受 `event=match`；`photo_b64` 可选 |
| GET | `/api/events` | `from/to/person/device/limit/offset` |
| GET | `/api/events.csv` | CSV（UTF-8 BOM） |
| GET | `/api/persons` | 人员、特征数、签到次数/最近签到 |
| POST | `/api/facebank` | body=JCDF 原始字节；头 `X-Device`、`X-CRC32` |
| GET | `/api/facebank/latest[?id=|?device=]` | 下载快照原字节 |
| GET | `/api/facebank/meta` | 快照版本列表 |
| GET | `/api/photo/<event_id>` | 鉴权后返回 JPEG |
| POST | `/api/heartbeat` | `{device,lib_n,fw,link}` |
| GET | `/api/devices` | 设备在线列表 |

**签到事件示例**

```bash
TOKEN='<从 /etc/facecheck.json 读取>'
curl -X POST http://127.0.0.1:8080/api/events \
  -H "X-Token: $TOKEN" -H "Content-Type: application/json" \
  -d '{"event":"match","device":"pc-qt-test","person":"cloudtest",
       "sim":0.92,"sim_raw":0.92,"stable":1,"faces":1,
       "src":"pc","link":"usb-cdc","fps":4.8,"infer_ms":95,"lib_n":1,
       "ts":"2026-10-24T23:32:14+0800","photo_b64":"<base64 JPEG>"}'
```

**上传人脸库快照**

```bash
CRC=$(python3 -c 'import zlib,sys;print(zlib.crc32(open(sys.argv[1],"rb").read())&0xffffffff)' face.bin)
curl -X POST http://127.0.0.1:8080/api/facebank \
  -H "X-Token: $TOKEN" -H "Content-Type: application/octet-stream" \
  -H "X-Device: LAPTOP-XXXX" -H "X-CRC32: $CRC" \
  --data-binary @face.bin
```

**错误码**：400 参数/JSON、401 Token/cookie、404 资源不存在、413 体积超限、422 JCDF/CRC 校验失败、500 内部错误、503 并发满。

---

## 8. Qt 上位机对接

生产 Qt（`qt_host/build`）「云」页：

- 启用云签到、服务器 URL、Token、设备名、签到抓拍、注册/删除后自动同步；
- 「保存配置并测试连接」→ 探活；「上传板端人脸库到云端」/「从云端同步人脸库到板端」；
- 心跳每 60 秒；上报失败事件落盘，重启后补发；401 会暂停队列等改 Token。

测试 Qt（`qt_host/build-pctest`，`-PcTest`）另有「PC测试」页：

- 配合测试固件 `APP_FACEAI_PC_TEST=y`：进入测试库 → 选官方脸图 → 注册 → 反复送图识别；
- **送出的原图直接作为 `photo_b64`**；测试事件走独立测试云与独立 spool；
- 测试模式激活时不会把 match / 自动库同步发到生产云；退出后回摄像头与生产库。

---

## 9. 运维

```bash
systemctl status facecheck
journalctl -u facecheck -f
curl -s http://127.0.0.1:8080/api/health
ss -lntp | grep 8080
systemctl show -p MemoryCurrent facecheck
df -h /var/lib/facecheck
```

**备份**（建议每日/每周）

```bash
sqlite3 /var/lib/facecheck/facecheck.db ".backup '/var/lib/facecheck/backup.db'"
tar czf /root/facecheck_data_$(date +%F).tgz \
  /var/lib/facecheck/facecheck.db /var/lib/facecheck/photos /etc/facecheck.json
```

**恢复**：`systemctl stop facecheck` → 还原 DB/照片/配置 → `systemctl start facecheck`。

**清理**：照片默认 90 天自动清理；如需手工清理，先停服务再删 `photos/` 下过期目录。
**不要**直接删除 `facecheck.db`（会丢全部签到记录）。

---

## 10. 测试实例（18080）

```bash
systemctl status facecheck-test
journalctl -u facecheck-test -f
curl -s http://127.0.0.1:18080/api/health
```

- 独立 Token/数据/照片，生产 8080 不受影响；
- PC 端通过 `ssh -L 18080:127.0.0.1:18080 <部署账号>@<CLOUD_IP> -N` 访问；
- 不用时：`systemctl stop facecheck-test`（不影响生产）。

---

## 11. 故障排查

| 现象 | 排查 |
|---|---|
| 公网 8080 超时 | 先看本机出口 IP（`curl https://ipinfo.io/ip`）；在 SWAS「防火墙」把 8080 源 IP 改成当前 `/32`；再看 `ss -lntp` 与 `systemctl status facecheck` |
| `/api/*` 返回 401 | `X-Token` 是否与 `/etc/facecheck.json` 一致；Web 是否登录/cookie 过期 |
| `/api/facebank` 422 | `face.bin` 不是 JCDF、CRC 头与实际不符、长度/版本错误；用 Qt 导出的原包 |
| 413 | 请求体 >8MB 或照片 >2MB；压缩照片/减小体量 |
| Qt 显示连接正常但无签到 | 云页是否勾选「启用云签到」；板端是否真的产生 `event=match`；是否在 PC 测试模式（测试模式不会发生产云） |
| 照片打不开/404 | 事件没有照片；照片超过保留期已被清理；未登录（401） |
| `database is locked` | 瞬时并发写；服务已用 WAL + busy_timeout；避免同时手工 sqlite 写 |
| 服务反复重启 | `journalctl -u facecheck -n 50`；常见为配置 token/secret 为空或数据目录权限 |
| 内存不足 | `MemoryMax=256M`；查看 `MemoryCurrent`；小内存机器建议加 swap |

---

## 12. 已知限制与验收清单

- **云端不做特征比对**：只保存/下发板端快照；不合并、不重排 id。
- esp-dl 特征跨 boot/跨设备存在非确定性；多设备共用同一库前必须实测同一人相似度。
- 公网是明文 HTTP，靠 Token + 来源 IP 限制；如需更强安全，后续可加域名 + HTTPS。
- 家宽出口 IP 会变化，防火墙规则需同步；可考虑 RAM 子账号 + 定时脚本自动更新。

**快速验收**

1. `/api/health` 200；
2. Web 登录，人员/快照/设备可见；
3. Qt 云页「测试连接」正常，心跳出现在「设备」；
4. 真实或 PC 送图产生 match，事件+照片出现在「签到记录」；
5. CSV 可打开（UTF-8 BOM）；
6. 测试实例 18080 与生产 8080 数据互不影响。

---

*程序源码与测试方案：`cloud_server/`（`app.py`、`db.py`、`facebank.py`、`web/`、`TEST_PLAN_PC_IMAGE.md`）；硬件/固件说明见仓库 `docs/library/H264重构交接与阶段计划.md`。*
