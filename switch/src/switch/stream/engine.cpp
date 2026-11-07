#include "engine.hpp"

#include "../../core/http.hpp"
#include "../../core/i18n.hpp"
#include "json.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/log.h>
#include <mbedtls/aes.h>
#include <peer.h>
}

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <netdb.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

extern "C" void gnx_peer_log_set(void (*cb)(const char* line));

namespace {

gnx::stream::Engine* g_active_engine = nullptr;
bool g_peer_initialized = false;

void stream_dimensions(int mode, int& width, int& height) {
    const bool docked =
        appletGetOperationMode() == AppletOperationMode_Console;
    switch (mode) {
        case 1:
        case 2:
            width = 1280;
            height = 720;
            break;
        case 3:
        case 4:
            width = 1920;
            height = 1080;
            break;
        case 5:
            // FSR source resolution: fewer pixels at the same bitrate means a
            // much better picture per pixel; EASU brings it back to the panel.
            width = 960;
            height = 540;
            break;
        case 6:
            // 480p is handheld-only. Docked it would need a 5x area upscale,
            // beyond FSR 1's 1x..4x range, so it promotes to 540p.
            width = docked ? 960 : 854;
            height = docked ? 540 : 480;
            break;
        default:
            width = docked ? 1920 : 1280;
            height = docked ? 1080 : 720;
            break;
    }
}

int stream_bitrate(int mode, int width, bool fsr) {
    // HQ modes request the same resolution with a higher encoder ceiling.
    int ceiling = 15000000;
    switch (mode) {
        case 1: ceiling = 15000000; break;  // 720p
        case 2: ceiling = 20000000; break;  // 720HQ
        case 3: ceiling = 25000000; break;  // 1080p
        case 4: ceiling = 30000000; break;  // 1080HQ
        case 5: ceiling = 10000000; break;  // 540p (FSR source)
        case 6: ceiling = 7000000; break;   // 480p (FSR source)
        default: ceiling = width >= 1920 ? 25000000 : 15000000; break;
    }
    // FSR 1 keeps the picture usable at a lower bitrate, so while it is active
    // the ceiling is trimmed by 10% to leave headroom on the Switch's WiFi.
    return fsr ? ceiling * 9 / 10 : ceiling;
}

void av_log_capture(void*, int level, const char* format, va_list args) {
    if (level > AV_LOG_WARNING || !g_active_engine) return;
    char message[320]{};
    std::vsnprintf(message, sizeof(message), format, args);
    std::size_t length = std::strlen(message);
    while (length && (message[length - 1] == '\n' || message[length - 1] == '\r')) {
        message[--length] = '\0';
    }
    if (length) g_active_engine->log(std::string("ffmpeg| ") + message);
}

std::string uuid_v4() {
    std::array<unsigned char, 16> bytes{};
    randomGet(bytes.data(), bytes.size());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);
    char output[37]{};
    std::snprintf(output, sizeof(output),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                  bytes[12], bytes[13], bytes[14], bytes[15]);
    return output;
}

std::string without_port(const std::string& host) {
    const auto colon = host.find(':');
    return colon == std::string::npos ? host : host.substr(0, colon);
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string query_session_id(const std::string& query) {
    const std::string lowered = lower_ascii(query);
    for (const char* name : {"sessionid=", "session="}) {
        const auto at = lowered.find(name);
        if (at == std::string::npos) continue;
        const auto start = at + std::strlen(name);
        const auto end = query.find('&', start);
        return query.substr(start, end == std::string::npos ? std::string::npos
                                                            : end - start);
    }
    return {};
}

std::string find_nested_string(const nlohmann::json& value,
                               std::initializer_list<const char*> keys,
                               int depth = 0) {
    if (depth > 10) return {};
    if (value.is_object()) {
        for (const char* key : keys) {
            const auto item = value.find(key);
            if (item != value.end() && item->is_string()) {
                return item->get<std::string>();
            }
        }
        for (auto item = value.begin(); item != value.end(); ++item) {
            const std::string found = find_nested_string(item.value(), keys, depth + 1);
            if (!found.empty()) return found;
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            const std::string found = find_nested_string(item, keys, depth + 1);
            if (!found.empty()) return found;
        }
    }
    return {};
}

void collect_candidate_strings(const nlohmann::json& value,
                               std::vector<std::string>& output,
                               const std::string& key = {}, int depth = 0) {
    if (depth > 10) return;
    if (value.is_string() && lower_ascii(key) == "candidate") {
        std::string candidate = value.get<std::string>();
        if (candidate.rfind("a=", 0) == 0) candidate.erase(0, 2);
        if (candidate.rfind("candidate:", 0) == 0 &&
            std::find(output.begin(), output.end(), candidate) == output.end()) {
            output.push_back(std::move(candidate));
        }
        return;
    }
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            collect_candidate_strings(item.value(), output, item.key(), depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) {
            collect_candidate_strings(item, output, key, depth + 1);
        }
    }
}

struct IceEntry {
    std::string url;
    std::string username;
    std::string credential;
};

void collect_ice_entries(const nlohmann::json& value,
                         std::vector<IceEntry>& entries, int depth = 0) {
    if (depth > 8) return;
    if (value.is_object()) {
        const auto urls = value.find("urls");
        if (urls != value.end()) {
            std::vector<std::string> values;
            if (urls->is_string()) values.push_back(urls->get<std::string>());
            if (urls->is_array()) {
                for (const auto& url : *urls) {
                    if (url.is_string()) values.push_back(url.get<std::string>());
                }
            }
            const std::string username = find_nested_string(value, {"username"});
            const std::string credential = find_nested_string(value, {"credential"});
            for (const std::string& url : values) {
                if (!url.empty() && entries.size() < 5) {
                    entries.push_back({url, username, credential});
                }
            }
            if (!values.empty()) return;
        }
        for (auto item = value.begin(); item != value.end(); ++item) {
            collect_ice_entries(item.value(), entries, depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) collect_ice_entries(item, entries, depth + 1);
    }
}

std::vector<std::string> candidates_from_sdp(const std::string& sdp) {
    std::vector<std::string> candidates;
    std::size_t at = 0;
    while ((at = sdp.find("a=candidate:", at)) != std::string::npos) {
        const std::size_t end = sdp.find_first_of("\r\n", at);
        candidates.push_back(sdp.substr(at + 2, end - at - 2));
        at = end == std::string::npos ? sdp.size() : end;
    }
    return candidates;
}

int axis_changed(int before, int after) {
    return std::abs(before - after) > 1100;
}

int controller_axis(int value) {
    // Android TV scales MotionEvent axes to this symmetric range. libnx can
    // produce -32768, which is outside that range by one.
    return std::clamp(value, -32767, 32767);
}

uint16_t read_le16(const uint8_t* value) {
    return static_cast<uint16_t>(value[0]) |
           (static_cast<uint16_t>(value[1]) << 8);
}

std::array<uint8_t, 512> g_gf_exp{};
std::array<uint8_t, 256> g_gf_log{};
std::once_flag g_gf_once;

void init_gf256() {
    uint16_t value = 1;
    for (int exponent = 0; exponent < 255; ++exponent) {
        g_gf_exp[exponent] = static_cast<uint8_t>(value);
        g_gf_log[value] = static_cast<uint8_t>(exponent);
        value <<= 1;
        if (value & 0x100U) value ^= 0x11dU;
    }
    for (int exponent = 255; exponent < 512; ++exponent) {
        g_gf_exp[exponent] = g_gf_exp[exponent - 255];
    }
}

uint8_t gf_multiply(uint8_t left, uint8_t right) {
    if (left == 0 || right == 0) return 0;
    return g_gf_exp[static_cast<unsigned int>(g_gf_log[left]) +
                    static_cast<unsigned int>(g_gf_log[right])];
}

uint8_t gf_inverse(uint8_t value) {
    if (value == 0) return 0;
    return g_gf_exp[255U - g_gf_log[value]];
}

bool invert_gf_matrix(std::vector<uint8_t>& matrix, std::size_t order,
                      std::vector<uint8_t>& inverse) {
    if (order == 0 || matrix.size() != order * order) return false;
    inverse.assign(order * order, 0);
    for (std::size_t row = 0; row < order; ++row) {
        inverse[row * order + row] = 1;
    }
    for (std::size_t column = 0; column < order; ++column) {
        std::size_t pivot = column;
        while (pivot < order && matrix[pivot * order + column] == 0) ++pivot;
        if (pivot == order) return false;
        if (pivot != column) {
            for (std::size_t at = 0; at < order; ++at) {
                std::swap(matrix[column * order + at],
                          matrix[pivot * order + at]);
                std::swap(inverse[column * order + at],
                          inverse[pivot * order + at]);
            }
        }
        const uint8_t scale = gf_inverse(matrix[column * order + column]);
        for (std::size_t at = 0; at < order; ++at) {
            matrix[column * order + at] =
                gf_multiply(matrix[column * order + at], scale);
            inverse[column * order + at] =
                gf_multiply(inverse[column * order + at], scale);
        }
        for (std::size_t row = 0; row < order; ++row) {
            if (row == column) continue;
            const uint8_t factor = matrix[row * order + column];
            if (factor == 0) continue;
            for (std::size_t at = 0; at < order; ++at) {
                matrix[row * order + at] ^=
                    gf_multiply(factor, matrix[column * order + at]);
                inverse[row * order + at] ^=
                    gf_multiply(factor, inverse[column * order + at]);
            }
        }
    }
    return true;
}

int json_int(const nlohmann::json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end()) return 0;
    if (value->is_number_integer() || value->is_number_unsigned()) {
        return value->get<int>();
    }
    if (value->is_string()) {
        try {
            return std::stoi(value->get<std::string>());
        } catch (...) {
        }
    }
    return 0;
}

