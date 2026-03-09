/*
 * @doc     docs/modules/audio-player/design/01-ios-audio-player-design.md §4.4
 * @purpose 录音记录管理
 */

import Foundation
import Combine

class RecordingManager: ObservableObject {
    @Published var recordings: [Recording] = []

    private let recordingsKey = "savedRecordings"

    init() {
        loadRecordings()
    }

    // MARK: - CRUD Operations

    func loadRecordings() {
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