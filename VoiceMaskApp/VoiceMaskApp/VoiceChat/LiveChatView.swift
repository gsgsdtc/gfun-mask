/*
 * @doc     docs/modules/voice-chat/design/08-live-chat-ios-design.md §2, §3
 * @purpose Live 聊天专用页面：唤醒词触发 + VAD 自动控制
 *          feat-08: 无需手动按钮，完全自动化
 */

import SwiftUI

// MARK: - LiveChatView（Live 模式专用）

struct LiveChatView: View {

    @ObservedObject var viewModel: VoiceChatViewModel
    @ObservedObject var ble: BLEManager
    @State private var showSettings = false

    var body: some View {
        VStack(spacing: 0) {
            // 状态栏：BLE + WS 状态
            LiveChatStatusBar(
                bleState: ble.connectionState,
                isWSConnected: viewModel.isWebSocketConnected,
                state: viewModel.state,
                onSettingsTap: { showSettings = true }
            )

            // 对话气泡列表
            ChatMessageList(messages: viewModel.messages)

            Divider()

            // 底部控制区（Live 模式：仅显示状态和重置）
            VStack(spacing: 12) {
                // 状态描述
                LiveChatStatusLabel(
                    state: viewModel.state,
                    duration: viewModel.recordingDuration
                )

                // Live 模式说明
                Text("说 \"Jarvis\" 唤醒，自动检测语音")
                    .font(.caption)
                    .foregroundColor(.secondary)

                // 重置按钮（仅非休眠状态显示）
                if viewModel.state != .sleep {
                    Button(action: {
                        // 调试重置：强制回到监听状态
                        ble.l2capHandler?.sendStopRecord()
                    }) {
                        Label("重置", systemImage: "arrow.counterclockwise")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                    }
                }
            }
            .padding(.horizontal, 24)
            .padding(.vertical, 16)
        }
        .sheet(isPresented: $showSettings) {
            PipecatSettingsSheet(viewModel: viewModel)
        }
        .onAppear {
            viewModel.connectWebSocket()
            setupBLECallbacks()
        }
        .onDisappear {
            viewModel.disconnectWebSocket()
        }
    }

    private func setupBLECallbacks() {
        // feat-08: 设置 BLE 帧回调（Live 模式专用）
        ble.l2capHandler?.onAudioFrame = { [weak viewModel] data in
            viewModel?.handleBLEAudioFrame(data)
        }
        ble.l2capHandler?.onVADStart = { [weak viewModel] in
            viewModel?.handleVADStart()
        }
        ble.l2capHandler?.onEndOfUtterance = { [weak viewModel] in
            viewModel?.handleEndOfUtterance()
        }
        ble.l2capHandler?.onRecordingStarted = { [weak viewModel] in
            viewModel?.handleRecordingStarted()
        }
    }
}

// MARK: - LiveChatStatusBar

struct LiveChatStatusBar: View {

    let bleState: ConnectionState
    let isWSConnected: Bool
    let state: LiveChatState
    let onSettingsTap: () -> Void

    private var bleDot: Color {
        switch bleState.color {
        case .green:  return .green
        case .yellow: return .yellow
        case .red:    return .red
        }
    }

    var body: some View {
        HStack(spacing: 8) {
            // BLE 状态
            Circle().fill(bleDot).frame(width: 8, height: 8)
            Text(bleState.label)
                .font(.caption)
                .foregroundColor(.primary)

            Spacer()

            // Live 状态指示
            LiveStateIndicator(state: state)

            Spacer()

            // WS 状态
            Circle()
                .fill(isWSConnected ? Color.blue : Color.gray)
                .frame(width: 8, height: 8)
            Text(isWSConnected ? "已连接" : "未连接")
                .font(.caption)
                .foregroundColor(isWSConnected ? .blue : .secondary)

            // 设置按钮
            Button(action: onSettingsTap) {
                Image(systemName: "gearshape")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .accessibilityLabel("服务器设置")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(Color(.systemGroupedBackground))
    }
}

// MARK: - LiveStateIndicator

struct LiveStateIndicator: View {
    let state: LiveChatState

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: iconName)
                .font(.caption2)
                .foregroundColor(iconColor)
            Text(statusText)
                .font(.caption)
                .foregroundColor(iconColor)
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(iconColor.opacity(0.1))
        .cornerRadius(8)
    }

    private var iconName: String {
        switch state {
        case .sleep:       return "moon.fill"
        case .listening:   return "ear.fill"
        case .processing: return "cpu.fill"
        case .ttsPlaying:  return "speaker.wave.2.fill"
        }
    }

    private var iconColor: Color {
        switch state {
        case .sleep:       return .gray
        case .listening:   return .green
        case .processing:   return .orange
        case .ttsPlaying:  return .blue
        }
    }

    private var statusText: String {
        switch state {
        case .sleep:       return "休眠"
        case .listening:   return "监听"
        case .processing:   return "处理"
        case .ttsPlaying:  return "播放"
        }
    }
}

// MARK: - LiveChatStatusLabel

struct LiveChatStatusLabel: View {

    let state: LiveChatState
    let duration: TimeInterval

    var displayText: String {
        switch state {
        case .sleep:
            return "😴 休眠中，说 \"Jarvis\" 唤醒"
        case .listening:
            return String(format: "👂 监听中... %.1fs", duration)
        case .processing:
            return "⚙️ 处理中..."
        case .ttsPlaying:
            return "🔊 播放回复中..."
        }
    }

    var body: some View {
        HStack(spacing: 6) {
            if state == .processing || state == .ttsPlaying {
                ProgressView().scaleEffect(0.7)
            }
            Text(displayText)
                .font(.subheadline)
                .foregroundColor(.secondary)
        }
        .frame(height: 24)
    }
}
