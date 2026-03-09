# Design: 02 - BLE 音频帧协议设计

> 所属模块：ble-channel
> 关联需求：docs/feat/feat-02-esp32-audio-capture-opus-encoding.md
> 更新日期：2026-03-05
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

在 feat-01 验证的 BLE L2CAP 通道基础上，定义音频数据传输的帧协议，支持：
- 音频帧传输（Opus 编码数据）
- 控制指令（开始/停止录音）
- 录音结束标记

### 1.2 设计约束

- 复用 feat-01 的 BLE L2CAP CoC 信道（PSM=128, MTU=512）
- 帧格式需向后兼容 Phase 1 的 "hello world" 测试
- 帧头设计需预留扩展空间（后续 VAD、心跳等）

---

## 2. 帧协议设计

### 2.1 帧格式

```
┌──────────┬──────────┬─────────────────────┐
│ Frame Type│ Length   │ Payload             │
│ (1 byte)  │ (2 bytes)│ (N bytes)           │
└──────────┴──────────┴─────────────────────┘
```

| 字段 | 大小 | 说明 |
|------|------|------|
| Frame Type | 1 byte | 帧类型标识 |
| Length | 2 bytes | Payload 长度（Little-Endian） |
| Payload | N bytes | 实际数据内容 |

### 2.2 帧类型定义

| Frame Type | 值 | 方向 | 说明 |
|------------|-----|------|------|
| FRAME_TYPE_HEARTBEAT | 0x00 | ESP32 → iOS | 心跳保活（Phase 3） |
| FRAME_TYPE_AUDIO | 0x01 | ESP32 → iOS | Opus 音频帧 |
| FRAME_TYPE_VAD_PREWARM | 0xFF | ESP32 → iOS | VAD 预警（Phase 3） |
| FRAME_TYPE_END_OF_UTTERANCE | 0xFE | ESP32 → iOS | 说话结束标记（Phase 3） |
| FRAME_TYPE_CMD_START_RECORD | 0x10 | iOS → ESP32 | 开始录音指令 |
| FRAME_TYPE_CMD_STOP_RECORD | 0x11 | iOS → ESP32 | 停止录音指令 |
| FRAME_TYPE_RECORD_END | 0x12 | ESP32 → iOS | 录音结束确认 |

### 2.3 Payload 格式

#### 2.3.1 音频帧 (0x01)

```
Payload = [ Opus encoded audio data ]
```

- Opus 帧大小：典型 20ms 帧约 40-80 字节（@ 16kbps）
- 单次 L2CAP 传输可能包含多个 Opus 帧（取决于 MTU）

#### 2.3.2 控制指令 (0x10, 0x11)

```
Payload = [] (空)
```

控制指令无 Payload，仅需 Frame Type + Length=0

#### 2.3.3 录音结束 (0x12)

```
Payload = [ total_frames (4 bytes LE) ]
```

- `total_frames`：本次录音发送的音频帧总数，用于 iOS 校验完整性

---

## 3. 通信流程

### 3.1 录音流程

```
iOS                                    ESP32
 │                                       │
 │──── FRAME_TYPE_CMD_START_RECORD ────►│
 │     [0x10][0x00 0x00]                 │ 启动麦克风采集
 │                                       │
 │◄─── FRAME_TYPE_AUDIO ───────────────│
 │     [0x01][len][opus data]            │
 │                                       │
 │◄─── FRAME_TYPE_AUDIO ───────────────│
 │     [0x01][len][opus data]            │
 │                                       │
 │        ... (持续发送)                  │
 │                                       │
 │──── FRAME_TYPE_CMD_STOP_RECORD ─────►│
 │     [0x11][0x00 0x00]                 │ 停止采集
 │                                       │
 │◄─── FRAME_TYPE_RECORD_END ──────────│
 │     [0x12][0x04 0x00][total_frames]   │ 确认结束
 │                                       │
```

### 3.2 错误处理

| 场景 | 处理方式 |
|------|---------|
| 音频帧丢失 | iOS 检测帧序号不连续，记录警告（后续可扩展重传机制） |
| 控制指令发送失败 | iOS 超时重试（最多 3 次），失败则断开重连 |
| L2CAP 缓冲区满 | ESP32 丢弃当前帧，继续采集（避免阻塞） |

---

## 4. 接口设计

