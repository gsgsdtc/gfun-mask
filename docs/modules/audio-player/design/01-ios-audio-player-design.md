# Design: 01 - iOS 音频存储与播放设计

> 所属模块：audio-player
> 关联需求：docs/feat/feat-02-esp32-audio-capture-opus-encoding.md
> 关联 BLE 设计：docs/modules/ble-channel/design/02-audio-frame-protocol-design.md
> 更新日期：2026-03-05
> 状态：草稿

---

## 1. 设计概述

### 1.1 目标

实现 iOS 端音频接收、Opus 文件保存、录音管理、音频播放功能。接收 ESP32 通过 BLE L2CAP 传输的 Opus 音频帧，封装为可播放的音频文件，提供录音列表管理和播放器界面。

### 1.2 设计约束

- **iOS 版本**：iOS 16.0+
- **框架**：SwiftUI + CoreBluetooth + AVFoundation
- **音频格式**：Opus 编码，封装为 CAF 容器格式
- **文件存储**：App Sandbox Documents 目录

---

## 2. 音频接收设计

### 2.1 帧解析器

```swift
// FrameParser.swift

struct AudioFrame {
    let type: UInt8
    let payload: Data
}

class FrameParser {
    private var buffer = Data()

    /// 解析从 L2CAP inputStream 读取的原始数据
    /// - Parameter data: 原始数据
    /// - Returns: 解析完成的帧数组
    func parse(_ data: Data) -> [AudioFrame] {
        buffer.append(data)
        var frames: [AudioFrame] = []

        while buffer.count >= 3 {
            let frameType = buffer[0]
            let payloadLen = UInt16(buffer[1]) | (UInt16(buffer[2]) << 8)

            // 检查是否有完整帧
            guard buffer.count >= 3 + Int(payloadLen) else {
                break // 数据不完整，等待更多数据
            }

            // 提取 payload
            let payload = buffer.subdata(in: 3..<3+Int(payloadLen))

            frames.append(AudioFrame(type: frameType, payload: payload))

            // 移除已解析的数据
            buffer.removeFirst(3 + Int(payloadLen))
        }

        return frames
    }

    /// 重置解析器状态
    func reset() {
        buffer.removeAll()
    }
}
```

### 2.2 音频接收器

```swift
// AudioReceiver.swift

import Foundation
import Combine

class AudioReceiver: ObservableObject {
    @Published var isReceiving = false
    @Published var frameCount: Int = 0
    @Published var recordingDuration: TimeInterval = 0

    private var opusDataBuffer = Data()
    private var expectedFrames: UInt32?
    private let frameParser = FrameParser()
    private var startTime: Date?

    // MARK: - Public Methods

    func startReceiving() {
        opusDataBuffer.removeAll()
        frameCount = 0
        recordingDuration = 0
        expectedFrames = nil
        startTime = Date()
        isReceiving = true
    }

    func stopReceiving() -> Data? {
        isReceiving = false
        return opusDataBuffer.isEmpty ? nil : opusDataBuffer
    }

    /// 处理从 L2CAP 收到的原始数据
    func handleRawData(_ data: Data) {
        let frames = frameParser.parse(data)

        for frame in frames {
            handleFrame(frame)
        }
    }

    // MARK: - Private Methods

    private func handleFrame(_ frame: AudioFrame) {
        switch frame.type {
        case 0x01: // FRAME_TYPE_AUDIO
            handleAudioFrame(frame.payload)

        case 0x12: // FRAME_TYPE_RECORD_END
            handleRecordEnd(frame.payload)

        default:
            print("Unknown frame type: 0x\(String(frame.type, radix: 16))")
        }
    }

    private func handleAudioFrame(_ payload: Data) {
        guard isReceiving else { return }

        // 将 Opus 帧添加到缓冲区
        // 帧格式: [长度(2字节)] + [Opus 数据]
        var frameData = Data()
        let len = UInt16(payload.count)
        frameData.append(contentsOf: [UInt8(len & 0xFF), UInt8(len >> 8)])
        frameData.append(payload)

        opusDataBuffer.append(frameData)
        frameCount += 1

        // 更新录音时长（每帧 20ms）
        recordingDuration = Date().timeIntervalSince(startTime ?? Date())
    }

    private func handleRecordEnd(_ payload: Data) {
        guard payload.count == 4 else { return }
        expectedFrames = payload.withUnsafeBytes { $0.load(as: UInt32.self) }

        // 校验帧数
        if let expected = expectedFrames {
            print("Record ended. Expected: \(expected), Received: \(frameCount)")
        }

        isReceiving = false
    }

    /// 校验接收完整性
    func verifyIntegrity() -> Bool {
        guard let expected = expectedFrames else { return true }
        return UInt32(frameCount) == expected
    }
}
```

