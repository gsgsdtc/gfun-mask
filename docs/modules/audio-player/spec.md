# Module Spec: audio-player

> 模块：iOS 音频存储与播放
> 最近同步：2026-03-10
> 状态：Phase 2 完成（PCM WAV 存储与播放）

---

## 1. 模块概述

实现 iOS 端音频接收、文件保存、录音管理、音频播放功能。当前阶段接收 ESP32 的 PCM 直传帧，封装为标准 WAV 文件（16kHz, 16-bit, mono）供 AVAudioPlayer 直接播放，以验证音频采集质量。后续切换为 Opus 解码。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | BLE L2CAP 通道（ble-channel 模块）提供音频帧 |
| 下游 | 无（本地播放） |
| 输入 | PCM 音频帧 payload（640B/帧）、用户操作（播放/删除） |
| 输出 | WAV 文件持久化、音频播放 |

### 1.2 技术选型

| 组件 | 选型 | 说明 |
|------|------|------|
| 当前音频格式 | WAV (PCM 16kHz 16-bit mono) | PCM 直传验证阶段，AVAudioPlayer 直接支持 |
| 目标音频格式 | Opus → CAF/OGG | Phase 3 切换 |
| 音频播放 | AVAudioPlayer | iOS 系统播放器，支持 WAV |
| 文件存储 | FileManager + App Sandbox | Documents/Recordings/ |
| 元数据持久化 | UserDefaults (JSON) | Recording 列表 |
| UI | SwiftUI | ContentView 内嵌 |

---

## 2. 功能规格

### 2.1 音频接收

| 功能 | 说明 |
|------|------|
| 帧解析 | 由 L2CAPHandler + FrameParser 解析，回调音频 payload（已去帧头） |
| 帧缓存 | AudioReceiver 将每帧加 2 字节长度前缀后追加到 opusDataBuffer |
| 完整性校验 | 通过 RECORD_END 帧的 uint32 总帧数进行对比 |
| 录音时长 | 按 20ms/帧累计（Date().timeIntervalSince(startTime)） |

### 2.2 文件保存

| 功能 | 说明 |
|------|------|
| 格式 | 标准 WAV（RIFF PCM） |
| 文件名 | `recording_yyyyMMdd_HHmmss.wav` |
| 存储位置 | `Documents/Recordings/` |
| 处理步骤 | 剥离 2 字节长度前缀 → 拼接纯 PCM → 写 WAV 头 → 保存文件 |

### 2.3 录音管理

| 功能 | 说明 |
|------|------|
| 列表展示 | RecordingListView，显示文件名/时长/大小/日期 |
| 详情查看 | RecordingDetailView，播放器 + 元数据 |
| 删除 | 从 UserDefaults 列表 + 文件系统同步删除 |
| 滑动删除 | SwiftUI List onDelete 支持 |

### 2.4 音频播放

| 功能 | 说明 |
|------|------|
| 播放控制 | 播放/暂停/停止/跳转（togglePlayPause + seek） |
| 进度显示 | Timer 每 0.1s 更新 currentTime |
| 加载 | `AVAudioPlayer(contentsOf: url)` 直接加载 WAV |

---

## 3. 模块结构（实际代码）

```
VoiceMaskApp/VoiceMaskApp/
├── Audio/
│   ├── AudioReceiver.swift      # 音频帧接收与缓存
│   ├── AudioFileWriter.swift    # PCM → WAV 文件封装与保存
│   ├── AudioPlayer.swift        # AVAudioPlayer 封装
│   ├── RecordingManager.swift   # 录音记录 CRUD（UserDefaults）
│   └── FrameParser.swift        # BLE 帧协议解析器
├── BLE/
│   ├── BLEManager.swift         # CoreBluetooth 管理器
│   └── L2CAPHandler.swift       # L2CAP Stream + 音频帧回调
├── Models/
│   └── Recording.swift          # 录音数据模型
└── ContentView.swift            # 主界面（录音控制 + 列表 + 详情，三合一）
```

> 注：原 spec 中设计的 `OpusDecoder.swift`、`Views/`、`ViewModels/` 目录暂未创建，功能直接在 ContentView.swift 中内嵌实现。

---

## 4. 接口定义

### 4.1 AudioReceiver.swift

```swift
class AudioReceiver: ObservableObject {
    @Published var isReceiving: Bool
    @Published var frameCount: Int
    @Published var recordingDuration: TimeInterval

    func startReceiving()                           // 重置状态，标记开始
    func stopReceiving() -> Data?                   // 停止，返回 opusDataBuffer
    func handleAudioFrame(_ data: Data)             // 追加帧（+2B 长度前缀）
    func handleRecordEnd(expectedFrames: UInt32)    // 记录预期帧数，停止接收
    func verifyIntegrity() -> Bool                  // 校验 frameCount == expectedFrames
}
```

### 4.2 AudioFileWriter.swift

```swift
struct AudioFileWriter {
    /// 将 AudioReceiver 的帧数据（含 2B 长度前缀）转换为 WAV 文件
    /// 剥离长度前缀 → 拼接纯 PCM → 写 RIFF WAV 头 → 保存
    static func writeAudioFile(
        opusData: Data,          // 实际为带长度前缀的 PCM 帧数据
        sampleRate: Int = 16000,
        channels: Int = 1
    ) -> URL?

    static func generateFileName() -> String    // "recording_yyyyMMdd_HHmmss"
    static func getRecordingsDirectory() -> URL // Documents/Recordings/
    static func calculateFileSize(_ data: Data) -> Int64
}
```

### 4.3 Recording.swift

