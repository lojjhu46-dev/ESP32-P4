# 白天人脸识别开发规划（2026-10-04 起草）

> 目标：把白天（tier0）人脸识别从"能跑"做到"好用"。结合 espressif 官方例程
> （esp-dl human_face_recognition、ESP-WHO、ESP-BOX factory demo 门禁）与本项目
> 实际架构（app_faceai/app_imgpipe/JCLINK/LVGL UI）给出功能清单与落地路径。
> 批准后按阶段实施，完成情况回写 HANDOVER.md。
>
> **进度（2026-10-06 晚）**：✅ 第 1 轮 F1/F2/F3/F5 完成（回归 `_face_r1_test.py`，
> 实现/坑/验收见 HANDOVER 第廿一节）；✅ 追加完成跨核缓存一致性修复（第 1.5 轮，
> 廿一节）；✅ **第 2 轮 F4/F6/F7 完成（2026-10-06，HANDOVER 第廿二节：F4 轻跟踪节流、
> F6 事件去重、F7 多人脸、发布端按需发布、回归套件修复后 24/24）**；
> ✅ **F8 完成（2026-10-07，见廿二节 16/17 节）+ F9 容量完成（2026-10-11，见 HANDOVER
> 第廿三节：名表迁移 `/storage/names.json`、人数上限 10→64，回归 25/25）**；
> ✅ **F10 特征库备份/恢复完成（2026-10-11，见 HANDOVER 第廿四节：JCLINK 0x06 块传输 +
> `db_export/db_import` + `jc_test.py db_backup/db_restore`，`_f10_test.py` 12/12、回归 25/25）
> —— 第 3 轮（F8/F9/F10）全部收口**。
> ⏳ 剩余：真机阈值标定 A 侧补测（需真人在场）+ P3 可选进阶（F11/F12/F13）。
> ⚠️ 诊断探针已门控（DIAG_DEEP 默认 n，DIAG_KP 无副作用可开）。
> ⚠️ 已知既有问题：esp-dl v3.3.11 特征提取会话模式双模态（同人跨照片 musk2 聚
> 0.77 或 0.42，同会话内也可能混跳）——匹配判定已用窗口最大 sim 抑制，回归 P4
> 已改自适应阈值；升 MBF（F11）决策等标定数据。

## 一、现状盘点（已核实代码）

管线：`MSRMNP 检测（13.1+2.4ms@P4，box+score+5关键点）→ HumanFaceFeat::run(img, keypoint)
（内部按关键点做 112×112 对齐裁剪，MFN 512 维/2048B 特征，93ms@P4）→ DataBase 查询
（FAT /storage/face.db）+ NVS 名表（上限 10 人/32 特征，我们自己定的）`。

已有的：单帧注册（命令武装 + ts_us 防错配）、1:N 比对（单阈值 0.70）、每人多特征、
list/delete/clear、全框上报（只对最大脸提特征）、白天 50fps 采集 / AI ~33fps、
夜视档（tier≥1）门控停识别。

**与官方/社区例程的差距**（也是本规划的主线）：

| 维度 | 现状 | 官方例程/社区产品做法 |
|---|---|---|
| 注册 | 单帧采样 | 多帧采样（esp-box 门禁向导式采集；门禁产品惯例 3~5 帧） |
| 判定 | 单阈值，每帧报 match/reject | 双阈值 + 连续 K 帧确认 + 事件去重（访问控制类产品标准做法） |
| 人脸 | 只识别最大脸 | 识别画面前 2~3 张（esp-who 例程按检测列表遍历） |
| 管理 | 命令行 list/delete | 屏上向导/键盘输入/列表删除（esp-box factory_demo） |
| 阈值 | 拍脑袋 0.70 | 用注册者+陌生人的相似度分布标定（官方 README 明确要求按场景调阈值） |

模型选项（esp-dl v3.3.11 官方数据）：
- 特征：MFN_S8_V1（现用，93ms，IJB-C TAR@FAR=1e-4 = 90.03%）vs MBF_S8_V1（188ms，93.94%）
- 检测：MSR+MNP（现用，~15ms，mAP50-95 0.366）vs ESPDET_PICO_224（50ms，0.504）vs 416（186ms，0.597）

## 二、功能清单（按优先级）

### P0 识别质量（白天识别"准不稳"的核心）

