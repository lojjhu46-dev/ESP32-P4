# esp-dl issue draft（向 Espressif 上报用，英文正文可直接粘贴到 GitHub issue）

> 提交入口：https://github.com/espressif/esp-dl/issues/new （选 Bug report 模板时把下面的
> 内容对应填入；本文件即完整正文）。提交后把 issue 链接回填到 HANDOVER 第廿二节 15 节。

---

**Title:** `[Bug] HumanFaceFeat (MFN_S8_V1) output is non-deterministic across runs with bit-identical input on ESP32-P4 — first divergent module varies randomly`

## Environment

- Chip: ESP32-P4 (rev v1.3), CPU 360 MHz, PSRAM 32 MB HEX @ 200 MHz
- IDF: v5.5.5
- esp-dl: v3.3.11 (component-managed checkout)
- Model: `human_face_feat_mfn_s8_v1.espdl` (MFN_S8_V1, 1.24 MB weights), input 112×112 via `FeatImagePreprocessor` (similarity-transform crop from a 300×300 JPEG-decoded frame)
- Runtime: `RUNTIME_MODE_SINGLE_CORE` (library default), AI task pinned to CPU0
- Use case: face recognition 1:N — the feature non-determinism makes fixed similarity thresholds impossible (thresholds must be re-calibrated every boot)

## Symptom

Feeding **the very same JPEG frame** through the pipeline repeatedly, the extracted feature vector differs from run to run. Self-similarity (cosine between the same image's features and the enrolled feature extracted from that same image) varies **0.24 – 1.00** across frames.

## What we verified is deterministic (all ruled out as the cause)

Our result frames carry CRC32 fingerprints at each pipeline stage. Across 10+ consecutive sends of the same JPEG:

| Stage | Fingerprint | Result |
|---|---|---|
| JPEG-decoded frame (producer side) | `frm_crc` over the decoded buffer | **constant** |
| Model input after repack/alignment | `ai_crc` over the exact bytes fed to the model | **constant** |
| Detection keypoints (5-point, drive the 112×112 crop) | reported vector | **constant** (1 unique value) |
| Detection box | reported | **constant** |

- The application runs on a single core; decode/serial/AI tasks are pinned (CPU0), matching the library's `RUNTIME_MODE_SINGLE_CORE`.
- We also **zero the whole model workspace before every inference** (`ModelContext::root_reset()` — both arena roots memset(0) — exposed via a small patch) **and write the zeros back to PSRAM** (`esp_cache_msync(..., C2M | UNALIGNED)`) to exclude both stale-workspace reads and CPU-cache/accelerator incoherency. The non-determinism persists.

## Per-module output CRC bisection

We patched `Model` with a `run_trace()` that executes the plan (SINGLE_CORE) and, after **every module**, CRC32s that module's output tensors (logical bytes, via `m_outputs_index` → context variables; the last entry covers the whole arena). Two traced runs back-to-back on the same frame, same core, same zeroed+C2M'd workspace:

| Round | First divergent module (of 103) | Frame self-sim |
|---|---|---|
| 1 | module #68 | 0.998 |
| 2 | module #9 | 0.996 |
| 3 | module #8 | 0.406 |
| 4 | module #13 | 0.239 |
| 5 | module #4 | 0.998 |
| 6 | module #2 | 0.960 |

Two key properties:

1. **The two runs inside the same frame already diverge** — same core, same stack depth, identical input, identical (zeroed) workspace. No "history" is involved.
2. **The first divergent module varies randomly** across rounds (#2/#4/#8/#9/#13/#68). A bug in one specific kernel would make the same module diverge first every time.

Together with the constant input fingerprints, the only consistent explanation is that **memory reads during kernel execution return different data at random points** — i.e., something in the P4 execution path (DSA/DMA/cache interaction, or PSRAM timing at 200 MHz) reads memory non-deterministically. App-side memory hygiene (zeroing, cache writeback, core pinning) does not fix it.

## Additional observation: randomly-poisoned workspace → 15 s inference

As a control, we added a variant that fills the workspace with `esp_fill_random` instead of zeros before inference. With a poisoned workspace, a normally-130 ms inference takes **~15 seconds** per frame, and results are still "reasonable" feature vectors. This shows that ops do read (uninitialized) workspace memory beyond what they write, and that poisoned data triggers a pathological slow path (suspect: denormal float handling or data-driven loops) — possibly the same read path involved in the non-determinism.

## Cross-boot effect

Without workspace zeroing, the non-determinism presents as a per-boot "mode": the same image self-similarity clusters around different values per boot (e.g., ~0.79 vs ~0.995 in earlier tests; within a boot it is mostly stable, occasionally per-frame bimodal). Fixed similarity thresholds therefore cannot be shared across boots — every device boot effectively needs its own calibration.

## How to reproduce (sketch)

1. Take the `human_face_recognition` example (ESP32-P4, MFN_S8_V1, SINGLE_CORE), load one static image into PSRAM, detect the face once (fixed keypoints), then run `HumanFaceFeat::run()` N times on the identical `img_t` + identical keypoints.
2. Print `esp_rom_crc32_le(0, feat->data, feat->get_bytes())` after each run.
3. Expected on affected hardware: the CRC varies across runs despite identical inputs.
4. To exclude workspace state: zero the whole variable-tensor arena (`ModelContext::root_reset()` + `esp_cache_msync(C2M)`) before each run — variance persists.
5. To confirm the workspace is actually read: fill it with `esp_fill_random` before each run — outputs change with the random fill, and inference time explodes (≈15 s for MFN).

Happy to provide our `run_trace()`/`zero_workspace()`/`poison_workspace()` patches, full traces, and board logs.

## Impact

- Any esp-dl feature/matching use case on ESP32-P4 cannot use absolute similarity thresholds across boots; every boot requires re-calibration.
- Recognition accuracy is bounded by single-frame feature noise (in our face-recognition pipeline: only multi-frame voting + a high-confidence single-frame fast path keep the false-positive rate at zero).

---

## （提交前自查清单——中文，不会进入 issue）

- [ ] 标题/正文语言：英文 ✓
- [ ] 附上我们能给的补充材料：`run_trace()`/`zero_workspace()`/`poison_workspace()` patch（在
      `esp-dl/esp-dl/dl/model/` 与 `esp-dl/esp-dl/vision/recognition/dl_feat_base.*`，见 HANDOVER
      第廿二节 15 节登记表）、两份完整 trace（各 103+1 项）
- [ ] esp-dl 版本、IDF 版本、芯片 rev 写清 ✓（rev v1.3 值得强调——如果 Espressif 手头是 rev ≥3.0
      的板子测不出，rev 差异本身就是线索）
- [ ] 提交后：issue 链接回填 HANDOVER 第廿二节 15 节
