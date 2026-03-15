# CLAUDE.md

本文件为 Claude Code 提供项目开发指引。

## 项目概览

**VoiceMask** 是一款可穿戴 AI 语音助理系统，由以下部分组成：
- **ESP32 固件**：通过麦克风采集音频，Opus 编码后经 BLE L2CAP 流式传输至 iOS
- **iOS App**：接收 BLE L2CAP 数据，连接 Pipecat 云端完成 STT/LLM/TTS 管道处理
- **Pipecat 后端**：云端语音处理管道

**当前阶段**：Phase 1 — BLE L2CAP 通道验证（Hello World）

## 构建命令

### ESP32 固件（ESP-IDF）

**环境初始化**（每次新终端会话需执行）：
```bash
export IDF_PYTHON_ENV_PATH=/Users/guoshiguang/.espressif/python_env/idf5.5_py3.13_env
source ~/esp/v5.5.2/esp-idf/export.sh
```

**编译**：
```bash
cd firmware
idf.py build
```

**串口监视**：
```bash
idf.py -p $(ls /dev/cu.usb* | head -1) monitor
```

**烧录固件**：

`idf.py flash` 对 ESP32-S3 Box Lite 无效，需直接调用 esptool 并指定 `--before default_reset --connect-attempts 20`：

```bash
cd firmware
PORT=/dev/cu.usbmodem21101  # 根据实际端口调整

/Users/guoshiguang/.espressif/python_env/idf5.5_py3.13_env/bin/python \
  ~/esp/v5.5.2/esp-idf/components/esptool_py/esptool/esptool.py \
  --chip esp32s3 -p $PORT -b 460800 \
  --before default_reset --connect-attempts 20 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 2MB \
  0x0     build/bootloader/bootloader.bin \
  0x10000 build/voicemask_firmware.bin \
  0x8000  build/partition_table/partition-table.bin
```

> 如连接仍失败，备用方案：按住 **BOOT** → 短按 **RST** → 松开 **BOOT** 进入下载模式，将命令中的 `--before default_reset` 改为 `--before no_reset` 后执行。

### iOS App

```bash
cd VoiceMaskApp
open VoiceMaskApp.xcodeproj
```

Xcode 编译：`Cmd+B`，运行：`Cmd+R`

### Pipecat 后端服务

```bash
cd pipecat-server
.venv/bin/python main.py
# 或通过 Makefile
make server
```

服务启动后监听 `http://0.0.0.0:8765`（WebSocket）。

## 架构说明

### BLE L2CAP 通信流程

```
ESP32 (NimBLE)                    iOS (CoreBluetooth)
     │                                  │
     │── BLE 广播 ───────────────────►│ 扫描（PSM Service UUID 过滤）
     │   设备名: "VoiceMask-01"        │
     │                                  │
     │◄────── BLE 连接 ────────────────│
     │                                  │
     │◄── GATT 读取 PSM 特征值 ─────────│
     │── 返回 PSM=128 (0x80) ─────────►│
     │                                  │
     │◄── L2CAP 通道建立 (PSM=128) ────│
     │                                  │
     │── L2CAP 数据 ("hello world") ──►│ 每 3 秒发送
```

**关键设计决策**：
- **NimBLE 而非 Bluedroid**：ESP-IDF 的 Bluedroid 栈不支持 BLE L2CAP CoC，必须使用 NimBLE
- **PSM 通过 GATT 暴露**：PSM 值（128）通过 GATT 特征值动态下发，iOS 侧可自动发现
- **协议帧类型**（规划中）：`0x00` 心跳，`0xFF` VAD 预热，`0xFE` 语音结束，`0x01` 音频帧

### ESP32 固件结构

```
firmware/main/
├── main.c           # 入口，NimBLE 初始化
├── ble_gap.c/.h     # GAP 广播与连接管理
├── ble_gatts.c/.h   # GATT 服务，PSM 特征值
├── ble_l2cap.c/.h   # L2CAP CoC 通道，数据传输
└── hello_timer.c/.h # 3 秒定时器（hello world）
```

### iOS App 结构

```
VoiceMaskApp/VoiceMaskApp/
├── VoiceMaskAppApp.swift   # SwiftUI App 入口
├── ContentView.swift       # 调试 UI（状态栏 + 消息列表）
├── Info.plist              # Bundle 配置，蓝牙权限
└── BLE/
    ├── BLEManager.swift    # CoreBluetooth 管理，扫描/连接/L2CAP
    └── L2CAPHandler.swift  # L2CAP 流读取
```

## BLE 常量

| 常量 | 值 | 用途 |
|------|----|------|
| 设备名 | `VoiceMask-01` | ESP32 广播名称 |
| PSM 服务 UUID | `0000AE00-0000-1000-8000-00805F9B34FB` | GATT 服务，用于 PSM 发现 |
| PSM 特征值 UUID | `0000AE01-0000-1000-8000-00805F9B34FB` | 存储 PSM 值 |
| PSM 值 | 128（`0x80`）| L2CAP 通道标识符 |
| L2CAP MTU | 512 字节 | 通道数据包大小 |

## ESP-IDF 配置

`firmware/sdkconfig.defaults` 关键配置：
```
CONFIG_BT_NIMBLE_ENABLED=y              # 使用 NimBLE（L2CAP CoC 必需）
CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=1    # 最多 1 个 L2CAP 通道
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE=512  # 512 字节 mbuf 块
```

## 未来阶段规划

- **Phase 2**：音频管道（VAD、Opus 编码、动态 BLE 连接参数）
- **Phase 3**：Pipecat 集成（WebRTC、STT/LLM/TTS）
- **Phase 4**：后台保活、通话处理、音频闪避

## 工程规范

### 监控规则

**所有接入 pipecat-server 的外部服务（STT / LLM / TTS 及未来新增服务）必须有对应的监控埋点。**

| 指标 | 要求 |
|------|------|
| 首包时间（TTFA/TTFT） | 必须记录（流式服务）；批量服务记录总耗时并保留字段供后续升级 |
| 总响应时间 | 必须记录 |
| 存储 | 写入 SQLite `conversations` 表对应字段 |
| 日志 | 每次请求在 `[Latency]` 行输出对应耗时 |
| 慢请求告警 | 任一环节超过 1000ms 输出 `WARNING` |

**新服务接入 Checklist（设计阶段必须包含）**：
- [ ] 在 `LatencyRecord` 中新增对应计时字段
- [ ] 在 `db.py` / `conversations` 表新增对应列
- [ ] 在 `/api/admin/stats` 响应中包含该服务的平均耗时
- [ ] 在 Admin 详情页耗时分解面板中展示

> 参考实现：`docs/modules/pipecat-pipeline/design/04-pipeline-latency-logging-backend-design.md`

---

## 相关文档

- Epic：`docs/epic/epic-01.md` — 完整产品愿景与里程碑
- 架构：`docs/epic/iOS 实时语音外设互联架构设计方案.md`
- 需求：`docs/feat/feat-01-esp32-ios-ble-l2cap-hello-world.md`
- ESP32 设计：`docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md`
- iOS 设计：`docs/modules/ble-channel/design/01-ble-l2cap-hello-world-ios-design.md`
