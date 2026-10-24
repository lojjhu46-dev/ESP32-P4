#pragma once
/*
 * app_faceai —— 板端边缘人脸检测 / 特征提取 / 1:N 比对（任务五）
 *
 *   帧源 app_imgpipe(RGB888) --> HumanFaceDetect(MSR+MNP)
 *        --> HumanFaceFeat(MFN, 112x112) --> dl::recognition::DataBase(/storage/face.db)
 *        --> 0x02 JSON 回传 PC
 *
 * 特性：
 *   - 特征库落盘到 storage(FAT) 分区 /storage/face.db（esp-dl 自带 DataBase，
 *     按 id 存 L2 归一化特征），名字 <-> id 映射放 NVS(namespace "faceai")。
 *   - 命令由 app_usb_link 的 0x03 帧驱动（cmd 回调只投队列，解析在 AI 任务里）：
 *       {"cmd":"start"} / {"cmd":"stop"} / {"cmd":"mode","detect":true,"match":true,"thr":0.7}
 *       {"cmd":"register","id":"user1"}  武装注册（立即回 ack；之后第一帧含人脸就入库）
 *       {"cmd":"delete","id":"user1"} / {"cmd":"list"} / {"cmd":"clear"}
 *     所有命令都立即回一条 0x02：register 回 ack，注册结果在下一帧的结果帧里
 *     以 event=registered + match=<名字> 上报。一个人可以注册多张（lib 只列唯一名字）。
 *   - 每个输入帧都会回一条 0x02（窗口=1 流控靠它推进），字段：
 *     faces/boxes/match/sim/fps/infer_ms/dropped/event (+lib/msg/num)
 */
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 由 main 在 app_imgpipe_init() 之后调用：加载名字表(已从命令回调投队列)、建 AI 任务
 * 并立即返回；人脸模型（约 1.4MB rodata）在 AI 任务里加载，加载完成后接管 0x02 应答。
 * 在就绪之前由 app_imgpipe 的内置 ack 兜底，PC 不会卡住。
 */
esp_err_t app_faceai_init(void);

/* 提前挂载 /storage（FAT+WL）：main 在 app_camera_init() 之前调用，先占内部 RAM。
   幂等；重复调用返回已挂载状态。 */
esp_err_t app_faceai_storage_init(void);

/* 模型加载完成、开始具备识别能力 */
bool app_faceai_ready(void);

#ifdef __cplusplus
}
#endif
