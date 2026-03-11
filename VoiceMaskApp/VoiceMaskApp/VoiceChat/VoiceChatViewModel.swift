/*
 * @doc     docs/modules/voice-chat/design/03-ios-voice-chat-frontend-design.md §4
 * @purpose 语音聊天 ViewModel：管理 ChatState、消息列表、WebSocket 连接、音频转发
 */

import Foundation
import Combine
import Network

// MARK: - 状态枚举

enum ChatState: Equatable {
    case idle
    case recording
    case processing
    case playing
    case error(String)

    static func == (lhs: ChatState, rhs: ChatState) -> Bool {
        switch (lhs, rhs) {
        case (.idle, .idle), (.recording, .recording), (.processing, .processing), (.playing, .playing):
            return true
        case (.error(let a), .error(let b)):
            return a == b
        default:
            return false
        }
    }

    var statusText: String {
        switch self {
        case .idle:       return "点击开始说话"
        case .recording:  return "收音中..."
        case .processing: return "处理中..."
        case .playing:    return "播放回复中"
        case .error(let msg): return "错误：\(msg)"
        }
    }

    var isRecording: Bool { self == .recording }
    var canStart: Bool { self == .idle }
}

// MARK: - ViewModel

@MainActor
final class VoiceChatViewModel: ObservableObject {

    @Published var chatState: ChatState = .idle
    @Published var messages: [ChatMessage] = []
    @Published var isWebSocketConnected = false
    @Published var recordingDuration: TimeInterval = 0

    // 持久化服务器地址
    @Published var serverURL: String {
        didSet { UserDefaults.standard.set(serverURL, forKey: "PipecatServerURL") }
    }

    private let wsClient = PipecatWebSocketClient()
    private let ttsPlayer = TtsAudioPlayer()
    private var recordingTimer: Timer?
    private let pathMonitor = NWPathMonitor()
    private var networkReady = false

    init() {
        serverURL = UserDefaults.standard.string(forKey: "PipecatServerURL")
            ?? "ws://192.168.50.125:8765/ws"
        setupCallbacks()
        startNetworkMonitor()
    }

    // MARK: - 设置回调

    private func setupCallbacks() {
        wsClient.onConnectionChange = { [weak self] connected in
            Task { @MainActor in
                self?.isWebSocketConnected = connected
                if !connected && self?.chatState != .idle {
                    self?.chatState = .idle
                }
            }
        }

        wsClient.onEvent = { [weak self] event in
            Task { @MainActor in
                self?.handlePipecatEvent(event)
            }
        }

        ttsPlayer.onPlaybackFinished = { [weak self] in
            Task { @MainActor in
                self?.chatState = .idle
            }
        }
    }

    // MARK: - 网络监听

    private func startNetworkMonitor() {
        pathMonitor.pathUpdateHandler = { [weak self] path in
            guard let self else { return }
            let satisfied = path.status == .satisfied
            Task { @MainActor in
                self.networkReady = satisfied
                // 网络恢复时，若未连接则自动重连
                if satisfied && !self.wsClient.isConnected && !self.wsClient.isConnecting {
                    self.wsClient.connect(to: self.serverURL)
                }
            }
        }
        pathMonitor.start(queue: DispatchQueue(label: "nw.monitor"))
    }

    // MARK: - WebSocket 连接

    func connectWebSocket() {
        // 网络未就绪时跳过，等 pathMonitor 触发
        guard networkReady else { return }
        // 已连接或连接中时不重复发起，防止 onAppear 多次触发导致连接抖动
        guard !wsClient.isConnected && !wsClient.isConnecting else { return }
        wsClient.connect(to: serverURL)
    }

    func disconnectWebSocket() {
        wsClient.disconnect()
    }

    // MARK: - BLE 音频帧转发

    func handleBLEAudioFrame(_ data: Data) {
        guard chatState == .recording else { return }
        wsClient.sendAudioFrame(data)
    }

    // MARK: - 对话控制

    func startRecording() {
        guard chatState.canStart && isWebSocketConnected else {
            print("[ViewModel] startRecording: 条件不满足 canStart=\(chatState.canStart) wsConnected=\(isWebSocketConnected)")
            return
        }
        print("[ViewModel] ▶ startRecording → state=.recording")
        chatState = .recording
        recordingDuration = 0
        recordingTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.recordingDuration += 0.1
            }
        }
        wsClient.startRecording()
    }

    func stopRecording() {
        guard chatState.isRecording else { return }
        recordingTimer?.invalidate()
        recordingTimer = nil
        print("[ViewModel] ■ stopRecording → state=.processing (duration=\(String(format: "%.1f", recordingDuration))s)")
        chatState = .processing
        wsClient.stopRecording()
    }

    // MARK: - Pipecat 事件处理

    private func handlePipecatEvent(_ event: PipecatEvent) {
        switch event {
        case .ready:
            print("[ViewModel] ✓ Pipecat ready，连接已就绪")

        case .transcriptFinal(let text):
            print("[ViewModel] ← transcript_final: '\(text)'")
            if !text.isEmpty {
                messages.append(ChatMessage(role: .user, text: text))
            }

        case .llmDone(let text):
            print("[ViewModel] ← llm_done: '\(text.prefix(80))'")
            if !text.isEmpty {
                messages.append(ChatMessage(role: .assistant, text: text))
            }

        case .ttsStart:
            print("[ViewModel] ← tts_start → state=.playing")
            chatState = .playing
            ttsPlayer.reset()

        case .ttsAudio(let data):
            ttsPlayer.appendAudio(data)

        case .ttsEnd:
            print("[ViewModel] ← tts_end → playBuffered()")
            ttsPlayer.playBuffered()

        case .error(let code, let message):
            print("[ViewModel] ← error: code=\(code) message=\(message) → state=.error")
            chatState = .error("\(code): \(message)")
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
                if case .error = self?.chatState ?? .idle {
                    print("[ViewModel] error 自动恢复 → state=.idle")
                    self?.chatState = .idle
                }
            }

        case .pong:
            break
        }
    }

    // MARK: - 清除会话

    func clearMessages() {
        messages.removeAll()
    }
}
