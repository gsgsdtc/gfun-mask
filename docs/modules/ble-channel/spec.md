# Module Spec: ble-channel

> 模块：BLE L2CAP 通信通道
> 最近同步：2026-03-04
> 状态：Phase 1 完成 (Hello World)

---

## 1. 模块概述

实现 ESP32 与 iOS 之间的 BLE L2CAP CoC（面向连接信道）双向通信，为后续音频流传输、VAD 预警、心跳保活提供底层通道。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | 无（物理层 BLE 信号） |
| 下游 | ESP32 音频采集模块（Phase 2）、iOS Pipecat Client SDK（Phase 3） |
| 输入 | BLE 空中数据包、iOS/ESP32 本地控制指令 |
| 输出 | BLE 空中数据包、L2CAP 数据帧 |

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
| Advertising Data | Flags + 128-bit Service UUID | 用于 iOS 服务过滤 |

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
| MTU | 512 bytes | 信道最大传输单元 |
| 方向 | 双向 | 当前仅使用 ESP32 → iOS |

### 2.4 数据帧格式（Phase 1）

```
[ UTF-8 encoded string ]
```

- 内容：`"hello world"`（11 字节）
- 无帧头，无长度前缀
- Phase 2 将扩展为带帧头协议（`0x00/0xFF/0xFE/0x01`）

---

## 3. 模块结构

### 3.1 ESP32 固件

```
firmware/main/
├── main.c              # 入口，NimBLE 协议栈初始化
├── ble_gap.c/.h        # GAP 广播与连接管理
├── ble_gatts.c/.h      # GATT Server，PSM Characteristic
├── ble_l2cap.c/.h      # L2CAP CoC 信道管理
└── hello_timer.c/.h    # 周期定时器
```

### 3.2 iOS App

```
VoiceMaskApp/VoiceMaskApp/
├── VoiceMaskAppApp.swift   # SwiftUI App 入口
├── ContentView.swift       # 调试界面
└── BLE/
    ├── BLEManager.swift    # CoreBluetooth 管理器
    └── L2CAPHandler.swift  # L2CAP Stream 处理
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
void vm_l2cap_init(void);                    // 初始化 L2CAP 内存池
void ble_l2cap_on_ble_connect(uint16_t conn_handle);  // BLE 连接建立回调
void ble_l2cap_on_ble_disconnect(void);      // BLE 断连回调
void ble_l2cap_send_hello(void);             // 发送 "hello world"
```

**hello_timer.h**
```c
#define HELLO_INTERVAL_MS  3000   // 发送间隔 3 秒

void hello_timer_start(void);     // 启动定时器
void hello_timer_stop(void);      // 停止定时器
```

### 4.2 iOS 公开接口

**BLEManager**
```swift
class BLEManager: NSObject, ObservableObject {
    @Published var connectionState: ConnectionState
    @Published var messages: [BLEMessage]

    func clearMessages()          // 清空消息列表
    func appendMessage(_ content: String)  // 追加消息
}
```

**L2CAPHandler**
```swift
class L2CAPHandler: NSObject, StreamDelegate {
    init(channel: CBL2CAPChannel, manager: BLEManager)
    func open()   // 启动 Stream 监听
    func close()  // 关闭 Stream
}
```

---

## 5. 状态机

### 5.1 ESP32 连接状态

```
┌─────────┐    scan      ┌───────────┐    connect    ┌──────────┐
│ Advertising │ ───────► │ Connected │ ───────────► │ L2CAP Open │
└─────────┘            └───────────┘             └──────────┘
     ▲                       │                         │
     │                       │ disconnect              │ disconnect
     └───────────────────────┴─────────────────────────┘
```

### 5.2 iOS 扫描状态

```swift
enum ConnectionState {
    case unauthorized    // 蓝牙权限未授权
    case scanning        // 扫描中
    case connecting      // 连接中
    case connected(name: String)  // 已连接
    case disconnected    // 已断连
}
```

---

## 6. 核心逻辑

### 6.1 连接建立流程

```
ESP32                                    iOS
   │                                       │
   │──── BLE Advertising (PSM UUID) ─────►│
   │                                       │ scanForPeripherals(withServices:)
   │◄─────────── connect ─────────────────│
   │                                       │
   │  GAP Connect Event                    │
   │                                       │ discoverServices
   │◄── discoverCharacteristics ──────────│
   │                                       │
   │◄────── readValue (PSM) ──────────────│
   │──── PSM = 128 (0x80 0x00) ───────────►│
   │                                       │
   │◄────── openL2CAPChannel(128) ────────│
   │  L2CAP CoC Connected                  │
   │                                       │
   │──── "hello world" (every 3s) ────────►│
```

### 6.2 断连重连

- ESP32：断连后自动重新广播
- iOS：断连后自动重新扫描
- 无需用户干预

---

## 7. 配置参数

### 7.1 ESP32 (sdkconfig.defaults)

```
CONFIG_BT_NIMBLE_ENABLED=y              # 使用 NimBLE
CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=1    # 最大 L2CAP 信道数
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE=512  # mbuf 块大小
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=30  # mbuf 块数量
```

### 7.2 iOS (Info.plist)

```xml
<key>NSBluetoothAlwaysUsageDescription</key>
<string>需要蓝牙权限以连接 VoiceMask 外设并接收语音助手消息。</string>
```

---

## 8. 验收状态

### Phase 1 — BLE 通道验证

| 验收项 | 状态 | 备注 |
|--------|------|------|
| ESP32 固件能被 iOS App 发现 | ✅ | PSM UUID 放在 Advertising Data |
| 建立 BLE L2CAP 连接 | ✅ | 通过 GATT PSM 协商 |
| 每 3 秒发送 "hello world" | ✅ | 定时器驱动 |
| iOS 正确接收并显示消息 | ✅ | 调试界面 |
| 断连后自动重连 | ✅ | 双端自动恢复 |

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-04 | feat #01 | Phase 1 完成：ESP32 + iOS BLE L2CAP Hello World |
| 2026-03-04 | fix | 修复 NimBLE 符号冲突：`ble_gatts_init` → `vm_gatts_init`，`ble_l2cap_init` → `vm_l2cap_init` |
| 2026-03-04 | fix | 修复 iOS 扫描不到设备：将 PSM Service UUID 从 Scan Response 移到 Advertising Data |