int open_udp_channel(const std::string& host, int port,
                     std::string& error, int& localPort) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses = nullptr;
    const std::string service = std::to_string(port);
    const int lookup = getaddrinfo(host.c_str(), service.c_str(), &hints,
                                   &addresses);
    if (lookup != 0 || !addresses) {
        error = gnx::i18n::tr("No se pudo resolver el servidor UDP nativo.", "Could not resolve the native UDP server.");
        return -1;
    }

    int fd = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        fd = ::socket(address->ai_family, address->ai_socktype,
                      address->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(addresses);
    if (fd < 0) {
        error = gnx::i18n::tr("No se pudo abrir el canal UDP nativo.", "Could not open the native UDP channel.");
        return -1;
    }

    timeval timeout{};
    timeout.tv_usec = 200000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    int receiveBuffer = 512000;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receiveBuffer,
               sizeof(receiveBuffer));
    sockaddr_storage local{};
    socklen_t localSize = sizeof(local);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&local), &localSize) == 0) {
        if (local.ss_family == AF_INET) {
            localPort = ntohs(reinterpret_cast<sockaddr_in*>(&local)->sin_port);
        } else if (local.ss_family == AF_INET6) {
            localPort = ntohs(reinterpret_cast<sockaddr_in6*>(&local)->sin6_port);
        }
    }
    const char ping[] = "ping";
    for (int attempt = 0; attempt < 10; ++attempt) {
        ::send(fd, ping, sizeof(ping) - 1, 0);
    }
    return fd;
}

}  // namespace

namespace gnx::stream {

Engine::Engine(ZERODROID::BoosteroidAPI& api, SDL_Renderer* renderer)
    : api_(api), renderer_(renderer) {
    if (!g_peer_initialized) {
        peer_init();
        g_peer_initialized = true;
    }
}

Engine::~Engine() { stop(); }

void Engine::global_shutdown() {
    if (!g_peer_initialized) return;
    g_peer_initialized = false;
    peer_deinit();
}

void Engine::set_stream_resolution(int mode) {
    streamResolution_ = std::clamp(mode, 0, 6);
}

void Engine::set_codec_preference(CodecPreference preference) {
    codecPreference_ = preference;
}

void Engine::set_fps_mode(int mode) {
    fpsMode_ = std::clamp(mode, 0, 2);
}

void Engine::set_transport_preference(TransportPreference preference) {
    transportPreference_ = preference;
}

void Engine::set_upscaler_preference(UpscalerPreference preference) {
    upscalerPreference_ = preference;
    dkVideo_.set_upscaler(static_cast<int>(preference));
}

void Engine::start(int appId, const std::string& preferredGateway) {
    stop();
    appId_ = appId;
    preferredGateway_ = preferredGateway;
    sessionId_.clear();
    gatewayHost_.clear();
    gatewayApiBase_.clear();
    peerId_.clear();
    error_.clear();
    quit_ = false;
    gotFrame_ = false;
    gotVideoPacket_ = false;
    gotAudioPacket_ = false;
    gotAccessUnit_ = false;
    presentedFirstFrame_ = false;
    channelAssociationReady_ = false;
    peerState_ = PEER_CONNECTION_NEW;
    controllerId_ = -1;
    inputCommand_ = 0;
    pingMs_ = -1;
    lastKeyframeRequestMs_ = 0;
    mouseX_ = 0.5f;
    mouseY_ = 0.5f;
    mouseVisible_ = false;
    mouseButtonDown_ = -1;
    mouseReleaseAt_ = 0;
    dkVideo_.set_virtual_mouse_cursor(mouseX_, mouseY_, mouseVisible_);
    lastMediaTicks_ = 0;
    nativeMediaStarted_ = false;
    nativeStartedTicks_ = 0;
    dataChannelOpened_ = false;
    padInitialized_ = false;
    inputLogged_ = false;
    remoteCandidates_.clear();
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        nativeUdpHost_.clear();
        nativeKeyHex_.clear();
        nativeVideoPort_ = 0;
        nativeAudioPort_ = 0;
        nativeGroups_.clear();
    }
    // The client speaks H.265 by default and H.264 when requested or when the
    // HEVC decoder cannot be opened (Auto). The jitter buffer, the decoder and
    // the renderer HUD must all agree on the active codec.
    VideoCodec codec = codecPreference_ == CodecPreference::H264
                           ? VideoCodec::H264
                           : VideoCodec::H265;
    bool videoReady = video_.init(renderer_, codec);
    bool hevcInitFailed = false;
    if (!videoReady && codecPreference_ == CodecPreference::Auto &&
        codec == VideoCodec::H265) {
        hevcInitFailed = true;
        video_.shutdown();
        codec = VideoCodec::H264;
        videoReady = video_.init(renderer_, codec);
    }
    activeCodec_ = codec;
    activeTransport_ = 0;
    jitter_.set_codec(codec);
    jitter_.reset();
    dkVideo_.set_video_codec(codec);
    dkVideo_.set_transport_kind(0);
    pendingCodec_ = -1;
    videoBytes_ = 0;
    nativePackets_ = 0;
    nativeLostPackets_ = 0;
    nativeFecRecovered_ = 0;
    nativeDroppedFrames_ = 0;
    netStatsTicks_ = 0;
    netStatsBytes_ = 0;
    netStatsNacks_ = 0;
    netStatsPackets_ = 0;
    netStatsNativePackets_ = 0;
    netStatsNativeLost_ = 0;
    {
        int streamWidth = 1280;
        int streamHeight = 720;
        stream_dimensions(streamResolution_, streamWidth, streamHeight);
        dkVideo_.set_bitrate_cap_mbps(
            static_cast<int>(stream_bitrate(
                                 streamResolution_, streamWidth,
                                 upscalerPreference_ ==
                                     UpscalerPreference::Fsr1) /
                             1000000));
    }
    nextPresentCounter_ = 0;
    presentedSequence_ = 0;

    std::remove("sdmc:/switch/ZERODROID/stream-prev.log");
    std::rename("sdmc:/switch/ZERODROID/stream.log",
                "sdmc:/switch/ZERODROID/stream-prev.log");
    logFile_ = std::fopen("sdmc:/switch/ZERODROID/stream.log", "w");
    if (logFile_) std::setvbuf(logFile_, nullptr, _IOLBF, 0);
    g_active_engine = this;
    gnx_peer_log_set([](const char* line) {
        if (g_active_engine) g_active_engine->log(std::string("peer| ") + line);
    });
    av_log_set_callback(&av_log_capture);

    if (hevcInitFailed) log("hevc decoder unavailable; fell back to h264");
    log(std::string("video decoder ") + (videoReady ? "ready" : "FAILED") +
        " codec=" + video_codec_label(codec) +
        " (preference=" +
        (codecPreference_ == CodecPreference::Auto ? "auto"
         : codecPreference_ == CodecPreference::H265 ? "h265" : "h264") +
        ")");
    audio_.init();
    sharedFrame_ = av_frame_alloc();
    presentFrame_ = av_frame_alloc();
    sharedFrameValid_ = false;

    if (!videoReady) {
        fail(gnx::i18n::tr("No se pudo abrir el decodificador de video.",
                           "Could not open the video decoder."));
        return;
    }

    state_ = EngineState::StartingSession;
    set_status(gnx::i18n::tr("Solicitando una maquina a Boosteroid...", "Requesting a machine from Boosteroid..."));
    workerThread_ = std::thread(&Engine::worker, this);
    decodeThread_ = std::thread(&Engine::decode_loop, this);
}

void Engine::stop() {
    quit_ = true;
    videoCv_.notify_all();
    if (control_.connected()) {
        const int id = controllerId_.load();
        if (id > 0) {
            control_.send_text(nlohmann::json({
                {"type", "controller"}, {"action", "disconnected"},
                {"id", id}}).dump());
        }
        control_.send_text(nlohmann::json({
            {"type", "settings"}, {"action", "terminating"}}).dump());
    }
    control_.close();
    stop_native_udp();
    if (workerThread_.joinable()) workerThread_.join();
    if (decodeThread_.joinable()) decodeThread_.join();
    destroy_peer();
    dkVideo_.shutdown();
    audio_.shutdown();
    video_.shutdown();
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        videoQueue_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (sharedFrame_) av_frame_free(&sharedFrame_);
        if (presentFrame_) av_frame_free(&presentFrame_);
        sharedFrameValid_ = false;
    }
    if (!gatewayApiBase_.empty() && !peerId_.empty() && !sessionId_.empty()) {
        try {
            gnx::Http http;
            http.get(gatewayApiBase_ + "/api/hangup?peerid=" +
                     gnx::Http::urlencode(peerId_) + "&sessionId=" +
                     gnx::Http::urlencode(sessionId_));
        } catch (...) {
        }
    }
    if (!sessionId_.empty()) api_.stopStreamingSession(sessionId_);
    sessionId_.clear();
    if (g_active_engine == this) {
        gnx_peer_log_set(nullptr);
        g_active_engine = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(logMutex_);
        if (logFile_) std::fclose(logFile_);
        logFile_ = nullptr;
    }
    if (state_ != EngineState::Failed && state_ != EngineState::Idle) {
        state_ = EngineState::Stopped;
    }
}

