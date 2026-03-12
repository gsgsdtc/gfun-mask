#!/usr/bin/env swift
// 独立 WebSocket 集成测试脚本
// 用法：swift test_ws.swift [ws://地址:端口/ws]
// 示例：swift test_ws.swift ws://192.168.5.125:8765/ws

import Foundation

let urlString = CommandLine.arguments.count > 1
    ? CommandLine.arguments[1]
    : "ws://192.168.5.125:8765/ws"

guard let url = URL(string: urlString) else {
    print("❌ 无效地址: \(urlString)")
    exit(1)
}

print("📡 正在连接: \(urlString)")

let semaphore = DispatchSemaphore(value: 0)
var exitCode: Int32 = 0

let session = URLSession(configuration: .default)
let task = session.webSocketTask(with: url)

// ── 接收函数 ────────────────────────────────────────────────
func receiveNext() {
    task.receive { result in
        switch result {
        case .failure(let error):
            print("❌ 接收错误: \(error)")
            exitCode = 1
            semaphore.signal()

        case .success(let msg):
            switch msg {
            case .string(let text):
                handleJSON(text)
            case .data(let data):
                if let first = data.first, first == 0x7B { // '{'
                    if let text = String(data: data, encoding: .utf8) {
                        handleJSON(text)
                    }
                } else if let first = data.first, first == 0xAA {
                    print("🔊 收到 TTS 音频帧，大小: \(data.count - 1) bytes")
                    receiveNext()
                } else {
                    print("📦 收到二进制帧，大小: \(data.count) bytes，首字节: \(String(format: "0x%02X", data.first ?? 0))")
                    receiveNext()
                }
            @unknown default:
                receiveNext()
            }
        }
    }
}

func handleJSON(_ text: String) {
    guard let data = text.data(using: .utf8),
          let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
          let type_ = json["type"] as? String
    else {
        print("⚠️  无法解析 JSON: \(text)")
        receiveNext()
        return
    }

    switch type_ {
    case "ready":
        print("✅ 收到 ready 事件 — 连接建立成功！")
        print()

        // 发送 ping 测试
        print("📤 发送 ping...")
        let ping = "{\"type\":\"ping\"}"
        task.send(.string(ping)) { err in
            if let err = err { print("❌ 发送 ping 失败: \(err)") }
        }
        receiveNext()

    case "pong":
        print("✅ 收到 pong 响应")
        print()
        print("═══════════════════════════════════")
        print("✅ 集成测试通过：连接正常，收发正常")
        print("═══════════════════════════════════")
        task.cancel(with: .goingAway, reason: nil)
        semaphore.signal()

    case "error":
        let code = json["code"] as? String ?? "?"
        let msg  = json["message"] as? String ?? ""
        print("❌ 服务器错误 [\(code)]: \(msg)")
        exitCode = 1
        semaphore.signal()

    default:
        print("📨 事件: \(type_) → \(json)")
        receiveNext()
    }
}

// ── 启动 ────────────────────────────────────────────────────
task.resume()
receiveNext()

// 10 秒超时
DispatchQueue.global().asyncAfter(deadline: .now() + 10) {
    print("⏰ 超时：10 秒内未收到 ready 事件")
    print("   请检查：1) 后端服务是否运行  2) IP/端口是否正确  3) 防火墙")
    exitCode = 1
    semaphore.signal()
}

semaphore.wait()
exit(exitCode)