```swift
struct Recording: Identifiable, Codable {
    let id: UUID
    let fileName: String
    let fileURL: URL
    let duration: TimeInterval
    let fileSize: Int64
    let createdAt: Date
    let format: String          // "PCM WAV 16kHz Mono 16-bit"

    var formattedDuration: String   // "MM:SS"
    var formattedSize: String       // "XX.X KB"
    var formattedDate: String       // 中文日期
}
```

### 4.4 RecordingManager.swift

```swift
class RecordingManager: ObservableObject {
    @Published var recordings: [Recording]

    func loadRecordings()                           // 从 UserDefaults 加载，过滤已删文件
    func addRecording(_ recording: Recording)       // 插入列表头部
    func deleteRecording(_ recording: Recording)    // 删文件 + 从列表移除
    func deleteRecordings(at offsets: IndexSet)     // 批量删除（SwiftUI onDelete）
}
```

### 4.5 AudioPlayer.swift

```swift
class AudioPlayer: ObservableObject {
    @Published var isPlaying: Bool
    @Published var currentTime: TimeInterval
    @Published var duration: TimeInterval
    @Published var isLoading: Bool

    func load(url: URL) -> Bool       // AVAudioPlayer(contentsOf:) 加载 WAV
    func play()                       // 设置 AVAudioSession + 播放
    func pause()
    func stop()
    func seek(to time: TimeInterval)
    func togglePlayPause()

    static func formatTime(_ time: TimeInterval) -> String  // "MM:SS"
}
```

---

## 5. 数据流

```
BLE L2CAP SDU (643B)
     │
     ▼
┌─────────────┐  FrameParser  ┌─────────────┐
│L2CAPHandler │ ─────────────► │AudioReceiver│
│             │  640B payload  │             │
└─────────────┘                └─────────────┘
                                    │ opusDataBuffer
                                    │ (帧数 × 642B)
                                    ▼
                              ┌──────────────┐
                              │AudioFileWriter│ 剥前缀 → WAV 封装
                              └──────────────┘
                                    │ .wav 文件
                                    ▼
                              ┌──────────────┐
                              │RecordingMgr  │ UserDefaults 持久化
                              └──────────────┘
                                    │
                                    ▼
                              ┌──────────────┐
                              │  SwiftUI UI  │
                              └──────────────┘
                                    │ 用户点击播放
                                    ▼
                              ┌──────────────┐
                              │ AudioPlayer  │ AVAudioPlayer(contentsOf:)
                              └──────────────┘
```

---

## 6. 文件存储

### 6.1 目录结构

```
App Documents/
└── Recordings/
    ├── recording_20260310_143021.wav
    └── recording_20260310_091533.wav
```

### 6.2 WAV 文件格式（内部结构）

```
RIFF (4B) | fileSize (4B) | WAVE (4B)
fmt  (4B) | 16 (4B) | PCM=1 (2B) | ch=1 (2B) | 16000 (4B) | 32000 (4B) | 2 (2B) | 16 (2B)
data (4B) | dataSize (4B) | [raw PCM int16 LE ...]
```

### 6.3 元数据（UserDefaults JSON）

```json
[
  {
    "id": "UUID-xxx",
    "fileName": "recording_20260310_143021",
    "fileURL": "file://.../recording_20260310_143021.wav",
    "duration": 10.24,
    "fileSize": 327722,
    "createdAt": "2026-03-10T14:30:21Z",
    "format": "PCM WAV 16kHz Mono 16-bit"
  }
]
```

---

## 7. UI 设计（实际实现）

### 7.1 TabView 结构

```
TabView
├── Tab 1：录音控制（RecordingControlView）
│   ├── StatusBar（BLE 连接状态指示灯）
│   ├── 录音中动画（waveform.circle.fill）或待机（mic.circle）
│   ├── 已录制 X 秒 / 已接收 X 帧
│   └── 开始/停止按钮
└── Tab 2：录音列表（RecordingListView）
    └── 点击 → Sheet：RecordingDetailView
        ├── 播放/暂停按钮
        ├── 进度条
        └── 元数据详情
```

---

## 8. 验收状态

### Phase 2 — PCM WAV 存储与播放

| 验收项 | 状态 | 备注 |
|--------|------|------|
| 接收音频帧并缓存 | ✅ | AudioReceiver，+2B 长度前缀 |
| 封装为 WAV 文件 | ✅ | RIFF PCM 16kHz mono |
| 录音列表展示 | ✅ | SwiftUI List + UserDefaults |
| 录音详情查看 | ✅ | RecordingDetailView |
| 音频播放（AVAudioPlayer） | ✅ | 直接加载 WAV |
| 删除录音功能 | ✅ | 滑动删除 |
| 完整性校验（RECORD_END） | ✅ | verifyIntegrity() |

### Phase 3 — Opus 解码（待完成）

| 验收项 | 状态 |
|--------|------|
| 集成 Opus 解码库 | ⏳ |
| 封装为 CAF/OGG 格式 | ⏳ |
| iOS 端 Opus 解码播放 | ⏳ |

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-05 | feat #02 | 创建模块规格，定义 Opus 接收与播放接口 |
| 2026-03-09 | feat | 实现完整功能：AudioReceiver/FileWriter/Player/RecordingManager |
| 2026-03-09 | fix | FrameParser 崩溃修复（Data.removeFirst EXC_BREAKPOINT → cursor 方式） |
| 2026-03-10 | fix | AudioFileWriter 改为写 WAV 格式（剥 2B 前缀 + RIFF 头），替代 .raw |
| 2026-03-10 | fix | AudioPlayer 改用 `AVAudioPlayer(contentsOf:)` 直接加载 WAV |
| 2026-03-10 | fix | L2CAPHandler 读缓冲区 512 → 1024，避免 643B PCM 帧被截断 |
