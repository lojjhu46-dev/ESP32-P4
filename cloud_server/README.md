# facecheck 云签到服务

> 中文部署/使用说明（服务器现场版，含 API、运维、排障与测试实例）：
> [SERVICE_GUIDE.md](SERVICE_GUIDE.md)

极轻量签到后端：**人脸比对留在 ESP32-P4 板端**，云端只接收 Qt 上位机转发的
`event=="match"` 签到事件、可选抓拍，以及板端 `db_export` 的 JCDF 人脸库快照。

- 纯 Python 3.6 标准库：`http.server` + `sqlite3`，无 pip 依赖、无 Nginx/Docker
- 常驻内存约 30–50 MB（systemd `MemoryMax=256M`）
- HTTP + Token；公网部署请用阿里云安全组限制来源 IP

## 目录

```
app.py             HTTP 服务、鉴权、路由
db.py              SQLite 连接与 DDL（WAL）
facebank.py        JCDF 信封 / face.db 解析（只读，不做比对）
web/               后台页面（原生 JS，无 CDN）
install.sh         服务器安装（用户、目录、systemd、防火墙）
test_client.py     本地/远程验收脚本
config.example.json
```

运行数据（默认 `/var/lib/facecheck/`）：

```
facecheck.db          events / facebank_snapshots / persons / devices
photos/YYYY/MM/DD/    签到抓拍（JPEG，默认保留 90 天）
```

## 安装（Alibaba Cloud Linux 3 / RHEL 8 系）

```bash
# Windows Git Bash 打包上传（服务器无 git）
tar czf facecheck.tgz cloud_server
scp facecheck.tgz root@<CLOUD_IP>:/tmp/
ssh root@<CLOUD_IP> "tar xzf /tmp/facecheck.tgz -C /opt && bash /opt/cloud_server/install.sh"
```

安装脚本会生成 `/etc/facecheck.json`（含 Token 与 cookie 密钥），并打印一次 Token；
重复执行不会覆盖已有 Token。随后在阿里云安全组放行 **8080**（仅办公网出口 IP）。

```bash
systemctl status facecheck
journalctl -u facecheck -f
curl http://127.0.0.1:8080/api/health
```

## 本地运行

```bash
cp cloud_server/config.example.json /tmp/facecheck.local.json
# 把 token / cookie_secret 改成随机值，data_dir 改成临时目录
python cloud_server/app.py --config /tmp/facecheck.local.json --port 18080
python cloud_server/test_client.py --url http://127.0.0.1:18080 --token <TOKEN>
```

`test_client.py` 默认使用 `_dev/face_db_before_r1.bin` 与 `pc_host/_noface.jpg`，
覆盖：未授权、登录 cookie、库上传/下载 CRC 往返、坏 CRC/坏信封、事件与照片、
去重、CSV BOM、心跳、9MB 超限。

## API 摘要

鉴权：`X-Token: <token>`（Qt/脚本）或 `POST /api/login` 换取的 `fc_sess` cookie（Web）。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/health` | 无鉴权 |
| POST | `/api/login` `/api/logout` | 登录/退出 |
| POST | `/api/events` | 签到事件（仅 `event=match`），`photo_b64` 可选 |
| GET | `/api/events` | 按 from/to/person/device/limit/offset 查询 |
| GET | `/api/events.csv` | CSV（UTF-8 BOM，Excel 可直接打开） |
| GET | `/api/persons` | 名单（从快照解析）+ 签到统计 |
| POST | `/api/facebank` | body=JCDF 原始字节；`X-CRC32`/`X-Device` 可选 |
| GET | `/api/facebank/latest[?id=|?device=]` | 下载快照（原字节） |
| GET | `/api/facebank/meta` | 版本列表（保留最近 5 个/设备） |
| GET | `/api/photo/<event_id>` | 鉴权后返回 JPEG |
| POST | `/api/heartbeat` | 设备在线心跳 |
| GET | `/api/devices` | 设备列表 |

## 重要语义

- **云端不比对特征**。esp-dl 特征跨 boot/跨设备存在非确定性，云端只做字节级快照
  保存与下发；多设备共库前必须用同一人实测相似度。
- 快照是整体覆盖的：云端从不合并特征、不重排 face.db 的 id（删除会留 id=0 空洞）。
- 下发到板端会覆盖现有库；板端导入前自动备份为 `face_bak.db` / `names.bak`。
- 未知姓名事件照常入库并标记 `unknown`，不会自动创建人员。
- 服务端按 `person+device` 做滚动去重：生产默认 **1800 s（半小时一次）**，测试实例 10 s；命中重复时事件与抓拍照片都不写入。板端另有 5 s 事件边沿去重（不受这里影响，实时 UI 仍每帧显示人脸）。窗口值通过 `/api/health` 的 `dedup_window_s` 暴露，上位机据此同步静默。

## 运维

```bash
journalctl -u facecheck --since today
systemctl restart facecheck
du -sh /var/lib/facecheck/photos
sqlite3 /var/lib/facecheck/facecheck.db 'select count(*) from events;'
```

照片默认保留 90 天（`photo_keep_days`），后台每小时级 janitor 清理并 checkpoint WAL。
1 GiB 无 swap 的机器建议额外创建 1G swapfile（见安装脚本末尾提示）。
