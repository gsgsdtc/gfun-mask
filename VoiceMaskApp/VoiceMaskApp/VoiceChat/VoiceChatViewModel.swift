/*
 * @doc     docs/modules/voice-chat/design/08-live-chat-ios-design.md
 * @purpose 语音聊天 ViewModel：集成 LiveChatManager，处理 BLE 事件和 UI 状态
 */

import Foundation
import Combine
import Network

// MARK: - ViewModel

@MainActor
final class VoiceChatViewModel: ObservableObject {

    // LiveChatManager 管理核心状态机
    let liveChatManager: LiveChatManager

    // 便捷访问
    var state: LiveChatState { liveChatManager.state }
    var messages: [ChatMessage] { liveChatManager.messages }

    @Published var isWebSocketConnected = false
    @Published var recordingDuration: TimeInterval = 0
    @Published var isManualRecording = false

    // 持久化服务器地址
    @Published var serverURL: String {
        didSet { UserDefaults.standard.set(serverURL, forKey: "PipecatServerURL") }
    }

    private var recordingTimer: Timer?
    private let pathMonitor = NWPathMonitor()
    private var networkReady = false
    private var cancellables = Set<AnyCancellable>()

    init() {
        let wsClient = PipecatWebSocketClient()
        let ttsPlayer = TtsAudioPlayer()
        self.liveChatManager = LiveChatManager(wsClient: wsClient, ttsPlayer: ttsPlayer)

        self.serverURL = UserDefaults.standard.string(forKey: "PipecatServerURL")
            ?? "ws://192.168.50.125:8765/ws"

        setupCallbacks()
        startNetworkMonitor()
    }

    // MARK: - 设置回调

    private func setupCallbacks() {
        // WebSocket 连接状态
        liveChatManager.wsClient.onConnectionChange = { [weak self] connected in
            Task { @MainActor in
                self?.isWebSocketConnected = connected
            }
        }

        // 监听 LiveChatManager 状态变化用于调试
        liveChatManager.$state
            .sink { [weak self] newState in
                print("[ViewModel] State changed to: \(newState.statusText)")
                if newState == .listening {
                    self?.startRecordingTimer()
                } else {
                    self?.stopRecordingTimer()
                }
            }
            .store(in: &cancellables)
    }

    // MARK: - 网络监听

    private func startNetworkMonitor() {
        pathMonitor.pathUpdateHandler = { [weak self] path in
            guard let self else { return }
            let satisfied = path.status == .satisfied
            Task { @MainActor in
                self.networkReady = satisfied
                // 网络恢复时，若未连接且需要重连则自动连接
                if satisfied && !self.liveChatManager.wsClient.isConnected
                    && !self.liveChatManager.wsClient.isConnecting
                    && self.liveChatManager.state != .sleep {
                    self.liveChatManager.ensureWebSocketConnected(serverURL: self.serverURL)
                }
            }
        }
        pathMonitor.start(queue: DispatchQueue(label: "nw.monitor"))
    }

    // MARK: - WebSocket 连接

    func connectWebSocket() {
        guard networkReady else { return }
        liveChatManager.ensureWebSocketConnected(serverURL: serverURL)
    }

    func disconnectWebSocket() {
        liveChatManager.disconnectWebSocket()
    }

    // MARK: - BLE 事件转发

    /// BLE 音频帧 (0x01)
    func handleBLEAudioFrame(_ data: Data) {
        liveChatManager.handleAudioFrame(data)
    }

    /// VAD 开始 (0x03)
    func handleVADStart() {
        liveChatManager.handleVADStart()
        // 如果需要连接 WebSocket，确保已连接
        if liveChatManager.state == .listening {
            connectWebSocket()
        }
    }

    /// 语音结束 (0xFE)
    func handleEndOfUtterance() {
        liveChatManager.handleEndOfUtterance()
    }

    /// 录音已开始通知 (0x13)
    func handleRecordingStarted() {
        liveChatManager.handleRecordingStarted()
        connectWebSocket()
    }

    // MARK: - 手动模式控制

    /// 开始手动录音
    func startManualRecording() {
        isManualRecording = true
        connectWebSocket()
        // 手动模式下，直接进入 listening 状态
        if liveChatManager.state == .sleep {
            liveChatManager.wakeFromSleep()
        }
        startRecordingTimer()
        print("[ViewModel] 手动录音开始")
    }

    /// 停止手动录音
    func stopManualRecording() {
        guard isManualRecording else { return }
        isManualRecording = false
        stopRecordingTimer()
        // 发送 stop 触发 ASR
        liveChatManager.handleEndOfUtterance()
        print("[ViewModel] 手动录音结束")
    }

    // MARK: - 录音计时

    private func startRecordingTimer() {
        recordingDuration = 0
        recordingTimer?.invalidate()
        recordingTimer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            Task { @MainActor in
                self?.recordingDuration += 0.1
            }
        }
    }

    private func stopRecordingTimer() {
        recordingTimer?.invalidate()
        recordingTimer = nil
    }

    // MARK: - 清除会话

    func clearMessages() {
        liveChatManager.clearMessages()
    }
}