**F1 多帧注册向导**
- 做：`{"cmd":"register","name":"X","shots":3}` 进入采集态；AI 对连续帧做质量门控
  （检测分 ≥0.6、脸宽 ≥ 80px、关键点齐全）后自动收 3 帧、各自提特征入库（复用现有多
  特征/一人多行机制），事件 `event:"enrolling", msg:"2/3"` 逐帧回报，完成回
  `event:"registered"`。中途 `register` 再次下发或 `stop` 可取消。
- 依据：esp-box 门禁向导 + 门禁产品多采样惯例；我们已支持一人多特征，只差采集流程。
- 落点：app_faceai.cpp 注册状态机（现 s_reg_pending 单帧逻辑扩展）；单帧注册
  （不带 shots）保持兼容。

**F2 时域平滑：连续 K 帧确认才算识别**
- 做：保留最近 K（默认 5）帧的比对结果（环形缓冲，按帧序），同一 id 命中 ≥M（默认 3）
  才输出 `event:"recognized"`；不足时输出 `event:"tracking"`（UI 可显示 "确认中"）。
  对 reject（有脸无命中）同样要连续 M 帧 `event:"stranger"` 才算陌生人。
- 依据：单帧 1:N 比对天然有误报/抖动，访问控制类产品全部做时间一致性；官方例程
  README 也强调阈值需配合场景调，平滑是最便宜的第一层。
- 落点：app_faceai.cpp 比对分支加小状态机；结果帧新增 `stable`(0/1)。

**F3 双阈值分区**
- 做：`MATCH_THR_HI`（默认 0.80，直接计入确认）与 `MATCH_THR_LO`（默认 0.70，灰区：
  计入"疑似"但不计满分）；F2 的 M 帧确认对灰区要求更严（M+1）。
- 依据：单阈值下"要么放过要么误报"；双阈值 + 帧一致是低算力防误报的标准组合。
- 落点：Kconfig 两个新项 + `{"cmd":"mode","thr":x}` 保留兼容（同时设双阈值）。

**F4 轻跟踪 + 特征提取节流**
- 做：上一帧最大脸 box 与当前帧按 IoU 匹配（>0.4 视为同一人脸）；同人且位移小（中心
  移动 < 10% 脸宽）时**跳过 93ms 的特征提取**、沿用上一次比对结论，只更新 box；新脸
  /大位移才提特征。白天 50fps 下特征提取频率自然降到 5~10 次/秒，省下的 CPU 给
  0x05 预览和将来屏端 UI。
- 依据：esp-who/esp-box 均为"检测每帧、特征按需"；这是把 93ms 摊薄的标准手法。
- 落点：app_faceai.cpp 主循环（IoU 计算十余行，无新依赖）。
- ✅ **已实现（2026-10-06）**：`APP_FACEAI_TRACK_HOLD_MS`（编译默认 **250ms**，0=关），
  运行时可调 `{"cmd":"mode","trk_hold_ms":400}`。验收字段：结果帧 `reuse` / `feat_hz`。
  实测结论与踩坑（详见 HANDOVER 第廿一节）：
  1. **150ms 是死值**：AI 一帧 175ms（相机）/244ms（PC 送图），保持窗根本触发不了；
  2. 提特征频率 ≈ **1/HOLD**，结果帧率最高 **+154%**（HOLD=1000ms 时 3.9→9.9fps）；
  3. **跳过帧不进平滑窗** → 确认延迟随 HOLD 线性变长（F4/F2 耦合，选 HOLD 的核心权衡）；
  4. 判据是几何的 → 同位置 **HOLD 内换人会张冠李戴**（F7/F8 需重新审视）；
  5. 顺带查明：**关掉 0x05 预览流，AI 帧率 5.7→10.5fps** —— 预览流与 AI 争 2 槽帧池
     是当前吞吐瓶颈，提吞吐应优先解决这个。

⚡ **管线吞吐（2026-10-06 与 F4 同期做完，详见 HANDOVER 第廿一节）**：
1. **帧池 2 槽 = 自限流**：AI 占槽 ~190ms 期间相机无法发布 → 池发布率锁死在 ≈AI 消费率；
   **改 4 槽（`APP_CAM_PIPE_SLOTS`）后 AI 吞吐 5.2→10.8 fps（+108%）**、drop 归零。
2. **每帧全帧 CRC32 诊断探针 = 18.5ms/帧**（单核 ~30%）→ 已门控
   （`APP_CAM_FRAME_CRC`，默认 n）。**凡是"每帧全帧扫描"的诊断都必须默认关。**