---

## 3. 文件保存设计

### 3.1 CAF 文件格式

iOS 原生支持 CAF (Core Audio Format) 容器，可直接封装 Opus 数据。

```
CAF 文件结构:
┌────────────────────┐
│ File Header        │
├────────────────────┤
│ 'desc' Chunk       │ ← 音频格式描述 (Opus, 16kHz, Mono)
├────────────────────┤
│ 'data' Chunk       │ ← Opus 音频数据
│   [Frame 1]        │
│   [Frame 2]        │
│   ...              │
└────────────────────┘
```

### 3.2 文件写入器

```swift
// AudioFileWriter.swift

import Foundation
import AVFoundation

struct AudioFileWriter {

    /// 将 Opus 数据写入 CAF 文件
    /// - Parameters:
    ///   - opusData: Opus 帧数据（每帧带长度前缀）
    ///   - sampleRate: 采样率
    ///   - channels: 声道数
    /// - Returns: 文件 URL
    static func writeOpusFile(
        opusData: Data,
        sampleRate: Int = 16000,
        channels: Int = 1
    ) -> URL? {

        let fileName = generateFileName()
        let fileURL = getRecordingsDirectory()
            .appendingPathComponent("\(fileName).caf")

        // 创建目录
        try? FileManager.default.createDirectory(
            at: getRecordingsDirectory(),
            withIntermediateDirectories: true
        )

        // 配置音频格式
        var formatDesc: AudioStreamBasicDescription = AudioStreamBasicDescription(
            mSampleRate: Float64(sampleRate),
            mFormatID: kAudioFormatOpus,
            mFormatFlags: 0,
            mBytesPerPacket: 0,
            mFramesPerPacket: 960, // Opus 20ms @ 48kHz internal
            mBytesPerFrame: 0,
            mChannelsPerFrame: UInt32(channels),
            mBitsPerChannel: 0,
            mReserved: 0
        )

        // 创建音频文件
        var audioFile: AudioFileID?
        let status = AudioFileCreateWithURL(
            fileURL as CFURL,
            kAudioFileCAFType,
            &formatDesc,
            [.eraseFile],
            &audioFile
        )

        guard status == noErr, let file = audioFile else {
            print("Failed to create audio file: \(status)")
            return nil
        }

        defer {
            AudioFileClose(file)
        }

        // 写入数据
        var dataSize = UInt32(opusData.count)
        AudioFileWriteBytes(file, false, 0, &dataSize, [UInt8](opusData))

        return fileURL
    }

    /// 生成唯一文件名
    static func generateFileName() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        return "recording_\(formatter.string(from: Date()))"
    }

    /// 获取录音目录
    static func getRecordingsDirectory() -> URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Recordings")
    }
}
```

### 3.3 替代方案：使用 AVAudioRecorder

如果原生 Opus 支持有问题，可使用以下方案：

1. **方案 A**：接收 PCM 数据，使用 AVAudioRecorder 录制为 AAC/M4A
2. **方案 B**：使用第三方 Opus 解码库（如 opus-swift），解码后保存为 WAV

---

## 4. 录音管理设计

### 4.1 数据模型

