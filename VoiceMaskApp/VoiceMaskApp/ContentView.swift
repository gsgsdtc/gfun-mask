/*
 * @doc     docs/modules/audio-player/design/01-ios-audio-player-design.md §6
 * @purpose 主界面：BLE 连接状态 + 录音控制 + 录音列表
 */

import SwiftUI

struct ContentView: View {

    @StateObject private var ble = BLEManager()
    @StateObject private var audioReceiver = AudioReceiver()
    @StateObject private var recordingManager = RecordingManager()
    @State private var showRecordingDetail: Recording?

    var body: some View {
        TabView {
            // Tab 1: 录音控制
            RecordingControlView(
                ble: ble,
                audioReceiver: audioReceiver,
                recordingManager: recordingManager
            )
            .tabItem {
                Label("录音", systemImage: "mic.circle")
            }

            // Tab 2: 录音列表
            RecordingListView(
                recordingManager: recordingManager,
                selectedRecording: $showRecordingDetail
            )
            .tabItem {
                Label("列表", systemImage: "list.bullet")
            }
        }
        .onAppear {
            setupBLECallbacks()
        }
        .onChange(of: ble.l2capHandler != nil) { isConnected in
            if isConnected {
                setupBLECallbacks()
            }
        }
        .sheet(item: $showRecordingDetail) { recording in
            RecordingDetailView(recording: recording)
        }
    }

    private func setupBLECallbacks() {
        guard let handler = ble.l2capHandler else {
            print("[BLE] setupBLECallbacks: l2capHandler is nil, skip")
            return
        }
        print("[BLE] setupBLECallbacks: registering callbacks on handler \(handler)")

        // 设置音频帧回调
        handler.onAudioFrame = { [weak audioReceiver] data in
            print("[AUDIO] onAudioFrame: \(data.count) bytes")
            audioReceiver?.handleAudioFrame(data)
        }

        // 设置录音结束回调
        handler.onRecordEnd = { [weak audioReceiver] totalFrames in
            print("[AUDIO] onRecordEnd: totalFrames=\(totalFrames)")
            audioReceiver?.handleRecordEnd(expectedFrames: totalFrames)

            // 保存录音
            if let opusData = audioReceiver?.stopReceiving() {
                saveRecording(opusData: opusData, duration: audioReceiver?.recordingDuration ?? 0)
            }
        }

        // 设置消息回调（Phase 1 兼容）
        handler.onMessage = { [weak ble] text in
            print("[BLE] onMessage: \(text)")
            ble?.appendMessage(text)
        }
    }

    private func saveRecording(opusData: Data, duration: TimeInterval) {
        guard let url = AudioFileWriter.writeAudioFile(opusData: opusData) else {
            print("Failed to save recording")
            return
        }

        let recording = Recording(
            fileName: url.deletingPathExtension().lastPathComponent,
            fileURL: url,
            duration: duration,
            fileSize: AudioFileWriter.calculateFileSize(opusData)
        )

        recordingManager.addRecording(recording)
    }
}

// MARK: - RecordingControlView

struct RecordingControlView: View {
    @ObservedObject var ble: BLEManager
    @ObservedObject var audioReceiver: AudioReceiver
    @ObservedObject var recordingManager: RecordingManager

