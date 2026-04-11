/*
 * @doc     docs/modules/voice-chat/design/08-live-chat-ios-design.md
 * @purpose Live 聊天状态机管理器：处理 BLE 帧、状态转换、WebSocket 交互、休眠计时
 *          4 状态：sleep / listening / processing / ttsPlaying
 */

import Foundation
import Combine

// MARK: - 状态枚举

enum LiveChatState: Equatable {
    case sleep          // 休眠：WebSocket 已断开，等待唤醒
    case listening      // 监听中：已唤醒，VAD 运行中，转发音频
    case processing     // 处理中：VAD 结束，等待 LLM/TTS
    case ttsPlaying     // 播放中：TTS 正在播放

    var statusText: String {
        switch self {
        case .sleep:       return "😴 休眠中，说 \"Jarvis\" 唤醒"
        case .listening:   return "👂 正在监听..."
        case .processing:  return "⚙️ 处理中..."
        case .ttsPlaying:  return "🔊 播放回复中"
        }
    }

    var isSleeping: Bool { self == .sleep }
    var isListening: Bool { self == .listening }
    var isProcessing: Bool { self == .processing }
    var isPlaying: Bool { self == .ttsPlaying }
}

// MARK: - LiveChatManager

@MainActor
final class LiveChatManager: ObservableObject {

    @Published var state: LiveChatState = .sleep
    @Published var messages: [ChatMessage] = []

    // 依赖注入
    let wsClient: PipecatWebSocketClient
    let ttsPlayer: TtsAudioPlayer

    private var sleepTimer: SleepTimer?
    private var cancellables = Set<AnyCancellable>()

    // 唤醒后需要重建 WebSocket 的标记
    private var needsWebSocketReconnect = true

    init(wsClient: PipecatWebSocketClient, ttsPlayer: TtsAudioPlayer) {
        self.wsClient = wsClient
        self.ttsPlayer = ttsPlayer
        self.sleepTimer = SleepTimer(duration: 300) // 5 分钟
        self.sleepTimer?.onTimeout = { [weak self] in
            Task { @MainActor in
                self?.enterSleep()
            }
        }
        setupCallbacks()
    }

    // MARK: - 回调设置

    private func setupCallbacks() {
        // TTS 播放完成
        ttsPlayer.onPlaybackFinished = { [weak self] in
            Task { @MainActor in
                guard let self = self else { return }
                if self.state == .ttsPlaying {
                    self.state = .listening
                    self.resetSleepTimer()
                }
            }
        }

        // WebSocket 事件
        wsClient.onEvent = { [weak self] event in
            Task { @MainActor in
                self?.handlePipecatEvent(event)
            }
        }
    }

    // MARK: - BLE 帧处理

    /// 处理从 ESP32 发来的音频帧 (0x01)
    func handleAudioFrame(_ data: Data) {
        resetSleepTimer()

        switch state {
        case .sleep:
            // 休眠中收到音频，唤醒并重建 WebSocket
            wakeUpAndReconnect()
            // 音频将在 WebSocket 连接成功后发送

        case .listening:
            // 正常转发音频
            wsClient.sendAudioFrame(data)

        case .processing, .ttsPlaying:
            // 这两个状态下不应该收到音频帧（VAD 已结束）
            // 但如果收到，说明可能是新的对话开始，切换到 listening
            print("[LiveChat] Warning: received audio in \(state.statusText), switching to listening")
            state = .listening
            wsClient.sendAudioFrame(data)
        }
    }

    /// 处理 VAD 开始帧 (0x03)
    func handleVADStart() {
        resetSleepTimer()

        switch state {
        case .sleep:
            // 休眠中唤醒
            wakeUpAndReconnect()

        case .ttsPlaying:
            // 打断 TTS 播放
            print("[LiveChat] Interrupting TTS playback")
            ttsPlayer.stopImmediate()
            wsClient.sendInterrupt()
            state = .listening

        case .listening, .processing:
            // 正常说话开始，重置计时器但不改变状态
            break
        }
    }