```swift
// Recording.swift

import Foundation

struct Recording: Identifiable, Codable {
    let id: UUID
    let fileName: String
    let fileURL: URL
    let duration: TimeInterval
    let fileSize: Int64
    let createdAt: Date
    let format: String

    init(fileName: String, fileURL: URL, duration: TimeInterval, fileSize: Int64) {
        self.id = UUID()
        self.fileName = fileName
        self.fileURL = fileURL
        self.duration = duration
        self.fileSize = fileSize
        self.createdAt = Date()
        self.format = "Opus 16kHz Mono"
    }

    // MARK: - Formatted Properties

    var formattedDuration: String {
        let minutes = Int(duration) / 60
        let seconds = Int(duration) % 60
        return String(format: "%02d:%02d", minutes, seconds)
    }

    var formattedSize: String {
        let kb = Double(fileSize) / 1024.0
        return String(format: "%.1f KB", kb)
    }

    var formattedDate: String {
        let formatter = DateFormatter()
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return formatter.string(from: createdAt)
    }
}
```

### 4.2 录音管理器

```swift
// RecordingManager.swift

import Foundation
import Combine

class RecordingManager: ObservableObject {
    @Published var recordings: [Recording] = []

    private let recordingsKey = "savedRecordings"
    private var cancellables = Set<AnyCancellable>()

    init() {
        loadRecordings()
    }

    // MARK: - CRUD Operations

    func loadRecordings() {
        // 从 UserDefaults 加载元数据
        guard let data = UserDefaults.standard.data(forKey: recordingsKey),
              let decoded = try? JSONDecoder().decode([Recording].self, from: data) else {
            recordings = []
            return
        }

        // 过滤已删除的文件
        recordings = decoded.filter { FileManager.default.fileExists(atPath: $0.fileURL.path) }
    }

    func addRecording(_ recording: Recording) {
        recordings.insert(recording, at: 0) // 新录音在最前面
        saveRecordings()
    }

    func deleteRecording(_ recording: Recording) {
        // 删除文件
        try? FileManager.default.removeItem(at: recording.fileURL)

        // 从列表移除
        recordings.removeAll { $0.id == recording.id }
        saveRecordings()
    }

    func deleteRecordings(at offsets: IndexSet) {
        for index in offsets {
            let recording = recordings[index]
            try? FileManager.default.removeItem(at: recording.fileURL)
        }
        recordings.remove(atOffsets: offsets)
        saveRecordings()
    }

    // MARK: - Private Methods

    private func saveRecordings() {
        guard let data = try? JSONEncoder().encode(recordings) else { return }
        UserDefaults.standard.set(data, forKey: recordingsKey)
    }
}
```

---

## 5. 音频播放设计

### 5.1 播放器封装

```swift
// AudioPlayer.swift

import Foundation
import AVFoundation
import Combine

class AudioPlayer: ObservableObject {
    @Published var isPlaying = false
    @Published var currentTime: TimeInterval = 0
    @Published var duration: TimeInterval = 0
    @Published var isLoading = false

    private var player: AVAudioPlayer?
    private var timer: Timer?
    private var currentURL: URL?

    // MARK: - Public Methods

    func load(url: URL) -> Bool {
        stop()
        currentURL = url

        do {
            player = try AVAudioPlayer(contentsOf: url)
            player?.prepareToPlay()
            duration = player?.duration ?? 0
            return true
        } catch {
            print("Failed to load audio: \(error)")
            return false
        }
    }

    func play() {
        guard let player = player else { return }

        // 配置音频会话
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
            try AVAudioSession.sharedInstance().setActive(true)
        } catch {
            print("Failed to set up audio session: \(error)")
        }

        player.play()
        isPlaying = true
        startTimer()
    }

    func pause() {
        player?.pause()
        isPlaying = false
        stopTimer()
    }

    func stop() {
        player?.stop()
        player?.currentTime = 0
        isPlaying = false
        currentTime = 0
        stopTimer()
    }

    func seek(to time: TimeInterval) {
        player?.currentTime = time
        currentTime = time
    }

    func togglePlayPause() {
        if isPlaying {
            pause()
        } else {
            play()
        }
    }

    // MARK: - Private Methods

    private func startTimer() {
        timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            guard let self = self, let player = self.player else { return }
            self.currentTime = player.currentTime

            if !player.isPlaying {
                self.isPlaying = false
                self.stopTimer()
            }
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }
}
```

