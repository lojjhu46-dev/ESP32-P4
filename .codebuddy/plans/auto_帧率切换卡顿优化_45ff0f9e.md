---
name: auto 帧率切换卡顿优化
overview: 把 auto 模式切换帧率的路径从"19 个同步 V4L2 ioctl"砍到"升档 1 个 / 降档 3 个"（利用 set_fps 内置的 exp clamp，删除 get_ctrl 大套餐，降档的 exp 拉满延迟 600ms 执行），并加耗时观测字段（af_ms）量化验证切换不再卡顿。
todos:
  - id: optimize-apply
    content: 重写 auto_fps_apply：升档零额外 ioctl；降档公式算 exp_max 并延迟 600ms 拉满
    status: completed
  - id: add-af-ms
    content: auto_fps_apply 记录耗时，结果帧新增 af_ms 字段，切换日志带耗时
    status: completed
    dependencies:
      - optimize-apply
  - id: verify-switch
    content: 编译烧录，伪造运动/静止连续 5 次切换验证：流中断
    status: completed
    dependencies:
      - add-af-ms
---

## 用户需求

用户实测反馈：**auto 模式下检测到运动物体切换 fps 时极易卡死几秒**（画面与结果流冻结），需要优化。

## 根因（已做只读代码确认）

`auto_fps_apply()` 在 faceai 任务里**同步**执行 ~19 个 V4L2 ioctl：

- `app_camera_set_fps()` = 1 个 ioctl（内部还有 query desc 走 I2C）
- `app_camera_get_ctrl()` = **8 个 control × 2 ioctl = 16 个 ioctl**（每个都走 I2C 读 sensor）
- `app_camera_set_ctrl()` = 2 个 ioctl（set exp + set gain）

这些 ioctl 全部要与 camera 任务的 DQBUF/QBUF（阻塞等帧）竞争 esp_video 设备锁与 I2C 总线，期间 faceai 不取帧、不发 0x02/0x05 → UI 表现为冻结。再叠加 sensor 改 VTS 后的帧同步窗口。

**核心杠杆（已从驱动代码确认）**：`sc2336_set_fps()` **内置 exp clamp**——VTS 变小时自动把曝光压到新上限：

- **升档（15→50fps）**：exp 被自动 clamp 到 994 = 新上限**天然拉满** → 升档完全不需要任何额外 ioctl
- **降档（50→15fps）**：exp=994 合法保持（画面暂暗），要拉满到 3327 才需 1 个 set exp ioctl

## 优化目标

切换路径从 19 个 ioctl 砍到 **升档 1 个 / 降档 1 个（+600ms 后补 2 个）**，用“渐进变亮”替代“瞬间卡顿”；并加耗时观测字段（af_ms）量化验证。

## 技术方案

### 1. 升档路径：零额外 ioctl（最大杠杆）

`auto_fps_apply()` 检测到 fps 增大（如 15→50）时**只调** `app_camera_set_fps(fps)` + `app_imgpipe_reset_prev()`。exp 由驱动内置 clamp 自动压到新上限（994），行为与现在完全一致但省掉 18 个 ioctl。

### 2. 降档路径：公式替代查询 + 延迟拉满

- 删掉 `app_camera_get_ctrl()`（16 个 ioctl）。exp 上限直接按公式算（与实测值完全一致）：
`exp_max = base_vts * base_fps / fps - 6 = 1000*50/fps - 6`（15fps ⇒ 3327）
- gain 用已有缓存 `s_cam_gain_max`（恒 192）
- **切换瞬间只做 set_fps（1 个 ioctl）**；exp/gain 拉满挂到 `auto_fps_tick()` 恢复运动估计的同一时机（600ms 后，新增 `s_auto_boost_pending` 标志）执行 2 个 set ioctl。画面在半秒内“渐亮”，而不是瞬间冻结。

### 3. 可观测性

`auto_fps_apply()` 用 `esp_timer` 记录切换总耗时，结果帧新增 `"af_ms"` 字段（auto 模式恒带），切换日志带耗时。若仍有卡顿可立即区分是 ioctl 慢、sensor 帧同步慢还是锁竞争。

### 4. 验证方法（复用已验证的无物理运动触发手法）

- `mv_thr=1` 伪造运动 → 升档；`mv_ratio=100` 伪造静止 → 降档
- 连续 5 次切换，`watch` 观察 0x02 流中断时长（目标 <0.5s）与 `af_ms`
- 降档后 ~1s 内画面亮度恢复（exp 回 3327）
- 稳定性：无重启、无 task_wdt

### 性能与稳定性说明

- 保留现有“暂停运动估计 500ms”防并发重启防护
- 保留切换后 `app_imgpipe_reset_prev()`（防亮度跳变被误判成运动）
- 后备方案（本次不做，写进文档）：若优化后切换仍 >1s，把切换请求投递到 camera 任务上下文执行（跨任务握手），彻底消除锁竞争

### 目录结构（改动文件）

```
jc_p4_app/
├── main/
│   └── app_faceai.cpp      # [MODIFY] auto_fps_apply() 重写：升档零额外 ioctl、
│                           #   降档公式算 exp + 延迟拉满（s_auto_boost_pending 挂到
│                           #   auto_fps_tick 的 600ms 时机）；af_ms 耗时统计与结果帧字段
└── docs/
    ├── HANDOVER.md         # [MODIFY] 第十节补：卡顿根因（19 ioctl 竞争）、优化方案
    │                       #   （clamp 杠杆 + 公式 + 延迟拉满）、优化前后 af_ms 实测数据、后备方案
    └── usb_link_protocol.md # [MODIFY] 结果帧补 "af_ms" 字段说明
```