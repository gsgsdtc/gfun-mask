# Design: 01 - iOS BLE L2CAP Hello World App 设计

> 所属模块：ble-channel
> 关联需求：docs/feat/feat-01-esp32-ios-ble-l2cap-hello-world.md
> 关联 ESP32 设计：docs/modules/ble-channel/design/01-ble-l2cap-hello-world-esp32-design.md
> 更新日期：2026-03-03
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

开发 iOS App，主动扫描并连接 ESP32 设备，通过读取 GATT PSM Characteristic 完成 L2CAP 信道协商，持续接收 ESP32 每 3 秒发送的 `"hello world"` 消息，并将消息内容与时间戳实时展示在界面上。断连后自动重扫描重连。

### 1.2 设计约束

- iOS ≥ 16.0，使用 SwiftUI + CoreBluetooth
- 必须使用 `CBPeripheral.openL2CAPChannel(PSM:)` 建立 L2CAP CoC，不得用 GATT Notify 替代
- 蓝牙权限：`NSBluetoothAlwaysUsageDescription`（前台使用场景，本 feat 不涉及后台）
- 本 feat 为开发者调试工具，UI 以功能性为主，无需打磨视觉

---

## 2. 接口设计

### 2.1 CoreBluetooth 关键接口使用

| 接口 | 用途 |
|------|------|
| `CBCentralManager.scanForPeripherals(withServices:)` | 按 PSM Service UUID 过滤扫描 |
| `CBCentralManager.connect(_:options:)` | 连接 ESP32 |
| `CBPeripheral.discoverServices(_:)` | 发现 PSM Service |
| `CBPeripheral.discoverCharacteristics(_:for:)` | 发现 PSM Characteristic |
| `CBPeripheral.readValue(for:)` | 读取 PSM 值 |
| `CBPeripheral.openL2CAPChannel(_:)` | 用 PSM 建立 L2CAP 信道 |
| `CBL2CAPChannel.inputStream` | 读取 ESP32 发送的数据 |

### 2.2 GATT 协议常量（与 ESP32 设计对齐）

| 常量 | 值 |
|------|----|
| PSM Service UUID | `0000AE00-0000-1000-8000-00805F9B34FB` |
| PSM Characteristic UUID | `0000AE01-0000-1000-8000-00805F9B34FB` |
| L2CAP PSM | 128（从 Characteristic 读取，非硬编码）|

---

## 3. 逻辑设计

### 3.1 连接建立流程

```
App 启动
    │
    ▼
CBCentralManager 初始化
等待 state == .poweredOn
    │
    ▼
按 PSM Service UUID 扫描外设
    │
    ▼
发现 VoiceMask-01 ──► 停止扫描 ──► connect()
    │
    ▼
didConnect ──► discoverServices([PSM_SERVICE_UUID])
    │
    ▼
didDiscoverServices ──► discoverCharacteristics([PSM_CHAR_UUID])
    │
    ▼
didDiscoverCharacteristics ──► readValue(for: psmCharacteristic)
    │
    ▼
didUpdateValue ──► 解析 PSM（2字节 Little-Endian）
    │
    ▼
openL2CAPChannel(PSM)
    │
    ▼
didOpen channel ──► 将 inputStream 加入 RunLoop
    │
    ▼
stream(_:handle:) 事件循环
hasBytesAvailable ──► read() ──► 解析为 UTF-8 字符串
    │
    ▼
追加消息记录（content + timestamp）──► 刷新 UI
    │
    ▼
didDisconnect ──► 清理 channel 和 stream
    └──► 重新扫描（回到扫描步骤）
```

### 3.2 业务规则

- 扫描时按 Service UUID 过滤，避免发现无关设备
- PSM 值从 Characteristic 动态读取，不硬编码，保持与固件一致
- 消息列表保留最近 100 条，超出时丢弃最旧的
- 界面显示连接状态（扫描中 / 已连接 / 已断连）

