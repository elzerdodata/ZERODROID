#pragma once

#include <SDL2/SDL.h>
#include <switch.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../core/boosteroid_api.hpp"
#include "json.hpp"
#include "audio_player.hpp"
#include "dk_video_renderer.hpp"
#include "video_decoder.hpp"
#include "video_jitter.hpp"
#include "websocket.hpp"

extern "C" {
#include <peer_connection.h>
}

namespace gnx::stream {

enum class EngineState {
    Idle,
    StartingSession,
    ConnectingGateway,
    Negotiating,
    WaitingForVideo,
    Streaming,
    Failed,
    Stopped,
};

class Engine {
public:
    Engine(ZERODROID::BoosteroidAPI& api, SDL_Renderer* renderer);
    ~Engine();

    static void global_shutdown();

    void start(int appId, const std::string& preferredGateway = {});
    void stop();

    EngineState state() const { return state_.load(); }
    std::string status() const;
    std::string error() const;
    std::string gateway() const;

    // libnx state -> Boosteroid's Android TV controller protocol.
    void send_gamepad(HidAnalogStickState left, HidAnalogStickState right,
                      u64 buttons);

    // Touchpad-style virtual mouse. deltas are in 1920x1080 design pixels.
    void virtual_mouse_move(float dx, float dy);
    void virtual_mouse_click(int button);
    void virtual_mouse_hide();
    void set_xbox_face_layout(bool enabled) { xboxFaceLayout_ = enabled; }
    void set_quick_menu_state(const QuickMenuState& state) {
        dkVideo_.set_quick_menu_state(state);
    }
    // 0 = auto, 1 = 720p, 2 = 720HQ, 3 = 1080p, 4 = 1080HQ,
    // 5 = 540p (FSR source), 6 = 480p (FSR source, handheld only).
    void set_stream_resolution(int mode);
    // Settings -> Advanced. Auto tries H.265 first and falls back to H.264.
    void set_codec_preference(CodecPreference preference);
    // 0 = auto (current default), 1 = 30 fps, 2 = 60 fps.
    void set_fps_mode(int mode);
    // Settings -> Advanced. Auto prefers Native UDP (proto: 2).
    void set_transport_preference(TransportPreference preference);
    // Settings -> Advanced. FSR 1 upscales the received frame (and sharpens it
    // at 1:1) to keep low-bitrate sessions sharp.
    void set_upscaler_preference(UpscalerPreference preference);
    TransportPreference transport_preference() const {
        return transportPreference_;
    }
    // 0 = None/negotiating, 1 = Native UDP (proto: 2 / FEC), 2 = WebRTC
    int active_transport() const { return activeTransport_.load(); }
    VideoCodec active_codec() const { return activeCodec_.load(); }
    int ping_ms() const { return pingMs_.load(); }

    struct RumbleCommand {
        uint16_t low{0};
        uint16_t high{0};
        uint32_t durationMs{400};
    };
    bool take_rumble(RumbleCommand& out);

    bool begin_deko_output();
    void end_deko_output();
    void pump_video();

    // Public so the process-wide libpeer/FFmpeg log redirects can write to the
    // active session log without reaching through private state.
    void log(const std::string& line);

private:
    struct VideoAccessUnit {
        std::vector<uint8_t> data;
        uint32_t timestamp{0};
    };

    struct NativeVideoGroup {
        uint16_t id{0};
        uint16_t dataPackets{0};
        uint16_t totalPackets{0};
        std::size_t receivedCount{0};
        std::vector<std::vector<uint8_t>> chunks;
        std::vector<bool> received;
    };

    void worker();
    // One full streaming session. Returns NoVideo when the transport came up
    // but no frame ever arrived (the only case eligible for a codec fallback).
    enum class SessionResult { Completed, NoVideo, Fatal };
    SessionResult run_session();
    bool prepare_codec_fallback();
    int stream_fps() const { return fpsMode_ == 1 ? 30 : 60; }
    void update_net_stats(uint64_t nowMs);
    void decode_loop();
    bool connect_gateway(const ZERODROID::StreamSessionConfig& config);
    bool setup_peer();
    bool start_native_udp(const std::string& host, int videoPort, int audioPort);
    void stop_native_udp();
    void native_video_loop();
    void native_audio_loop();
    void handle_native_video_packet(const uint8_t* data, size_t size);
    bool recover_native_group(NativeVideoGroup& group);
    bool decrypt_native_chunk(uint16_t group, uint16_t index,
                              const uint8_t* encrypted, size_t size,
                              std::vector<uint8_t>& plaintext);
    void destroy_peer();
    void poll_remote_candidates();
    void handle_control_message(const std::string& raw);
    void send_control_json(const std::string& payload);
    void send_input_json(nlohmann::json payload);
    void set_status(const std::string& value);
    void fail(const std::string& value);
    void request_keyframe_locked();