### 4.1 ESP32 发送接口

```c
// ble_l2cap.h

typedef enum {
    FRAME_TYPE_HEARTBEAT = 0x00,
    FRAME_TYPE_AUDIO = 0x01,
    FRAME_TYPE_VAD_PREWARM = 0xFF,
    FRAME_TYPE_END_OF_UTTERANCE = 0xFE,
    FRAME_TYPE_CMD_START_RECORD = 0x10,
    FRAME_TYPE_CMD_STOP_RECORD = 0x11,
    FRAME_TYPE_RECORD_END = 0x12,
} frame_type_t;

/**
 * @brief 发送带帧头的数据
 *
 * @param type 帧类型
 * @param payload 数据负载（可为 NULL）
 * @param len 负载长度
 * @return 0 成功, -1 失败
 */
int ble_l2cap_send_frame(frame_type_t type, const uint8_t *payload, uint16_t len);
```

### 4.2 iOS 接收接口

```swift
// FrameParser.swift

struct AudioFrame {
    let type: UInt8
    let payload: Data
}

class FrameParser {
    /// 解析 L2CAP 接收的原始数据，返回完整帧
    /// - Parameter data: 从 inputStream 读取的原始数据
    /// - Returns: 解析完成的帧数组（可能多个）
    func parse(_ data: Data) -> [AudioFrame]
}

// L2CAPHandler.swift 扩展
extension L2CAPHandler {
    /// 发送控制指令
    func sendCommand(_ type: UInt8)

    /// 接收音频帧回调
    var onAudioFrame: ((Data) -> Void)?

    /// 录音结束回调
    var onRecordEnd: ((UInt32) -> Void)?
}
```

---

## 5. 与 Phase 1 兼容性

### 5.1 向后兼容策略

Phase 1 的 "hello world" 消息无帧头，为保持兼容：

**方案：ESP32 固件版本协商**
- ESP32 在 GATT Service 中新增 Firmware Version Characteristic
- iOS 读取版本号，决定使用旧协议（无帧头）或新协议（带帧头）
- Phase 2 强制使用新协议

**GATT 扩展**：

| Characteristic | UUID | 类型 | 说明 |
|----------------|------|------|------|
| Firmware Version | `0000AE02-...` | Read | 返回版本号，如 `[0x02, 0x00]` = v2.0 |

### 5.2 迁移路径

| 版本 | ESP32 行为 | iOS 行为 |
|------|-----------|---------|
| v1.0 (Phase 1) | 发送裸 "hello world" | 直接显示字符串 |
| v2.0 (Phase 2) | 发送带帧头数据 | 解析帧头后处理 |

---

## 6. 测试方案

### 6.1 单元测试

| 测试项 | 输入 | 期望结果 |
|--------|------|---------|
| 帧封包 | type=0x01, payload=[0xAA, 0xBB] | [0x01, 0x02, 0x00, 0xAA, 0xBB] |
| 帧解析 | [0x10, 0x00, 0x00] | type=0x10, payload=[] |
| 粘包解析 | [0x01,0x02,0x00,0xAA,0xBB, 0x01,0x01,0x00,0xCC] | 两个帧 |

### 6.2 集成测试

| 测试项 | 步骤 | 期望结果 |
|--------|------|---------|
| 开始/停止录音 | iOS 发送指令，ESP32 响应 | 状态机正确切换 |
| 音频帧传输 | 连续发送 100 帧 | iOS 正确解析所有帧 |
| 录音结束校验 | ESP32 发送 RECORD_END | iOS 校验帧数一致 |

---

## 7. 影响评估

### 7.1 对现有模块的影响

| 模块 | 变更 |
|------|------|
| ble-channel (ESP32) | 修改 `ble_l2cap_send` 为 `ble_l2cap_send_frame` |
| ble-channel (iOS) | 新增 `FrameParser` 类，修改 `L2CAPHandler` |
| ble-channel (spec) | 更新协议接口定义 |

### 7.2 后续扩展

- Phase 3 新增帧类型：`0x00` 心跳、`0xFF` VAD 预警、`0xFE` 说话结束
- 支持帧序号（可选，用于丢帧检测）

### 7.3 回滚方案

- ESP32 固件独立烧录，可回滚至 Phase 1 版本
- iOS 通过 Firmware Version 自动适配协议版本