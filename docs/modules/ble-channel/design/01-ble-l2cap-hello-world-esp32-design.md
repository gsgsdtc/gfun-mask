# Design: 01 - ESP32 BLE L2CAP Hello World 固件设计

> 所属模块：ble-channel
> 关联需求：docs/feat/feat-01-esp32-ios-ble-l2cap-hello-world.md
> 关联 iOS 设计：docs/modules/ble-channel/design/01-ble-l2cap-hello-world-ios-design.md
> 更新日期：2026-03-03
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

实现 ESP32 固件，通过 BLE 广播被 iOS 发现，经 GATT 完成 PSM 协商后建立 L2CAP CoC 信道，之后每 3 秒通过该信道发送一条 `"hello world"` 字符串，并在连接断开时能被 iOS 重新发起的连接恢复。

### 1.2 设计约束

- 框架：ESP-IDF ≥ 5.x，使用 Bluedroid BLE 协议栈
- 必须使用 L2CAP CoC，不得用 GATT Notify 替代数据传输
- PSM 值通过 GATT Characteristic 暴露给 iOS，而非硬编码协商
- 发送间隔 3 秒（常量定义，便于后续调整）
- 固件无需持久化存储，重启后行为一致

---

## 2. 协议接口设计

### 2.1 BLE 广播

| 字段 | 值 | 说明 |
|------|----|------|
| Device Name | `VoiceMask-01` | iOS 扫描时识别设备 |
| Advertising Type | Connectable Undirected | 允许 iOS 主动连接 |
| Advertising Interval | 100ms | 快速被发现 |

### 2.2 GATT 服务（PSM 协商用）

| 角色 | UUID | 类型 | 说明 |
|------|------|------|------|
| PSM Service | `0000AE00-0000-1000-8000-00805F9B34FB` | Primary Service | 承载 PSM 协商 |
| PSM Characteristic | `0000AE01-0000-1000-8000-00805F9B34FB` | Read | iOS 读取此值获得 L2CAP PSM |

- PSM Characteristic 值：固定返回 2 字节 Little-Endian 整数，如 `0x80 0x00`（PSM = 128）
- iOS 读取后，调用 `openL2CAPChannel(PSM: 128)` 建立信道

### 2.3 L2CAP CoC 信道

| 参数 | 值 | 说明 |
|------|----|------|
| PSM | 128（`0x80`） | 与 GATT Characteristic 值一致 |
| MTU | 512 bytes | 足够承载 `"hello world"` 及后续音频帧 |
| 方向 | 双向 | 本 feat 仅使用 ESP32 → iOS 方向 |

### 2.4 数据帧格式（本 feat）

```
[ payload bytes ]
```

- 内容：UTF-8 编码的 `"hello world"`（11 字节）
- 无帧头，无长度前缀（后续 feat 扩展为带帧头协议）

---

## 3. 逻辑设计

### 3.1 固件主流程

```
上电启动
    │
    ▼
初始化 NVS / BT Controller / Bluedroid
    │
    ▼
注册 GATT Server
注册 PSM Service + Characteristic
    │
    ▼
开始 BLE 广播 (Device Name: VoiceMask-01)
    │
    ▼
等待 iOS 连接 ◄─────────────────────┐
    │                               │
    ▼                               │
iOS 连接成功 (GAP Connect Event)     │
    │                               │
    ▼                               │
iOS 读取 PSM Characteristic         │
(GATTS Read Event → 返回 PSM=128)   │
    │                               │
    ▼                               │
iOS 发起 L2CAP 连接                  │
(L2CAP Connect Event)               │
    │                               │
    ▼                               │
启动 3 秒定时器                       │
    │                               │
    ▼                               │
定时器触发 ──► 发送 "hello world"      │
    │         (esp_ble_l2cap_send)  │
    │                               │
    ▼                               │
检测到断连 (L2CAP Disconnect Event)  │
停止定时器                           │
重新开始广播 ────────────────────────┘
```

### 3.2 业务规则

- 同一时刻只维护一条 L2CAP 连接（单客户端）
- 定时器在 L2CAP 信道建立后启动，断连后立即停止
- 广播在断连后自动重启，无需重新上电

### 3.3 边界与异常处理

| 场景 | 处理方式 |
|------|---------|
| iOS 连接后未读 PSM 直接发起 L2CAP | 正常接受连接（PSM 已注册，无需 GATT 前置） |
| L2CAP 发送失败（缓冲区满） | 丢弃当次帧，等待下一个定时器周期重试 |
| BT Controller 初始化失败 | 打印错误日志，重启芯片（`esp_restart()`） |
| 多个设备尝试同时连接 | 接受第一个连接后停止广播，断连后重新广播 |

---

## 4. 模块结构

```
main/
├── main.c              # 入口，初始化流程
├── ble_gap.c/.h        # GAP 广播与连接管理
├── ble_gatts.c/.h      # GATT Server，PSM Characteristic
├── ble_l2cap.c/.h      # L2CAP 信道管理，数据发送
└── hello_timer.c/.h    # 3 秒定时器，触发发送
```

---

## 5. 测试方案

### 5.1 测试策略

| 层级 | 范围 | 工具 |
|------|------|------|
| 固件单测 | 帧封包/解包逻辑 | Unity (ESP-IDF 内置) |
| 集成联调 | 与 iOS App 完整握手 + 数据收发 | 配合 iOS App 手动验证 |
| 稳定性测试 | 连续运行 1 小时，统计丢帧率 | iOS App 计数显示 |

### 5.2 关键用例

| 用例 | 输入条件 | 期望结果 |
|------|---------|---------|
| 正常连接发送 | 固件上电，iOS 扫描连接 | 每 3 秒收到 `"hello world"` |
| PSM 读取 | iOS 读取 GATT Characteristic | 返回 PSM=128 |
| 断连重连 | iOS 关闭再重开 App | 固件重新广播，10 秒内重连 |
| 发送失败恢复 | 模拟 L2CAP 缓冲区满 | 下一周期继续发送，不崩溃 |

---

## 6. 影响评估

### 6.1 对现有功能的影响

无（新模块）

### 6.2 后续扩展接口

本设计预留以下扩展点，供后续 feat 使用：
- `ble_l2cap_send(data, len)` 接口抽象化，后续音频帧复用同一函数
- 帧格式预留帧头扩展（当前无帧头，后续加 `0x00/0xFF/0xFE/0x01` 帧类型字节）
- PSM Characteristic 与帧协议解耦，PSM 固定后期可移除 GATT 协商步骤

### 6.3 回滚方案

固件独立烧录，回滚只需重新烧录上一版 bin 文件，不影响 iOS App。
