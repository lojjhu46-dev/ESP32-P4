## 修复：预览画面上下区域镜像不一致（第 1.5 轮缓存修复引入的顺序回归）

### 根因（已读码确认）
`publish_raw`（camera 任务，CPU1）顺序为：`pipe_copy_into`（我上轮在此处加了 C2M 回写）→ AE/运动估计（只读）→ DN（写槽，高帧率跳过）→ **`pipe_postprocess` 在槽内就地做白平衡+镜像（CPU 写，发生在 C2M 之后）** → `pipe_publish`。
镜像产生的脏行留在 CPU1 的 L1 未回写；消费者（faceai 走 L2、stream 的 JPEG 编码 DMA 直读 RAM）读到的是**回写过的未镜像内容**，只有已被 L1 自然淘汰的行才是镜像后的——表现为画面上下区域左右排序相反、分界随负载漂移（"错位"）。
JPEG 编码驱动已自带输入 C2M/输出 M2C（jpeg_encode.c:280/314），app_stream 无需改动。

### 修复（app_imgpipe.c，两处小改）
1. `pipe_copy_into`：移除我加的 C2M 与 CRC（回退为纯拷贝——回写时机错误）。
2. `publish_raw`：在 `pipe_postprocess` 之后、`pipe_publish` 之前统一做 **CRC 指纹 + C2M 回写**（覆盖 DN+镜像之后的最终内容；此处之后该帧再无 CPU 写入）。

### 验证
1. 烧录后 `jc_test.py img` 抓 2~3 帧 JPEG，Read 目视确认：整幅画面镜像方向一致、无上下分界（原症状消除）。
2. `_face_r1_test.py` 重跑确认仍 19/19（PC 源路径不经过此改动，回归保险）。
3. HANDOVER 廿一节补记该顺序坑："C2M 回写必须放在该帧所有 CPU 写入之后、发布之前；镜像/降噪都在拷贝之后才执行"。