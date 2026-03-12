# Module Spec: ble-channel

> 模块：BLE L2CAP 通信通道
> 最近同步：2026-03-12
> 状态：Phase 2 完成（音频帧协议 + PCM 直传）

---

## 1. 模块概述

实现 ESP32 与 iOS 之间的 BLE L2CAP CoC（面向连接信道）双向通信，支持音频帧传输和控制指令。Phase 1 实现 Hello World 通道验证；Phase 2 实现完整帧协议（音频帧、控制指令、流控）。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | 无（物理层 BLE 信号） |
| 下游 | ESP32 音频采集模块（audio-capture）、iOS 音频存储模块（audio-player） |
| 输入 | BLE 空中数据包、iOS 控制指令（START/STOP_RECORD） |
| 输出 | BLE 音频帧（PCM/Opus）、控制响应帧（RECORD_END） |

### 1.2 技术选型

| 端 | 框架 | 说明 |
|----|------|------|
| ESP32 | ESP-IDF + NimBLE | Bluedroid 不支持 L2CAP CoC，必须用 NimBLE |
| iOS | CoreBluetooth | 系统 BLE 框架，支持 L2CAP CoC (iOS 11+) |

---

## 2. 协议接口

### 2.1 BLE 广播

| 字段 | 值 | 说明 |
|------|----|------|
| Device Name | `VoiceMask-01` | iOS 扫描时识别 |
| Advertising Type | Connectable Undirected | 允许 iOS 主动连接 |
| Advertising Interval | 100ms | 快速被发现 |
| Advertising Data | Flags + 128-bit Service UUID | 用于 iOS 服务过滤（在 Advertising Data 中，非 Scan Response） |

### 2.2 GATT 服务（PSM 协商）

| 角色 | UUID | 类型 | 说明 |
|------|------|------|------|
| PSM Service | `0000AE00-0000-1000-8000-00805F9B34FB` | Primary Service | 承载 PSM 协商 |
| PSM Characteristic | `0000AE01-0000-1000-8000-00805F9B34FB` | Read | iOS 读取获得 L2CAP PSM |

**PSM Characteristic 值格式**：
- 2 字节 Little-Endian 整数
- 当前值：`0x80 0x00` → PSM = 128

### 2.3 L2CAP CoC 信道

| 参数 | 值 | 说明 |
|------|----|------|
| PSM | 128 (`0x80`) | 动态 PSM，范围 0x80-0xFF |
| CoC MTU（ESP32 端） | 1024 bytes | 须大于最大帧 643B（PCM 帧头 3B + 640B payload） |
| 读缓冲区（iOS 端） | 1024 bytes | 单次 read() 缓冲，覆盖最大帧 |
| 方向 | 双向 | ESP32 → iOS 音频帧；iOS → ESP32 控制指令 |

### 2.4 数据帧格式（Phase 2）

**帧结构（通用）：**
```
[ Frame Type (1B) ][ Payload Len Lo (1B) ][ Payload Len Hi (1B) ][ Payload (N B) ]
```

**帧类型定义：**

| 帧类型 | 值 | 方向 | Payload | 说明 |
|--------|----|------|---------|------|
| HEARTBEAT | `0x00` | ESP32 → iOS | 无 | Phase 3 心跳保活 |
| AUDIO | `0x01` | ESP32 → iOS | PCM/Opus 数据 | 音频帧（当前为 PCM 直传 640B） |
| CMD_START_RECORD | `0x10` | iOS → ESP32 | 无 | 启动录音 |
| CMD_STOP_RECORD | `0x11` | iOS → ESP32 | 无 | 停止录音 |
| RECORD_END | `0x12` | ESP32 → iOS | 4B uint32 总帧数 | 录音结束确认 |
| END_OF_UTTERANCE | `0xFE` | ESP32 → iOS | 无 | Phase 3 VAD 句子结束 |
| VAD_PREWARM | `0xFF` | ESP32 → iOS | 无 | Phase 3 VAD 预热 |