    static void on_video(uint8_t* data, size_t size, void* user);
    static void on_audio(uint8_t* data, size_t size, void* user);
    static void on_peer_state(PeerConnectionState state, void* user);
    static void on_channel_open(void* user);
    static void on_channel_message(char* data, size_t size, void* user,
                                   uint16_t sid);

    ZERODROID::BoosteroidAPI& api_;
    SDL_Renderer* renderer_{nullptr};
    int appId_{0};
    std::string preferredGateway_;
    std::string sessionId_;
    std::string gatewayHost_;
    std::string gatewayApiBase_;
    std::string peerId_;

    std::atomic<EngineState> state_{EngineState::Idle};
    std::atomic<bool> quit_{false};
    std::atomic<bool> gotFrame_{false};
    std::atomic<bool> gotVideoPacket_{false};
    std::atomic<bool> gotAudioPacket_{false};
    std::atomic<bool> gotAccessUnit_{false};
    std::atomic<bool> presentedFirstFrame_{false};
    std::atomic<bool> channelAssociationReady_{false};
    std::atomic<PeerConnectionState> peerState_{PEER_CONNECTION_NEW};
    std::atomic<int> controllerId_{-1};
    std::atomic<uint32_t> inputCommand_{0};
    std::atomic<bool> xboxFaceLayout_{false};
    std::atomic<int> pingMs_{-1};
    std::atomic<uint64_t> lastKeyframeRequestMs_{0};
    int streamResolution_{0};
    CodecPreference codecPreference_{CodecPreference::Auto};
    TransportPreference transportPreference_{TransportPreference::Auto};
    UpscalerPreference upscalerPreference_{UpscalerPreference::Fsr1};
    std::atomic<VideoCodec> activeCodec_{VideoCodec::H265};
    std::atomic<int> activeTransport_{0};  // 0 = none, 1 = Native UDP, 2 = WebRTC
    int fpsMode_{0};  // 0 = auto, 1 = 30, 2 = 60
    // Set by the worker to ask the decode thread (the only owner of video_)
    // to reopen the decoder with another codec. -1 = nothing pending.
    std::atomic<int> pendingCodec_{-1};
    std::atomic<uint64_t> videoBytes_{0};
    std::atomic<uint32_t> nativePackets_{0};
    std::atomic<uint32_t> nativeLostPackets_{0};
    std::atomic<uint32_t> nativeFecRecovered_{0};
    std::atomic<uint32_t> nativeDroppedFrames_{0};
    uint64_t netStatsTicks_{0};
    uint64_t netStatsBytes_{0};
    uint32_t netStatsNacks_{0};
    uint32_t netStatsPackets_{0};
    uint32_t netStatsNativePackets_{0};
    uint32_t netStatsNativeLost_{0};
    float mouseX_{0.5f};
    float mouseY_{0.5f};
    bool mouseVisible_{false};
    int mouseButtonDown_{-1};
    uint64_t mouseReleaseAt_{0};
    std::atomic<uint64_t> lastMediaTicks_{0};
    std::atomic<bool> nativeMediaStarted_{false};
    std::atomic<uint64_t> nativeStartedTicks_{0};

    mutable std::mutex statusMutex_;
    std::string status_;
    std::string error_;

    std::thread workerThread_;
    std::thread decodeThread_;
    WssClient control_;
    PeerConnection* peer_{nullptr};
    std::mutex peerMutex_;
    bool dataChannelOpened_{false};
    std::unordered_set<std::string> remoteCandidates_;
    uint64_t nextCandidatePoll_{0};

    std::mutex nativeMutex_;
    std::string nativeUdpHost_;
    std::string nativeKeyHex_;
    int nativeVideoPort_{0};
    int nativeAudioPort_{0};
    int nativeVideoSocket_{-1};
    int nativeAudioSocket_{-1};
    std::thread nativeVideoThread_;
    std::thread nativeAudioThread_;
    std::unordered_map<uint16_t, NativeVideoGroup> nativeGroups_;

    VideoJitterBuffer jitter_;
    VideoDecoder video_;
    AudioPlayer audio_;
    DkVideoRenderer dkVideo_;
    std::mutex videoMutex_;
    std::condition_variable videoCv_;
    std::deque<VideoAccessUnit> videoQueue_;

    std::mutex frameMutex_;
    AVFrame* sharedFrame_{nullptr};
    AVFrame* presentFrame_{nullptr};
    bool sharedFrameValid_{false};
    uint64_t sharedFrameSequence_{0};
    uint64_t presentedSequence_{0};
    double nextPresentCounter_{0.0};

    std::mutex inputMutex_;
    bool padInitialized_{false};
    bool inputLogged_{false};
    HidAnalogStickState previousLeft_{};
    HidAnalogStickState previousRight_{};
    u64 previousButtons_{0};

    std::mutex rumbleMutex_;
    RumbleCommand rumble_;
    bool rumblePending_{false};

    std::mutex logMutex_;
    FILE* logFile_{nullptr};
};

}  // namespace gnx::stream
