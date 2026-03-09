/*
 * @doc     docs/modules/audio-player/design/01-ios-audio-player-design.md §4.3
 * @purpose 录音数据模型
 */

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
        self.format = "PCM WAV 16kHz Mono 16-bit"
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
        formatter.locale = Locale(identifier: "zh_CN")
        return formatter.string(from: createdAt)
    }
}