### 5.2 Opus 解码支持

如果 AVAudioPlayer 不支持 Opus，需要使用第三方库：

```swift
// OpusDecoder.swift

import Foundation

// 使用 opus-swift 或其他第三方库
// pod 'opus-swift'

class OpusDecoder {
    private var decoder: OpaquePointer?

    init?() {
        var error: Int32 = 0
        decoder = opus_decoder_create(16000, 1, &error)
        guard error == OPUS_OK else {
            return nil
        }
    }

    deinit {
        if let decoder = decoder {
            opus_decoder_destroy(decoder)
        }
    }

    func decode(opusData: Data) -> Data? {
        var pcmBuffer = [Int16](repeating: 0, count: 5760) // 最大帧大小
        let frameSize = opus_decode(
            decoder,
            [UInt8](opusData),
            Int32(opusData.count),
            &pcmBuffer,
            Int32(pcmBuffer.count),
            0 // decode_fec
        )

        guard frameSize > 0 else { return nil }

        // 转换为 Data
        return Data(bytes: pcmBuffer, count: Int(frameSize) * 2)
    }
}
```

---

## 6. UI 设计

### 6.1 录音列表视图

```swift
// RecordingListView.swift

import SwiftUI

struct RecordingListView: View {
    @StateObject private var manager = RecordingManager()

    var body: some View {
        NavigationView {
            List {
                ForEach(manager.recordings) { recording in
                    NavigationLink(destination: RecordingDetailView(recording: recording)) {
                        RecordingRowView(recording: recording)
                    }
                }
                .onDelete { offsets in
                    manager.deleteRecordings(at: offsets)
                }
            }
            .navigationTitle("录音")
            .toolbar {
                EditButton()
            }
        }
    }
}

struct RecordingRowView: View {
    let recording: Recording

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(recording.fileName)
                .font(.headline)

            HStack {
                Text(recording.formattedDuration)
                Text("•")
                Text(recording.formattedSize)
                Text("•")
                Text(recording.formattedDate)
            }
            .font(.caption)
            .foregroundColor(.secondary)
        }
        .padding(.vertical, 4)
    }
}
```

### 6.2 录音详情视图

```swift
// RecordingDetailView.swift

import SwiftUI

struct RecordingDetailView: View {
    let recording: Recording
    @StateObject private var player = AudioPlayer()

    var body: some View {
        VStack(spacing: 24) {
            // 播放器控件
            VStack(spacing: 16) {
                // 播放/暂停按钮
                Button(action: {
                    if player.currentURL != recording.fileURL {
                        _ = player.load(url: recording.fileURL)
                    }
                    player.togglePlayPause()
                }) {
                    Image(systemName: player.isPlaying ? "pause.circle.fill" : "play.circle.fill")
                        .font(.system(size: 64))
                        .foregroundColor(.blue)
                }

                // 进度条
                VStack {
                    ProgressView(value: player.currentTime, total: player.duration)
                        .progressViewStyle(.linear)

                    HStack {
                        Text(formatTime(player.currentTime))
                        Spacer()
                        Text(formatTime(player.duration))
                    }
                    .font(.caption)
                    .foregroundColor(.secondary)
                }
            }
            .padding()

            Spacer()

            // 详情信息
            VStack(alignment: .leading, spacing: 12) {
                DetailRow(label: "文件名", value: recording.fileName)
                DetailRow(label: "时长", value: recording.formattedDuration)
                DetailRow(label: "大小", value: recording.formattedSize)
                DetailRow(label: "创建时间", value: recording.formattedDate)
                DetailRow(label: "编码格式", value: recording.format)
            }
            .padding()

            Spacer()

            // 删除按钮
            Button(role: .destructive) {
                // 删除逻辑
            } label: {
                Text("删除录音")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.bordered)
            .padding()
        }
        .navigationTitle("录音详情")
        .onAppear {
            _ = player.load(url: recording.fileURL)
        }
        .onDisappear {
            player.stop()
        }
    }

    private func formatTime(_ time: TimeInterval) -> String {
        let minutes = Int(time) / 60
        let seconds = Int(time) % 60
        return String(format: "%02d:%02d", minutes, seconds)
    }
}

struct DetailRow: View {
    let label: String
    let value: String

    var body: some View {
        HStack {
            Text(label)
                .foregroundColor(.secondary)
            Spacer()
            Text(value)
        }
    }
}
```

