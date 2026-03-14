# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

**VoiceMask** is a wearable AI voice assistant system consisting of:
- **ESP32 Firmware**: Captures audio via microphone, encodes with Opus, streams through BLE L2CAP to iOS
- **iOS App**: Receives BLE L2CAP data, connects to Pipecat cloud for STT/LLM/TTS pipeline
- **Pipecat Backend** (future): Cloud-based voice processing pipeline

**Current Phase**: Phase 1 — BLE L2CAP channel verification (Hello World)

## Build Commands

### ESP32 Firmware (ESP-IDF)

```bash
cd firmware

# Build
idf.py build

# Flash to device (detect port automatically)
idf.py -p $(ls /dev/cu.usb* | head -1) flash

# Monitor serial output
idf.py -p $(ls /dev/cu.usb* | head -1) monitor

# Build and flash in one command
idf.py -p $(ls /dev/cu.usb* | head -1) build flash monitor
```

### iOS App

```bash
cd VoiceMaskApp
# Open in Xcode
open VoiceMaskApp.xcodeproj
```

Build with Xcode: `Cmd+B`, Run: `Cmd+R`

## Architecture

### BLE L2CAP Communication

```
ESP32 (NimBLE)                    iOS (CoreBluetooth)
     │                                  │
     │── BLE Advertising ─────────────►│ Scan (PSM Service UUID filter)
     │   Device Name: "VoiceMask-01"   │
     │                                  │
     │◄────── BLE Connect ─────────────│
     │                                  │
     │◄── GATT Read PSM Characteristic ─│
     │── Returns PSM=128 (0x80) ───────►│
     │                                  │
     │◄── L2CAP Channel Open (PSM=128) ─│
     │                                  │
     │── L2CAP Data ("hello world") ───►│ Stream every 3 seconds
```

**Key Design Decisions**:
- **NimBLE over Bluedroid**: ESP-IDF's Bluedroid stack does NOT support BLE L2CAP CoC. Must use NimBLE.
- **PSM via GATT**: PSM value (128) is exposed through a GATT characteristic, allowing iOS to discover it dynamically.
- **Protocol Frame Types** (future): `0x00` heartbeat, `0xFF` VAD pre-warm, `0xFE` end-of-utterance, `0x01` audio frame

### ESP32 Firmware Structure

```
firmware/main/
├── main.c           # Entry point, NimBLE initialization
├── ble_gap.c/.h     # GAP advertising & connection management
├── ble_gatts.c/.h   # GATT server, PSM characteristic
├── ble_l2cap.c/.h   # L2CAP CoC channel, data transmission
└── hello_timer.c/.h # 3-second timer for "hello world"
```

### iOS App Structure

```
VoiceMaskApp/VoiceMaskApp/
├── VoiceMaskAppApp.swift   # SwiftUI App entry
├── ContentView.swift       # Debug UI (status bar + message list)
├── Info.plist              # Bundle config, Bluetooth permission
└── BLE/
    ├── BLEManager.swift    # CoreBluetooth manager, scan/connect/L2CAP
    └── L2CAPHandler.swift  # L2CAP stream reading
```

## BLE Constants

| Constant | Value | Usage |
|----------|-------|-------|
| Device Name | `VoiceMask-01` | ESP32 advertising name |
| PSM Service UUID | `0000AE00-0000-1000-8000-00805F9B34FB` | GATT service for PSM discovery |
| PSM Characteristic UUID | `0000AE01-0000-1000-8000-00805F9B34FB` | Contains PSM value |
| PSM Value | 128 (`0x80`) | L2CAP channel identifier |
| L2CAP MTU | 512 bytes | Channel payload size |

## ESP-IDF Configuration

Key settings in `firmware/sdkconfig.defaults`:
```
CONFIG_BT_NIMBLE_ENABLED=y              # Use NimBLE (required for L2CAP CoC)
CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=1    # Max 1 L2CAP channel
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE=512  # 512-byte mbuf blocks
```

## Future Phases

- **Phase 2**: Audio pipeline (VAD, Opus encoding, dynamic BLE connection parameters)
- **Phase 3**: Pipecat integration (WebRTC, STT/LLM/TTS)
- **Phase 4**: Background survival, phone call handling, ducking

## Engineering Rules

### 监控规则（Observability Rule）

**所有接入 pipecat-server 的外部服务（STT / LLM / TTS 及未来新增服务）必须有对应的监控埋点。**

具体要求：

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

## Related Documentation

- Epic: `docs/epic/epic-01.md` — Full product vision and milestones
- Architecture: `docs/epic/iOS 实时语音外设互联架构设计方案.md`
- Feat: `docs/feat/feat-01-esp32-ios-ble-l2cap-hello-world.md`
- ESP32 Design: `docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md`
- iOS Design: `docs/modules/ble-channel/design/01-ble-l2cap-hello-world-ios-design.md`