Engine::SessionResult Engine::run_session() {
    ZERODROID::StreamSessionConfig config;
    log("launch appId=" + std::to_string(appId_));
    if (!api_.startStreamingSession(
            appId_, config, &quit_, [this](const std::string& message) {
                log("api| " + message);
                set_status(message);
            })) {
        if (quit_) return SessionResult::Fatal;
        fail(api_.lastError().empty() ? gnx::i18n::tr("No se pudo iniciar el juego.", "Could not start the game.")
                                     : api_.lastError());
        return SessionResult::Fatal;
    }
    if (quit_) return SessionResult::Fatal;
    sessionId_ = config.sessionId;
    log("sessionId=" + sessionId_ + " assignedGateway=" +
        (config.assignedGateway.empty() ? "no" : "yes") +
        " signedQueryBytes=" + std::to_string(config.signedQuery.size()) +
        " legacyQueries=" +
        std::to_string(config.sessionQueries.size()) + " gateways=" +
        std::to_string(config.gateways.size()));
    if (!connect_gateway(config)) return SessionResult::Fatal;

    bool peerRequested = false;
    bool announcedController = false;
    bool announcedMouse = false;
    uint64_t lastControllerAnnouncement = 0;
    uint64_t lastPingRequest = 0;
    uint64_t connectedAt = SDL_GetTicks64();
    uint64_t lastConsent = connectedAt;
    uint64_t lastFeedback = connectedAt;
    nextCandidatePoll_ = connectedAt;

    while (!quit_ && control_.connected()) {
        std::string message;
        std::string socketError;
        if (control_.read_text(message, 2, socketError)) {
            handle_control_message(message);
            const auto parsed = nlohmann::json::parse(message, nullptr, false);
            if (!parsed.is_discarded() && parsed.is_object() &&
                parsed.value("type", std::string()) == "settings" &&
                parsed.value("action", std::string()) == "webrtc") {
                peerRequested = true;
            }
        } else if (!socketError.empty() && !quit_) {
            fail(socketError);
            return SessionResult::Fatal;
        }

        const uint64_t controlNow = SDL_GetTicks64();
        if (!announcedMouse) {
            announcedMouse = true;
            send_control_json(nlohmann::json({
                {"type", "mouse"}, {"action", "connected"},
                {"LeftBtnState", false},
                {"MiddleBtnState", false},
                {"RightBtnState", false}}).dump());
        }
        if (controlNow - lastPingRequest >= 2000) {
            lastPingRequest = controlNow;
            control_.send_ping();
        }
        const int measuredPing = control_.last_ping_ms();
        if (measuredPing >= 0) {
            pingMs_ = measuredPing;
            jitter_.set_rtt_ms(measuredPing);
        }
        update_net_stats(controlNow);
        if (controllerId_.load() <= 0 &&
            (!announcedController ||
             controlNow - lastControllerAnnouncement >= 2000)) {
            announcedController = true;
            lastControllerAnnouncement = controlNow;
            // The Android TV client uses String.valueOf(localDeviceId). The
            // gateway returns this name together with the assigned remote id.
            send_control_json(nlohmann::json({
                {"type", "controller"}, {"action", "connected"},
                {"name", "1"}}).dump());
            log("controller registration requested name=1");
        }
        if (state_ == EngineState::Failed) return SessionResult::Fatal;

        if (nativeMediaStarted_) {
            const uint64_t now = SDL_GetTicks64();
            if (state_ == EngineState::WaitingForVideo && !gotFrame_ &&
                nativeStartedTicks_ > 0 && now - nativeStartedTicks_ > 30000) {
                fail(gotVideoPacket_
                         ? gnx::i18n::tr("Llegan paquetes UDP, pero no se pudo reconstruir el video.", "UDP packets arrive, but video could not be reconstructed.")
                         : gnx::i18n::tr("El gateway nativo conecto, pero no envio video UDP.", "The native gateway connected, but sent no UDP video."));
                return SessionResult::NoVideo;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (peerRequested && !peer_) {
            if (!setup_peer()) return SessionResult::Fatal;
        }

        if (peer_) {
            bool drained = false;
            {
                std::lock_guard<std::mutex> lock(peerMutex_);
                for (int index = 0; peer_ && index < 64; ++index) {
                    if (peer_connection_loop(peer_) > 0) drained = true;
                    else break;
                }
                if (channelAssociationReady_ && !dataChannelOpened_) {
                    dataChannelOpened_ =
                        peer_connection_create_datachannel_sid(
                            peer_, DATA_CHANNEL_RELIABLE, 0, 0,
                            const_cast<char*>("ClientDataChannel"),
                            const_cast<char*>(""), 0) == 0;
                    log(dataChannelOpened_ ? "ClientDataChannel opened"
                                           : "ClientDataChannel open deferred");
                }
            }

            const uint64_t now = SDL_GetTicks64();
            const PeerConnectionState peerState = peerState_.load();
            if ((peerState == PEER_CONNECTION_CONNECTED ||
                 peerState == PEER_CONNECTION_COMPLETED) &&
                state_ == EngineState::Negotiating) {
                state_ = EngineState::WaitingForVideo;
                set_status(gnx::i18n::tr("Conectado. Esperando el primer fotograma...", "Connected. Waiting for the first frame..."));
                send_control_json(nlohmann::json({
                    {"type", "settings"}, {"action", "ready"}}).dump());
                send_control_json(nlohmann::json({
                    {"type", "stream"}, {"action", "page"},
                    {"is_visible", true}}).dump());
            }
            if (peerState == PEER_CONNECTION_FAILED ||
                peerState == PEER_CONNECTION_DISCONNECTED) {
                fail(gnx::i18n::tr("La conexion WebRTC con el gateway fallo.", "The WebRTC connection to the gateway failed."));
                return SessionResult::Fatal;
            }
            if (now - lastConsent > 2000) {
                lastConsent = now;
                std::lock_guard<std::mutex> lock(peerMutex_);
                if (peer_) peer_connection_send_consent(peer_);
            }
            if (now - lastFeedback > 1000) {
                lastFeedback = now;
                uint8_t fraction = 0;
                uint32_t lost = 0, highest = 0;
                std::lock_guard<std::mutex> lock(peerMutex_);
                if (peer_ && jitter_.report_stats(&fraction, &lost, &highest)) {
                    peer_connection_send_receiver_report(peer_, fraction, lost,
                                                         highest, 0);
                    peer_connection_send_remb(peer_, 20000000);
                }
            }
            if (state_ == EngineState::WaitingForVideo &&
                now - connectedAt > 30000 && !gotFrame_) {
                fail(gnx::i18n::tr("WebRTC conecto, pero Boosteroid no envio video.", "WebRTC connected, but Boosteroid sent no video."));
                return SessionResult::NoVideo;
            }
            if (!drained) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            if (SDL_GetTicks64() - connectedAt > 20000 && !peerRequested) {
                fail(gnx::i18n::tr("El gateway no solicito iniciar WebRTC.", "The gateway did not request WebRTC startup."));
                return SessionResult::Fatal;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (!quit_ && state_ != EngineState::Failed) {
        state_ = EngineState::Stopped;
        set_status(gnx::i18n::tr("La sesion termino.", "The session ended."));
    }
    return SessionResult::Completed;
}

// Runs one streaming session and, when the user chose Auto and the H.265
// session never produced a frame, retries once with H.264 on the same VM.
void Engine::worker() {
    bool codecRetried = false;
    for (;;) {
        const SessionResult result = run_session();
        if (result != SessionResult::NoVideo || quit_ || codecRetried ||
            codecPreference_ != CodecPreference::Auto ||
            activeCodec_.load() != VideoCodec::H265) {
            return;
        }
        codecRetried = true;
        log("no video with h265; retrying the session with h264");
        if (!prepare_codec_fallback()) return;
    }
}

// Tear the failed session down and ask the decode thread to reopen the decoder
// as H.264. Runs on the worker thread; never joins the worker or decode thread.
bool Engine::prepare_codec_fallback() {
    control_.close();
    stop_native_udp();
    destroy_peer();
    sessionId_.clear();
    gatewayHost_.clear();
    gatewayApiBase_.clear();
    peerId_.clear();
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        videoQueue_.clear();
    }
    gotFrame_ = false;
    gotVideoPacket_ = false;
    gotAudioPacket_ = false;
    gotAccessUnit_ = false;
    presentedFirstFrame_ = false;
    channelAssociationReady_ = false;
    peerState_ = PEER_CONNECTION_NEW;
    controllerId_ = -1;
    pingMs_ = -1;
    dataChannelOpened_ = false;
    nativeMediaStarted_ = false;
    nativeStartedTicks_ = 0;
    lastMediaTicks_ = 0;
    lastKeyframeRequestMs_ = 0;
    padInitialized_ = false;
    inputLogged_ = false;
    remoteCandidates_.clear();
    nextCandidatePoll_ = 0;
    videoBytes_ = 0;
    netStatsTicks_ = 0;
    netStatsBytes_ = 0;
    netStatsNacks_ = 0;
    netStatsPackets_ = 0;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        nativeUdpHost_.clear();
        nativeKeyHex_.clear();
        nativeVideoPort_ = 0;
        nativeAudioPort_ = 0;
        nativeGroups_.clear();
    }

    activeCodec_ = VideoCodec::H264;
    activeTransport_ = 0;
    jitter_.set_codec(VideoCodec::H264);
    jitter_.reset();
    dkVideo_.set_video_codec(VideoCodec::H264);
    dkVideo_.set_transport_kind(0);
    nativePackets_ = 0;
    nativeLostPackets_ = 0;
    nativeFecRecovered_ = 0;
    nativeDroppedFrames_ = 0;
    netStatsNativePackets_ = 0;
    netStatsNativeLost_ = 0;

    // The decode thread is the only owner of video_ while streaming, so the
    // reopen is handed to it and we wait for the swap to be consumed.
    pendingCodec_ = static_cast<int>(VideoCodec::H264);
    videoCv_.notify_all();
    for (int i = 0; i < 400 && pendingCodec_.load() >= 0 && !quit_; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (quit_) return false;
    if (pendingCodec_.load() >= 0) {
        fail(gnx::i18n::tr("No se pudo reiniciar el decodificador.",
                           "Could not restart the video decoder."));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        error_.clear();
    }
    state_ = EngineState::StartingSession;
    set_status(gnx::i18n::tr("Reintentando en H.264...",
                             "Retrying with H.264..."));
    return true;
}

bool Engine::connect_gateway(const ZERODROID::StreamSessionConfig& config) {
    state_ = EngineState::ConnectingGateway;
    set_status(gnx::i18n::tr("Conectando con el gateway de streaming...", "Connecting to the streaming gateway..."));
    // The details endpoint signs a particular VM gateway.  It must take
    // priority over any generic/preferred list entry.
    if (!config.assignedGateway.empty()) gatewayHost_ = config.assignedGateway;
    if (gatewayHost_.empty() && !preferredGateway_.empty()) {
        gatewayHost_ = preferredGateway_;
    }
    if (gatewayHost_.empty() && !config.gateways.empty()) {
        gatewayHost_ = config.gateways.front().host;
    }
    if (gatewayHost_.rfind("wss://", 0) == 0) gatewayHost_.erase(0, 6);
    if (gatewayHost_.rfind("https://", 0) == 0) gatewayHost_.erase(0, 8);
    const auto slash = gatewayHost_.find('/');
    if (slash != std::string::npos) gatewayHost_.erase(slash);
    if (gatewayHost_.empty()) {
        fail(gnx::i18n::tr("Boosteroid no devolvio un gateway de streaming.", "Boosteroid did not return a streaming gateway."));
        return false;
    }

    std::string query = config.signedQuery;
    if (!query.empty() && query.front() == '?') query.erase(0, 1);
    if (query.empty()) {
        for (const std::string& candidate : config.sessionQueries) {
            std::string normalized = candidate;
            if (!normalized.empty() && normalized.front() == '?') {
                normalized.erase(0, 1);
            }
            if (query_session_id(normalized) == sessionId_ &&
                normalized.find('&') != std::string::npos) {
                query = std::move(normalized);
                break;
            }
        }
    }
    if (query.empty()) {
        fail(gnx::i18n::tr("Falta la firma para conectarse al gateway.", "The gateway signature is missing."));
        return false;
    }

    gatewayApiBase_ = "https://" + without_port(gatewayHost_) + "/webrtc";
    // Exact Android TV control route and device identity. x/y select the
    // requested stream resolution; Automatic follows the current Switch output
    // mode (720p handheld, 1080p docked).
    int streamWidth = 1280;
    int streamHeight = 720;
    stream_dimensions(streamResolution_, streamWidth, streamHeight);
    const std::string path = "/native?" + query +
        "&x=" + std::to_string(streamWidth) +
        "&y=" + std::to_string(streamHeight) +
        "&lang=es&tv=1&clientType=native&devType=tv&os=atv";
    std::string error;
    log("gateway=" + gatewayHost_ + " queryLength=" +
        std::to_string(query.size()) + " controlPath=/native device=tv requested=" +
        std::to_string(streamWidth) + "x" + std::to_string(streamHeight));
    if (!control_.connect(gatewayHost_, path, "romfs:/cacert.pem", error)) {
        fail(error.empty() ? gnx::i18n::tr("No se pudo abrir el canal de control.", "Could not open the control channel.") : error);
        return false;
    }
    return true;
}

bool Engine::start_native_udp(const std::string& host, int videoPort,
                              int audioPort) {
    if (nativeMediaStarted_) return true;
    if (host.empty() || videoPort <= 0 || audioPort <= 0) {
        fail(gnx::i18n::tr("El gateway devolvio una configuracion UDP incompleta.", "The gateway returned an incomplete UDP configuration."));
        return false;
    }

    std::string error;
    int videoLocalPort = 0;
    int audioLocalPort = 0;
    const int videoSocket = open_udp_channel(
        host, videoPort, error, videoLocalPort);
    if (videoSocket < 0) {
        fail(error);
        return false;
    }
    const int audioSocket = open_udp_channel(
        host, audioPort, error, audioLocalPort);
    if (audioSocket < 0) {
        ::close(videoSocket);
        fail(error);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        nativeUdpHost_ = host;
        nativeVideoPort_ = videoPort;
        nativeAudioPort_ = audioPort;
        nativeVideoSocket_ = videoSocket;
        nativeAudioSocket_ = audioSocket;
    }
    nativeStartedTicks_ = SDL_GetTicks64();
    nativeMediaStarted_ = true;
    activeTransport_.store(1);
    dkVideo_.set_transport_kind(1);
    state_ = EngineState::WaitingForVideo;
    set_status(gnx::i18n::tr("Canal nativo conectado. Esperando el primer fotograma...", "Native channel connected. Waiting for the first frame..."));
    log("native UDP ready videoLocalPort=" +
        std::to_string(videoLocalPort) + " audioLocalPort=" +
        std::to_string(audioLocalPort));
    nativeVideoThread_ = std::thread(&Engine::native_video_loop, this);
    nativeAudioThread_ = std::thread(&Engine::native_audio_loop, this);
    return true;
}

void Engine::stop_native_udp() {
    int videoSocket = -1;
    int audioSocket = -1;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        videoSocket = nativeVideoSocket_;
        audioSocket = nativeAudioSocket_;
        nativeVideoSocket_ = -1;
        nativeAudioSocket_ = -1;
    }
    if (videoSocket >= 0) {
        ::shutdown(videoSocket, SHUT_RDWR);
        ::close(videoSocket);
    }
    if (audioSocket >= 0) {
        ::shutdown(audioSocket, SHUT_RDWR);
        ::close(audioSocket);
    }
    if (nativeVideoThread_.joinable()) nativeVideoThread_.join();
    if (nativeAudioThread_.joinable()) nativeAudioThread_.join();
    nativeMediaStarted_ = false;
}

void Engine::native_video_loop() {
    int socket = -1;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        socket = nativeVideoSocket_;
    }
    std::array<uint8_t, 2048> packet{};
    uint64_t lastKeepalive = SDL_GetTicks64();
    while (!quit_ && socket >= 0) {
        const ssize_t received = ::recv(socket, packet.data(), packet.size(), 0);
        if (received > 0) {
            handle_native_video_packet(packet.data(),
                                       static_cast<size_t>(received));
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR && !quit_) {
            log("native video UDP receive error=" + std::to_string(errno));
            break;
        }
        const uint64_t now = SDL_GetTicks64();
        if (now - lastKeepalive >= 10000) {
            static constexpr char keepalive[] = "{\"type\":\"keepalive\"}";
            ::send(socket, keepalive, sizeof(keepalive) - 1, 0);
            lastKeepalive = now;
        }
    }
}

void Engine::native_audio_loop() {
    int socket = -1;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        socket = nativeAudioSocket_;
    }
    std::array<uint8_t, 2048> packet{};
    uint64_t lastKeepalive = SDL_GetTicks64();
    while (!quit_ && socket >= 0) {
        const ssize_t received = ::recv(socket, packet.data(), packet.size(), 0);
        if (received >= 12 && (packet[0] & 0xc0U) == 0x80U &&
            (packet[1] & 0x7fU) == 97U) {
            on_audio(packet.data(), static_cast<size_t>(received), this);
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                   errno != EINTR && !quit_) {
            log("native audio UDP receive error=" + std::to_string(errno));
            break;
        }
        const uint64_t now = SDL_GetTicks64();
        if (now - lastKeepalive >= 10000) {
            static constexpr char keepalive[] = "{\"type\":\"keepalive\"}";
            ::send(socket, keepalive, sizeof(keepalive) - 1, 0);
            lastKeepalive = now;
        }
    }
}

bool Engine::decrypt_native_chunk(uint16_t group, uint16_t index,
                                  const uint8_t* encrypted, size_t size,
                                  std::vector<uint8_t>& plaintext) {
    if (!encrypted || size == 0 || size % 16 != 0) return false;
    std::string keyHex;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        keyHex = nativeKeyHex_;
    }
    if (keyHex.size() % 2 != 0) return false;
    std::vector<uint8_t> key(keyHex.size() / 2);
    auto nibble = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
    };
    for (std::size_t at = 0; at < key.size(); ++at) {
        const int high = nibble(keyHex[at * 2]);
        const int low = nibble(keyHex[at * 2 + 1]);
        if (high < 0 || low < 0) return false;
        key[at] = static_cast<uint8_t>((high << 4) | low);
    }
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) return false;

    std::array<uint8_t, 16> iv{};
    iv[6] = static_cast<uint8_t>(group & 0xffU);
    iv[7] = static_cast<uint8_t>(group >> 8);
    iv[14] = static_cast<uint8_t>(index & 0xffU);
    iv[15] = static_cast<uint8_t>(index >> 8);
    plaintext.resize(size);
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    const int keyResult = mbedtls_aes_setkey_dec(
        &aes, key.data(), static_cast<unsigned int>(key.size() * 8));
    const int decryptResult = keyResult == 0
        ? mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, size, iv.data(),
                                encrypted, plaintext.data())
        : keyResult;
    mbedtls_aes_free(&aes);
    return decryptResult == 0;
}