    var body: some View {
        VStack(spacing: 20) {
            // 连接状态
            StatusBar(state: ble.connectionState)

            Divider()

            // 录音状态
            VStack(spacing: 16) {
                if audioReceiver.isReceiving {
                    // 录音中
                    VStack(spacing: 8) {
                        Image(systemName: "waveform.circle.fill")
                            .font(.system(size: 64))
                            .foregroundColor(.red)

                        Text("录音中...")
                            .font(.headline)

                        Text("已录制 \(String(format: "%.1f", audioReceiver.recordingDuration)) 秒")
                            .font(.subheadline)
                            .foregroundColor(.secondary)

                        Text("已接收 \(audioReceiver.frameCount) 帧")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                } else {
                    // 待机状态
                    VStack(spacing: 8) {
                        Image(systemName: "mic.circle")
                            .font(.system(size: 64))
                            .foregroundColor(.gray)

                        Text("准备录音")
                            .font(.headline)
                    }
                }

                // 控制按钮
                HStack(spacing: 40) {
                    // 开始录音
                    Button(action: startRecording) {
                        Image(systemName: "play.circle.fill")
                            .font(.system(size: 44))
                            .foregroundColor(.green)
                    }
                    .disabled(!isConnected || audioReceiver.isReceiving)

                    // 停止录音
                    Button(action: stopRecording) {
                        Image(systemName: "stop.circle.fill")
                            .font(.system(size: 44))
                            .foregroundColor(.red)
                    }
                    .disabled(!audioReceiver.isReceiving)
                }
            }
            .padding()

            Spacer()

            // 统计信息
            HStack {
                Text("录音总数：\(recordingManager.recordings.count)")
                    .font(.footnote)
                    .foregroundColor(.secondary)
                Spacer()
            }
            .padding(.horizontal)
            .padding(.bottom)
        }
        .navigationTitle("VoiceMask")
    }

    private var isConnected: Bool {
        if case .connected = ble.connectionState {
            return true
        }
        return false
    }

    private func startRecording() {
        print("[UI] startRecording tapped, handler=\(String(describing: ble.l2capHandler))")
        audioReceiver.startReceiving()
        ble.l2capHandler?.sendStartRecord()
    }

    private func stopRecording() {
        print("[UI] stopRecording tapped, handler=\(String(describing: ble.l2capHandler))")
        ble.l2capHandler?.sendStopRecord()
    }
}

// MARK: - StatusBar

struct StatusBar: View {

    let state: ConnectionState

    private var dotColor: Color {
        switch state.color {
        case .green:  return .green
        case .yellow: return .yellow
        case .red:    return .red
        }
    }

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(dotColor)
                .frame(width: 10, height: 10)
            Text(state.label)
                .font(.subheadline)
                .foregroundColor(.primary)
            Spacer()
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .background(Color(.systemGroupedBackground))
    }
}

// MARK: - RecordingListView

struct RecordingListView: View {
    @ObservedObject var recordingManager: RecordingManager
    @Binding var selectedRecording: Recording?

    var body: some View {
        List {
            if recordingManager.recordings.isEmpty {
                Text("暂无录音")
                    .foregroundColor(.secondary)
            } else {
                ForEach(recordingManager.recordings) { recording in
                    Button(action: { selectedRecording = recording }) {
                        RecordingRowView(recording: recording)
                    }
                    .buttonStyle(.plain)
                }
                .onDelete { offsets in
                    recordingManager.deleteRecordings(at: offsets)
                }
            }
        }
        .navigationTitle("录音列表")
        .toolbar {
            EditButton()
        }
    }
}

// MARK: - RecordingRowView

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

// MARK: - RecordingDetailView

struct RecordingDetailView: View {
    let recording: Recording
    @StateObject private var player = AudioPlayer()
    @Environment(\.dismiss) var dismiss

    var body: some View {
        NavigationView {
            VStack(spacing: 24) {
                // 播放器控件
                VStack(spacing: 16) {
                    Button(action: { player.togglePlayPause() }) {
                        Image(systemName: player.isPlaying ? "pause.circle.fill" : "play.circle.fill")
                            .font(.system(size: 64))
                            .foregroundColor(.blue)
                    }

                    // 进度条
                    VStack {
                        ProgressView(value: player.currentTime, total: player.duration)
                            .progressViewStyle(.linear)

                        HStack {
                            Text(AudioPlayer.formatTime(player.currentTime))
                            Spacer()
                            Text(AudioPlayer.formatTime(player.duration))
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
                    // TODO: 实现删除
                    dismiss()
                } label: {
                    Text("删除录音")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .padding()
            }
            .navigationTitle("录音详情")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("完成") { dismiss() }
                }
            }
            .onAppear {
                _ = player.load(url: recording.fileURL)
            }
            .onDisappear {
                player.stop()
            }
        }
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

// MARK: - Preview

#Preview {
    ContentView()
}
