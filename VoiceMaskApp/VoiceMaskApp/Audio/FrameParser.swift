/*
 * @doc     docs/modules/ble-channel/design/02-audio-frame-protocol-design.md
 * @purpose 帧解析器：
 *   - 解析从 L2CAP inputStream 读取的原始数据
 *   - 支持粘包处理（多个帧在一次读取中）
 *   - 返回解析完成的帧数组
 *
 * 修复说明：
 *   - 原实现使用 buffer.removeFirst(n)，Foundation.Data 内部用 startIndex offset 优化，
 *     不会真正移动内存，导致 buffer[0] 越界崩溃（EXC_BREAKPOINT）。
 *   - 改为 cursor（Int 偏移量）方式，遍历时不修改 buffer，
 *     解析完成后一次性 removeFirst，彻底规避此问题。
 */

import Foundation

// MARK: - 帧类型

enum FrameType: UInt8 {
    case heartbeat        = 0x00
    case audio            = 0x01
    case vadPrewarm       = 0xFF
    case endOfUtterance   = 0xFE
    case cmdStartRecord   = 0x10
    case cmdStopRecord    = 0x11
    case recordEnd        = 0x12
}

// MARK: - 音频帧

struct AudioFrame {
    let type: UInt8
    let payload: Data

    var frameType: FrameType? { FrameType(rawValue: type) }
}

// MARK: - 帧解析器

final class FrameParser {

    // 内部积累缓冲区，跨次调用保留未完成帧的数据
    private var buffer = Data()

    /// 解析新到达的原始数据，返回所有完整帧
    func parse(_ data: Data) -> [AudioFrame] {
        buffer.append(data)

        var frames: [AudioFrame] = []
        var cursor = 0                          // 当前读取位置（相对于 buffer 起始）
        let bytes = buffer                      // 不可变快照，避免 startIndex 问题

        while cursor + 3 <= bytes.count {
            let base = bytes.startIndex + cursor

            let frameType  = bytes[base]
            let payloadLen = Int(bytes[base + 1]) | (Int(bytes[base + 2]) << 8)

            // 数据不完整，等下次
            guard cursor + 3 + payloadLen <= bytes.count else { break }

            let payloadStart = base + 3
            let payloadEnd   = payloadStart + payloadLen
            let payload      = bytes[payloadStart..<payloadEnd]

            frames.append(AudioFrame(type: frameType, payload: Data(payload)))
            cursor += 3 + payloadLen
        }

        // 一次性移除已解析数据，剩余不完整帧留给下次
        if cursor > 0 {
            buffer.removeFirst(cursor)
        }

        return frames
    }

    /// 重置解析器状态（连接断开时调用）
    func reset() {
        buffer = Data()         // 重新分配，彻底清空 startIndex offset
    }

    var bufferSize: Int { buffer.count }
}