**控制指令帧（iOS → ESP32）：**
```
[ 0x10 或 0x11 ][ 0x00 ][ 0x00 ]   // payload 长度 = 0
```

**PCM 音频帧（ESP32 → iOS，当前阶段）：**
```
[ 0x01 ][ 0x80 ][ 0x02 ][ 640 字节 PCM int16 little-endian ]
```

### 2.5 L2CAP 流控（TX Stall）

| 状态 | 触发 | 处理 |
|------|------|------|
| TX_STALLED | `ble_l2cap_send()` 返回 `BLE_HS_ESTALLED` | 标记 stall，丢弃当帧，等待 TX_UNSTALLED 事件 |
| TX_UNSTALLED | `BLE_L2CAP_EVENT_COC_TX_UNSTALLED` 事件 | 清除 stall 标记，恢复发送 |

---

## 3. 模块结构

### 3.1 ESP32 固件

```
firmware/main/
├── main.c              # 入口，NimBLE 协议栈初始化
├── ble_gap.c/.h        # GAP 广播与连接管理
├── ble_gatts.c/.h      # GATT Server，PSM Characteristic
├── ble_l2cap.c/.h      # L2CAP CoC 信道管理、帧收发、流控
└── hello_timer.c/.h    # 周期定时器（Phase 1 保留）
```

### 3.2 iOS App

```
VoiceMaskApp/VoiceMaskApp/
├── BLE/
│   ├── BLEManager.swift    # CoreBluetooth 管理器（扫描/连接/GATT）
│   └── L2CAPHandler.swift  # L2CAP Stream 处理、帧解析、指令发送
└── Audio/
    └── FrameParser.swift   # 帧协议解析器（cursor 方式，支持粘包）
```

---

## 4. 接口定义

### 4.1 ESP32 公开接口

**ble_gap.h**
```c
void ble_gap_on_sync(void);              // NimBLE Host 就绪回调
void ble_gap_start_advertising(void);    // 启动广播
```

**ble_gatts.h**
```c
#define BLE_L2CAP_PSM  128u   // L2CAP PSM 值

void vm_gatts_init(void);     // 注册 PSM GATT 服务
```

**ble_l2cap.h**
```c
typedef enum {
    FRAME_TYPE_HEARTBEAT       = 0x00,
    FRAME_TYPE_AUDIO           = 0x01,
    FRAME_TYPE_CMD_START_RECORD = 0x10,
    FRAME_TYPE_CMD_STOP_RECORD  = 0x11,
    FRAME_TYPE_RECORD_END      = 0x12,
    FRAME_TYPE_VAD_PREWARM     = 0xFF,
    FRAME_TYPE_END_OF_UTTERANCE = 0xFE,
} frame_type_t;

typedef void (*ble_l2cap_cmd_callback_t)(frame_type_t cmd);

void vm_l2cap_init(void);                                        // 初始化 L2CAP 内存池
void ble_l2cap_on_ble_connect(uint16_t conn_handle);             // BLE 连接建立后注册 CoC Server
void ble_l2cap_on_ble_disconnect(void);                          // BLE 断连清理状态
void ble_l2cap_set_cmd_callback(ble_l2cap_cmd_callback_t cb);    // 注册控制指令回调
int  ble_l2cap_send_frame(frame_type_t type, const uint8_t *payload, uint16_t len);  // 发送帧
bool ble_l2cap_is_tx_ready(void);                                // 查询 TX 是否就绪
void ble_l2cap_send_hello(void);                                 // Phase 1 发送 hello world
```

### 4.2 iOS 公开接口

**BLEManager**
```swift
class BLEManager: NSObject, ObservableObject {
    @Published var connectionState: ConnectionState
    @Published var messages: [BLEMessage]
    var l2capHandler: L2CAPHandler?   // 当前 L2CAP 信道处理器

    func clearMessages()
    func appendMessage(_ content: String)
}

enum ConnectionState {
    case unauthorized
    case scanning
    case connecting
    case connected(name: String)
    case disconnected
}
```