bool Engine::recover_native_group(NativeVideoGroup& group) {
    const std::size_t dataCount = group.dataPackets;
    const std::size_t totalCount = group.totalPackets;
    if (dataCount == 0 || totalCount <= dataCount || totalCount > 255 ||
        group.receivedCount < dataCount || group.chunks.size() != totalCount ||
        group.received.size() != totalCount) return false;

    std::vector<std::size_t> available;
    available.reserve(dataCount);
    for (std::size_t index = 0;
         index < totalCount && available.size() < dataCount; ++index) {
        if (group.received[index] && group.chunks[index].size() == 1410) {
            available.push_back(index);
        }
    }
    if (available.size() != dataCount) return false;

    std::call_once(g_gf_once, init_gf256);
    const std::size_t parityCount = totalCount - dataCount;
    std::vector<uint8_t> decodeMatrix(dataCount * dataCount, 0);
    for (std::size_t row = 0; row < dataCount; ++row) {
        const std::size_t shard = available[row];
        if (shard < dataCount) {
            decodeMatrix[row * dataCount + shard] = 1;
            continue;
        }
        const std::size_t parityRow = shard - dataCount;
        for (std::size_t column = 0; column < dataCount; ++column) {
            const uint8_t denominator = static_cast<uint8_t>(
                (parityCount + column) ^ parityRow);
            if (denominator == 0) return false;
            decodeMatrix[row * dataCount + column] =
                gf_inverse(denominator);
        }
    }
    std::vector<uint8_t> inverse;
    if (!invert_gf_matrix(decodeMatrix, dataCount, inverse)) return false;

    std::size_t recovered = 0;
    for (std::size_t missing = 0; missing < dataCount; ++missing) {
        if (group.received[missing]) continue;
        std::vector<uint8_t> shard(1410, 0);
        for (std::size_t source = 0; source < dataCount; ++source) {
            const uint8_t coefficient =
                inverse[missing * dataCount + source];
            if (coefficient == 0) continue;
            const auto& input = group.chunks[available[source]];
            if (coefficient == 1) {
                for (std::size_t at = 0; at < shard.size(); ++at) {
                    shard[at] ^= input[at];
                }
            } else {
                for (std::size_t at = 0; at < shard.size(); ++at) {
                    shard[at] ^= gf_multiply(coefficient, input[at]);
                }
            }
        }
        group.chunks[missing] = std::move(shard);
        group.received[missing] = true;
        ++recovered;
    }
    if (recovered > 0) {
        log("native Reed-Solomon recovered packets=" +
            std::to_string(recovered));
    }
    return std::all_of(group.received.begin(),
                       group.received.begin() + dataCount,
                       [](bool value) { return value; });
}

