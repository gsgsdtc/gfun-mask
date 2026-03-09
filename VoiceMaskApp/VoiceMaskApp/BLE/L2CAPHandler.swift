/*
 * @doc     docs/modules/ble-channel/design/02-audio-frame-protocol-design.md
 * @purpose L2CAP 信道 Stream 处理：
 *   - 支持帧协议解析（Phase 2）
 *   - 向后兼容 Phase 1 的无帧头 "hello world"
 *   - 支持发送控制指令
 *   - 提供音频帧和录音结束回调
 */

import Foundation
import CoreBluetooth

// MARK: - 帧类型常量（与 ESP32 对齐）

let FRAME_TYPE_HEARTBEAT: UInt8        = 0x00
let FRAME_TYPE_AUDIO: UInt8            = 0x01
let FRAME_TYPE_VAD_PREWARM: UInt8      = 0xFF
let FRAME_TYPE_END_OF_UTTERANCE: UInt8 = 0xFE
let FRAME_TYPE_CMD_START_RECORD: UInt8 = 0x10
let FRAME_TYPE_CMD_STOP_RECORD: UInt8  = 0x11
let FRAME_TYPE_RECORD_END: UInt8       = 0x12

final class L2CAPHandler: NSObject, StreamDelegate {

    private let channel: CBL2CAPChannel
    private weak var manager: BLEManager?
    private var buffer = Data()
    private let frameParser = FrameParser()

    // MARK: - 回调

    /// 音频帧回调
    var onAudioFrame: ((Data) -> Void)?

    /// 录音结束回调
    var onRecordEnd: ((UInt32) -> Void)?

    /// 普通消息回调（Phase 1 兼容）
    var onMessage: ((String) -> Void)?

    // MARK: - 初始化

    init(channel: CBL2CAPChannel, manager: BLEManager) {
        self.channel = channel
        self.manager = manager
    }

    func open() {
        print("[L2CAP] open: setting up streams")
        channel.inputStream.delegate = self
        channel.outputStream.delegate = self
        channel.inputStream.schedule(in: .main, forMode: .default)
        channel.outputStream.schedule(in: .main, forMode: .default)
        channel.inputStream.open()
        channel.outputStream.open()
        print("[L2CAP] open: streams opened")
    }

    func close() {
        print("[L2CAP] close: closing streams")
        channel.inputStream.close()
        channel.outputStream.close()
        channel.inputStream.remove(from: .main, forMode: .default)
        channel.outputStream.remove(from: .main, forMode: .default)
        frameParser.reset()
        buffer.removeAll()
    }

    // MARK: - 发送控制指令

    func sendCommand(_ type: UInt8) {
        guard channel.outputStream.hasSpaceAvailable else {
            print("[L2CAP] sendCommand 0x\(String(type, radix: 16)): output stream not ready")
            return
        }

        // 控制指令帧：[Frame Type][Length = 0x00 0x00]
        var frame = Data([type, 0x00, 0x00])
        let frameCount = frame.count
        let written = frame.withUnsafeMutableBytes {
            channel.outputStream.write($0.baseAddress!.assumingMemoryBound(to: UInt8.self), maxLength: frameCount)
        }
        print("[L2CAP] sendCommand 0x\(String(type, radix: 16)): written=\(written) bytes")
    }

    func sendStartRecord() {
        sendCommand(FRAME_TYPE_CMD_START_RECORD)
    }

    func sendStopRecord() {
        sendCommand(FRAME_TYPE_CMD_STOP_RECORD)
    }

    // MARK: - StreamDelegate

    func stream(_ aStream: Stream, handle eventCode: Stream.Event) {
        guard aStream === channel.inputStream else { return }

        switch eventCode {
        case .hasBytesAvailable:
            readAvailableBytes()

        case .errorOccurred:
            let error = aStream.streamError
            print("[L2CAP] stream error: \(error?.localizedDescription ?? "nil"), code=\(error?._code ?? -1)")
            close()

        case .endEncountered:
            print("[L2CAP] stream end encountered")
            close()

        default:
            break
        }
    }

    // MARK: - Private

    private func readAvailableBytes() {
        let bufSize = 1024  // 大于最大帧 643B（PCM 直传）
        var buf = [UInt8](repeating: 0, count: bufSize)

        while channel.inputStream.hasBytesAvailable {
            let n = channel.inputStream.read(&buf, maxLength: bufSize)
            guard n > 0 else { break }
            buffer.append(contentsOf: buf[0..<n])
        }

        processBuffer()
    }

    private func processBuffer() {
        print("[L2CAP] processBuffer: \(buffer.count) bytes in buffer")

        // 尝试按帧协议解析
        let frames = frameParser.parse(buffer)
        buffer.removeAll()

        print("[L2CAP] processBuffer: parsed \(frames.count) frames")
        for frame in frames {
            handleFrame(frame)
        }

        // 如果帧解析器没有消费任何数据，尝试 Phase 1 兼容模式
        if frames.isEmpty && !buffer.isEmpty {
            if let text = String(data: buffer, encoding: .utf8), !text.isEmpty {
                print("[L2CAP] processBuffer: Phase1 text=\(text)")
                onMessage?(text)
                buffer.removeAll()
            }
        }
    }

    private func handleFrame(_ frame: AudioFrame) {
        print("[L2CAP] handleFrame: type=0x\(String(frame.type, radix: 16)), payload=\(frame.payload.count) bytes")
        switch frame.type {
        case FRAME_TYPE_AUDIO:
            onAudioFrame?(frame.payload)

        case FRAME_TYPE_RECORD_END:
            if frame.payload.count == 4 {
                let totalFrames = frame.payload.withUnsafeBytes { $0.load(as: UInt32.self) }
                onRecordEnd?(totalFrames)
            }

        case FRAME_TYPE_HEARTBEAT:
            // Phase 3: 心跳处理
            break

        case FRAME_TYPE_VAD_PREWARM, FRAME_TYPE_END_OF_UTTERANCE:
            // Phase 3: VAD 相关
            break

        default:
            print("L2CAP: Unknown frame type 0x\(String(frame.type, radix: 16))")
        }
    }
}
