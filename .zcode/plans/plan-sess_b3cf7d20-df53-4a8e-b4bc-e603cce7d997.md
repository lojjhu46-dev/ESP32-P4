# ISP AE/AWB 优化 —— app 级软件闭环 + 切帧卡顿治理

## 背景结论（调查已确认）
- 当前固件里 Espressif IPA 闭环实际开着（`sdkconfig:2637` `CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER=y`，与 defaults 意图相悖）：prio 11 的 isp_task 每帧 DQBUF 统计 + 必要时向 /dev/video0 写 exp/gain、向 /dev/video20 写 WB——与 camera 取流、faceai 手动命令**三方抢锁**，是切帧卡顿的头号嫌疑；还会用初始 metadata 把手动 exp/gain 压回默认值。
- P4 无 HW AE（ISP 硬件只做统计），AE 必须软件闭环。app 侧接口已齐备且验证过：exp/gain 写入（`app_camera_set_ctrl`）、软件 WB（`app_imgpipe_set_wb_gain`，kr=1320/kb=1645 实测 G/(R+B)/2 → 1.08）、帧内统计基础设施（DN/运动估计同款稀疏采样）。
- `app_camera_set_wb()` 失败根因 = 把 ISP 设备(/dev/video20)的控件发到了 CSI 设备(/dev/video0)——本方案不需要修它（软件 WB 代替），文档里修正认知即可。

## 改动内容

### 1. 关闭 IPA 控制器（治卡顿的第一刀）
- `sdkconfig`：`CONFIG_ESP_VIDEO_ENABLE_ISP_PIPELINE_CONTROLLER` → n（defaults 里本来就是注释掉的）。ISP 图像处理链（CCM/demosaic/gamma 等）不受影响，只去掉 3A 软件任务。
- 观察项：AUTO 降档 exp 拉满偶发不生效的遗留问题可能随之消失（若曾被 IPA metadata 覆盖）。

### 2. 新增 `app_camauto.c/h`：AE + AWB 自动引擎
仿照 `auto_fps` 状态机风格，tick 由 faceai 主循环每帧调用（`camauto_tick()`，内部按时间节流 ~200ms）：
- **统计输入**：`app_imgpipe` 在 `publish_raw()`（镜像/降噪之前）对摄像头帧做稀疏网格采样（每 8 像素×每 8 行，≈6400 点），累计 luma 与 R/G/B 均值存入 stats 缓存；仅 AE/AWB 开启时计算（关闭零开销）。faceai 的每帧刷新纪律不变（不缓存快照）。
- **AE 控制律**：亮度均值 vs 目标（`CONFIG_APP_CAM_AE_TARGET` 默认 120）± 死区才动作；先调 exp 后调 gain（下降时先降 gain）；exp 上限跟帧率走 `1000*50/fps - 6`；50Hz 抗频闪量化（exp 取 500 行整数倍，`APP_CAM_AE_FLICKER50` 默认 y）；步进限幅 + 到顶保持（暗处不抖）；auto_fps 切换后 300ms 冷却内不介入，boost 优先。
- **AWB 控制律**：灰世界 `kr=G/R、kb=G/B`（钳位 800..2500，EMA 平滑 + 死区），亮度低于门限（`APP_CAM_AWB_MIN_LUMA` 默认 40）时不校（暗帧噪声大不可信）；写现有 `app_imgpipe_set_wb_gain()`（纯变量，零阻塞）。
- **手动优先**：任何手动 exp/gain（cam 命令、Night/Day 预设、滑块）→ AE 退出；手动 wb_kr/wb_kb → AWB 退出。与"手动 fps 退出 auto_fps"同规则。

### 3. 传感器写操作改走"camera 任务信箱"（治卡顿的第二刀 + 回避重启坑）
- `app_camera` 增加轻量 pending-mailbox：AE 调节、auto_fps 的 set_fps/boost 等**写操作不再从 faceai 直接 ioctl**，投递请求由 camera_task 在自己循环的安全点（QBUF 返回后、下一次 DQBUF 之前）串行执行。
- 所有可能的 ioctl 并发消失（同类即文档记录的"无日志整板重启"坑），运动估计无需再为写操作暂停 500ms——现 1~2 秒卡顿的主要嫌疑（isp_task 三方抢锁 + 切帧后 500ms 停顿窗口）同时消除。保留旧暂停模式代码作为回退开关（Kconfig）。
- 读操作（get_ctrl 等低频读）维持现状。

### 4. 命令 / 结果帧 / UI
- `{"cmd":"cam"}` 新增：`ae`:bool、`awb`:bool、`ae_target`:N；结果帧/ack msg 增加 `ae/awb/luma/ae_target` 状态字段。
- 模拟器 `ui_shared.c`：第 4 行（或新增行）加 **AE / AWB** 切换按钮（激活时高亮）；滑块显示当前值，拖动即退出对应自动模式。
- `Kconfig.projbuild`：`APP_CAM_AE`(y)、`APP_CAM_AE_TARGET`(120)、`APP_CAM_AE_FLICKER50`(y)、`APP_CAM_AWB`(y)、`APP_CAM_AWB_MIN_LUMA`(40)。

### 5. 文档
- `HANDOVER.md` 增补第十一节：AE/AWB 闭环设计、IPA 控制器关闭决策、set_wb 错设备根因、0x00980914/15=HFLIP/VFLIP 认知修正、实测数据。

## 验证（真机，COM4）
1. 烧录前按惯例 `chcp 65001` + `PYTHONIOENCODING=utf-8`；`idf.py build` 离线构建通过（不动 override_path 本地组件）。
2. AE：正常光 luma 收敛至目标 ± 死区（`tools/rgb_stats.py`）；手遮/开灯亮度跟踪无振荡；暗处 exp/gain 到顶保持。
3. AWB：开启后 G/(R+B)/2 → ~1.0（rgb_stats.py 量化），对比截图存 `docs/img/`。
4. 卡顿：auto_fps 切换 `af_ms` 与体感对比（目标：无可感知冻结）；切换窗口内丢帧计数。
5. 联动与稳定性：Night/Day 预设→自动退出；10 分钟连续跑无 task_wdt/重启/0x02 中断，DN/运动估计不受影响。

## 风险与对策
- AE 步长/死区/平滑参数可能需 1~2 轮真机标定（命令和 Kconfig 都留了调节口）。
- 若信箱化后仍有不稳迹象（重启坑根因未明），回退运动估计暂停模式并缩小 AE 写频率。
- 初始状态不变：exp=519(驱动默认)+gain=72(DEF_GAIN) = 已知良好白天预设，关闭 IPA 控制器无回归风险。