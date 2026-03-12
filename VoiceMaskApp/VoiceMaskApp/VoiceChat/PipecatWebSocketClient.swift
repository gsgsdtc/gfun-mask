/*
 * @doc     docs/modules/voice-chat/design/03-ios-voice-chat-frontend-design.md §5.3
 *          docs/modules/pipecat-pipeline/spec.md §2
 * @purpose Pipecat 协议封装层：连接管理、消息序列化/反序列化、心跳 Ping、断线重连
 *          是唯一接触协议字符串和帧格式的地方，ViewModel 层只调用类型化方法
 */

import Foundation

// TTS 音频帧前缀（服务端 iOSProtocolSerializer 约定：0xAA + MP3 bytes）
private let TTS_AUDIO_PREFIX: UInt8 = 0xAA

// MARK: - 事件类型

enum PipecatEvent {
    case ready
    case transcriptFinal(text: String)
    case llmDone(text: String)
    case ttsStart
    case ttsAudio(data: Data)
    case ttsEnd
    case error(code: String, message: String)
    case pong
}

// MARK: - Client

final class PipecatWebSocketClient: NSObject {

    var onEvent: ((PipecatEvent) -> Void)?
    var onConnectionChange: ((Bool) -> Void)?

    private var webSocketTask: URLSessionWebSocketTask?
    private var urlSession: URLSession!
    private(set) var isConnected = false
    private(set) var isConnecting = false

    // 心跳
    private var pingTimer: Timer?
    private var pongTimeoutTimer: Timer?

    // 断线重连
    private var lastURL: String = ""
    private var retryInterval: TimeInterval = 1      // 当前退避间隔，成功后重置为 1s
    private var retryWorkItem: DispatchWorkItem?
    private var intentionalDisconnect = false         // 区分主动断开 vs 意外断连

    override init() {
        super.init()
        urlSession = URLSession(configuration: .default, delegate: nil, delegateQueue: .main)
    }

    // MARK: - 连接管理

    func connect(to urlString: String) {
        // 自动将 http(s):// 转换为 ws(s)://，避免握手失败（Error -1011）
        let normalized = urlString
            .replacingOccurrences(of: "^https://", with: "wss://", options: .regularExpression)
            .replacingOccurrences(of: "^http://",  with: "ws://",  options: .regularExpression)
        guard let url = URL(string: normalized) else {
            print("[WS] Invalid URL: \(normalized)")
            return
        }
        intentionalDisconnect = false
        lastURL = normalized
        retryInterval = 1
        retryWorkItem?.cancel()
        retryWorkItem = nil

        teardownConnection()

        isConnecting = true
        webSocketTask = urlSession.webSocketTask(with: url)
        webSocketTask?.resume()
        // 握手异步完成，isConnected 由 markConnected()（收到 ready 后）置 true
        receiveNext()
        print("[WS] Connecting to \(normalized)...")
    }

    func disconnect() {
        intentionalDisconnect = true
        retryWorkItem?.cancel()
        retryWorkItem = nil
        retryInterval = 1
        teardownConnection()
    }

    private func teardownConnection() {
        stopHeartbeat()
        isConnecting = false
        webSocketTask?.cancel(with: .goingAway, reason: nil)
        webSocketTask = nil
        if isConnected {
            isConnected = false
            onConnectionChange?(false)
        }
    }

    // MARK: - 类型化命令（协议字符串只在此处出现）

    /// 开始一次录音会话，通知服务端清空缓冲区
    func startRecording() {
        print("[WS] ▶ sendControl: start")
        sendJSON(["type": "start"])
    }

    /// 结束录音，触发服务端 STT
    func stopRecording() {
        print("[WS] ■ sendControl: stop")
        sendJSON(["type": "stop"])
    }

    /// 发送 PCM 音频帧（裸二进制，无协议头）
    func sendAudioFrame(_ data: Data) {
        guard isConnected else {
            print("[WS] sendAudioFrame: dropped \(data.count) bytes (not connected)")
            return
        }
        send(.data(data))
        audioFrameCount += 1
        audioByteCount += data.count
        if audioFrameCount % 50 == 0 {
            print("[WS] sendAudioFrame: \(audioFrameCount) frames / \(audioByteCount) bytes sent so far")
        }
    }

    private var audioFrameCount = 0
    private var audioByteCount = 0

    private func resetAudioStats() {
        audioFrameCount = 0
        audioByteCount = 0
    }

    // MARK: - 内部发送

    private func sendJSON(_ dict: [String: String]) {
        guard isConnected,
              let data = try? JSONSerialization.data(withJSONObject: dict),
              let text = String(data: data, encoding: .utf8)
        else { return }
        send(.string(text))
    }

    private func send(_ message: URLSessionWebSocketTask.Message) {
        webSocketTask?.send(message) { error in
            if let error = error {
                print("[WS] Send error: \(error)")
            }
        }
    }

    // MARK: - 接收循环