void Engine::handle_native_video_packet(const uint8_t* data, size_t size) {
    // Android TV's native transport uses a 10-byte Reed-Solomon header plus
    // a 2-byte plaintext length and one 1408-byte AES-CBC block.
    if (!data || size != 1420) return;
    lastMediaTicks_ = SDL_GetTicks64();
    ++nativePackets_;
    if (!gotVideoPacket_.exchange(true)) {
        log("first native video UDP packet bytes=" + std::to_string(size));
    }
    const uint16_t groupId = read_le16(data + 2);
    const uint16_t packetIndex = read_le16(data + 4);
    const uint16_t dataPackets = read_le16(data + 6);
    const uint16_t totalPackets = read_le16(data + 8);
    if (dataPackets == 0 || totalPackets < dataPackets ||
        dataPackets > 1024 || totalPackets > 1024 ||
        packetIndex >= totalPackets) return;

    NativeVideoGroup complete;
    {
        std::lock_guard<std::mutex> lock(nativeMutex_);
        if (nativeGroups_.size() >= 8 &&
            nativeGroups_.find(groupId) == nativeGroups_.end()) {
            nativeGroups_.erase(nativeGroups_.begin());
        }
        auto& group = nativeGroups_[groupId];
        if (group.dataPackets != dataPackets ||
            group.totalPackets != totalPackets) {
            group = NativeVideoGroup();
            group.id = groupId;
            group.dataPackets = dataPackets;
            group.totalPackets = totalPackets;
            group.chunks.resize(totalPackets);
            group.received.assign(totalPackets, false);
        }
        if (!group.received[packetIndex]) {
            group.chunks[packetIndex].assign(data + 10, data + size);
            group.received[packetIndex] = true;
            ++group.receivedCount;
        }
        const bool allOriginal = std::all_of(
            group.received.begin(), group.received.begin() + dataPackets,
            [](bool value) { return value; });
        if ((!allOriginal && group.receivedCount < group.dataPackets) ||
            nativeKeyHex_.empty()) {
            return;
        }
        complete = std::move(group);
        nativeGroups_.erase(groupId);
    }

    const bool allOriginal = std::all_of(
        complete.received.begin(),
        complete.received.begin() + complete.dataPackets,
        [](bool value) { return value; });
    if (!allOriginal) {
        size_t missing = 0;
        for (size_t i = 0; i < complete.dataPackets; ++i) {
            if (!complete.received[i]) ++missing;
        }
        nativeLostPackets_ += static_cast<uint32_t>(missing);
        if (!recover_native_group(complete)) {
            ++nativeDroppedFrames_;
            log("native UDP: Reed-Solomon FEC recovery failed (missing " +
                std::to_string(missing) + " packets); dropping frame");
            video_.force_keyframe_wait();
            request_keyframe_locked();
            return;
        }
        nativeFecRecovered_ += static_cast<uint32_t>(missing);
    }

    std::vector<uint8_t> accessUnit;
    accessUnit.reserve(static_cast<size_t>(complete.dataPackets) * 1408);
    for (uint16_t index = 0; index < complete.dataPackets; ++index) {
        const auto& chunk = complete.chunks[index];
        if (chunk.size() != 1410) return;
        const uint16_t payloadBytes = read_le16(chunk.data());
        if (payloadBytes > 1408) return;
        std::vector<uint8_t> plaintext;
        if (!decrypt_native_chunk(complete.id, index, chunk.data() + 2,
                                  1408, plaintext)) return;
        if (index == 0 &&
            (plaintext.size() < 4 || plaintext[0] != 0 ||
             plaintext[1] != 0 || plaintext[2] != 0)) return;
        accessUnit.insert(accessUnit.end(), plaintext.begin(),
                          plaintext.begin() + payloadBytes);
    }
    if (accessUnit.empty()) return;
    videoBytes_ += accessUnit.size();
    if (!gotAccessUnit_.exchange(true)) {
        log(std::string("first complete native ") +
            video_codec_label(activeCodec_.load()) + " access unit bytes=" +
            std::to_string(accessUnit.size()));
    }
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        if (videoQueue_.size() >= 64) videoQueue_.clear();
        VideoAccessUnit unit;
        unit.data = std::move(accessUnit);
        unit.timestamp = complete.id;
        videoQueue_.push_back(std::move(unit));
    }
    videoCv_.notify_one();
}