3. 日光下分段计时：管线每秒发布 **51 帧**，发布端每帧 **~18ms**（copy 8.6 + WB/镜像 8.3）
   → **CPU1 ~87%**；消费侧 M2C 失效只有 **15µs**（可忽略）。
   → **下一步候选：发布端"按需发布"**（上一帧未被取走且无消费者等待时跳过 copy/post），
   估计能省 CPU1 的大头，直接给 AI 与预览腾算力。

**F5 阈值标定工具**
- 做：`pc_host/thr_calib.py`——对着已注册者连续收 N 帧记 sim 分布（min/p5/median），
  再对未注册者收 N 帧记分布，给出两分布的间隔与建议 HI/LO。真实光照、真实距离下做。
- 依据：官方 README 要求按场景调阈值；我们没有测量就没有 0.70/0.80 的依据。
- 落点：pc_host 新脚本（复用 protocol.py）。

### P1 事件与体验

**F6 事件去重与陌生人事件**
- 做：`recognized` 同一人 T 秒（默认 5s，可配）内只报一次；`stranger`（有脸未命中且
  F2 确认）同脸 T 秒一次。结果帧 event 语义：`recognized / stranger / none / enrolling /
  registered / error`（保留 `match/reject` 兼容映射：recognized→match）。
- 依据：esp-box 门禁"识别成功欢迎一次"而非每帧刷屏；host 侧日志/联动也都需要事件化。
- 落点：app_faceai.cpp 事件层；pc_host/jc_test.py watch 打印适配。
- ✅ **已实现（2026-10-06，HANDOVER 第廿二节）**：`APP_FACEAI_EVENT_DEDUP_MS`（默认 5000，
  运行时 `{"cmd":"mode","dedup_ms":N}`，0=关）。窗口内 event 回落 `none`，match/sim/stable
  照常带（UI 名字不闪断）；事件名保持 match/reject 兼容映射。门在 track_cache 之后施加，
  F4 复用帧回放值同样过门。回归 P3 内直接断言 ✓。

**F7 多人脸识别**
- 做：对检测框按面积取前 N（默认 2，Kconfig 可 1~3）张提特征分别比对，结果帧 boxes
  每项带各自 name/sim（现结构 boxes[] 扩展字段，向后兼容）。
- 依据：esp-who 例程按检测列表处理；门禁"多人同时进门"场景。
- 落点：app_faceai.cpp（MFN 93ms×2 仍 < 200ms，配 F4 节流白天无压力）。
- ✅ **已实现（2026-10-06，HANDOVER 第廿二节）**：`APP_FACEAI_MULTI_MAX`（默认 2，运行时
  `{"cmd":"mode","multi":N}`）。主脸（第 1 大）走完整 F2/F4/F6；副脸裸比对（raw_sim≥thr_lo
  才报名字）；`boxes[i]` 第 5/6 位追加 name/sim。F4 复用帧整帧跳过、副脸标签从快照回放。
  验收 `_f7_test.py`（双人拼图 PC 送图）：双人 10/10、零张冠李戴（14/14）。
  ⚠️ 已知局限：副脸无平滑窗，esp-dl 单帧野值偶发闪错名（1/16，复跑未现）——根治需按脸平滑。

**F8 模拟器 UI 升级（同源 ui_shared.c）**
- 做：① Register 弹窗加 shots 选择与进度（"采集中 2/3"，读 enrolling 事件）；② 结果行
  显示稳定态（✓ 姓名 / … 确认中 / ? 陌生人）；③ 名单显示每人特征数。
- 依据：esp-box factory_demo 的屏上管理 UI 是我们 800×1280 屏到货后的直接模板
  （factory_demo/main/face_recognition_access_control）。
- ✅ **已实现（2026-10-07 凌晨，HANDOVER 廿二节 16 节）**：①②③全部落地 + 硬需求
  "实时检出反馈（脸宽 px）" + F7 每框名字/相似度标签。固件配 lib_feats 平行数组。
  主界面截图验证 ✓；弹窗交互待用户走查。

### P2 数据管理

**F9 容量提升**
- 做：名表从 NVS 2048B 字符串迁移/扩展为 `/storage/names.json`（NVS 保留兜底），
  上限 10 人 → 64 人；特征库 esp-dl DataBase 按自增 id 走 FAT，容量受 8MB storage
  分区限制（每特征 2050B，64 人 × 5 特征 ≈ 0.65MB，充裕）。
