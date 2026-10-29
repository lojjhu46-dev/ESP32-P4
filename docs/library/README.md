# 文档集中目录 `docs/library/`

> 2026-10-29 从 `docs/`（6 份）与 `jc_p4_app/docs/`（5 份 + `img/` 24 张截图）集中到此处，共 **35 个文件**；迁移采用「先复制 → 逐文件 SHA-256 与原件比对（35/35 一致）→ 再改路径指针 → 最后删原件」，台账见 `_dev/docs_migration_manifest.md`（不入库），改前留底 `_dev/docs_migration_preflight_20261029.patch`。

## 先读哪个

1. **遇到现象先查** [`嵌入式软件测试与修复实践手册.md`](嵌入式软件测试与修复实践手册.md) —— 44 个已修 bug 的分类入口、18 条判据、测试矩阵与硬件联调纪律。
2. **推进里程碑看** [`H264重构交接与阶段计划.md`](H264重构交接与阶段计划.md) —— §11 M0 / §12 M2 / §13 AE+检测 / §14 人脸库 / §15 云签到（§15.9、§15.10 是实测结论与遗留清单）。
3. **要历史全量结论与验收数据看** [`HANDOVER.md`](HANDOVER.md)（3045 行主交接文档；§4「必读的坑」是一页速查）。

## 全部文档

| 文件 | 用途 | 原路径 |
|---|---|---|
| `嵌入式软件测试与修复实践手册.md` | 案例台账 + 判据 + 测试矩阵 + 联调纪律（入口） | `docs/` |
| `H264重构交接与阶段计划.md` | H.264 方向决策、已核实事实、阶段计划、云签到 §15 | `docs/` |
| `2026-10-29_face.db膨胀_槽号与下标错位_同名重注册零命中_修复记录.md` | face.db 三件套的取证过程与被推翻假设全表 | `docs/` |
| `2026-10-22_storage挂载失败_删除注册失效_修复记录.md` | `HW_H264` 挤爆内部 RAM → `/storage` 挂不上的逐字日志与对照实验 | `docs/` |
| `JC-ESP32P4-M3-DEV-开发要点.md` | 板卡引脚表与硬件结论（本项目自己整理，不依赖厂商资料包） | `docs/` |
| `项目框架构建指南.md` | 从零搭同类工程的步骤 | `docs/` |
| `HANDOVER.md` | ★ 主交接文档：技术结论 / 踩坑 / 验收记录 / 目录约定（第 6 节） | `jc_p4_app/docs/` |
| `usb_link_protocol.md` | JCLINK v1 协议规范（0x01~0x07，两端共用） | `jc_p4_app/docs/` |
| `face_recog_day_plan.md` | 人脸识别功能规划（P0~P3 + 三轮实施）与进度 | `jc_p4_app/docs/` |
| `esp-dl_issue_draft.md` | esp-dl 特征非确定性 issue 英文草稿（⏳ 待提交 Espressif） | `jc_p4_app/docs/` |
| `pc_link_references.md` | esp-dl / tinyusb / jpeg / lv_port 技术核实记录 | `jc_p4_app/docs/` |
| `img/`（24 个 png/jpg） | UI 与验收截图，被 `HANDOVER.md` 正文引用 | `jc_p4_app/docs/img/` |

## 路径约定（改文档时请遵守）

- **目录内互指**：写裸文件名，如 `[HANDOVER.md](HANDOVER.md)`、`[H264重构交接与阶段计划.md](H264重构交接与阶段计划.md)`。
- **指向仓库代码**：多一层 `../`，如 `../../jc_p4_app/main/app_faceai.cpp`、`../../qt_host/src/main_window.cpp`。
- **正文里的路径文字**：一律写仓库根相对形式 `docs/library/<文件>`，便于任何位置的读者直接定位。
- ⚠️ 文档被外部按行号锚点引用（`HANDOVER.md#L205` 等 9 处），**改写时只能同行替换，不得增删行**；若必须增删行，要同步修正所有 `#L` 与 `:NNN` 引用。
- 各模块 README（仓库根、`jc_p4_app/`、`qt_host/`、`cloud_server/`）留在原地作为入口，只维护指向本目录的链接。
- 真实服务器地址、设备标识、名单实名等**只记在本机私有速查 `docs/private/`（被 git 忽略，不入库）**；仓库文档一律用 `<CLOUD_IP>` / `LAPTOP-XXXX` / `personA` 这类占位符。改完文档可跑 `_dev/privacy_scan.js` 复查。