bool Engine::setup_peer() {
    activeTransport_.store(2);
    dkVideo_.set_transport_kind(2);
    state_ = EngineState::Negotiating;
    set_status(gnx::i18n::tr("Negociando video y audio WebRTC...", "Negotiating WebRTC video and audio..."));
    peerId_ = uuid_v4();

    std::vector<IceEntry> iceEntries;
    try {
        gnx::Http http;
        const auto response = http.get(
            gatewayApiBase_ + "/api/getIceServers?sessionId=" +
            gnx::Http::urlencode(sessionId_), {"Accept: application/json"});
        log("ICE servers HTTP " + std::to_string(response.status));
        if (response.ok()) {
            const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
            if (!parsed.is_discarded()) collect_ice_entries(parsed, iceEntries);
        }
    } catch (const std::exception& error) {
        log(std::string("ICE server lookup: ") + error.what());
    }
    if (iceEntries.empty()) {
        iceEntries.push_back({"stun:stun.l.google.com:19302", {}, {}});
    }

    PeerConfiguration peerConfig{};
    for (std::size_t i = 0; i < iceEntries.size() && i < 5; ++i) {
        peerConfig.ice_servers[i].urls = iceEntries[i].url.c_str();
        peerConfig.ice_servers[i].username = iceEntries[i].username.empty()
            ? nullptr : iceEntries[i].username.c_str();
        peerConfig.ice_servers[i].credential = iceEntries[i].credential.empty()
            ? nullptr : iceEntries[i].credential.c_str();
    }
    peerConfig.audio_codec = CODEC_OPUS;
    peerConfig.video_codec = CODEC_H265;
    peerConfig.datachannel = DATA_CHANNEL_STRING;
    peerConfig.onaudiotrack = &Engine::on_audio;
    peerConfig.onvideotrack = &Engine::on_video;
    peerConfig.user_data = this;

    std::string offer;
    {
        std::lock_guard<std::mutex> lock(peerMutex_);
        peer_ = peer_connection_create(&peerConfig);
        if (!peer_) {
            fail(gnx::i18n::tr("No se pudo crear PeerConnection.", "Could not create PeerConnection."));
            return false;
        }
        peer_connection_oniceconnectionstatechange(peer_, &Engine::on_peer_state);
        peer_connection_ondatachannel(peer_, &Engine::on_channel_message,
                                      &Engine::on_channel_open, nullptr);
        const char* generated = peer_connection_create_offer(peer_);
        if (!generated) {
            fail(gnx::i18n::tr("No se pudo crear la oferta WebRTC.", "Could not create the WebRTC offer."));
            return false;
        }
        offer = generated;
    }
    log("offer bytes=" + std::to_string(offer.size()));

    try {
        gnx::Http http;
        const std::string url = gatewayApiBase_ + "/api/call?peerid=" +
            gnx::Http::urlencode(peerId_) + "&sessionId=" +
            gnx::Http::urlencode(sessionId_);
        const auto response = http.post(
            url, nlohmann::json({{"type", "offer"}, {"sdp", offer}}).dump(),
            {"Accept: application/json", "Content-Type: application/json"});
        log("WebRTC offer HTTP " + std::to_string(response.status));
        if (!response.ok()) {
            fail(gnx::i18n::tr("El gateway rechazo la oferta WebRTC (HTTP ", "The gateway rejected the WebRTC offer (HTTP ") +
                 std::to_string(response.status) + ").");
            return false;
        }
        const auto answerJson = nlohmann::json::parse(response.body, nullptr, false);
        const std::string answer = answerJson.is_discarded()
            ? std::string()
            : find_nested_string(answerJson, {"sdp"});
        if (answer.empty()) {
            fail(gnx::i18n::tr("El gateway devolvio una respuesta WebRTC no valida.", "The gateway returned an invalid WebRTC response."));
            return false;
        }

        // libpeer builds ICE pairs only when the remote description is first
        // installed. Boosteroid trickles its candidates through a REST poll,
        // so gather at least the first route before setRemoteDescription.
        const bool answerContainsCandidates =
            !candidates_from_sdp(answer).empty();
        std::vector<std::string> answerCandidates;
        const auto candidateDeadline = std::chrono::steady_clock::now() +
                                       std::chrono::seconds(8);
        while (!answerContainsCandidates && answerCandidates.empty() &&
               std::chrono::steady_clock::now() < candidateDeadline && !quit_) {
            try {
                const auto candidateResponse = http.get(
                    gatewayApiBase_ + "/api/getIceCandidate?peerid=" +
                    gnx::Http::urlencode(peerId_) + "&sessionId=" +
                    gnx::Http::urlencode(sessionId_),
                    {"Accept: application/json"});
                if (candidateResponse.ok()) {
                    const auto candidateJson = nlohmann::json::parse(
                        candidateResponse.body, nullptr, false);
                    if (!candidateJson.is_discarded()) {
                        collect_candidate_strings(candidateJson, answerCandidates);
                    }
                }
            } catch (...) {
            }
            if (answerCandidates.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }
        {
            std::lock_guard<std::mutex> lock(peerMutex_);
            if (!answerContainsCandidates) {
                for (const std::string& candidate : answerCandidates) {
                    if (remoteCandidates_.insert(candidate).second) {
                        peer_connection_add_ice_candidate(
                            peer_, const_cast<char*>(candidate.c_str()));
                    }
                }
            }
            peer_connection_set_remote_description(
                peer_, answer.c_str(), SDP_TYPE_ANSWER);
        }
        log("answer bytes=" + std::to_string(answer.size()) +
            " remoteCandidates=" +
            std::to_string(answerContainsCandidates
                               ? candidates_from_sdp(answer).size()
                               : answerCandidates.size()));

        // The offer already contains gathered candidates. Also trickle them to
        // the endpoint used by the official web client for gateway variants
        // that do not consume candidates embedded in SDP.
        int mediaIndex = 0;
        for (const std::string& candidate : candidates_from_sdp(offer)) {
            const nlohmann::json body = {
                {"candidate", candidate}, {"sdpMid", "0"},
                {"sdpMLineIndex", mediaIndex}};
            try {
                http.post(gatewayApiBase_ + "/api/addIceCandidate?peerid=" +
                              gnx::Http::urlencode(peerId_) + "&sessionId=" +
                              gnx::Http::urlencode(sessionId_),
                          body.dump(), {"Content-Type: application/json"});
            } catch (...) {
            }
        }
    } catch (const std::exception& error) {
        fail(std::string(gnx::i18n::tr("Fallo de señalizacion WebRTC: ", "WebRTC signaling failure: ")) + error.what());
        return false;
    }
    return true;
}

void Engine::poll_remote_candidates() {
    if (!peer_) return;
    try {
        gnx::Http http;
        const auto response = http.get(
            gatewayApiBase_ + "/api/getIceCandidate?peerid=" +
            gnx::Http::urlencode(peerId_) + "&sessionId=" +
            gnx::Http::urlencode(sessionId_), {"Accept: application/json"});
        if (!response.ok()) return;
        const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) return;
        std::vector<std::string> candidates;
        collect_candidate_strings(parsed, candidates);
        std::lock_guard<std::mutex> lock(peerMutex_);
        for (const std::string& candidate : candidates) {
            if (!remoteCandidates_.insert(candidate).second) continue;
            peer_connection_add_ice_candidate(
                peer_, const_cast<char*>(candidate.c_str()));
            log("remote ICE " + candidate.substr(0, 120));
        }
    } catch (...) {
    }
}

void Engine::handle_control_message(const std::string& raw) {
    const auto message = nlohmann::json::parse(raw, nullptr, false);
    if (message.is_discarded() || !message.is_object()) return;
    const std::string type = message.value("type", std::string());
    const std::string action = message.value("action", std::string());
    log("control type=" + (type.empty() ? "?" : type) + " action=" +
        (action.empty() ? "?" : action) + " bytes=" +
        std::to_string(raw.size()));

    if (type == "settings" && action == "udpforward") {
        const std::string host = message.value("ip", std::string());
        const int videoPort = json_int(message, "videoport");
        const int audioPort = json_int(message, "audioport");
        log("native UDP assignment host=" +
            (host.empty() ? std::string("missing") : host) +
            " videoPort=" + std::to_string(videoPort) +
            " audioPort=" + std::to_string(audioPort));
        start_native_udp(host, videoPort, audioPort);
        return;
    }
    if (type == "settings" && action == "streamIds") {
        log("native stream dimensions=" +
            std::to_string(json_int(message, "width")) + "x" +
            std::to_string(json_int(message, "height")));
        return;
    }
    if (type == "stream" && action == "key") {
        const auto value = message.find("value");
        if (value != message.end() && value->is_string()) {
            {
                std::lock_guard<std::mutex> lock(nativeMutex_);
                nativeKeyHex_ = value->get<std::string>();
            }
            log("native stream key received bytes=" +
                std::to_string(value->get_ref<const std::string&>().size() / 2));
            send_control_json(nlohmann::json({
                {"type", "stream"}, {"action", "key"}, {"value", "ok"}}).dump());
        }
        return;
    }

    if (type == "stream" && action == "getstatus") {
        int streamWidth = 1280;
        int streamHeight = 720;
        stream_dimensions(streamResolution_, streamWidth, streamHeight);
        const int proto =
            transportPreference_ == TransportPreference::WebRtc ? 1 : 2;
        const bool fsr = upscalerPreference_ == UpscalerPreference::Fsr1;
        const int bitrateMax = stream_bitrate(streamResolution_, streamWidth,
                                              fsr);
        send_control_json(nlohmann::json({
            {"type", "stream"}, {"action", "status"}, {"value", "ok"},
            {"params", {
                {"type", "androidTV"}, {"ver", "v.2.5.10.tv"},
                {"gpu", "Tegra X1"}, {"proto", proto},
                {"codec", activeCodec_.load() == VideoCodec::H265 ? "h265"
                                                                  : "h264"},
                {"framerate_max", stream_fps()},
                {"bitrate_max", bitrateMax},
                {"hdr", false}}}}).dump());
        log("getstatus reply proto=" + std::to_string(proto) +
            " codec=" + (activeCodec_.load() == VideoCodec::H265 ? "h265" : "h264") +
            " fps=" + std::to_string(stream_fps()) +
            " bitrate_max=" + std::to_string(bitrateMax) +
            " fsr=" + (fsr ? "1" : "0"));
        return;
    }
    if (type == "controller" && action == "connected") {
        const int id = json_int(message, "id");
        if (id > 0) {
            controllerId_ = id;
            {
                std::lock_guard<std::mutex> lock(inputMutex_);
                padInitialized_ = false;
                inputLogged_ = false;
            }
            log("controller registered name=" +
                message.value("name", std::string("?")) +
                " id=" + std::to_string(id));
        } else {
            log("controller connected response without usable id");
        }
        return;
    }
    if (type == "controller" && action == "rumble") {
        std::lock_guard<std::mutex> lock(rumbleMutex_);
        rumble_.low = static_cast<uint16_t>(std::clamp(
            message.value("left", 0), 0, 65535));
        rumble_.high = static_cast<uint16_t>(std::clamp(
            message.value("right", 0), 0, 65535));
        rumble_.durationMs = 400;
        rumblePending_ = true;
    }
}

void Engine::send_control_json(const std::string& payload) {
    if (!control_.send_text(payload)) return;
    log("send control " + payload.substr(0, 200));
}