- 依据：README 明确每特征 2050B；我们现在的 10 人上限是自己名表编码卡死的。
- ✅ **已实现（2026-10-11，真机验证 ✅，见 HANDOVER 第廿三节）**：`/storage/names.json`
  （cJSON 数组）+ NVS 只读兜底 + 首次开机自动迁移；`APP_FACEAI_MAX_PERSONS`(64)/
  `APP_FACEAI_MAX_FEATS`(320)。**⚠️ 附带必需项 `CONFIG_FATFS_LFN_HEAP=y`**——FATFS 默认
  不支持长文件名，`names.json`（4 字符扩展名）会静默写失败（真机踩到）。回归 25/25，
  实测注册 12 人成功、只擦 storage 后从 NVS 自动迁移成功。

**F10 备份导出/导入**
- 做：`{"cmd":"db_export"}` → 0x02 分片回 JSON（名表 + face.db base64）；
  `{"cmd":"db_import",...}` 恢复。PC 端 `jc_test.py db_backup/db_restore`。
- 依据：设备端特征库不可迁移是社区常见痛点；实现成本低。
- ✅ **已实现（2026-10-11，真机验证 ✅，见 HANDOVER 第廿四节）**：落地时**改用新增的
  JCLINK **0x06 块传输帧**（双向分片）+ 二进制信封（`"JCDF"+ver+face_len+names_len+names+face.db`）
  —— 原方案的 0x02 分片会与流式结果帧混流、0x03 受 256B 命令上限、base64 还膨胀 33%。
  语义按用户确认：**覆盖式恢复**、**原样打包**、**导入前自动 .bak**（`face_bak.db`/`names.bak`）；
  校验失败整批拒绝且库不变。`_f10_test.py` 12/12、回归 25/25。

### P3 可选进阶（按需再排期）

- **F11 模型升级**：MFN→MBF（IJB-C 90.0%→93.9%，代价 93→188ms、flash +2.1MB 需分区表
  放大 app 或模型走独立分区）；检测换 ESPDET_PICO_224（mAP 0.366→0.504，15→50ms，
  小脸/侧脸检出更好）。**先用 F5 的标定数据决定要不要升**——如果 MFN 在我们的光照/
  距离下误报可接受，就不必付 2 倍延迟。
- **F12 活体/防照片**：esp-dl 无现成 liveness 模型；廉价方案 = 识别成功后随机指令
  （眨眼/张嘴需关键点序列，或头部小幅移动需帧间光流）做二次确认。列为准入场景立项。
- **F13 事件外发**：WiFi 已通，可选把 recognized/stranger 事件 MQTT/WebSocket 推给
  家居/门禁后端（社区门禁项目的标准集成面）。

## 三、实施顺序（建议三轮）

| 轮次 | 内容 | 验收 |
|---|---|---|
| 第 1 轮 | F1+F2+F3+F5（注册向导、平滑确认、双阈值、标定工具） | 真机：同一人 1m/3m/侧脸注册后识别稳、陌生人稳定报 stranger 不误报；标定脚本出 HI/LO 数据 |
| 第 2 轮 | F4+F6+F7（跟踪节流、事件去重、多人脸） | 白天 50fps 下 AI 占空比下降（infer 频率 5~10/s）、双人同框各自出结果 |
| 第 3 轮 | F8+F9+F10（UI 向导、容量、备份） | 模拟器全流程（注册向导到名单管理）可用；备份恢复一致 |

每轮完成回写 HANDOVER 并跑一次夜间/白天回归（night_accept_test.py + ae_accept_test.py
不受影响）。

## 四、参考（已核实）

- esp-dl v3.3.11（本地检出）`examples/human_face_recognition`：HumanFaceRecognizer
  enroll/recognize/clear 流程；DB 后端 fatfs_flash/sdcard/spiffs；每特征 2050B
- esp-dl 模型 README（本地检出）：MFN/MBF、MSR+MNP/ESPDET_PICO 的延迟与 IJB-C 指标
- espressif/human_face_recognition 组件（components.espressif.com v0.2.1）：
  enroll/recognize + 阈值调整说明
- ESP-WHO 人脸识别指南（developer.espressif.com）：按钮注册、阈值调参、LCD 显示
- espressif/esp-box factory_demo（main/face_recognition_access_control）：屏上注册向导、
  键盘输入名字、单人/全量删除、识别欢迎——本项目屏端 UI 的直接模板
