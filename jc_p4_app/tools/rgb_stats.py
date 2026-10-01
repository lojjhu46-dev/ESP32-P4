# -*- coding: utf-8 -*-
"""统计 0x05 raw 帧（RGB565 LE）的 R/G/B 均值 —— 用于调白平衡/亮度。

用法: python tools/rgb_stats.py <file.rgb565>
输出: R/G/B 归一化均值、green/cool 比值（>1 说明偏绿）与整体亮度
"""
import struct
import sys

if len(sys.argv) < 2:
    print("usage: python tools/rgb_stats.py <file.rgb565>")
    raise SystemExit(1)

data = open(sys.argv[1], "rb").read()
n = len(data) // 2
v = struct.unpack("<%dH" % n, data[: n * 2])

r = sum((x >> 11) & 0x1F for x in v) / n * 255.0 / 31.0
g = sum((x >> 5) & 0x3F for x in v) / n * 255.0 / 63.0
b = sum(x & 0x1F for x in v) / n * 255.0 / 31.0
avg = sum(v) / n
rb = (r + b) / 2.0

print("R=%.1f G=%.1f B=%.1f" % (r, g, b))
print("G/(R+B)/2 = %.2f   (>1 = green cast, 1.0 = neutral)" % (g / rb if rb else 0.0))
print("avg16=%.0f max16=%d" % (avg, max(v)))

# 噪声估计：水平相邻像素的亮度差均值（同一场景对比时，越大 = 噪点越多）
# 用法: python tools/rgb_stats.py x.rgb565 [width=640]
w = int(sys.argv[2]) if len(sys.argv) > 2 else 640
lum = [((x >> 11) & 0x1F) * 8 + ((x >> 5) & 0x3F) * 4 + (x & 0x1F) * 8 for x in v]
diffs = [abs(lum[i + 1] - lum[i]) for i in range(len(lum) - 1) if (i + 1) % w]
print("noise(grad)=%.1f   (>0; 同场景下越大噪点越多)" % (sum(diffs) / len(diffs)))

# 二阶水平差分（拉普拉斯）：对随机噪声敏感、对线性边缘不敏感 —— 比一阶梯度更能
# 把"噪点"和"真实边缘"区分开，判断时域降噪效果优先看这个。
lap = [abs(2 * lum[i] - lum[i - 1] - lum[i + 1])
       for i in range(1, len(lum) - 1) if i % w and (i + 1) % w]
print("noise2(lap)=%.2f   (二阶差分：噪点指标，越小越干净)" % (sum(lap) / len(lap)))
