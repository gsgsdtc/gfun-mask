/*
 * @doc     docs/modules/audio-player/design/01-ios-audio-player-design.md §4.2
 * @purpose 音频文件写入
 *
 * PCM 直传模式：每帧 payload 为 640 字节原始 PCM int16（16kHz, 16-bit, mono）
 * AudioReceiver.handleAudioFrame() 在每帧前加了 2 字节 little-endian 长度前缀：
 *   [len_lo, len_hi, pcm_640_bytes] × N 帧
 * 本类负责去除长度前缀，将纯 PCM 写入标准 WAV 文件，便于 AVAudioPlayer 直接播放验证。
 */

import Foundation

struct AudioFileWriter {

    /// 将音频数据写入 WAV 文件（PCM 直传模式）
    /// - Parameter frameData: AudioReceiver 输出的数据（每帧 = 2字节长度前缀 + PCM payload）
    /// - Returns: 写入成功后的 WAV 文件 URL
    static func writeAudioFile(
        opusData frameData: Data,
        sampleRate: Int = 16000,
        channels: Int = 1
    ) -> URL? {

        // 1. 剥离每帧的 2 字节长度前缀，提取纯 PCM
        var pcmData = Data()
        var offset = 0
        while offset + 2 <= frameData.count {
            let lenLo = frameData[offset]
            let lenHi = frameData[offset + 1]
            let frameLen = Int(lenLo) | (Int(lenHi) << 8)
            offset += 2
            guard offset + frameLen <= frameData.count else { break }
            pcmData.append(frameData[offset..<(offset + frameLen)])
            offset += frameLen
        }

        guard !pcmData.isEmpty else {
            print("AudioFileWriter: No PCM data to write")
            return nil
        }

        // 2. 构造 WAV 文件（RIFF/PCM）
        let wavData = buildWAV(pcm: pcmData, sampleRate: sampleRate, channels: channels)

        // 3. 写入文件
        let fileName = generateFileName()
        let fileURL = getRecordingsDirectory()
            .appendingPathComponent("\(fileName).wav")

        try? FileManager.default.createDirectory(
            at: getRecordingsDirectory(),
            withIntermediateDirectories: true
        )

        do {
            try wavData.write(to: fileURL)
            let durationSec = Double(pcmData.count / 2) / Double(sampleRate)
            print("AudioFileWriter: Saved \(pcmData.count) PCM bytes → \(String(format: "%.2f", durationSec))s WAV at \(fileURL.lastPathComponent)")
            return fileURL
        } catch {
            print("AudioFileWriter: Failed to write WAV: \(error)")
            return nil
        }
    }

    // MARK: - WAV header builder

    private static func buildWAV(pcm: Data, sampleRate: Int, channels: Int) -> Data {
        let bitsPerSample = 16
        let byteRate = sampleRate * channels * bitsPerSample / 8
        let blockAlign = channels * bitsPerSample / 8
        let dataSize = pcm.count
        let riffSize = 36 + dataSize

        var wav = Data()

        func write(_ str: String) {
            wav.append(contentsOf: str.utf8)
        }
        func writeLE32(_ v: Int) {
            wav.append(contentsOf: [
                UInt8(v & 0xFF),
                UInt8((v >> 8) & 0xFF),
                UInt8((v >> 16) & 0xFF),
                UInt8((v >> 24) & 0xFF)
            ])
        }
        func writeLE16(_ v: Int) {
            wav.append(contentsOf: [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)])
        }

        write("RIFF");    writeLE32(riffSize)
        write("WAVE")
        write("fmt ");    writeLE32(16)
        writeLE16(1)      // PCM format
        writeLE16(channels)
        writeLE32(sampleRate)
        writeLE32(byteRate)
        writeLE16(blockAlign)
        writeLE16(bitsPerSample)
        write("data");    writeLE32(dataSize)
        wav.append(pcm)

        return wav
    }

    // MARK: - Helpers

    static func generateFileName() -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd_HHmmss"
        return "recording_\(formatter.string(from: Date()))"
    }

    static func getRecordingsDirectory() -> URL {
        FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Recordings")
    }

    static func calculateFileSize(_ data: Data) -> Int64 {
        return Int64(data.count)
    }
}
