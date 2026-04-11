/*
 * @doc     docs/modules/voice-chat/design/08-live-chat-ios-design.md §2, §3
 * @purpose Live 聊天主页面：状态栏 + 对话气泡列表 + 状态指示
 *          feat-08: 自动唤醒词触发，VAD 自动结束，无需手动按钮
 */

import SwiftUI

// MARK: - VoiceChatView（主页面）

struct VoiceChatView: View {

    @ObservedObject var viewModel: VoiceChatViewModel
    @ObservedObject var ble: BLEManager
    @State private var showSettings = false

    var body: some View {
        VStack(spacing: 0) {
            // 状态栏：BLE + WS 状态
            VoiceChatStatusBar(
                bleState: ble.connectionState,
                isWSConnected: viewModel.isWebSocketConnected,
                onSettingsTap: { showSettings = true }
            )

            // 对话气泡列表
            ChatMessageList(messages: viewModel.messages)

            Divider()

            // 底部控制区（手动模式）
            VStack(spacing: 12) {
                // 状态描述
                VoiceChatStatusLabel(
                    state: viewModel.state,
                    duration: viewModel.recordingDuration
                )

                // 手动开始/停止按钮
                HStack(spacing: 40) {
                    Button(action: {
                        viewModel.startManualRecording()
                        ble.l2capHandler?.sendStartRecord()
                    }) {
                        Image(systemName: "play.circle.fill")
                            .font(.system(size: 44))
                            .foregroundColor(viewModel.isManualRecording ? .gray : .green)
                    }
                    .disabled(viewModel.isManualRecording)

                    Button(action: {
                        viewModel.stopManualRecording()
                        ble.l2capHandler?.sendStopRecord()
                    }) {
                        Image(systemName: "stop.circle.fill")
                            .font(.system(size: 44))
                            .foregroundColor(viewModel.isManualRecording ? .red : .gray)
                    }
                    .disabled(!viewModel.isManualRecording)
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

    private var isBLEConnected: Bool {
        if case .connected = ble.connectionState { return true }
        return false
    }

    private func setupBLECallbacks() {
        // feat-08: 设置 BLE 帧回调
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

// MARK: - VoiceChatStatusBar

struct VoiceChatStatusBar: View {

    let bleState: ConnectionState
    let isWSConnected: Bool
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

            // WS 状态
            Circle()
                .fill(isWSConnected ? Color.blue : Color.gray)
                .frame(width: 8, height: 8)
            Text(isWSConnected ? "服务器已连接" : "服务器未连接")
                .font(.caption)
                .foregroundColor(isWSConnected ? .blue : .secondary)

            // 设置按钮
            Button(action: onSettingsTap) {
                Image(systemName: "gearshape")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            .accessibilityLabel("Pipecat 服务器设置")
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
        .background(Color(.systemGroupedBackground))
    }
}

// MARK: - ChatMessageList

struct ChatMessageList: View {

    let messages: [ChatMessage]

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 12) {
                    if messages.isEmpty {
                        Text("还没有对话，点击下方按钮开始说话")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                            .frame(maxWidth: .infinity, alignment: .center)
                            .padding(.top, 40)
                    } else {
                        ForEach(messages) { msg in
                            ChatBubbleView(message: msg)
                                .id(msg.id)
                        }
                    }
                }
                .padding(.horizontal, 16)
                .padding(.vertical, 12)
            }
            .onChange(of: messages.count) { _ in
                if let last = messages.last {
                    withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
        }
    }
}

// MARK: - ChatBubbleView

struct ChatBubbleView: View {

    let message: ChatMessage

    private var isUser: Bool { message.role == .user }

    var body: some View {
        HStack {
            if isUser { Spacer(minLength: 60) }

            Text(message.text)
                .padding(.horizontal, 14)
                .padding(.vertical, 10)
                .background(isUser ? Color.blue : Color(.systemGray5))
                .foregroundColor(isUser ? .white : .primary)
                .clipShape(RoundedRectangle(cornerRadius: 16))
                .frame(maxWidth: UIScreen.main.bounds.width * 0.75, alignment: isUser ? .trailing : .leading)
                .accessibilityLabel(isUser ? "你说：\(message.text)" : "助手说：\(message.text)")

            if !isUser { Spacer(minLength: 60) }
        }
        .frame(maxWidth: .infinity, alignment: isUser ? .trailing : .leading)
    }
}

// MARK: - VoiceChatStatusLabel

struct VoiceChatStatusLabel: View {

    let state: LiveChatState
    let duration: TimeInterval

    var displayText: String {
        switch state {
        case .listening:
            return String(format: "🎙 录音中... %.1fs", duration)
        default:
            return state.statusText
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

// MARK: - PipecatSettingsSheet

struct PipecatSettingsSheet: View {

    @ObservedObject var viewModel: VoiceChatViewModel
    @Environment(\.dismiss) var dismiss
    @State private var urlInput: String = ""
    @State private var testResult: String? = nil
    @State private var isTesting = false

    var body: some View {
        NavigationView {
            Form {
                Section(header: Text("Pipecat 服务器地址")) {
                    TextField("ws://192.168.x.x:8765/ws", text: $urlInput)
                        .autocapitalization(.none)
                        .keyboardType(.URL)

                    Text("示例：ws://192.168.50.125:8765/ws")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }

                Section {
                    Button(action: testConnection) {
                        HStack {
                            if isTesting {
                                ProgressView().scaleEffect(0.8)
                            } else {
                                Image(systemName: "network")
                            }
                            Text("测试连接")
                        }
                    }
                    .disabled(isTesting)

                    if let result = testResult {
                        Text(result)
                            .font(.caption)
                            .foregroundColor(result.contains("✅") ? .green : .red)
                    }
                }
            }
            .navigationTitle("服务器配置")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("完成") {
                        viewModel.serverURL = urlInput
                        dismiss()
                    }
                }
            }
            .onAppear {
                urlInput = viewModel.serverURL
            }
        }
    }

    private func testConnection() {
        isTesting = true
        testResult = nil
        // 自动将 http(s):// 转换为 ws(s)://
        let normalized = urlInput
            .replacingOccurrences(of: "^https://", with: "wss://", options: .regularExpression)
            .replacingOccurrences(of: "^http://",  with: "ws://",  options: .regularExpression)
        guard let url = URL(string: normalized) else {
            testResult = "❌ 地址格式错误"
            isTesting = false
            return
        }
        let session = URLSession.shared
        let task = session.webSocketTask(with: url)
        task.resume()
        task.receive { result in
            DispatchQueue.main.async {
                isTesting = false
                switch result {
                case .success:
                    testResult = "✅ 连接成功"
                case .failure(let error):
                    testResult = "❌ \(error.localizedDescription)"
                }
                task.cancel(with: .goingAway, reason: nil)
            }
        }
        // 3 秒超时
        DispatchQueue.main.asyncAfter(deadline: .now() + 3) {
            if isTesting {
                isTesting = false
                testResult = "❌ 连接超时"
                task.cancel(with: .goingAway, reason: nil)
            }
        }
    }
}
