# WiFi/BLE 恢复方案（不加任何硬件——板上 C6 就是无线模块）

## 背景与根因

板载 **ESP32-C6**（Wi-Fi 6 + BLE 5，SDIO 连 P4，出厂已烧 2.3.2 从机固件）就是无线模块，无需另加。之前摘除的原因：**版本错配**——工程解析到 esp_hosted 宿主组件 3.0.7，与出厂从机固件 2.3.2 协议不匹配（"major version mismatch"）。

厂商 wifi_scan 模板的 dependencies.lock 给出了**与出厂固件配套的精确版本对**（厂商真机验证过）：
- `espressif/esp_wifi_remote` **0.14.2**
- `espressif/esp_hosted` **2.0.13**（+ 传递依赖 `espressif/eppp_link` 0.3.1）
- IDF 5.5.0（本机 5.5.5 兼容）

板级配置（厂商 sdkconfig 权威值）：SDIO **slot 1**、总线宽 4、复位 GPIO **54** 高有效、从机 esp32c6。

## 实施步骤

### 1. 组件获取（离线，沿用项目既有模式）
- 先直接试：版本钉死后 `idf.py reconfigure`，若组件注册表现已可达则自动完成
- 注册表不可达时（本项目已知问题）：从 GitHub 按 tag 下载到本地检出 + override_path（与 esp-dl 同模式，clone 失败重试即可）：
  - `esp-wifi-remote` **v0.14.2** → `D:\esp32project\ESP32-P4\esp_wifi_remote`
  - `esp-hosted-mg` **v2.0.13** → `D:\esp32project\ESP32-P4\esp_hosted`
  - `eppp_link` **v0.3.1** → `D:\esp32project\ESP32-P4\eppp_link`（esp_wifi_remote 私有依赖）

### 2. `main/idf_component.yml`：恢复并钉死版本
```yaml
espressif/esp_wifi_remote: {version: "0.14.2", override_path: "../../esp_wifi_remote"}
espressif/esp_hosted:      {version: "2.0.13", override_path: "../../esp_hosted"}
espressif/eppp_link:       {version: "0.3.1",  override_path: "../../eppp_link"}
```

### 3. 恢复三处构建摘除点
- `main/CMakeLists.txt`：`CONFIG_APP_ENABLE_WIFI` 门控里的 app_wifi.c + esp_wifi_remote/esp_wifi（已存在，随配置生效）
- `main/main.c`：恢复 `app_wifi_init()` 调用（netif_init_safe 已是幂等，esp_hosted 构造函数恢复运行无碍）
- `sdkconfig`：`APP_ENABLE_WIFI=y`

### 4. esp_hosted 板级配置（sdkconfig.defaults，从厂商 demo 逐项复制）
SDIO 宿主接口、slot 1、总线宽 4、复位 GPIO 54 高有效、从机 esp32c6、RX 优化默认

### 5. `app_wifi.c` 增强（在既有扫描+连接骨架上）
- 凭据写入改 `esp_wifi_set_storage(WIFI_STORAGE_FLASH)`：一次配置断电保存
- 新增运行时命令 `{"cmd":"wifi","ssid":X,"pass":Y}`（faceai 命令路由，与 cam/stream 同风格）：无需重烧即可换 WiFi；Kconfig SSID 留空 = 开机只做扫描验证
- 状态照旧走 APP_MOD_WIFI 磁贴 + got-IP 事件

### 6. BLE（第二阶段，可行性先行）
- esp_hosted 2.0.13 下载后 grep 其 Kconfig 确认 BT/HCI 支持（esp-hosted-MG 的 BLE 走 HCI-over-transport，P4 侧跑 NimBLE 主机栈）
- 支持则：启用 `APP_ENABLE_BLE` + NimBLE 初始化 + **广播 + 扫描** demo（手机 nRF Connect 可见即验证），Kconfig/状态磁贴同风格
- 不支持则：如实说明（出厂 2.3.2 固件的 BT 能力限制），给出后续选项（JP1 串口重刷 C6 从机固件到带 BT 的版本，需接线）——不阻塞 WiFi 交付

### 7. 验证
- 开机串口：SDIO 传输初始化、无 "major version mismatch"、扫描到 AP 列表（= C6 链路通）
- `{"cmd":"wifi","ssid":..,"pass":..}` → got IP → 手机热点/路由器侧确认关联；板子双 IP（网口 192.168.33.2 + WiFi DHCP）后 3333 服务两个口都可达
- BLE：nRF Connect 扫到广播名
- 回归：网口链路、AI 帧率（esp_hosted 任务默认 CPU0，AI 同核——若掉帧再调绑核）、相机/夜视不受影响

## 风险与注意
- 组件下载若 GitHub 也断：重试即可（esp-dl 当年同样问题，重试成功）
- WiFi 任务的 CPU 占用与 AI 同核：验证 AI fps，掉帧则把 hosted 任务绑 CPU1 调优（备忘不阻塞）
- 扫描是阻塞调用（启动期 ~2-4s），维持原设计