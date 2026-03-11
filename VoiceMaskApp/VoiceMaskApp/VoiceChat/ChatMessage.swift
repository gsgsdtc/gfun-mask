/*
 * @doc     docs/modules/voice-chat/design/03-ios-voice-chat-frontend-design.md §4
 * @purpose 语音聊天的对话消息模型
 */

import Foundation

struct ChatMessage: Identifiable {
    let id: UUID
    let role: Role
    let text: String
    let timestamp: Date

    enum Role {
        case user
        case assistant
    }

    init(role: Role, text: String) {
        self.id = UUID()
        self.role = role
        self.text = text
        self.timestamp = Date()
    }
}