**L2CAPHandler**
```swift
final class L2CAPHandler: NSObject, StreamDelegate {
    // 回调
    var onAudioFrame: ((Data) -> Void)?         // 收到音频帧 payload
    var onRecordEnd: ((UInt32) -> Void)?        // 收到 RECORD_END（含总帧数）
    var onMessage: ((String) -> Void)?          // Phase 1 兼容文本消息

    init(channel: CBL2CAPChannel, manager: BLEManager)
    func open()                     // 开启 Stream 监听
    func close()                    // 关闭 Stream，重置 FrameParser
    func sendStartRecord()          // 发送 CMD_START_RECORD 帧
    func sendStopRecord()           // 发送 CMD_STOP_RECORD 帧
}
```

**FrameParser**
```swift
final class FrameParser {
    func parse(_ data: Data) -> [AudioFrame]   // 解析，支持跨次粘包
    func reset()                               // 连接断开时重置缓冲区
    var bufferSize: Int { get }
}

enum FrameType: UInt8 {
    case heartbeat = 0x00, audio = 0x01, vadPrewarm = 0xFF,
         endOfUtterance = 0xFE, cmdStartRecord = 0x10,
         cmdStopRecord = 0x11, recordEnd = 0x12
}

struct AudioFrame {
    let type: UInt8
    let payload: Data
    var frameType: FrameType?
}
```

---

## 5. 状态机

### 5.1 ESP32 连接状态

```
┌─────────────┐    iOS connect    ┌───────────┐    iOS openL2CAP   ┌────────────┐
│ Advertising │ ────────────────► │ Connected │ ─────────────────► │ CoC Open   │
└─────────────┘                   └───────────┘                    └────────────┘
       ▲                               │                                │
       │            disconnect         └──────────────┬─────────────────┘
       └──────────────────────────────────────────────┘
```

### 5.2 L2CAP TX 流控状态

```
┌──────────┐  BLE_HS_ESTALLED  ┌───────────┐
│ TX_READY │ ─────────────────► │ TX_STALLED│
└──────────┘                    └───────────┘
     ▲                               │
     │    TX_UNSTALLED event         │
     └───────────────────────────────┘
```

### 5.3 iOS 帧解析状态

```
[buffer 积累] → [cursor 扫描] → [完整帧输出] → [不完整帧留存]
```
- 使用 cursor 偏移量，不修改 buffer（规避 `Data.removeFirst` 的 startIndex offset 崩溃）
- 解析完成后一次性 `buffer.removeFirst(cursor)`

---

## 6. 核心逻辑

### 6.1 连接建立流程

```
ESP32                                    iOS
   │                                       │
   │──── BLE Advertising (PSM UUID) ─────►│
   │◄─────────── connect ─────────────────│
   │  GAP Connect Event                    │
   │                                       │ discoverServices → discoverCharacteristics
   │◄────── readValue (PSM) ──────────────│
   │──── PSM = 128 (0x80 0x00) ───────────►│
   │◄────── openL2CAPChannel(128) ────────│
   │  L2CAP CoC Connected                  │
   │  (注册 CoC Server 等待连接请求)         │
```

### 6.2 录音控制流程

```
iOS                           ESP32
 │── CMD_START_RECORD (0x10) ──►│
 │                               │  audio_pipeline_start()
 │                               │  [I2S → PCM → BLE L2CAP]
 │◄── AUDIO frames (0x01) ──────│  每帧 643 字节（3B 头 + 640B PCM）
 │                               │
 │── CMD_STOP_RECORD (0x11) ───►│
 │                               │  audio_pipeline_stop()
 │◄── RECORD_END (0x12) ────────│  payload = uint32 总帧数
```

### 6.3 断连重连

- ESP32：断连后自动重新广播，L2CAP 状态清零
- iOS：断连后自动重新扫描
- 无需用户干预

---

