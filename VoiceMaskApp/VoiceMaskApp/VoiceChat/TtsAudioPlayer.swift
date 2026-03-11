/*
 * @doc     docs/modules/voice-chat/design/03-ios-voice-chat-frontend-design.md §3.3
 * @purpose TTS 音频播放：缓冲服务端推送的 MP3 音频块，tts_end 后整体播放
 */

import Foundation
import AVFoundation

final class TtsAudioPlayer: NSObject, AVAudioPlayerDelegate {

    var onPlaybackFinished: (() -> Void)?

    private var audioBuffer = Data()
    private var audioPlayer: AVAudioPlayer?

    // MARK: - 缓冲

    func appendAudio(_ data: Data) {
        audioBuffer.append(data)
    }

    // MARK: - 播放（tts_end 后调用）

    func playBuffered() {
        guard !audioBuffer.isEmpty else {
            onPlaybackFinished?()
            return
        }
        do {
            try AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
            try AVAudioSession.sharedInstance().setActive(true)
            audioPlayer = try AVAudioPlayer(data: audioBuffer)
            audioPlayer?.delegate = self
            audioPlayer?.prepareToPlay()
            audioPlayer?.play()
        } catch {
            print("[TTS] Playback error: \(error)")
            reset()
            onPlaybackFinished?()
        }
    }

    func reset() {
        audioPlayer?.stop()
        audioPlayer = nil
        audioBuffer = Data()
    }

    // MARK: - AVAudioPlayerDelegate

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) {
        reset()
        onPlaybackFinished?()
    }
}
