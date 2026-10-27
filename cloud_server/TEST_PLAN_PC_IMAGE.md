# PC 送图 → 板端识别 → 云签到 端到端测试方案

目的：在**不依赖现场光照/摄像头**的前提下，用 PC 送官方测试脸图，验证本次改动涉及的
"板端识别 → 0x02 → 云签到事件/照片/人脸库"链路；不破坏现有 personA/personB 人脸库。

> **2026-10-24 更新**：已实现固件测试库（`APP_FACEAI_PC_TEST`）+ Qt「PC测试」页
> （`QT_HOST_PC_TEST`）+ 独立测试云（`install.sh --test`，18080 loopback），完整流程见
> `docs/H264重构交接与阶段计划.md` §15.8。**当前生产固件已裁掉 PC 源（`src=pc` 被拒）**，
> 本文件下方的 `_dev/cloud_pc_e2e.py` 只适用于旧 PC 源生产固件；现应使用测试构建 +
> Qt 测试页，或先给脚本补 `testlib` 分支。

## 覆盖范围

| 层 | 是否覆盖 | 说明 |
|---|---|---|
| 板端 PC 源 JPEG 解码 + imgpipe + MFN/1:N | ✅ | 0x01 图片送检，官方 bill2 图 |
| 板端 `register` / `match` / `list` | ✅ | 注册测试人 `cloudtest`，识别后恢复原库 |
| 云端事件（含照片） | ✅ | 本地云实例，`photo_b64` 为实际送检图 |
| 云端库快照上传/CRC/人员解析 | ✅ | 测试库快照（含 cloudtest）上传后校验 |
| 云端去重、CSV、鉴权 | ✅ | 同一事件重复上报、CSV BOM、无 Token 401 |
| Qt CloudClient 传输（事件/上传/下载） | ✅（另项） | `_dev/cloudqt_test` 已验证；本轮不重复 |
| Qt 页面 match→裁剪抓拍 | ⚠️ 未覆盖 | 需要真人/光照；PC 图不经过 Qt 的 QImage 裁剪路径 |
| 生产云 <CLOUD_IP> | ✅（另项） | health/鉴权/公网已验收；本轮用本地实例避免污染 |

## 安全前置

1. **备份板端库**：测试脚本先 `db_backup` 到 `_dev/face_db_before_cloudtest.bin`。
2. **不执行 `clear`**；只注册/删除测试人，最后 `db_restore` 原库（覆盖式，板端自动 `.bak`）。
3. **测试用本地云**：`127.0.0.1:18080`，独立 `_dev/cloudtest` 数据目录；不写生产库。
4. **释放 COM5**：测试期间关闭 Qt（否则串口被占用）；测试结束重启 Qt。
5. 不改固件、不改运行中的 H.264 编码参数（避免触发编码器重开停流）。

## 步骤（脚本 `_dev/cloud_pc_e2e.py`）

1. 连接 COM5；`db_backup` 原库（personA/personB）→ 校验 CRC/人员名。
2. `{"cmd":"src","value":"pc"}`；如存在旧 `cloudtest` 先删除（不影响其他名字）。
3. `{"cmd":"register","id":"cloudtest","shots":1}`，送 `bill2.jpg`；等到 `event=="registered"`。
4. 反复送 `bill2.jpg`（≤15 帧），等到 `event=="match"` 且 `match=="cloudtest"`；记录 sim/boxes。
5. `db_backup` 测试库 → 上传本地云 `/api/facebank`（`X-Device: pc-cloudtest`，CRC 校验）。
6. 用送检 JPEG 作为 `photo_b64` 上报 `/api/events`；断言：
   - 201 且 `unknown=false`（测试库快照已含 cloudtest）
   - 再次上报同事件 → 200 `dup=true`
   - `GET /api/photo/<id>` 字节与 bill2.jpg 完全一致
   - CSV 含该人与 UTF-8 BOM
7. `db_restore` 原库；`{"cmd":"src","value":"cam"}`；`list` 断言只剩 personA/personB。
8. 结束：关闭本地云、删除 `_dev/cloudtest` 测试数据（可选）；重启 Qt 并确认心跳/探活。

## 预期结果

- 板端：`registered`、`match (cloudtest, sim≈0.9+)`，恢复后 `list = personA/personB`。
- 云端：快照 CRC 与测试库一致、人员含 cloudtest；事件照片字节一致、去重生效、CSV 正常。
- 生产云数据不变（仍为 personA/personB 快照 #1，事件 0 条）。

## 风险与回退

- 注册/恢复失败：保留 `_dev/face_db_before_cloudtest.bin`，重跑 `jc_test.py db_restore`。
- 板端被误删/清空：用备份或云端快照 #1 恢复（Qt「从云端同步人脸库到板端」）。
- Qt 串口占用：先关闭 Qt，测试完重启 `qt_host.exe --serial COM5`。
- PC 图注册质量不佳：换 `bill1.jpg`/`musk1.jpg`，或把 `shots` 调到 3。

## 2026-10-24 实测结果（PASS）

| 步骤 | 结果 |
|---|---|
| 原库备份 | 45259 B，CRC `1bfdb39b`，personA×5 + personB |
| PC 源 + 阈值 | `src=pc`；0.61/0.61、fast 0.80 |
| 注册 cloudtest（bill2.jpg） | `registered`，`lib_n` 名表新增 |
| PC 图识别 | 第 3 帧 `event=match`，`match=cloudtest`，`sim=0.751` |
| 测试库导出+上传 | 47316 B，CRC `818021b2`，云端快照 #1（本地实例） |
| 事件上报 | 201，`unknown=false`，照片 20347 B 与送检图**逐字节一致** |
| 重复上报 | 200 `dup=true` |
| CSV | 200，UTF-8 BOM，含 cloudtest |
| 恢复原库 | `db_import ok: 6 feat(s), 2 person(s)`；`list=[personA,personB]`；`src=cam` |
| 生产云 | 未触碰：设备 LAPTOP / lib_n=6、人员 personA/personB、快照 #1、事件 0 |

观察：注册 cloudtest 后的测试库快照里名表只剩 personA×5 + cloudtest（personB 条目缺失），
恢复原库后 personA/personB 正常；属板端名表/槽位复用的既有怪癖，需后续单独排查
`name_add`/删除槽复用逻辑。测试云数据已删除，生产库未受影响。

未覆盖：Qt「match→裁剪抓拍」的 QImage 裁剪路径仍需真人+光照实测；
CloudClient 事件/上传/下载传输已由 `_dev/cloudqt_test` 单独验证。
