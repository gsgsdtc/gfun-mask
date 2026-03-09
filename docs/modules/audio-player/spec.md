# Module Spec: audio-player

> 模块：iOS 音频存储与播放
> 最近同步：2026-03-05
> 状态：Phase 2 开发中

---

## 1. 模块概述

实现 iOS 端音频接收、Opus 文件保存、录音管理、音频播放功能。接收 ESP32 通过 BLE L2CAP 传输的 Opus 音频帧，封装为可播放的音频文件，提供录音列表管理和播放器界面。

### 1.1 边界

| 边界 | 说明 |
|------|------|
| 上游 | BLE L2CAP 通道（ble-channel 模块） |
| 下游 | 无 |
| 输入 | Opus 音频帧、用户操作（播放/删除） |
| 输出 | 音频播放、文件持久化 |

### 1.2 技术选型

| 组件 | 选型 | 说明 |
|------|------|------|
| 音频解码 | AVAudioPlayer + Opus 解码 | iOS 原生 + 第三方库 |
| 文件格式 | .caf 或 .ogg | 支持 Opus 编码 |
| 数据持久化 | FileManager + App Sandbox | 本地文件存储 |
| UI | SwiftUI | 声明式界面 |

---

## 2. 功能规格

### 2.1 音频接收

| 功能 | 说明 |
|------|------|
| 帧解析 | 解析 BLE 音频帧协议（Frame Type + Payload） |
| 帧缓存 | 实时缓存接收的 Opus 帧 |
| 完整性校验 | 通过 RECORD_END 的帧数校验 |

### 2.2 文件保存

| 功能 | 说明 |
|------|------|
| 格式封装 | 将 Opus 帧封装为 .caf 文件 |
| 元数据 | 文件名、时长、大小、创建时间 |
| 存储位置 | App Documents 目录 |

### 2.3 录音管理

| 功能 | 说明 |
|------|------|
| 列表展示 | 显示所有录音记录 |
| 详情查看 | 文件名、时长、大小、创建时间、编码格式 |
| 删除功能 | 从列表和文件系统删除 |

### 2.4 音频播放

| 功能 | 说明 |
|------|------|
| 播放控制 | 播放/暂停/停止 |
| 进度显示 | 当前播放进度、总时长 |
| 后台播放 | 支持 App 切后台继续播放（可选） |

---

## 3. 模块结构

```
VoiceMaskApp/VoiceMaskApp/
├── Audio/
│   ├── AudioReceiver.swift       # 音频帧接收与缓存
│   ├── AudioFileWriter.swift     # Opus 文件封装与保存
│   ├── AudioPlayer.swift         # 音频播放器封装
│   ├── RecordingManager.swift    # 录音记录管理
│   └── OpusDecoder.swift         # Opus 解码（第三方库封装）
├── Models/
│   └── Recording.swift           # 录音数据模型
├── Views/
│   ├── RecordingListView.swift   # 录音列表页
│   ├── RecordingDetailView.swift # 录音详情页
│   └── AudioPlayerView.swift     # 播放器控件
└── ViewModels/
    └── RecordingListViewModel.swift
```

---

## 4. 接口定义

### 4.1 AudioReceiver.swift

```swift
/// 音频接收与缓存
class AudioReceiver: ObservableObject {
    @Published var isReceiving: Bool = false
    @Published var frameCount: Int = 0

    /// 开始接收录音
    func startReceiving()

    /// 停止接收，返回缓存的音频数据
    func stopReceiving() -> Data?

    /// 处理收到的音频帧
    func handleAudioFrame(_ data: Data)

    /// 处理录音结束确认
    func handleRecordEnd(expectedFrames: UInt32) -> Bool
}
```

### 4.2 AudioFileWriter.swift

```swift
/// 音频文件写入
struct AudioFileWriter {
    /// 将 Opus 数据封装为 .caf 文件
    static func writeOpusFile(
        opusData: Data,
        sampleRate: Int = 16000,
        channels: Int = 1
    ) -> URL?

    /// 生成唯一文件名
    static func generateFileName() -> String
}
```

### 4.3 Recording.swift

```swift
/// 录音数据模型
struct Recording: Identifiable, Codable {
    let id: UUID
    let fileName: String
    let fileURL: URL
    let duration: TimeInterval
    let fileSize: Int64
    let createdAt: Date
    let format: String // "Opus 16kHz"

    /// 格式化时长显示
    var formattedDuration: String

    /// 格式化大小显示
    var formattedSize: String
}
```

