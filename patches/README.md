# `patches/` —— 第三方组件的本地补丁留档

## `esp-dl-arena.patch`

| 项 | 值 |
|---|---|
| 目标组件 | `esp-dl/`（父仓库里是 **gitlink**，无 remote、无 `.gitmodules`，补丁不可能提交进它自己） |
| 基线版本 | tag **`v3.3.11`**（commit `5d9c360`，2026-09-03）—— 与根 README 里的克隆命令一致 |
| 规模 | 4 个文件 / +191 行（`dl/model/include/dl_model_base.hpp`、`dl/model/src/dl_model_base.cpp`、`vision/recognition/dl_feat_base.cpp`、`vision/recognition/dl_feat_base.hpp`） |
| 加了什么 | `Model::zero_workspace()` / `poison_workspace()`（走库内现成但零调用的 `ModelContext::root_reset()` 全 arena memset，含 C2M 回写）、`Model::run_trace()` 逐算子输出张量 CRC32、`Model::module_name(i)`、`Feat` 侧 `set_feat_trace/get_feat_trace` 透传 |
| 为什么需要 | esp-dl 特征提取在 P4 上**执行级非确定性**（见 `docs/library/HANDOVER.md` 廿二节 14/15 与 `docs/library/esp-dl_issue_draft.md`）。这套仪表是当时逐层二分定位的唯一手段，也是后续复现实验的前提；不打补丁 → `feat_ws` / `feat_bisect` 两个运行时命令无效、`app_faceai` 链接不上这些符号 |

### 打补丁

```bash
# 前提：已按根 README 克隆 esp-dl v3.3.11 到 ./esp-dl
git -C esp-dl apply -p1 ../patches/esp-dl-arena.patch    # 仓库内路径自带 esp-dl/ 前缀，故 -p1
git -C esp-dl diff --stat                                 # 应看到 4 files changed, 191 insertions(+)
```

### 校验补丁与当前工作树一致

```bash
git -C esp-dl apply --check --reverse -p1 ../patches/esp-dl-arena.patch && echo OK
```
（通过 = 本地 esp-dl 现状与该补丁严格对应；反向检查比"看文件存不存在"可靠。）

### 重新生成

```bash
git -C esp-dl diff > patches/esp-dl-arena.patch
```

⚠️ 升级 `esp-dl` 版本后此补丁很可能冲突（它改的是 `dl_model_base` 与 `dl_feat_base` 的内部实现），需要手工重打并更新本表；`minimize()` 会清掉算子名字表，跑 `run_trace` 前要么去掉 minimize 要么走 `m_variables`（详见 HANDOVER 廿二节 14 节）。