void Engine::send_input_json(nlohmann::json payload) {
    const uint32_t sequence = inputCommand_.fetch_add(1);
    if ((sequence + 1) % 20 == 0) {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        payload["time"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                              now).count();
    }

    // Native Android TV sends controller packets as plain JSON over the
    // control WSS. id_cmd/from_udp belong to the browser/WebRTC input path and
    // make this packet differ from the TV protocol.
    const bool native = nativeMediaStarted_.load();
    nlohmann::json controlPayload = payload;
    if (!native) {
        controlPayload["id_cmd"] = sequence;
        controlPayload["from_udp"] = false;
    }
    const bool sent = control_.send_text(controlPayload.dump());
    if (sent && !inputLogged_) {
        inputLogged_ = true;
        log("first input sent type=" +
            payload.value("type", std::string("?")) +
            " action=" + payload.value("action", std::string("?")) +
            " native=" + (native ? std::string("yes") : std::string("no")));
    }

    if (native) return;
    payload["id_cmd"] = sequence;
    payload["from_udp"] = true;
    const std::string channelPayload = payload.dump();
    std::lock_guard<std::mutex> lock(peerMutex_);
    if (peer_ && dataChannelOpened_ &&
        (peerState_ == PEER_CONNECTION_CONNECTED ||
         peerState_ == PEER_CONNECTION_COMPLETED)) {
        peer_connection_datachannel_send_text_sid(
            peer_, const_cast<char*>(channelPayload.data()),
            channelPayload.size(), 0);
    }
}

void Engine::virtual_mouse_move(float dx, float dy) {
    constexpr float kSensitivity = 1.8f;
    mouseX_ = std::clamp(mouseX_ + dx * kSensitivity / 1920.0f, 0.0f, 1.0f);
    mouseY_ = std::clamp(mouseY_ + dy * kSensitivity / 1080.0f, 0.0f, 1.0f);
    mouseVisible_ = true;
    dkVideo_.set_virtual_mouse_cursor(mouseX_, mouseY_, true);
    if (!control_.connected()) return;
    send_input_json({{"type", "mouse"}, {"action", "move"},
                     {"X", mouseX_}, {"Y", mouseY_},
                     {"offsetX", dx * kSensitivity},
                     {"offsetY", dy * kSensitivity},
                     {"isVisible", true}});
}

void Engine::virtual_mouse_click(int button) {
    if (button < 0 || !control_.connected()) return;
    if (mouseButtonDown_ >= 0) {
        send_input_json({{"type", "mouse"}, {"action", "button"},
                         {"btn", mouseButtonDown_}, {"isPressed", false}});
        mouseButtonDown_ = -1;
    }
    send_input_json({{"type", "mouse"}, {"action", "button"},
                     {"btn", button}, {"isPressed", true}});
    mouseButtonDown_ = button;
    mouseReleaseAt_ = SDL_GetTicks64() + 60;
    log("virtual mouse click button=" + std::to_string(button));
}

void Engine::virtual_mouse_hide() {
    if (!mouseVisible_) return;
    mouseVisible_ = false;
    dkVideo_.set_virtual_mouse_cursor(mouseX_, mouseY_, false);
    if (control_.connected()) {
        send_input_json({{"type", "mouse"}, {"action", "move"},
                         {"X", mouseX_}, {"Y", mouseY_},
                         {"offsetX", 0.0f}, {"offsetY", 0.0f},
                         {"isVisible", false}});
    }
}

void Engine::send_gamepad(HidAnalogStickState left,
                          HidAnalogStickState right, u64 buttons) {
    const int id = controllerId_.load();
    if (id <= 0 || !control_.connected()) return;
    std::lock_guard<std::mutex> lock(inputMutex_);

    auto sendButton = [&](u64 mask, int index) {
        const bool before = (previousButtons_ & mask) != 0;
        const bool after = (buttons & mask) != 0;
        if (!padInitialized_ || before != after) {
            send_input_json({{"type", "controller"}, {"action", "button"},
                             {"id", id}, {"button", index},
                             {"value", after ? 1 : 0}});
        }
    };
    if (xboxFaceLayout_.load()) {
        // Match the physical Xbox diamond: bottom=A, right=B, left=X, top=Y.
        sendButton(HidNpadButton_B, 0);
        sendButton(HidNpadButton_A, 1);
        sendButton(HidNpadButton_Y, 2);
        sendButton(HidNpadButton_X, 3);
    } else {
        // Nintendo labels are preserved: Switch A sends A, B sends B, etc.
        sendButton(HidNpadButton_A, 0);
        sendButton(HidNpadButton_B, 1);
        sendButton(HidNpadButton_X, 2);
        sendButton(HidNpadButton_Y, 3);
    }
    sendButton(HidNpadButton_L | HidNpadButton_LeftSL |
                   HidNpadButton_RightSL, 4);
    sendButton(HidNpadButton_R | HidNpadButton_LeftSR |
                   HidNpadButton_RightSR, 5);
    sendButton(HidNpadButton_Minus, 6);
    sendButton(HidNpadButton_Plus, 7);
    sendButton(HidNpadButton_StickL, 8);
    sendButton(HidNpadButton_StickR, 9);
    // Android can report digital trigger KeyEvents in addition to axes.
    sendButton(HidNpadButton_ZL, 10);
    sendButton(HidNpadButton_ZR, 11);

    auto sendAxis = [&](int index, int before, int after) {
        if (!padInitialized_ || axis_changed(before, after)) {
            send_input_json({{"type", "controller"}, {"action", "axes"},
                             {"id", id}, {"axes", index}, {"value", after}});
        }
    };
    const int leftX = controller_axis(left.x);
    const int leftY = controller_axis(-left.y);
    const int rightX = controller_axis(right.x);
    const int rightY = controller_axis(-right.y);
    sendAxis(0, controller_axis(previousLeft_.x), leftX);
    sendAxis(1, controller_axis(-previousLeft_.y), leftY);
    sendAxis(3, controller_axis(previousRight_.x), rightX);
    sendAxis(4, controller_axis(-previousRight_.y), rightY);
    const int oldZl = (previousButtons_ & HidNpadButton_ZL) ? 32767 : -32767;
    const int newZl = (buttons & HidNpadButton_ZL) ? 32767 : -32767;
    const int oldZr = (previousButtons_ & HidNpadButton_ZR) ? 32767 : -32767;
    const int newZr = (buttons & HidNpadButton_ZR) ? 32767 : -32767;
    sendAxis(2, oldZl, newZl);
    sendAxis(5, oldZr, newZr);

    int hat = 0;
    const bool up = buttons & HidNpadButton_Up;
    const bool down = buttons & HidNpadButton_Down;
    const bool leftPressed = buttons & HidNpadButton_Left;
    const bool rightPressed = buttons & HidNpadButton_Right;
    if (up && leftPressed) hat = 9;
    else if (up && rightPressed) hat = 3;
    else if (down && leftPressed) hat = 12;
    else if (down && rightPressed) hat = 6;
    else if (up) hat = 1;
    else if (rightPressed) hat = 2;
    else if (down) hat = 4;
    else if (leftPressed) hat = 8;

    int oldHat = 0;
    const bool oldUp = previousButtons_ & HidNpadButton_Up;
    const bool oldDown = previousButtons_ & HidNpadButton_Down;
    const bool oldLeft = previousButtons_ & HidNpadButton_Left;
    const bool oldRight = previousButtons_ & HidNpadButton_Right;
    if (oldUp && oldLeft) oldHat = 9;
    else if (oldUp && oldRight) oldHat = 3;
    else if (oldDown && oldLeft) oldHat = 12;
    else if (oldDown && oldRight) oldHat = 6;
    else if (oldUp) oldHat = 1;
    else if (oldRight) oldHat = 2;
    else if (oldDown) oldHat = 4;
    else if (oldLeft) oldHat = 8;
    if (!padInitialized_ || hat != oldHat) {
        send_input_json({{"type", "controller"}, {"action", "pad"},
                         {"id", id}, {"hat", hat}});
    }

    previousLeft_ = left;
    previousRight_ = right;
    previousButtons_ = buttons;
    padInitialized_ = true;
}

void Engine::on_video(uint8_t* data, size_t size, void* user) {
    auto* self = static_cast<Engine*>(user);
    self->lastMediaTicks_ = SDL_GetTicks64();
    self->videoBytes_ += size;
    if (!self->gotVideoPacket_.exchange(true)) {
        self->log("first video RTP packet bytes=" + std::to_string(size));
    }
    bool wantKeyframe = false;
    self->jitter_.receive(
        data, size, SDL_GetTicks64(),
        [self](const uint8_t* accessUnit, size_t bytes, uint32_t timestamp) {
            if (!self->gotAccessUnit_.exchange(true)) {
                self->log(std::string("first complete ") +
                          video_codec_label(self->activeCodec_.load()) +
                          " access unit bytes=" + std::to_string(bytes));
            }
            {
                std::lock_guard<std::mutex> lock(self->videoMutex_);
                if (self->videoQueue_.size() >= 64) self->videoQueue_.clear();
                VideoAccessUnit unit;
                unit.data.assign(accessUnit, accessUnit + bytes);
                unit.timestamp = timestamp;
                self->videoQueue_.push_back(std::move(unit));
            }
            self->videoCv_.notify_one();
        },
        [self](uint16_t pid, uint16_t blp) {
            if (self->peer_) peer_connection_send_nack(self->peer_, pid, blp);
        },
        &wantKeyframe);
    if (wantKeyframe) self->request_keyframe_locked();
}