## 7. 配置参数

### 7.1 ESP32 (sdkconfig.defaults)

```
CONFIG_BT_NIMBLE_ENABLED=y              # 使用 NimBLE
CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=1   # 最大 L2CAP 信道数
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE=512  # mbuf 块大小
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=30  # mbuf 块数量
```

**ble_l2cap.c 内部常量：**
```c
#define COC_MTU       1024   // CoC MTU（须 > 643B PCM 帧）
#define COC_BUF_COUNT 10     // 接收缓冲区数量
```

### 7.2 iOS (Info.plist)

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>需要蓝牙权限以连接 VoiceMask 外设并接收语音助手消息。</string>
```

---

## 8. 验收状态

### Phase 1 — BLE 通道验证 ✅

| 验收项 | 状态 |
|--------|------|
| ESP32 固件被 iOS App 发现 | ✅ |
| 建立 BLE L2CAP 连接 | ✅ |
| 每 3 秒发送 "hello world" | ✅ |
| iOS 正确接收并显示消息 | ✅ |
| 断连后自动重连 | ✅ |

### Phase 2 — 音频帧协议 ✅

| 验收项 | 状态 | 备注 |
|--------|------|------|
| 帧协议收发（type+len+payload） | ✅ | 双向 |
| iOS 发送 START/STOP_RECORD 控制帧 | ✅ | 3 字节帧 |
| ESP32 接收并触发录音启停 | ✅ | cmd_callback |
| ESP32 发送 PCM 音频帧（640B payload） | ✅ | PCM 直传模式 |
| iOS FrameParser 正确解析（含粘包） | ✅ | cursor 方式 |
| BLE L2CAP TX 流控（stall/unstall） | ✅ | BLE_HS_ESTALLED |
| ESP32 发送 RECORD_END 帧 | ✅ | 含总帧数 |

### 单元测试

| 测试文件 | 框架 | 覆盖逻辑 |
|----------|------|---------|
| `firmware/test/test_psm_encode.c` | ESP-IDF Unity | PSM 2 字节 Little-Endian 编解码 |

**测试用例（`firmware/test/test_psm_encode.c`）：**

| 用例 | 验证内容 |
|------|---------|
| `psm_encode: PSM=128 produces [0x80, 0x00]` | PSM=128 正确编码为 LE 序列 |
| `psm_decode: [0x80, 0x00] decodes to 128` | LE 序列正确解码回 PSM |
| `psm encode/decode round-trip` | 编解码互为逆操作，值无失真 |
| `psm_encode: PSM=255 boundary` | 边界值 255 编解码正确，无位运算溢出 |

**运行方式：**
```bash
idf.py -C firmware/test build && ./firmware/test/build/test_psm_encode.elf
```

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-04 | feat #01 | Phase 1 完成：ESP32 + iOS BLE L2CAP Hello World |
| 2026-03-04 | fix | 修复 NimBLE 符号冲突：`ble_gatts_init` → `vm_gatts_init`，`ble_l2cap_init` → `vm_l2cap_init` |
| 2026-03-04 | fix | 修复 iOS 扫描不到设备：PSM Service UUID 移到 Advertising Data |
| 2026-03-09 | feat #02 | Phase 2：实现完整帧协议（音频帧/控制指令/RECORD_END） |
| 2026-03-09 | fix | CoC MTU 512 → 1024（PCM 帧 643B > 512B） |
| 2026-03-09 | fix | iOS FrameParser：cursor 方式解析，修复 Data.removeFirst EXC_BREAKPOINT 崩溃 |
| 2026-03-09 | feat | BLE L2CAP TX 流控：BLE_HS_ESTALLED 检测 + TX_UNSTALLED 恢复 |
| 2026-03-09 | fix | iOS L2CAP 读缓冲区 512 → 1024，避免帧被截断 |
| 2026-03-12 | test | 新增 PSM 编解码单元测试（Unity）：psm_encode/psm_decode 含边界值覆盖 |