### 4.4 RecordingManager.swift

```swift
/// 录音记录管理
class RecordingManager: ObservableObject {
    @Published var recordings: [Recording] = []

    /// 加载所有录音
    func loadRecordings()

    /// 添加新录音
    func addRecording(_ recording: Recording)

    /// 删除录音
    func deleteRecording(_ recording: Recording)

    /// 获取录音文件 URL
    func fileURL(for recording: Recording) -> URL
}
```

### 4.5 AudioPlayer.swift

```swift
/// 音频播放器
class AudioPlayer: ObservableObject {
    @Published var isPlaying: Bool = false
    @Published var currentTime: TimeInterval = 0
    @Published var duration: TimeInterval = 0

    /// 加载音频文件
    func load(url: URL) -> Bool

    /// 播放
    func play()

    /// 暂停
    func pause()

    /// 停止
    func stop()

    /// 跳转到指定时间
    func seek(to time: TimeInterval)
}
```

---

## 5. 数据流

```
BLE L2CAP
     │
     ▼
┌─────────────┐    音频帧     ┌─────────────┐
│L2CAPHandler │ ───────────► │AudioReceiver│
└─────────────┘              └─────────────┘
                                   │
                                   │ Opus Data
                                   ▼
                            ┌─────────────┐
                            │AudioFileWr. │
                            └─────────────┘
                                   │
                                   │ .caf File
                                   ▼
                            ┌─────────────┐
                            │RecordingMgr │
                            └─────────────┘
                                   │
                                   ▼
                            ┌─────────────┐
                            │ UI (SwiftUI)│
                            └─────────────┘
                                   │
                                   │ User Play
                                   ▼
                            ┌─────────────┐
                            │AudioPlayer  │
                            └─────────────┘
```

---

## 6. UI 设计

### 6.1 录音列表页

```
┌─────────────────────────────────┐
│  录音                      [编辑] │
├─────────────────────────────────┤
│  ● 录音_20260305_143021         │
│    00:32  •  198 KB  •  今天 14:30│
├─────────────────────────────────┤
│  ● 录音_20260304_091533         │
│    01:45  •  632 KB  •  昨天 09:15│
├─────────────────────────────────┤
│  ● 录音_20260303_160722         │
│    00:15  •  92 KB  •  3月3日    │
└─────────────────────────────────┘
```

### 6.2 录音详情页

```
┌─────────────────────────────────┐
│  ←  录音详情                      │
├─────────────────────────────────┤
│                                 │
│         ▶️ 播放按钮              │
│                                 │
│  ─────────●─────────────        │
│  00:15          00:32           │
│                                 │
├─────────────────────────────────┤
│  文件名：录音_20260305_143021    │
│  时长：32 秒                     │
│  大小：198 KB                    │
│  创建时间：2026-03-05 14:30:21   │
│  编码格式：Opus 16kHz Mono       │
├─────────────────────────────────┤
│           [删除录音]             │
└─────────────────────────────────┘
```

---

## 7. 文件存储

### 7.1 目录结构

```
App Documents/
└── Recordings/
    ├── recording_20260305_143021.caf
    ├── recording_20260304_091533.caf
    └── recording_20260303_160722.caf
```

### 7.2 元数据持久化

使用 UserDefaults 或 JSON 文件存储录音列表元数据：

```json
[
  {
    "id": "UUID-xxx",
    "fileName": "recording_20260305_143021",
    "fileURL": "file://...",
    "duration": 32.5,
    "fileSize": 198000,
    "createdAt": "2026-03-05T14:30:21Z",
    "format": "Opus 16kHz Mono"
  }
]
```

---

## 8. 验收状态

| 验收项 | 状态 | 备注 |
|--------|------|------|
| 接收音频帧并缓存 | ⏳ | 待开发 |
| 封装为 .caf 文件 | ⏳ | 待开发 |
| 录音列表展示 | ⏳ | 待开发 |
| 录音详情查看 | ⏳ | 待开发 |
| 音频播放功能 | ⏳ | 待开发 |
| 删除录音功能 | ⏳ | 待开发 |

---

## 9. 变更记录

| 日期 | feat/fix | 变更内容 |
|------|----------|---------|
| 2026-03-05 | feat #02 | 创建模块规格，定义接口和 UI 结构 |