    /// 处理语音结束帧 (0xFE)
    func handleEndOfUtterance() {
        resetSleepTimer()

        switch state {
        case .sleep:
            // 休眠中收到结束帧，忽略
            print("[LiveChat] Ignoring END_OF_UTTERANCE in sleep state")

        case .listening:
            // 正常流程：发送 stop，进入 processing
            print("[LiveChat] VAD end detected, sending stop")
            wsClient.stopRecording()
            state = .processing

        case .processing, .ttsPlaying:
            // 不应该在这两个状态收到，忽略
            print("[LiveChat] Warning: END_OF_UTTERANCE in \(state.statusText)")
        }
    }

    /// 处理录音已开始通知 (0x13) - 唤醒词触发
    func handleRecordingStarted() {
        // 唤醒词触发后，ESP32 开始录音
        // 如果我们在休眠状态，需要唤醒
        if state == .sleep {
            wakeUpAndReconnect()
        }
    }

    // MARK: - WebSocket 连接管理

    func connectWebSocket(serverURL: String) {
        guard needsWebSocketReconnect else { return }
        wsClient.connect(to: serverURL)
        needsWebSocketReconnect = false
    }

    func disconnectWebSocket() {
        wsClient.disconnect()
        needsWebSocketReconnect = true
    }

    // MARK: - 私有方法

    private func enterSleep() {
        guard state != .sleep else { return }
        print("[LiveChat] Entering sleep state")

        // 停止 TTS 播放
        ttsPlayer.reset()

        // 发送 sleep 消息并断开 WebSocket
        wsClient.sendSleep()
        wsClient.disconnect()
        needsWebSocketReconnect = true

        state = .sleep
    }

    private func wakeUpAndReconnect() {
        guard state == .sleep else { return }
        print("[LiveChat] Waking up from sleep")

        state = .listening
        needsWebSocketReconnect = true

        // 重建 WebSocket 连接
        // 实际的连接由外部（ViewModel）触发，这里标记需要重连
    }

    private func handlePipecatEvent(_ event: PipecatEvent) {
        switch event {
        case .ready:
            // 连接建立后，如果在 listening 状态，发送 wake
            if state == .listening {
                wsClient.sendWake()
                resetSleepTimer()
            }

        case .transcriptFinal(let text):
            if !text.isEmpty {
                messages.append(ChatMessage(role: .user, text: text))
            }

        case .llmDone(let text):
            if !text.isEmpty {
                messages.append(ChatMessage(role: .assistant, text: text))
            }

        case .ttsStart:
            resetSleepTimer()
            state = .ttsPlaying

        case .ttsAudio(let data):
            ttsPlayer.appendAudio(data)

        case .ttsEnd:
            ttsPlayer.playBuffered()

        case .error(let code, let message):
            print("[LiveChat] Error: \(code) - \(message)")
            // 错误后回到 listening 状态（如果不在 sleep）
            if state != .sleep {
                state = .listening
            }

        case .pong:
            break
        }
    }

    private func resetSleepTimer() {
        sleepTimer?.reset()
    }

    // MARK: - 公共方法

    func clearMessages() {
        messages.removeAll()
    }

    /// 外部触发连接（从 ViewModel 调用）
    func ensureWebSocketConnected(serverURL: String) {
        guard needsWebSocketReconnect || !wsClient.isConnected else { return }
        connectWebSocket(serverURL: serverURL)
    }

    /// 手动模式：从休眠唤醒（不通过 WebSocket）
    func wakeFromSleep() {
        guard state == .sleep else { return }
        print("[LiveChat] Manual wake from sleep")
        state = .listening
        needsWebSocketReconnect = true
    }
}

// MARK: - 睡眠计时器

@MainActor
final class SleepTimer {

    private let duration: TimeInterval
    private var timer: Timer?
    var onTimeout: (() -> Void)?

    init(duration: TimeInterval) {
        self.duration = duration
    }

    func reset() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: duration, repeats: false) { [weak self] _ in
            self?.onTimeout?()
        }
    }

    func stop() {
        timer?.invalidate()
        timer = nil
    }
}