    private func receiveNext() {
        webSocketTask?.receive { [weak self] result in
            guard let self = self else { return }
            switch result {
            case .success(let message):
                self.handleMessage(message)
                self.receiveNext()
            case .failure(let error):
                print("[WS] Receive error: \(error)")
                self.handleUnexpectedDisconnect()
            }
        }
    }

    private func handleUnexpectedDisconnect() {
        let wasConnected = isConnected
        stopHeartbeat()
        isConnecting = false
        isConnected = false
        webSocketTask = nil
        if wasConnected {
            onConnectionChange?(false)
        }
        guard !intentionalDisconnect, !lastURL.isEmpty else { return }
        scheduleReconnect()
    }

    // MARK: - 断线重连（指数退避：1s → 2s → 4s → … → 30s）

    private func scheduleReconnect() {
        let delay = retryInterval
        retryInterval = min(retryInterval * 2, 30)
        print("[WS] Reconnecting in \(Int(delay))s...")

        let item = DispatchWorkItem { [weak self] in
            guard let self = self, !self.intentionalDisconnect else { return }
            self.connect(to: self.lastURL)
        }
        retryWorkItem = item
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: item)
    }

    // MARK: - 连接状态

    private func markConnected() {
        guard !isConnected else { return }
        isConnecting = false
        isConnected = true
        retryInterval = 1
        resetAudioStats()
        startHeartbeat()
        print("[WS] ✓ 连接建立（收到 ready）")
        onConnectionChange?(true)
    }

    // MARK: - 心跳（30s Ping / 30s Pong 超时）

    private func startHeartbeat() {
        stopHeartbeat()
        pingTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: true) { [weak self] _ in
            self?.sendPing()
        }
    }

    private func stopHeartbeat() {
        pingTimer?.invalidate()
        pingTimer = nil
        pongTimeoutTimer?.invalidate()
        pongTimeoutTimer = nil
    }

    private func sendPing() {
        guard isConnected else { return }
        sendJSON(["type": "ping"])
        // 30s 内未收到 pong → 静默断连，触发重连
        pongTimeoutTimer?.invalidate()
        pongTimeoutTimer = Timer.scheduledTimer(withTimeInterval: 30, repeats: false) { [weak self] _ in
            print("[WS] Pong timeout, reconnecting...")
            self?.handleUnexpectedDisconnect()
        }
    }

    private func resetPongTimeout() {
        pongTimeoutTimer?.invalidate()
        pongTimeoutTimer = nil
    }

    // MARK: - 消息解析

    private func handleMessage(_ message: URLSessionWebSocketTask.Message) {
        switch message {
        case .string(let text):
            parseJSONEvent(text)
        case .data(let data):
            parseBinaryFrame(data)
        @unknown default:
            break
        }
    }

    private func parseJSONEvent(_ text: String) {
        guard let data = text.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = json["type"] as? String
        else {
            print("[WS] ← 无法解析 JSON: \(text.prefix(120))")
            return
        }

        switch type {
        case "ready":
            print("[WS] ← ready")
            markConnected()
            onEvent?(.ready)
        case "transcript_final":
            let t = json["text"] as? String ?? ""
            print("[WS] ← transcript_final: '\(t)'")
            onEvent?(.transcriptFinal(text: t))
        case "llm_done":
            let t = json["text"] as? String ?? ""
            print("[WS] ← llm_done: '\(t.prefix(80))'")
            onEvent?(.llmDone(text: t))
        case "tts_start":
            print("[WS] ← tts_start")
            onEvent?(.ttsStart)
        case "tts_end":
            print("[WS] ← tts_end")
            onEvent?(.ttsEnd)
        case "error":
            let code = json["code"] as? String ?? "UNKNOWN"
            let msg  = json["message"] as? String ?? ""
            print("[WS] ← error: code=\(code) message=\(msg)")
            onEvent?(.error(code: code, message: msg))
        case "pong":
            print("[WS] ← pong")
            resetPongTimeout()
            onEvent?(.pong)
        default:
            print("[WS] ← unknown type: \(type)")
        }
    }

    private var ttsChunkCount = 0

    private func parseBinaryFrame(_ data: Data) {
        guard !data.isEmpty else { return }
        // TTS 音频帧：0xAA 前缀 + MP3 数据
        if data[0] == TTS_AUDIO_PREFIX {
            ttsChunkCount += 1
            if ttsChunkCount == 1 {
                print("[WS] ← tts_audio 第1块: \(data.count - 1) bytes MP3")
            }
            onEvent?(.ttsAudio(data: data.dropFirst()))
            return
        }
        // 服务端偶尔以 binary 帧发送 JSON（UTF-8 编码）
        if data[0] == UInt8(ascii: "{"),
           let text = String(data: data, encoding: .utf8) {
            parseJSONEvent(text)
            return
        }
        print("[WS] ← 未知二进制帧: prefix=0x\(String(format: "%02X", data[0])), \(data.count) bytes")
    }
}
