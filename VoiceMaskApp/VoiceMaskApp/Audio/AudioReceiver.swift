/*
 * @doc     docs/modules/audio-player/design/01-ios-audio-player-design.md §4.1
 * @purpose 音频接收与缓存
 */

import Foundation
import Combine

class AudioReceiver: ObservableObject {
    @Published var isReceiving = false
    @Published var frameCount: Int = 0
    @Published var recordingDuration: TimeInterval = 0

    private var opusDataBuffer = Data()
    private var expectedFrames: UInt32?
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

    /// 处理音频帧数据
    func handleAudioFrame(_ data: Data) {
        guard isReceiving else { return }

        // 将 Opus 帧添加到缓冲区
        // 帧格式: [长度(2字节 Little-Endian)] + [Opus 数据]
        var frameData = Data()
        let len = UInt16(data.count)
        frameData.append(contentsOf: [UInt8(len & 0xFF), UInt8(len >> 8)])
        frameData.append(data)

        opusDataBuffer.append(frameData)
        frameCount += 1

        // 更新录音时长（每帧 20ms）
        if let start = startTime {
            recordingDuration = Date().timeIntervalSince(start)
        }
    }

    /// 处理录音结束
    func handleRecordEnd(expectedFrames: UInt32) {
        self.expectedFrames = expectedFrames
        isReceiving = false
    }

    /// 校验接收完整性
    func verifyIntegrity() -> Bool {
        guard let expected = expectedFrames else { return true }
        return UInt32(frameCount) == expected
    }

    /// 获取预期帧数
    var expectedFrameCount: UInt32? {
        return expectedFrames
    }
}