void Engine::on_audio(uint8_t* data, size_t size, void* user) {
    auto* self = static_cast<Engine*>(user);
    self->lastMediaTicks_ = SDL_GetTicks64();
    if (!self->gotAudioPacket_.exchange(true)) {
        self->log("first audio RTP packet bytes=" + std::to_string(size));
    }
    if (size < 12) return;
    const uint8_t csrcCount = data[0] & 0x0fU;
    const bool extension = (data[0] & 0x10U) != 0;
    const bool padding = (data[0] & 0x20U) != 0;
    const uint16_t sequence =
        (static_cast<uint16_t>(data[2]) << 8) | data[3];
    size_t offset = 12 + static_cast<size_t>(csrcCount) * 4;
    if (extension) {
        if (offset + 4 > size) return;
        const uint16_t words =
            (static_cast<uint16_t>(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4 + static_cast<size_t>(words) * 4;
    }
    size_t end = size;
    if (padding && end > offset) {
        const uint8_t count = data[end - 1];
        if (count <= end - offset) end -= count;
    }
    if (offset <= end) self->audio_.submit(sequence, data + offset, end - offset);
}

void Engine::on_peer_state(PeerConnectionState state, void* user) {
    auto* self = static_cast<Engine*>(user);
    self->peerState_ = state;
    self->log(std::string("peer state=") +
              peer_connection_state_to_string(state));
}

void Engine::on_channel_open(void* user) {
    static_cast<Engine*>(user)->channelAssociationReady_ = true;
}

void Engine::on_channel_message(char* data, size_t size, void* user,
                                uint16_t) {
    static_cast<Engine*>(user)->handle_control_message(
        std::string(data, size));
}

void Engine::decode_loop() {
    while (!quit_) {
        VideoAccessUnit unit;
        int pendingCodec = -1;
        bool haveUnit = false;
        {
            std::unique_lock<std::mutex> lock(videoMutex_);
            videoCv_.wait_for(lock, std::chrono::milliseconds(20), [this] {
                return quit_.load() || !videoQueue_.empty() ||
                       pendingCodec_.load() >= 0;
            });
            if (quit_) break;
            pendingCodec = pendingCodec_.exchange(-1);
            if (pendingCodec < 0 && !videoQueue_.empty()) {
                unit = std::move(videoQueue_.front());
                videoQueue_.pop_front();
                haveUnit = true;
            }
        }
        if (pendingCodec >= 0) {
            // Codec fallback: video_ is owned by this thread while streaming,
            // so the worker hands the reopen over instead of touching it.
            const VideoCodec codec =
                pendingCodec == static_cast<int>(VideoCodec::H265)
                    ? VideoCodec::H265
                    : VideoCodec::H264;
            video_.shutdown();
            const bool ok = video_.init(renderer_, codec);
            log(std::string("decoder reopened codec=") +
                video_codec_label(codec) + (ok ? " ok" : " FAILED"));
            continue;
        }
        if (!haveUnit) continue;
        const bool decoded =
            video_.decode(unit.data.data(), unit.data.size());
        const bool decodeError = video_.take_error();
        if (decodeError || video_.waiting_keyframe()) {
            std::lock_guard<std::mutex> lock(peerMutex_);
            request_keyframe_locked();
        }
        if (!decoded) continue;
        {
            std::lock_guard<std::mutex> lock(frameMutex_);
            av_frame_unref(sharedFrame_);
            if (av_frame_ref(sharedFrame_, video_.current_frame()) == 0) {
                sharedFrameValid_ = true;
                ++sharedFrameSequence_;
            }
        }
        if (!gotFrame_.exchange(true)) {
            state_ = EngineState::Streaming;
            set_status(gnx::i18n::tr("Streaming activo", "Streaming active"));
            log("first decoded video frame");
        }
    }
}

bool Engine::begin_deko_output() {
    dkVideo_.set_logger([this](const char* value) {
        log(std::string("deko| ") + value);
    });
    const bool ready = dkVideo_.init();
    log(ready ? "deko output initialized" : "deko output initialization failed");
    return ready;
}

void Engine::end_deko_output() { dkVideo_.shutdown(); }

void Engine::pump_video() {
    if (mouseButtonDown_ >= 0 &&
        SDL_GetTicks64() >= mouseReleaseAt_) {
        send_input_json({{"type", "mouse"}, {"action", "button"},
                         {"btn", mouseButtonDown_}, {"isPressed", false}});
        mouseButtonDown_ = -1;
    }
    if (!dkVideo_.initialized() || !gotFrame_) return;
    constexpr double kDisplayHz = 59.94;
    const double interval =
        static_cast<double>(SDL_GetPerformanceFrequency()) / kDisplayHz;
    const double now = static_cast<double>(SDL_GetPerformanceCounter());
    if (now < nextPresentCounter_) return;

    AVFrame* frame = nullptr;
    uint64_t sequence = 0;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        if (sharedFrameValid_) {
            av_frame_unref(presentFrame_);
            if (av_frame_ref(presentFrame_, sharedFrame_) == 0) {
                frame = presentFrame_;
                sequence = sharedFrameSequence_;
            }
        }
    }
    if (frame) {
        dkVideo_.set_ping_ms(pingMs_.load());
        if (dkVideo_.render(frame)) {
            presentedSequence_ = sequence;
            if (!presentedFirstFrame_.exchange(true)) {
                log("first video frame presented");
            }
        }
    }
    nextPresentCounter_ = now + interval;
}

void Engine::request_keyframe_locked() {
    const uint64_t now = SDL_GetTicks64();
    const uint64_t last = lastKeyframeRequestMs_.load();
    if (last != 0 && now - last < 500) return;
    lastKeyframeRequestMs_.store(now);
    if (peer_) {
        peer_connection_request_keyframe(peer_);
        log("requested keyframe (webrtc pli)");
    } else if (nativeMediaStarted_.load()) {
        send_control_json(nlohmann::json({
            {"type", "stream"}, {"action", "keyframe"}}).dump());
        log("requested keyframe (native udp control)");
    }
}

void Engine::destroy_peer() {
    std::lock_guard<std::mutex> lock(peerMutex_);
    if (!peer_) return;
    peer_connection_close(peer_);
    peer_connection_destroy(peer_);
    peer_ = nullptr;
}

std::string Engine::status() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

std::string Engine::error() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return error_;
}

std::string Engine::gateway() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return gatewayHost_;
}

void Engine::set_status(const std::string& value) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = value;
}

void Engine::fail(const std::string& value) {
    log("FAIL " + value);
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        error_ = value;
        status_ = value;
    }
    state_ = EngineState::Failed;
}

bool Engine::take_rumble(RumbleCommand& out) {
    std::lock_guard<std::mutex> lock(rumbleMutex_);
    if (!rumblePending_) return false;
    out = rumble_;
    rumblePending_ = false;
    return true;
}

// Feed the performance HUD once per second: measured video bitrate, an
// estimate of packet loss (Reed-Solomon shards on Native UDP, NACKs on WebRTC)
// and the depth of the decode queue in milliseconds.
void Engine::update_net_stats(uint64_t nowMs) {
    const uint64_t bytes = videoBytes_.load();
    const int transport = activeTransport_.load();
    if (netStatsTicks_ == 0) {
        netStatsTicks_ = nowMs;
        netStatsBytes_ = bytes;
        netStatsNacks_ = jitter_.stats().nacks;
        netStatsPackets_ = jitter_.stats().packets;
        netStatsNativePackets_ = nativePackets_.load();
        netStatsNativeLost_ = nativeLostPackets_.load();
        return;
    }
    const uint64_t elapsed = nowMs - netStatsTicks_;
    if (elapsed < 1000) return;
    const float mbps = static_cast<float>(bytes - netStatsBytes_) * 8.0f /
                       (static_cast<float>(elapsed) / 1000.0f) / 1000000.0f;

    float loss = 0.0f;
    if (transport == 1) {  // Native UDP (proto: 2, Reed-Solomon FEC)
        const uint32_t pkts = nativePackets_.load() - netStatsNativePackets_;
        const uint32_t lost = nativeLostPackets_.load() - netStatsNativeLost_;
        loss = (pkts + lost > 0)
                   ? (100.0f * static_cast<float>(lost) /
                      static_cast<float>(pkts + lost))
                   : 0.0f;
    } else {  // WebRTC (RTP / NACK)
        const VideoJitterBuffer::Stats stats = jitter_.stats();
        const uint32_t packets = stats.packets - netStatsPackets_;
        const uint32_t nacks = stats.nacks - netStatsNacks_;
        loss = packets
                   ? 100.0f * static_cast<float>(nacks) /
                         static_cast<float>(packets)
                   : 0.0f;
        netStatsNacks_ = stats.nacks;
        netStatsPackets_ = stats.packets;
    }

    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(videoMutex_);
        queued = videoQueue_.size();
    }
    const int bufferMs = static_cast<int>(queued) * 1000 / stream_fps();
    dkVideo_.set_net_stats(mbps, loss, bufferMs);
    netStatsTicks_ = nowMs;
    netStatsBytes_ = bytes;
    netStatsNativePackets_ = nativePackets_.load();
    netStatsNativeLost_ = nativeLostPackets_.load();
}

void Engine::log(const std::string& line) {
    std::lock_guard<std::mutex> lock(logMutex_);
    if (!logFile_) return;
    std::fprintf(logFile_, "[%8llu] %s\n",
                 static_cast<unsigned long long>(SDL_GetTicks64()),
                 line.c_str());
}

}  // namespace gnx::stream