---

## 7. 与 BLE 模块集成

### 7.1 L2CAPHandler 扩展

```swift
// L2CAPHandler.swift 扩展

extension L2CAPHandler {
    func setupAudioCallbacks(
        receiver: AudioReceiver,
        onComplete: @escaping (Data?) -> Void
    ) {
        // 音频帧回调
        onAudioFrame = { [weak receiver] data in
            receiver?.handleRawData(data)
        }

        // 录音结束回调
        onRecordEnd = { [weak receiver] _ in
            guard let receiver = receiver else { return }
            let data = receiver.stopReceiving()
            onComplete(data)
        }
    }
}
```

### 7.2 BLEManager 集成

```swift
// BLEManager.swift 扩展

extension BLEManager {
    func startRecording(receiver: AudioReceiver) {
        guard l2capHandler != nil else { return }

        receiver.startReceiving()
        l2capHandler?.sendCommand(0x10) // FRAME_TYPE_CMD_START_RECORD
    }

    func stopRecording(receiver: AudioReceiver, completion: @escaping (URL?) -> Void) {
        l2capHandler?.sendCommand(0x11) // FRAME_TYPE_CMD_STOP_RECORD

        // 等待 RECORD_END 帧
        l2capHandler?.setupAudioCallbacks(receiver: receiver) { data in
            guard let opusData = data else {
                completion(nil)
                return
            }

            // 保存文件
            if let url = AudioFileWriter.writeOpusFile(opusData: opusData) {
                let recording = Recording(
                    fileName: url.deletingPathExtension().lastPathComponent,
                    fileURL: url,
                    duration: receiver.recordingDuration,
                    fileSize: Int64(opusData.count)
                )
                completion(url)
            } else {
                completion(nil)
            }
        }
    }
}
```

---

## 8. 错误处理

| 场景 | 处理方式 |
|------|---------|
| 文件写入失败 | 显示错误提示，音频数据保留在内存中 |
| 播放失败 | 显示"无法播放此文件"提示 |
| 帧数校验失败 | 显示警告，但仍保存文件 |
| 存储空间不足 | 检测可用空间，提前警告 |

---

## 9. 测试方案

### 9.1 单元测试

| 测试项 | 方法 | 期望结果 |
|--------|------|---------|
| 帧解析 | 输入多帧数据 | 正确解析所有帧 |
| 文件名生成 | 多次调用 | 每次生成唯一名称 |
| 录音管理 CRUD | 添加/删除/加载 | 状态正确更新 |

### 9.2 集成测试

| 测试项 | 步骤 | 期望结果 |
|--------|------|---------|
| 录音流程 | 开始→录制→停止→保存 | 文件正确保存 |
| 播放流程 | 加载→播放→暂停→停止 | 播放正常 |
| 完整性校验 | 模拟丢帧 | 显示警告 |

---

## 10. 影响评估

### 10.1 对现有模块的影响

| 模块 | 变更 |
|------|------|
| ble-channel | 扩展 L2CAPHandler，增加音频帧处理回调 |

### 10.2 回滚方案

- iOS App 独立工程，可切换 Git 分支回滚
- 不影响 ESP32 固件

---

## 11. 后续扩展

- **后台播放**：配置 Background Modes，支持锁屏播放
- **分享功能**：支持通过 AirDrop、Message 分享录音
- **波形显示**：录音时实时显示音频波形
- **iCloud 同步**：录音自动同步到 iCloud