### 3.3 边界与异常处理

| 场景 | 处理方式 |
|------|---------|
| 蓝牙未授权 | 展示提示文案，引导用户前往系统设置开启权限 |
| 扫描超时（30s 无结果） | 展示"未找到设备"提示，提供手动重试按钮 |
| PSM Characteristic 读取失败 | 断开连接，重新扫描 |
| L2CAP 信道打开失败 | 断开连接，重新扫描，日志记录错误码 |
| inputStream 读取乱码 | 丢弃该帧，不崩溃，记录日志 |
| 断连 | 自动触发重扫描，无需用户操作 |

---

## 4. 页面与组件设计

### 4.1 页面结构（单页面 App）

```
┌─────────────────────────────┐
│  VoiceMask BLE 调试          │
├─────────────────────────────┤
│  状态：● 已连接 VoiceMask-01  │
├─────────────────────────────┤
│  [消息列表]                   │
│  ┌───────────────────────┐  │
│  │ 14:03:21  hello world │  │
│  │ 14:03:18  hello world │  │
│  │ 14:03:15  hello world │  │
│  │ ...                   │  │
│  └───────────────────────┘  │
├─────────────────────────────┤
│  共收到：12 条   [清空]       │
└─────────────────────────────┘
```

### 4.2 组件层次

```
ContentView
├── StatusBar          # 连接状态指示（颜色 + 文字）
├── MessageListView    # 消息滚动列表
│   └── MessageRow    # 单条消息（时间戳 + 内容）
└── FooterBar          # 计数 + 清空按钮
```

### 4.3 状态定义

| 状态枚举 | 显示文案 | 指示灯颜色 |
|---------|---------|----------|
| `.scanning` | 扫描中... | 黄色（闪烁） |
| `.connecting` | 连接中... | 黄色 |
| `.connected(name)` | 已连接 {name} | 绿色 |
| `.disconnected` | 已断连，重新扫描中 | 红色 |
| `.unauthorized` | 请开启蓝牙权限 | 灰色 |

---

## 5. 测试方案

### 5.1 测试策略

| 层级 | 范围 | 说明 |
|------|------|------|
| 单元测试 | PSM 字节解析（2字节 LE → UInt16）| 纯函数，可离线测试 |
| 单元测试 | 消息列表上限（最多 100 条）逻辑 | 纯函数 |
| 集成联调 | 与 ESP32 固件完整握手 + 数据收发 | 真机 + 真实硬件 |
| 稳定性测试 | 连续运行 1 小时，界面持续更新 | 观察是否崩溃或卡顿 |

### 5.2 关键用例

| 用例 | 输入条件 | 期望结果 |
|------|---------|---------|
| 正常连接 | App 启动，ESP32 已上电广播 | 10 秒内建立连接，界面变绿 |
| 消息接收 | L2CAP 信道建立后 | 每 3 秒新增一条消息记录 |
| PSM 解析 | Characteristic 返回 `[0x80, 0x00]` | 解析 PSM = 128 |
| 断连重连 | 关闭 ESP32 电源后重新上电 | App 自动重连，继续收消息 |
| 消息超限 | 累计超过 100 条 | 自动丢弃最旧一条，列表保持 100 条 |
| 权限拒绝 | 系统蓝牙权限未授权 | 显示提示，不崩溃 |

---

## 6. 影响评估

### 6.1 对现有功能的影响

无（新项目）

### 6.2 后续扩展接口

- `BLEManager`（管理扫描/连接/L2CAP）设计为单例，后续 feat 直接复用
- `L2CAPChannel` 的 `inputStream` 读取逻辑抽象为 `FrameParser`，后续支持多帧类型（`0x00/0xFF/0xFE/0x01+N`）
- `MessageListView` 后续可复用为日志调试面板

### 6.3 回滚方案

iOS App 独立工程，回滚只需切换 Git 分支重新编译，不影响 ESP32 固件。
