#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "../core/boosteroid_api.hpp"
#include "../core/http.hpp"
#include "../core/i18n.hpp"
#include "../core/qrcodegen.hpp"
#include "json.hpp"
#include "covers.hpp"
#include "gfx.hpp"
#include "stream/engine.hpp"

// Render crisp QR Code via software surface to bypass HW rect limits
void draw_qr_code(SDL_Renderer* renderer, int startX, int startY, int boxSize, const std::string& text) {
    if (text.empty() || !renderer) return;

    static SDL_Texture* cached_qr = nullptr;
    static std::string cached_text = "";

    if (cached_text != text) {
        if (cached_qr) {
            SDL_DestroyTexture(cached_qr);
            cached_qr = nullptr;
        }
        cached_text = text;

        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::LOW);
            int modules = qr.getSize();
            int cellSize = boxSize / (modules + 4);

            SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, boxSize, boxSize, 32, SDL_PIXELFORMAT_RGBA32);
            if (surf) {
                SDL_FillRect(surf, NULL, SDL_MapRGBA(surf->format, 255, 255, 255, 255));
                int marginX = (boxSize - modules * cellSize) / 2;
                int marginY = (boxSize - modules * cellSize) / 2;
                Uint32 black = SDL_MapRGBA(surf->format, 12, 10, 24, 255);
                
                for (int y = 0; y < modules; ++y) {
                    for (int x = 0; x < modules; ++x) {
                        if (qr.getModule(x, y)) {
                            SDL_Rect r = {marginX + x * cellSize, marginY + y * cellSize, cellSize, cellSize};
                            SDL_FillRect(surf, &r, black);
                        }
                    }
                }
                cached_qr = SDL_CreateTextureFromSurface(renderer, surf);
                SDL_FreeSurface(surf);
            }
        } catch (...) {
        }
    }

    SDL_Rect dst = {startX, startY, boxSize, boxSize};
    if (cached_qr) {
        SDL_RenderCopy(renderer, cached_qr, NULL, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
        SDL_RenderFillRect(renderer, &dst);
    }
}

struct AsyncPollResult {
    ZERODROID::DeviceCodePollResult result{ZERODROID::DeviceCodePollResult::Error};
    std::string token;
};

struct AsyncLibraryResult {
    bool profileOk{false};
    bool gamesOk{false};
    ZERODROID::BoosteroidUser user;
    std::vector<ZERODROID::GameItem> games;
    std::string error;
};

struct AsyncServerConfigResult {
    bool ok{false};
    std::vector<ZERODROID::ServerLocation> locations;
    ZERODROID::ServerPreferences preferences;
    std::string error;
};

struct AsyncServerSaveResult {
    bool ok{false};
    std::string error;
};

struct ServerSettings {
    bool allowDistantRegions{true};
    int preferredLocationId{0};
    std::string preferredLocationLabel{"Automatico"};
    bool xboxFaceLayout{false};
    int language{0};  // 0 = Espanol, 1 = English
    // 0=auto, 1=720p, 2=720HQ, 3=1080p, 4=1080HQ, 5=540p (FSR source),
    // 6=480p (FSR source, handheld only).
    int resolution{0};
    // Settings -> Advanced. codec: 0=auto (H.265 then H.264), 1=H.265, 2=H.264.
    int codec{0};
    // fps: 0=auto (current default), 1=30, 2=60.
    int fps{0};
    // transport: 0=auto (Native UDP preferred), 1=Native UDP (proto: 2), 2=WebRTC (proto: 1).
    int transport{0};
    // upscaler: 0=off, 1=FSR 1 (EASU upscale + RCAS sharpening).
    int upscaler{1};
    // In-stream touchpad drives the virtual mouse. Off means touching the
    // screen outside the quick menu does nothing.
    bool mouse{true};
    std::vector<int> favoriteGameIds;
    std::vector<int> recentGameIds;
};

enum class LibraryTab {
    All = 0,
    Favorites = 1,
    Recent = 2,
};

ServerSettings load_server_settings() {
    ServerSettings settings;
    std::ifstream file("sdmc:/switch/ZERODROID/settings.json");
    if (!file.is_open()) return settings;
    try {
        nlohmann::json value;
        file >> value;
        settings.allowDistantRegions =
            value.value("allowDistantRegions", true);
        settings.preferredLocationId =
            value.value("preferredLocationId", 0);
        settings.preferredLocationLabel = value.value(
            "preferredLocationLabel", std::string("Automatico"));
        settings.xboxFaceLayout = value.value("xboxFaceLayout", false);
        settings.language = std::clamp(value.value("language", 0), 0, 1);
        settings.resolution = std::clamp(value.value("resolution", 0), 0, 6);
        settings.codec = std::clamp(value.value("codec", 0), 0, 2);
        settings.fps = std::clamp(value.value("fps", 0), 0, 2);
        settings.transport = std::clamp(value.value("transport", 0), 0, 2);
        settings.upscaler = std::clamp(value.value("upscaler", 1), 0, 1);
        settings.mouse = value.value("mouse", true);
        settings.favoriteGameIds = value.value(
            "favoriteGameIds", std::vector<int>{});
        settings.recentGameIds = value.value(
            "recentGameIds", std::vector<int>{});
    } catch (...) {
    }
    return settings;
}

bool save_server_settings(const ServerSettings& settings) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/ZERODROID", 0777);
    std::ofstream file("sdmc:/switch/ZERODROID/settings.json");
    if (!file.is_open()) return false;
    file << nlohmann::json({
        {"version", 6},
        {"allowDistantRegions", settings.allowDistantRegions},
        {"preferredLocationId", settings.preferredLocationId},
        {"preferredLocationLabel", settings.preferredLocationLabel},
        {"xboxFaceLayout", settings.xboxFaceLayout},
        {"language", settings.language},
        {"resolution", settings.resolution},
        {"codec", settings.codec},
        {"fps", settings.fps},
        {"transport", settings.transport},
        {"upscaler", settings.upscaler},
        {"mouse", settings.mouse},
        {"favoriteGameIds", settings.favoriteGameIds},
        {"recentGameIds", settings.recentGameIds},
    }).dump(2);
    return file.good();
}

bool contains_game_id(const std::vector<int>& values, int gameId) {
    return std::find(values.begin(), values.end(), gameId) != values.end();
}

void toggle_game_id(std::vector<int>& values, int gameId) {
    const auto found = std::find(values.begin(), values.end(), gameId);
    if (found == values.end()) {
        values.push_back(gameId);
    } else {
        values.erase(found);
    }
}

void remember_recent_game(std::vector<int>& values, int gameId) {
    values.erase(std::remove(values.begin(), values.end(), gameId), values.end());
    values.insert(values.begin(), gameId);
    constexpr std::size_t kRecentLimit = 30;
    if (values.size() > kRecentLimit) values.resize(kRecentLimit);
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

std::string open_search_keyboard(const std::string& initial, int language) {
    SwkbdConfig keyboard{};
    char buffer[256]{};
    if (R_FAILED(swkbdCreate(&keyboard, 0))) return initial;
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetGuideText(
        &keyboard, language == 1 ? "Search your games" : "Buscar juegos");
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
    const Result result = swkbdShow(&keyboard, buffer, sizeof(buffer));
    swkbdClose(&keyboard);
    return R_SUCCEEDED(result) ? std::string(buffer) : initial;
}

std::string fit_text(
    gnx::gfx::Gfx& gfx,
    const std::string& text,
    int maxWidth,
    gnx::gfx::FontSize size = gnx::gfx::FontSize::Small) {
    if (gfx.text_width(text, size) <= maxWidth) return text;

    std::string shortened = text;
    constexpr const char* suffix = "...";
    while (!shortened.empty() &&
           gfx.text_width(shortened + suffix, size) > maxWidth) {
        std::size_t previous = shortened.size() - 1;
        while (previous > 0 &&
               (static_cast<unsigned char>(shortened[previous]) & 0xc0U) == 0x80U) {
            --previous;
        }
        shortened.erase(previous);
    }
    return shortened + suffix;
}

bool point_in_rect(int x, int y, const gnx::stream::QuickRect& rect) {
    return x >= rect.x && x < rect.x + rect.w &&
           y >= rect.y && y < rect.y + rect.h;
}

constexpr uint64_t kVirtualMouseLongPressMs = 550;
constexpr uint64_t kVirtualMouseHideMs = 10000;
constexpr int kVirtualMouseMoveThreshold = 24;

// Left-stick navigation for the library/settings UI. A fresh direction fires
// immediately, then repeats after a short delay so menus feel like the D-pad.
struct AnalogNavigation {
    u64 directions = 0;
    uint64_t repeatAt = 0;

    u64 update(HidAnalogStickState stick) {
        constexpr int kThreshold = 16000;
        constexpr uint64_t kRepeatDelayMs = 350;
        constexpr uint64_t kRepeatEveryMs = 90;

        u64 next = 0;
        if (stick.y > kThreshold) next |= HidNpadButton_Up;
        if (stick.y < -kThreshold) next |= HidNpadButton_Down;
        if (stick.x < -kThreshold) next |= HidNpadButton_Left;
        if (stick.x > kThreshold) next |= HidNpadButton_Right;

        const uint64_t now = SDL_GetTicks64();
        u64 pressed = 0;
        if (next != directions) {
            pressed = next;
            directions = next;
            repeatAt = now + kRepeatDelayMs;
        } else if (next != 0 && now >= repeatAt) {
            pressed = next;
            repeatAt = now + kRepeatEveryMs;
        }
        return pressed;
    }
};

constexpr int kSettingsCardX = 480;
constexpr int kSettingsCardW = 960;
constexpr int kSettingsCardH = 170;
constexpr int kSettingsCardGap = 28;
constexpr int kSettingsViewTop = 205;
constexpr int kSettingsViewBottom = 890;

// Settings cards. The Advanced card expands in place and reveals the codec and
// frame-rate selectors, so the navigable list is rebuilt whenever it toggles.
enum class SettingsRowId {
    Regions,
    Controller,
    Location,
    Resolution,
    Advanced,
    Codec,
    Fps,
    Transport,
    Upscaler,
    Language,
    Logout,
};

std::vector<SettingsRowId> settings_rows(bool advancedOpen) {
    std::vector<SettingsRowId> rows = {
        SettingsRowId::Regions,    SettingsRowId::Controller,
        SettingsRowId::Location,   SettingsRowId::Resolution,
        SettingsRowId::Advanced,
    };
    if (advancedOpen) {
        rows.push_back(SettingsRowId::Codec);
        rows.push_back(SettingsRowId::Fps);
        rows.push_back(SettingsRowId::Transport);
        rows.push_back(SettingsRowId::Upscaler);
    }
    rows.push_back(SettingsRowId::Language);
    rows.push_back(SettingsRowId::Logout);
    return rows;
}


// Touch controller for the in-stream two-dot menu. Returns true when the tap
// belongs to the panel; a tap outside closes it.
bool handle_stream_settings_touch(gnx::stream::QuickMenuState& state,
                                  int x, int y, bool& layoutChanged,
                                  bool& mouseChanged, bool& leaveRequested) {
    layoutChanged = false;
    mouseChanged = false;
    leaveRequested = false;
    if (point_in_rect(x, y, gnx::stream::kQuickToggleRect)) {
        state.open = !state.open;
        return true;
    }
    if (!state.open) return false;
    if (!point_in_rect(x, y, gnx::stream::kQuickPanelRect)) {
        state.open = false;
        return false;
    }

    bool changed = false;
    if (point_in_rect(x, y,
                      gnx::stream::quick_row_rect(
                          gnx::stream::QuickPerformance))) {
        state.performance = !state.performance;
        changed = true;
    } else if (point_in_rect(
                   x, y,
                   gnx::stream::quick_row_rect(gnx::stream::QuickPing))) {
        state.ping = !state.ping;
        changed = true;
    } else if (point_in_rect(
                   x, y, gnx::stream::quick_row_rect(
                             gnx::stream::QuickController))) {
        state.xboxFaceLayout = !state.xboxFaceLayout;
        layoutChanged = true;
        changed = true;
    } else if (point_in_rect(
                   x, y,
                   gnx::stream::quick_row_rect(gnx::stream::QuickMouse))) {
        state.mouse = !state.mouse;
        mouseChanged = true;
        changed = true;
    } else if (point_in_rect(x, y, gnx::stream::kQuickResetRect)) {
        state.brightness = 0;
        state.contrast = 100;
        state.saturation = 100;
        state.gamma = 100;
        state.sharpness = 0;
        changed = true;
    } else if (point_in_rect(x, y, gnx::stream::kQuickLeaveRect)) {
        state.open = false;
        leaveRequested = true;
        changed = true;
    } else {
        for (int row = gnx::stream::QuickBrightness;
             row <= gnx::stream::QuickSharpness; ++row) {
            int direction = 0;
            if (point_in_rect(x, y, gnx::stream::quick_minus_rect(row))) {
                direction = -1;
            } else if (point_in_rect(
                           x, y, gnx::stream::quick_plus_rect(row))) {
                direction = 1;
            }
            if (direction == 0) continue;
            if (row == gnx::stream::QuickBrightness) {
                state.brightness += direction * 5;
            } else if (row == gnx::stream::QuickContrast) {
                state.contrast += direction * 10;
            } else if (row == gnx::stream::QuickSaturation) {
                state.saturation += direction * 10;
            } else if (row == gnx::stream::QuickGamma) {
                state.gamma += direction * 5;
            } else {
                state.sharpness = (state.sharpness + direction + 4) % 4;
            }
            changed = true;
            break;
        }
    }
    if (changed) state = gnx::stream::normalized_quick_menu(state);
    return true;
}

void draw_cover_texture(
    SDL_Renderer* renderer,
    SDL_Texture* texture,
    const SDL_Rect& destination) {
    if (!renderer || !texture) return;

    int width = 0;
    int height = 0;
    if (SDL_QueryTexture(texture, nullptr, nullptr, &width, &height) != 0 ||
        width <= 0 || height <= 0) {
        return;
    }

    SDL_Rect source{0, 0, width, height};
    const long long sourceRatio = static_cast<long long>(width) * destination.h;
    const long long targetRatio = static_cast<long long>(height) * destination.w;
    if (sourceRatio > targetRatio) {
        source.w = std::max(1, height * destination.w / destination.h);
        source.x = (width - source.w) / 2;
    } else if (sourceRatio < targetRatio) {
        source.h = std::max(1, width * destination.h / destination.w);
        source.y = (height - source.h) / 2;
    }
    SDL_RenderCopy(renderer, texture, &source, &destination);
}

void initialize_log() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/ZERODROID", 0777);
    if (std::freopen("sdmc:/switch/ZERODROID/zerodroid.log", "w", stderr)) {
        setvbuf(stderr, nullptr, _IOLBF, 0);
    }
}

// ZERODROID-Switch Main Engine with Direct 2D QR Rendering
int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    initialize_log();
    std::fprintf(stderr, "ZERODROID v%s starting\n", ZERODROID_VERSION);

    // 1. Initialize Nintendo Switch Network Sockets
    const bool socketsReady = R_SUCCEEDED(socketInitializeDefault());
    if (!socketsReady) {
        std::fprintf(stderr, "Socket init failed\n");
    } else {
        std::fprintf(stderr, "Network sockets ready\n");
    }

    // Initialize PL service for shared fonts (REQUIRED for text rendering)
    const bool plReady = R_SUCCEEDED(plInitialize(PlServiceType_User));
    if (!plReady) {
        std::fprintf(stderr, "PL service init failed\n");
    }

    // The QR flow is HTTPS. Switch has no system CA store, so use the bundle
    // shipped in this NRO's romfs.
    const bool romfsReady = R_SUCCEEDED(romfsInit());
    if (romfsReady) {
        gnx::Http::set_ca_bundle("romfs:/cacert.pem");
        std::fprintf(stderr, "romfs and TLS CA bundle ready\n");
    } else {
        std::fprintf(stderr, "romfs init failed; HTTPS will not work\n");
    }

    // 2. Initialize Graphical Subsystem (SDL2 2D Engine)
    gnx::gfx::Gfx gfx;
    if (!gfx.init()) {
        std::fprintf(stderr, "GFX Init failed!\n");
        if (romfsReady) romfsExit();
        if (plReady) plExit();
        if (socketsReady) socketExit();
        return EXIT_FAILURE;
    }
    auto covers = std::make_unique<gnx::Covers>(
        gfx, "sdmc:/switch/ZERODROID/covers");

    PadState pad;
    padInitializeDefault(&pad);
    AnalogNavigation analogNavigation;
    hidInitializeTouchScreen();

    // 3. Initialize ZERODROID Boosteroid Engine
    ZERODROID::BoosteroidAPI api;
    ServerSettings serverSettings = load_server_settings();
    gnx::i18n::set_english(serverSettings.language == 1);
    auto ui = [](const char* spanish, const char* english) {
        return gnx::i18n::tr(spanish, english);
    };
    gnx::stream::QuickMenuState quickMenuState;
    quickMenuState.xboxFaceLayout = serverSettings.xboxFaceLayout;
    quickMenuState.mouse = serverSettings.mouse;
    auto stream = std::make_unique<gnx::stream::Engine>(api, gfx.renderer());
    stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
    stream->set_quick_menu_state(quickMenuState);
    stream->set_stream_resolution(serverSettings.resolution);
    stream->set_codec_preference(
        serverSettings.codec == 1
            ? gnx::stream::CodecPreference::H265
            : serverSettings.codec == 2
                  ? gnx::stream::CodecPreference::H264
                  : gnx::stream::CodecPreference::Auto);
    stream->set_fps_mode(serverSettings.fps);
    stream->set_transport_preference(
        serverSettings.transport == 1
            ? gnx::stream::TransportPreference::NativeUdp
            : serverSettings.transport == 2
                  ? gnx::stream::TransportPreference::WebRtc
                  : gnx::stream::TransportPreference::Auto);
    stream->set_upscaler_preference(
        serverSettings.upscaler == 1
            ? gnx::stream::UpscalerPreference::Fsr1
            : gnx::stream::UpscalerPreference::Off);
    ZERODROID::DeviceCodeAuthArtifact authArtifact;

    bool hasSession = api.loadSessionFromSD();
    std::string loginStatus = ui("Preparando inicio de sesion...",
                                 "Preparing sign in...");
    if (!hasSession) {
        if (api.requestDeviceCode(authArtifact)) {
            loginStatus = ui("Esperando autorizacion desde el movil...",
                             "Waiting for authorization from your phone...");
            std::fprintf(stderr, "QR UUID generated\n");
        } else {
            loginStatus = ui("No se pudo crear el codigo QR.",
                             "Could not create the QR code.");
        }
    }

    using SteadyClock = std::chrono::steady_clock;
    auto qrCreatedAt = SteadyClock::now();
    auto nextPollAt = SteadyClock::now();
    std::future<AsyncPollResult> pollFuture;
    bool pollInFlight = false;

    std::future<AsyncLibraryResult> libraryFuture;
    bool libraryInFlight = false;
    bool libraryReady = false;
    bool libraryOk = false;
    ZERODROID::BoosteroidUser libraryUser;
    std::vector<ZERODROID::GameItem> libraryGames;
    std::vector<std::size_t> visibleGames;
    std::string libraryError;
    std::string libraryNotice;
    std::string searchQuery;
    LibraryTab libraryTab = LibraryTab::All;
    std::size_t selectedGame = 0;
    bool settingsOpen = false;
    bool serverConfigReady = false;
    bool serverConfigInFlight = false;
    std::future<AsyncServerConfigResult> serverConfigFuture;
    std::vector<ZERODROID::ServerLocation> serverLocations;
    std::string serverConfigError;
    std::size_t selectedLocation = 0;  // 0 = automatic
    int selectedSettingsRow = 0;       // index into settings_rows()
    bool settingsAdvancedOpen = false; // Advanced card expanded (codec/fps)
    int settingsScroll = 0;
    bool serverSaveInFlight = false;
    std::future<AsyncServerSaveResult> serverSaveFuture;
    bool launchPanel = false;
    bool dekoStreaming = false;
    bool touchWasDown = false;
    bool touchOnUi = false;
    bool touchMoved = false;
    int touchDownX = 0;
    int touchDownY = 0;
    int touchLastX = 0;
    int touchLastY = 0;
    uint64_t touchDownAt = 0;
    uint64_t lastTouchAt = SDL_GetTicks64();
    std::string launchingTitle;
    int launchingGameId = 0;

    auto rebuildVisibleGames = [&]() {
        visibleGames.clear();
        const std::string normalizedQuery = lowercase_ascii(searchQuery);
        const auto matchesSearch = [&](const ZERODROID::GameItem& game) {
            return normalizedQuery.empty() ||
                   lowercase_ascii(game.title).find(normalizedQuery) !=
                       std::string::npos;
        };
        if (libraryTab == LibraryTab::Recent) {
            for (int gameId : serverSettings.recentGameIds) {
                const auto found = std::find_if(
                    libraryGames.begin(), libraryGames.end(),
                    [gameId](const ZERODROID::GameItem& game) {
                        return game.id == gameId;
                    });
                if (found != libraryGames.end() && matchesSearch(*found)) {
                    visibleGames.push_back(static_cast<std::size_t>(
                        std::distance(libraryGames.begin(), found)));
                }
            }
        } else {
            for (std::size_t index = 0; index < libraryGames.size(); ++index) {
                const auto& game = libraryGames[index];
                if (libraryTab == LibraryTab::Favorites &&
                    !contains_game_id(serverSettings.favoriteGameIds, game.id)) {
                    continue;
                }
                if (matchesSearch(game)) visibleGames.push_back(index);
            }
        }
        selectedGame = visibleGames.empty()
            ? 0
            : std::min(selectedGame, visibleGames.size() - 1);
    };

    auto startServerConfigLoad = [&]() {
        if (!hasSession || serverConfigInFlight || serverSaveInFlight) return;
        serverConfigInFlight = true;
        serverConfigReady = false;
        serverConfigError.clear();
        serverConfigFuture = std::async(std::launch::async, [&api]() {
            AsyncServerConfigResult result;
            result.ok = api.getServerConfiguration(
                result.locations, result.preferences);
            if (!result.ok) result.error = api.lastError();
            return result;
        });
    };

    auto startServerSave = [&]() {
        if (!serverConfigReady || serverConfigInFlight || serverSaveInFlight) {
            return;
        }
        const ZERODROID::ServerPreferences preferences{
            serverSettings.allowDistantRegions,
            serverSettings.preferredLocationId};
        serverSaveInFlight = true;
        serverConfigError.clear();
        serverSaveFuture = std::async(
            std::launch::async, [&api, preferences]() {
                AsyncServerSaveResult result;
                result.ok = api.updateServerConfiguration(preferences);
                if (!result.ok) result.error = api.lastError();
                return result;
            });
    };

    auto startLibraryLoad = [&]() {
        if (!hasSession || libraryInFlight) return;
        libraryInFlight = true;
        libraryReady = false;
        libraryOk = false;
        libraryError.clear();
        libraryNotice.clear();
        std::fprintf(stderr, "Starting Boosteroid library load\n");
        libraryFuture = std::async(std::launch::async, [&api]() {
            AsyncLibraryResult result;
            result.profileOk = api.getUserProfile(result.user);
            const std::string profileError = api.lastError();
            result.gamesOk = api.getInstalledGames(result.games);
            if (!result.gamesOk) result.error = api.lastError();
            if (result.error.empty() && !result.profileOk) result.error = profileError;
            return result;
        });
    };

    auto createNewQr = [&]() {
        if (pollInFlight) return;
        if (api.requestDeviceCode(authArtifact)) {
            qrCreatedAt = SteadyClock::now();
            nextPollAt = qrCreatedAt;
            loginStatus = ui("Esperando autorizacion desde el movil...",
                             "Waiting for authorization from your phone...");
            std::fprintf(stderr, "New QR UUID generated\n");
        } else {
            loginStatus = ui("No se pudo crear el codigo QR.",
                             "Could not create the QR code.");
        }
    };

    auto logout = [&]() {
        if (libraryInFlight || serverConfigInFlight || serverSaveInFlight) {
            libraryNotice = ui(
                "Espera a que terminen las consultas para cerrar la sesion.",
                "Wait for the pending requests before signing out.");
            return false;
        }
        api.logout();
        hasSession = false;
        libraryReady = false;
        libraryOk = false;
        libraryGames.clear();
        visibleGames.clear();
        libraryUser = ZERODROID::BoosteroidUser();
        libraryError.clear();
        libraryNotice.clear();
        searchQuery.clear();
        libraryTab = LibraryTab::All;
        selectedGame = 0;
        settingsOpen = false;
        covers->drop_textures();
        createNewQr();
        return true;
    };

    if (hasSession) startLibraryLoad();

    // 4. Main SDL2 Graphical Render Loop
    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 kDown = padGetButtonsDown(&pad);
        const u64 kHeld = padGetButtons(&pad);
        const u64 navDown = kDown |
            (dekoStreaming ? 0 : analogNavigation.update(padGetStickPos(&pad, 0)));

        if (dekoStreaming) {
            bool leaveFromMenu = false;
            HidTouchScreenState touchState{};
            const bool touching =
                hidGetTouchScreenStates(&touchState, 1) > 0 &&
                touchState.count > 0;
            if (touching) {
                lastTouchAt = SDL_GetTicks64();
                const int x = touchState.touches[0].x * gnx::gfx::kWidth / 1280;
                const int y = touchState.touches[0].y * gnx::gfx::kHeight / 720;
                if (!touchWasDown) {
                    touchDownX = x;
                    touchDownY = y;
                    touchLastX = x;
                    touchLastY = y;
                    touchDownAt = SDL_GetTicks64();
                    touchMoved = false;
                    // While the quick panel is open, every touch is UI input.
                    // The two-dot toggle is UI even when the panel is closed.
                    touchOnUi =
                        quickMenuState.open ||
                        point_in_rect(x, y, gnx::stream::kQuickToggleRect);
                    if (!touchOnUi && quickMenuState.mouse) {
                        // First touchpad contact: show the cursor at centre.
                        stream->virtual_mouse_move(0.0f, 0.0f);
                    }
                } else {
                    const int moveX = x - touchLastX;
                    const int moveY = y - touchLastY;
                    if (!touchOnUi && quickMenuState.mouse &&
                        (moveX != 0 || moveY != 0)) {
                        if (std::abs(x - touchDownX) >
                                kVirtualMouseMoveThreshold ||
                            std::abs(y - touchDownY) >
                                kVirtualMouseMoveThreshold) {
                            touchMoved = true;
                        }
                        stream->virtual_mouse_move(static_cast<float>(moveX),
                                                   static_cast<float>(moveY));
                    }
                    touchLastX = x;
                    touchLastY = y;
                }
                touchWasDown = true;
            } else if (touchWasDown) {
                touchWasDown = false;
                const int dx = std::abs(touchLastX - touchDownX);
                const int dy = std::abs(touchLastY - touchDownY);
                if (touchOnUi) {
                    if (dx < 100 && dy < 100) {
                        const bool wasOpen = quickMenuState.open;
                        bool layoutChanged = false;
                        bool mouseChanged = false;
                        const bool quickTouch =
                            handle_stream_settings_touch(
                                quickMenuState, touchLastX, touchLastY,
                                layoutChanged, mouseChanged, leaveFromMenu);
                        if (quickTouch || wasOpen != quickMenuState.open) {
                            stream->set_quick_menu_state(quickMenuState);
                        }
                        if (layoutChanged) {
                            serverSettings.xboxFaceLayout =
                                quickMenuState.xboxFaceLayout;
                            stream->set_xbox_face_layout(
                                serverSettings.xboxFaceLayout);
                            save_server_settings(serverSettings);
                        }
                        if (mouseChanged) {
                            serverSettings.mouse = quickMenuState.mouse;
                            save_server_settings(serverSettings);
                            if (!quickMenuState.mouse) {
                                // Drop a cursor that may already be on screen.
                                stream->virtual_mouse_hide();
                            }
                        }
                    }
                } else if (quickMenuState.mouse && !touchMoved &&
                           dx < kVirtualMouseMoveThreshold &&
                           dy < kVirtualMouseMoveThreshold) {
                    const uint64_t heldMs =
                        SDL_GetTicks64() - touchDownAt;
                    stream->virtual_mouse_click(
                        heldMs >= kVirtualMouseLongPressMs ? 2 : 0);
                }
            }

            if (quickMenuState.mouse &&
                SDL_GetTicks64() - lastTouchAt >= kVirtualMouseHideMs) {
                stream->virtual_mouse_hide();
            }

            const u64 exitMask =
                HidNpadButton_L | HidNpadButton_R | HidNpadButton_Minus;
            const bool leaveStream =
                leaveFromMenu || (kHeld & exitMask) == exitMask;
            stream->send_gamepad(
                padGetStickPos(&pad, 0), padGetStickPos(&pad, 1), kHeld);
            stream->pump_video();

            const auto streamState = stream->state();
            if (leaveStream || streamState == gnx::stream::EngineState::Failed ||
                streamState == gnx::stream::EngineState::Stopped) {
                const std::string streamError = stream->error();
                stream->end_deko_output();
                stream->stop();
                dekoStreaming = false;
                touchWasDown = false;
                quickMenuState.open = false;
                if (!gfx.resume()) break;
                stream = std::make_unique<gnx::stream::Engine>(api, gfx.renderer());
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                covers = std::make_unique<gnx::Covers>(
                    gfx, "sdmc:/switch/ZERODROID/covers");
                libraryNotice = streamError.empty()
                    ? "Streaming cerrado."
                    : "Streaming termino: " + streamError;
            }
            continue;
        }

        if (kDown & HidNpadButton_Plus) {
            break; // Exit app cleanly
        }

        const auto now = SteadyClock::now();

        if (serverConfigInFlight &&
            serverConfigFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncServerConfigResult completed = serverConfigFuture.get();
                serverLocations = std::move(completed.locations);
                serverConfigError = std::move(completed.error);
                serverConfigReady = completed.ok;
                selectedLocation = 0;
                if (completed.ok) {
                    serverSettings.allowDistantRegions =
                        completed.preferences.allowDistantRegions;
                    serverSettings.preferredLocationId =
                        completed.preferences.preferredLocationId;
                    serverSettings.preferredLocationLabel = "Automatico";
                    for (std::size_t i = 0; i < serverLocations.size(); ++i) {
                        if (serverLocations[i].id ==
                            serverSettings.preferredLocationId) {
                            selectedLocation = i + 1;
                            serverSettings.preferredLocationLabel =
                                serverLocations[i].title;
                            break;
                        }
                    }
                    save_server_settings(serverSettings);
                }
            } catch (const std::exception& error) {
                serverConfigReady = false;
                serverConfigError = error.what();
            }
            serverConfigInFlight = false;
        }

        if (serverSaveInFlight &&
            serverSaveFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncServerSaveResult completed = serverSaveFuture.get();
                if (completed.ok) {
                    save_server_settings(serverSettings);
                    libraryNotice = ui(
                        "Configuracion de servidor guardada en Boosteroid.",
                        "Server settings saved to Boosteroid.");
                    serverConfigError.clear();
                } else {
                    serverConfigError = completed.error;
                    libraryNotice = ui("No se pudo guardar: ",
                                       "Could not save: ") + completed.error;
                }
            } catch (const std::exception& error) {
                serverConfigError = error.what();
            }
            serverSaveInFlight = false;
        }

        if (launchPanel &&
            stream->state() == gnx::stream::EngineState::Streaming) {
            if (launchingGameId != 0) {
                remember_recent_game(serverSettings.recentGameIds,
                                     launchingGameId);
                save_server_settings(serverSettings);
                rebuildVisibleGames();
                launchingGameId = 0;
            }
            covers->drop_textures();
            gfx.suspend();
            if (!stream->begin_deko_output()) {
                stream->stop();
                if (!gfx.resume()) break;
                stream = std::make_unique<gnx::stream::Engine>(api, gfx.renderer());
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                covers = std::make_unique<gnx::Covers>(
                    gfx, "sdmc:/switch/ZERODROID/covers");
                libraryNotice = ui(
                    "No se pudo iniciar la salida de video de Switch.",
                    "Could not start the Switch video output.");
                launchPanel = false;
            } else {
                launchPanel = false;
                dekoStreaming = true;
                continue;
            }
        }
        if (!hasSession && pollInFlight &&
            pollFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            const AsyncPollResult completed = pollFuture.get();
            pollInFlight = false;
            switch (completed.result) {
                case ZERODROID::DeviceCodePollResult::Succeeded:
                    hasSession = true;
                    loginStatus = ui("Sesion vinculada correctamente.",
                                     "Account linked successfully.");
                    std::fprintf(stderr, "QR login succeeded and session was saved\n");
                    startLibraryLoad();
                    break;
                case ZERODROID::DeviceCodePollResult::Expired:
                    std::fprintf(stderr, "QR expired; generating a replacement\n");
                    createNewQr();
                    break;
                case ZERODROID::DeviceCodePollResult::Error:
                    loginStatus = ui("Error de red. Reintentando...",
                                     "Network error. Retrying...");
                    std::fprintf(stderr, "QR poll error: %s\n", api.lastError().c_str());
                    nextPollAt = now + std::chrono::seconds(authArtifact.intervalSeconds);
                    break;
                case ZERODROID::DeviceCodePollResult::Pending:
                    loginStatus = ui(
                        "Esperando autorizacion desde el movil...",
                        "Waiting for authorization from your phone...");
                    nextPollAt = now + std::chrono::seconds(authArtifact.intervalSeconds);
                    break;
            }
        }

        if (!hasSession && !pollInFlight && !authArtifact.deviceCode.empty() && now >= nextPollAt) {
            const std::string code = authArtifact.deviceCode;
            pollFuture = std::async(std::launch::async, [&api, code]() {
                AsyncPollResult result;
                result.result = api.pollDeviceCodeStatus(code, result.token);
                return result;
            });
            pollInFlight = true;
        }

        if (!hasSession && !pollInFlight &&
            now - qrCreatedAt >= std::chrono::seconds(authArtifact.expiresInSeconds)) {
            createNewQr();
        }

        if (hasSession && libraryInFlight &&
            libraryFuture.wait_for(std::chrono::milliseconds(0)) ==
                std::future_status::ready) {
            try {
                AsyncLibraryResult completed = libraryFuture.get();
                libraryGames = std::move(completed.games);
                libraryUser = std::move(completed.user);
                libraryOk = completed.gamesOk;
                libraryError = std::move(completed.error);
                if (!libraryOk && libraryError.empty()) {
                    libraryError = ui(
                        "No se pudo cargar la biblioteca de Boosteroid.",
                        "Could not load the Boosteroid library.");
                }
                selectedGame = 0;
                rebuildVisibleGames();
                std::fprintf(stderr, "Library load finished: ok=%d games=%zu error=%s\n",
                             libraryOk ? 1 : 0, libraryGames.size(),
                             libraryError.c_str());
            } catch (const std::exception& error) {
                libraryOk = false;
                libraryError = error.what();
                std::fprintf(stderr, "Library worker exception: %s\n", error.what());
            } catch (...) {
                libraryOk = false;
                libraryError = ui(
                    "Fallo desconocido al cargar la biblioteca.",
                    "Unknown error while loading the library.");
                std::fprintf(stderr, "Unknown library worker exception\n");
            }
            libraryInFlight = false;
            libraryReady = true;
        }

        if (!hasSession && (kDown & HidNpadButton_Y)) {
            createNewQr();
        }

        if (hasSession && !settingsOpen && !launchPanel &&
            libraryReady && !libraryOk && !libraryInFlight &&
            (kDown & HidNpadButton_Y)) {
            startLibraryLoad();
        }

        if (launchPanel && (kDown & HidNpadButton_B)) {
            const std::string reason = stream->error();
            stream->stop();
            launchPanel = false;
            launchingGameId = 0;
            libraryNotice = reason.empty()
                ? ui("Inicio cancelado.", "Launch canceled.")
                : ui("No se pudo iniciar: ", "Could not start: ") + reason;
        }

        if (settingsOpen) {
            const std::size_t locationCount = serverLocations.size() + 1;
            const std::vector<SettingsRowId> rows =
                settings_rows(settingsAdvancedOpen);
            const int rowCount = static_cast<int>(rows.size());
            if (navDown & HidNpadButton_Up) {
                selectedSettingsRow = std::max(0, selectedSettingsRow - 1);
            }
            if (navDown & HidNpadButton_Down) {
                selectedSettingsRow =
                    std::min(rowCount - 1, selectedSettingsRow + 1);
            }
            selectedSettingsRow =
                std::clamp(selectedSettingsRow, 0, rowCount - 1);
            {
                const int stride = kSettingsCardH + kSettingsCardGap;
                const int contentH = rowCount * kSettingsCardH +
                                     (rowCount - 1) * kSettingsCardGap;
                const int viewH = kSettingsViewBottom - kSettingsViewTop;
                const int maxScroll = contentH > viewH ? contentH - viewH : 0;
                const int rowTop = selectedSettingsRow * stride;
                const int rowBottom = rowTop + kSettingsCardH;
                if (rowTop < settingsScroll) settingsScroll = rowTop;
                if (rowBottom > settingsScroll + viewH) {
                    settingsScroll = rowBottom - viewH;
                }
                settingsScroll = std::clamp(settingsScroll, 0, maxScroll);
            }

            const SettingsRowId current = rows[selectedSettingsRow];
            const u64 cycle = navDown & (HidNpadButton_Left |
                                         HidNpadButton_Right |
                                         HidNpadButton_A);
            if (current == SettingsRowId::Regions && serverConfigReady &&
                cycle) {
                serverSettings.allowDistantRegions =
                    !serverSettings.allowDistantRegions;
                save_server_settings(serverSettings);
                startServerSave();
            }
            if (current == SettingsRowId::Controller && cycle) {
                serverSettings.xboxFaceLayout =
                    !serverSettings.xboxFaceLayout;
                quickMenuState.xboxFaceLayout =
                    serverSettings.xboxFaceLayout;
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_quick_menu_state(quickMenuState);
                save_server_settings(serverSettings);
                libraryNotice = serverSettings.xboxFaceLayout
                    ? ui("Controles configurados con disposicion fisica Xbox.",
                         "Controls now use the physical Xbox layout.")
                    : ui("Controles configurados con etiquetas Nintendo.",
                         "Controls now use Nintendo button labels.");
            }
            if (current == SettingsRowId::Location && locationCount > 0) {
                if ((navDown & HidNpadButton_Left) && selectedLocation > 0) {
                    --selectedLocation;
                }
                if ((navDown & HidNpadButton_Right) &&
                    selectedLocation + 1 < locationCount) {
                    ++selectedLocation;
                }
            }
            if ((kDown & HidNpadButton_Y) && !serverConfigInFlight &&
                !serverSaveInFlight) startServerConfigLoad();
            if (current == SettingsRowId::Location &&
                (kDown & HidNpadButton_A) && serverConfigReady &&
                !serverConfigInFlight && !serverSaveInFlight) {
                serverSettings.preferredLocationId = selectedLocation == 0
                    ? 0 : serverLocations[selectedLocation - 1].id;
                serverSettings.preferredLocationLabel = selectedLocation == 0
                    ? "Automatico"
                    : serverLocations[selectedLocation - 1].title;
                save_server_settings(serverSettings);
                startServerSave();
            }
            if (current == SettingsRowId::Resolution && cycle) {
                const int direction =
                    (navDown & HidNpadButton_Left) ? -1 : 1;
                serverSettings.resolution =
                    (serverSettings.resolution + direction + 7) % 7;
                save_server_settings(serverSettings);
                switch (serverSettings.resolution) {
                    case 1:
                        libraryNotice = ui("Resolucion de streaming: 720p.",
                                           "Streaming resolution: 720p.");
                        break;
                    case 2:
                        libraryNotice = ui("Resolucion de streaming: 720HQ.",
                                           "Streaming resolution: 720HQ.");
                        break;
                    case 3:
                        libraryNotice = ui("Resolucion de streaming: 1080p.",
                                           "Streaming resolution: 1080p.");
                        break;
                    case 4:
                        libraryNotice = ui("Resolucion de streaming: 1080HQ.",
                                           "Streaming resolution: 1080HQ.");
                        break;
                    case 5:
                        libraryNotice = ui(
                            "Resolucion de streaming: 540p (fuente FSR, 10 Mbps).",
                            "Streaming resolution: 540p (FSR source, 10 Mbps).");
                        break;
                    case 6:
                        libraryNotice = ui(
                            "Resolucion de streaming: 480p (fuente FSR, 7 Mbps).",
                            "Streaming resolution: 480p (FSR source, 7 Mbps).");
                        break;
                    default:
                        libraryNotice = ui("Resolucion de streaming: Auto.",
                                           "Streaming resolution: Auto.");
                        break;
                }
            }
            if (current == SettingsRowId::Advanced && cycle) {
                settingsAdvancedOpen = !settingsAdvancedOpen;
                // Keep the cursor on the Advanced card when the list shrinks.
                selectedSettingsRow = 4;
            }
            if (current == SettingsRowId::Codec && cycle) {
                const int direction =
                    (navDown & HidNpadButton_Left) ? -1 : 1;
                serverSettings.codec =
                    (serverSettings.codec + direction + 3) % 3;
                stream->set_codec_preference(
                    serverSettings.codec == 1
                        ? gnx::stream::CodecPreference::H265
                        : serverSettings.codec == 2
                              ? gnx::stream::CodecPreference::H264
                              : gnx::stream::CodecPreference::Auto);
                save_server_settings(serverSettings);
                libraryNotice =
                    serverSettings.codec == 1
                        ? ui("Codec de video: H.265.",
                             "Video codec: H.265.")
                        : serverSettings.codec == 2
                              ? ui("Codec de video: H.264.",
                                   "Video codec: H.264.")
                              : ui("Codec de video: Auto (H.265, luego H.264).",
                                   "Video codec: Auto (H.265, then H.264).");
            }
            if (current == SettingsRowId::Fps && cycle) {
                const int direction =
                    (navDown & HidNpadButton_Left) ? -1 : 1;
                serverSettings.fps = (serverSettings.fps + direction + 3) % 3;
                stream->set_fps_mode(serverSettings.fps);
                save_server_settings(serverSettings);
                libraryNotice = serverSettings.fps == 1
                    ? ui("Limite de FPS: 30.", "FPS limit: 30.")
                    : serverSettings.fps == 2
                          ? ui("Limite de FPS: 60.", "FPS limit: 60.")
                          : ui("Limite de FPS: Auto.", "FPS limit: Auto.");
            }
            if (current == SettingsRowId::Transport && cycle) {
                const int direction =
                    (navDown & HidNpadButton_Left) ? -1 : 1;
                serverSettings.transport =
                    (serverSettings.transport + direction + 3) % 3;
                stream->set_transport_preference(
                    serverSettings.transport == 1
                        ? gnx::stream::TransportPreference::NativeUdp
                        : serverSettings.transport == 2
                              ? gnx::stream::TransportPreference::WebRtc
                              : gnx::stream::TransportPreference::Auto);
                save_server_settings(serverSettings);
                libraryNotice =
                    serverSettings.transport == 1
                        ? ui("Protocolo: UDP Nativo (proto: 2, Reed-Solomon FEC).",
                             "Protocol: Native UDP (proto: 2, Reed-Solomon FEC).")
                        : serverSettings.transport == 2
                              ? ui("Protocolo: WebRTC (RTP, NACK y PLI).",
                                   "Protocol: WebRTC (RTP, NACK and PLI).")
                              : ui("Protocolo: Auto (UDP Nativo preferido).",
                                   "Protocol: Auto (Native UDP preferred).");
            }
            if (current == SettingsRowId::Upscaler && cycle) {
                const int direction =
                    (navDown & HidNpadButton_Left) ? -1 : 1;
                serverSettings.upscaler =
                    (serverSettings.upscaler + direction + 2) % 2;
                stream->set_upscaler_preference(
                    serverSettings.upscaler == 1
                        ? gnx::stream::UpscalerPreference::Fsr1
                        : gnx::stream::UpscalerPreference::Off);
                save_server_settings(serverSettings);
                libraryNotice =
                    serverSettings.upscaler == 1
                        ? ui("Escalador: FSR 1 (AMD FidelityFX Super Resolution).",
                             "Upscaler: FSR 1 (AMD FidelityFX Super Resolution).")
                        : ui("Escalador: desactivado (muestreo bilineal).",
                             "Upscaler: off (bilinear sampling).");
            }
            if (current == SettingsRowId::Language && cycle) {
                serverSettings.language = serverSettings.language == 0 ? 1 : 0;
                gnx::i18n::set_english(serverSettings.language == 1);
                save_server_settings(serverSettings);
                libraryNotice = ui("Idioma cambiado a Espanol.",
                                   "Language changed to English.");
            }
            if (current == SettingsRowId::Logout &&
                (kDown & HidNpadButton_A)) {
                logout();
            }
            if (kDown & HidNpadButton_B) settingsOpen = false;
        } else if (!launchPanel && hasSession && libraryReady &&
                   (kDown & HidNpadButton_Minus)) {
            settingsOpen = true;
            selectedSettingsRow = 0;
            settingsScroll = 0;
            if (!serverConfigReady && !serverConfigInFlight) {
                startServerConfigLoad();
            }
        }

        if (!settingsOpen && !launchPanel && hasSession && libraryReady &&
            libraryOk) {
            constexpr std::size_t kColumns = 5;
            constexpr std::size_t kPageSize = 10;
            if (kDown & HidNpadButton_ZL) {
                const int tab = static_cast<int>(libraryTab);
                libraryTab = static_cast<LibraryTab>((tab + 2) % 3);
                selectedGame = 0;
                rebuildVisibleGames();
            }
            if (kDown & HidNpadButton_ZR) {
                const int tab = static_cast<int>(libraryTab);
                libraryTab = static_cast<LibraryTab>((tab + 1) % 3);
                selectedGame = 0;
                rebuildVisibleGames();
            }
            if (kDown & HidNpadButton_Y) {
                searchQuery = open_search_keyboard(
                    searchQuery, serverSettings.language);
                selectedGame = 0;
                rebuildVisibleGames();
            }
            if (kDown & HidNpadButton_StickR) {
                startLibraryLoad();
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_X)) {
                const auto& game = libraryGames[visibleGames[selectedGame]];
                const bool wasFavorite = contains_game_id(
                    serverSettings.favoriteGameIds, game.id);
                toggle_game_id(serverSettings.favoriteGameIds, game.id);
                save_server_settings(serverSettings);
                libraryNotice = wasFavorite
                    ? ui("Quitado de Favoritos.", "Removed from Favorites.")
                    : ui("Agregado a Favoritos.", "Added to Favorites.");
                rebuildVisibleGames();
            }
            if (!visibleGames.empty() &&
                (navDown & HidNpadButton_Left) && selectedGame % kColumns > 0) {
                --selectedGame;
            }
            if (!visibleGames.empty() && (navDown & HidNpadButton_Right) &&
                selectedGame + 1 < visibleGames.size() &&
                selectedGame % kColumns + 1 < kColumns) {
                ++selectedGame;
            }
            if (!visibleGames.empty() &&
                (navDown & HidNpadButton_Up) && selectedGame >= kColumns) {
                selectedGame -= kColumns;
            }
            if (!visibleGames.empty() && (navDown & HidNpadButton_Down) &&
                selectedGame + kColumns < visibleGames.size()) {
                selectedGame += kColumns;
            }
            if (!visibleGames.empty() &&
                (kDown & HidNpadButton_L) && selectedGame >= kPageSize) {
                selectedGame -= kPageSize;
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_R) &&
                selectedGame + kPageSize < visibleGames.size()) {
                selectedGame += kPageSize;
            }
            if (!visibleGames.empty() && (kDown & HidNpadButton_A)) {
                const ZERODROID::GameItem& game =
                    libraryGames[visibleGames[selectedGame]];
                launchingTitle = game.title;
                launchingGameId = game.id;
                libraryNotice.clear();
                launchPanel = true;
                quickMenuState.open = false;
                quickMenuState.ping = false;  // every new session starts hidden
                quickMenuState.xboxFaceLayout =
                    serverSettings.xboxFaceLayout;
                quickMenuState.mouse = serverSettings.mouse;
                stream->set_xbox_face_layout(serverSettings.xboxFaceLayout);
                stream->set_stream_resolution(serverSettings.resolution);
                stream->set_codec_preference(
                    serverSettings.codec == 1
                        ? gnx::stream::CodecPreference::H265
                        : serverSettings.codec == 2
                              ? gnx::stream::CodecPreference::H264
                              : gnx::stream::CodecPreference::Auto);
                stream->set_fps_mode(serverSettings.fps);
                stream->set_transport_preference(
                    serverSettings.transport == 1
                        ? gnx::stream::TransportPreference::NativeUdp
                        : serverSettings.transport == 2
                              ? gnx::stream::TransportPreference::WebRtc
                              : gnx::stream::TransportPreference::Auto);
                stream->set_upscaler_preference(
                    serverSettings.upscaler == 1
                        ? gnx::stream::UpscalerPreference::Fsr1
                        : gnx::stream::UpscalerPreference::Off);
                stream->set_quick_menu_state(quickMenuState);
                stream->start(game.id);
            }
        }

        covers->pump();

        // Begin Graphical Frame
        gfx.begin_frame();

        // Draw Header Glass Bar
        SDL_Rect headerRect = {0, 0, gnx::gfx::kWidth, 100};
        gfx.fill(headerRect, gnx::gfx::kBar);

        // Header Title
        gfx.text(std::string("ZERODROID - Boosteroid Cloud Gaming (v") + ZERODROID_VERSION + ")", 40, 30, gnx::gfx::FontSize::Title, gnx::gfx::kText);

        if (!hasSession) {
            // Main Card (Purple Glass)
            SDL_Rect cardRect = {160, 150, 1600, 780};
            gfx.fill(cardRect, gnx::gfx::kSurface);
            gfx.frame(cardRect, gnx::gfx::kAccent, 2);

            // Instructions Text
            gfx.text(ui("Escanea el codigo QR con tu movil",
                        "Scan the QR code with your phone"),
                     220, 200, gnx::gfx::FontSize::Title, gnx::gfx::kText);
            gfx.text(ui("Inicia sesion en Boosteroid y confirma la vinculacion.",
                        "Sign in to Boosteroid and confirm the connection."),
                     220, 290, gnx::gfx::FontSize::Body, gnx::gfx::kText);
            gfx.text(loginStatus, 220, 370, gnx::gfx::FontSize::Body, gnx::gfx::kWarn);
            gfx.text(ui("El QR se renueva automaticamente. Presiona (Y) para renovarlo ahora.",
                        "The QR refreshes automatically. Press (Y) to refresh now."),
                     220, 440, gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);

            // Render Crisp 2D QR Code directly onto Framebuffer
            draw_qr_code(gfx.renderer(), 1220, 280, 440, authArtifact.qrUrl);
        } else {
            if (settingsOpen) {
                SDL_Rect settingsCard = {400, 100, 1120, 880};
                gfx.fill(settingsCard, gnx::gfx::kSurface);
                gfx.frame(settingsCard, gnx::gfx::kChipEdge, 2);
                gfx.text(ui("CONFIGURACION DE ZERODROID", "ZERODROID SETTINGS"),
                         440, 130,
                         gnx::gfx::FontSize::Title, gnx::gfx::kText);

                const std::vector<SettingsRowId> rows =
                    settings_rows(settingsAdvancedOpen);
                const int rowCount = static_cast<int>(rows.size());
                const int stride = kSettingsCardH + kSettingsCardGap;
                const int contentH = rowCount * kSettingsCardH +
                                     (rowCount - 1) * kSettingsCardGap;
                const int viewH = kSettingsViewBottom - kSettingsViewTop;
                const int maxScroll = contentH > viewH ? contentH - viewH : 0;
                settingsScroll = std::clamp(settingsScroll, 0, maxScroll);

                auto rowOf = [&](SettingsRowId id) {
                    for (int i = 0; i < rowCount; ++i)
                        if (rows[i] == id) return i;
                    return -1;
                };
                auto settingsRow = [&](int row) {
                    return SDL_Rect{kSettingsCardX,
                                    kSettingsViewTop + row * stride -
                                        settingsScroll,
                                    kSettingsCardW, kSettingsCardH};
                };
                auto drawCard = [&](int row, bool danger = false) {
                    SDL_Rect r = settingsRow(row);
                    const bool selected = selectedSettingsRow == row;
                    gfx.fill(r, selected
                        ? (danger ? gnx::gfx::Color{72, 32, 48}
                                  : gnx::gfx::kSurfaceHi)
                        : gnx::gfx::kChip);
                    gfx.frame(r, selected
                        ? (danger ? gnx::gfx::kError : gnx::gfx::kFocus)
                        : gnx::gfx::kChipEdge,
                        selected ? 3 : 1);
                    return r;
                };
                // Disclosure arrow for the Advanced card: solid triangle drawn
                // with scanlines so it does not depend on font glyph coverage.
                auto draw_arrow = [&](int cx, int cy, bool up,
                                      gnx::gfx::Color color) {
                    constexpr int kHalfWidth = 20;
                    constexpr int kHeight = 26;
                    for (int row = 0; row < kHeight; row += 2) {
                        const int fromBase = up ? kHeight - 1 - row : row;
                        const int width =
                            kHalfWidth * 2 * (kHeight - fromBase) / kHeight;
                        if (width <= 0) continue;
                        gfx.fill({cx - width / 2, cy - kHeight / 2 + row,
                                  width, 2},
                                 color);
                    }
                };

                SDL_Rect contentClip = {kSettingsCardX, kSettingsViewTop,
                                        kSettingsCardW, viewH};
                SDL_RenderSetClipRect(gfx.renderer(), &contentClip);

                SDL_Rect distantRow = drawCard(rowOf(SettingsRowId::Regions));
                gfx.text(ui("Permitir conexion a regiones lejanas",
                            "Allow connections to distant regions"),
                         distantRow.x + 40, distantRow.y + 64,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                SDL_Rect toggleTrack = {
                    distantRow.x + distantRow.w - 150, distantRow.y + 61,
                    100, 46};
                gfx.fill(toggleTrack, serverSettings.allowDistantRegions
                    ? gnx::gfx::Color{16, 150, 95}
                    : gnx::gfx::Color{70, 74, 88});
                SDL_Rect toggleKnob = {
                    toggleTrack.x +
                        (serverSettings.allowDistantRegions ? 58 : 6),
                    toggleTrack.y + 6, 34, 34};
                gfx.fill(toggleKnob, gnx::gfx::kText);

                SDL_Rect controllerRow = drawCard(rowOf(SettingsRowId::Controller));
                gfx.text(ui("Disposicion de botones A / B / X / Y",
                            "A / B / X / Y button layout"),
                         controllerRow.x + 40, controllerRow.y + 70,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                SDL_Rect layoutChip = {
                    controllerRow.x + controllerRow.w - 330,
                    controllerRow.y + 52, 260, 64};
                gfx.fill(layoutChip, serverSettings.xboxFaceLayout
                    ? gnx::gfx::Color{16, 110, 80} : gnx::gfx::kBar);
                gfx.frame(layoutChip, gnx::gfx::kFocus, 2);
                gfx.text_centered(serverSettings.xboxFaceLayout ? "XBOX" : "NINTENDO",
                                  layoutChip.x + layoutChip.w / 2,
                                  layoutChip.y + 18,
                                  gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);

                SDL_Rect locationRow = drawCard(rowOf(SettingsRowId::Location));
                gfx.text(ui("Ubicacion de servidor preferida",
                            "Preferred server location"),
                         locationRow.x + 40, locationRow.y + 26,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("(Y) ACTUALIZAR", "(Y) REFRESH"),
                         locationRow.x + locationRow.w - 190,
                         locationRow.y + 26,
                         gnx::gfx::FontSize::Small, gnx::gfx::kFocus);

                SDL_Rect locationSelect = {
                    locationRow.x + 40, locationRow.y + 88, 700, 55};
                gfx.fill(locationSelect, gnx::gfx::kBar);
                gfx.frame(locationSelect, gnx::gfx::kChipEdge, 2);
                std::string locationLabel = ui("Automatico (recomendado)",
                                               "Automatic (recommended)");
                if (selectedLocation > 0 &&
                    selectedLocation <= serverLocations.size()) {
                    const auto& location = serverLocations[selectedLocation - 1];
                    locationLabel = location.title;
                    if (!location.country.empty()) {
                        locationLabel += " (" + location.country + ")";
                    }
                }
                gfx.text("<", locationSelect.x + 30, locationSelect.y + 6,
                         gnx::gfx::FontSize::Body, gnx::gfx::kFocus);
                gfx.text(fit_text(gfx, locationLabel, 560),
                         locationSelect.x + 90, locationSelect.y + 6,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(">", locationSelect.x + locationSelect.w - 45,
                         locationSelect.y + 6,
                         gnx::gfx::FontSize::Body, gnx::gfx::kFocus);

                SDL_Rect resolutionRow = drawCard(rowOf(SettingsRowId::Resolution));
                gfx.text(ui("Resolucion de streaming", "Streaming resolution"),
                         resolutionRow.x + 40, resolutionRow.y + 70,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                SDL_Rect resolutionChip = {
                    resolutionRow.x + resolutionRow.w - 330,
                    resolutionRow.y + 52, 260, 64};
                gfx.fill(resolutionChip, gnx::gfx::kBar);
                gfx.frame(resolutionChip, gnx::gfx::kFocus, 2);
                const char* resolutionLabel = "AUTO";
                switch (serverSettings.resolution) {
                    case 1: resolutionLabel = "720p"; break;
                    case 2: resolutionLabel = "720HQ"; break;
                    case 3: resolutionLabel = "1080p"; break;
                    case 4: resolutionLabel = "1080HQ"; break;
                    case 5: resolutionLabel = "540p FSR"; break;
                    case 6: resolutionLabel = "480p FSR"; break;
                    default: break;
                }
                gfx.text_centered(resolutionLabel,
                                  resolutionChip.x + resolutionChip.w / 2,
                                  resolutionChip.y + 18,
                                  gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);

                SDL_Rect advancedRow = drawCard(rowOf(SettingsRowId::Advanced));
                gfx.text(ui("Avanzado", "Advanced"),
                         advancedRow.x + 40, advancedRow.y + 52,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Codec, FPS, red y escalador",
                            "Codec, FPS, network and upscaler"),
                         advancedRow.x + 40, advancedRow.y + 96,
                         gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
                SDL_Rect advancedChip = {
                    advancedRow.x + advancedRow.w - 190,
                    advancedRow.y + 52, 120, 64};
                gfx.fill(advancedChip, settingsAdvancedOpen
                    ? gnx::gfx::Color{16, 110, 80} : gnx::gfx::kBar);
                gfx.frame(advancedChip, gnx::gfx::kFocus, 2);
                draw_arrow(advancedChip.x + advancedChip.w / 2,
                           advancedChip.y + advancedChip.h / 2,
                           settingsAdvancedOpen, gnx::gfx::kText);

                if (settingsAdvancedOpen) {
                    SDL_Rect codecRow = drawCard(rowOf(SettingsRowId::Codec));
                    gfx.text(ui("Codec de video", "Video codec"),
                             codecRow.x + 40, codecRow.y + 70,
                             gnx::gfx::FontSize::Body, gnx::gfx::kText);
                    SDL_Rect codecChip = {
                        codecRow.x + codecRow.w - 330,
                        codecRow.y + 52, 260, 64};
                    gfx.fill(codecChip, gnx::gfx::kBar);
                    gfx.frame(codecChip, gnx::gfx::kFocus, 2);
                    const char* codecLabel = "AUTO";
                    if (serverSettings.codec == 1) codecLabel = "H.265";
                    if (serverSettings.codec == 2) codecLabel = "H.264";
                    gfx.text_centered(codecLabel,
                                      codecChip.x + codecChip.w / 2,
                                      codecChip.y + 18,
                                      gnx::gfx::FontSize::Small,
                                      gnx::gfx::kText);

                    SDL_Rect fpsRow = drawCard(rowOf(SettingsRowId::Fps));
                    gfx.text(ui("Limite de FPS", "FPS limit"),
                             fpsRow.x + 40, fpsRow.y + 70,
                             gnx::gfx::FontSize::Body, gnx::gfx::kText);
                    SDL_Rect fpsChip = {
                        fpsRow.x + fpsRow.w - 330, fpsRow.y + 52, 260, 64};
                    gfx.fill(fpsChip, gnx::gfx::kBar);
                    gfx.frame(fpsChip, gnx::gfx::kFocus, 2);
                    const char* fpsLabel = "AUTO";
                    if (serverSettings.fps == 1) fpsLabel = "30";
                    if (serverSettings.fps == 2) fpsLabel = "60";
                    gfx.text_centered(fpsLabel,
                                      fpsChip.x + fpsChip.w / 2,
                                      fpsChip.y + 18,
                                      gnx::gfx::FontSize::Small,
                                      gnx::gfx::kText);

                    SDL_Rect transportRow =
                        drawCard(rowOf(SettingsRowId::Transport));
                    gfx.text(ui("Protocolo de red", "Network protocol"),
                             transportRow.x + 40, transportRow.y + 70,
                             gnx::gfx::FontSize::Body, gnx::gfx::kText);
                    SDL_Rect transportChip = {
                        transportRow.x + transportRow.w - 330,
                        transportRow.y + 52, 260, 64};
                    gfx.fill(transportChip, gnx::gfx::kBar);
                    gfx.frame(transportChip, gnx::gfx::kFocus, 2);
                    const char* transportLabel = "AUTO (NAT-UDP)";
                    if (serverSettings.transport == 1)
                        transportLabel = "NAT-UDP (FEC)";
                    if (serverSettings.transport == 2)
                        transportLabel = "WEBRTC";
                    gfx.text_centered(transportLabel,
                                      transportChip.x + transportChip.w / 2,
                                      transportChip.y + 18,
                                      gnx::gfx::FontSize::Small,
                                      gnx::gfx::kText);

                    SDL_Rect upscalerRow =
                        drawCard(rowOf(SettingsRowId::Upscaler));
                    gfx.text(ui("Escalador de imagen", "Image upscaler"),
                             upscalerRow.x + 40, upscalerRow.y + 70,
                             gnx::gfx::FontSize::Body, gnx::gfx::kText);
                    SDL_Rect upscalerChip = {
                        upscalerRow.x + upscalerRow.w - 330,
                        upscalerRow.y + 52, 260, 64};
                    gfx.fill(upscalerChip, serverSettings.upscaler == 1
                        ? gnx::gfx::Color{16, 110, 80} : gnx::gfx::kBar);
                    gfx.frame(upscalerChip, gnx::gfx::kFocus, 2);
                    gfx.text_centered(serverSettings.upscaler == 1
                                          ? "FSR 1" : "OFF",
                                      upscalerChip.x + upscalerChip.w / 2,
                                      upscalerChip.y + 18,
                                      gnx::gfx::FontSize::Small,
                                      gnx::gfx::kText);
                }

                SDL_Rect languageRow = drawCard(rowOf(SettingsRowId::Language));
                gfx.text(ui("Idioma de la interfaz", "Interface language"),
                         languageRow.x + 40, languageRow.y + 70,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                SDL_Rect languageChip = {
                    languageRow.x + languageRow.w - 330,
                    languageRow.y + 52, 260, 64};
                gfx.fill(languageChip, gnx::gfx::kBar);
                gfx.frame(languageChip, gnx::gfx::kFocus, 2);
                gfx.text_centered(serverSettings.language == 1
                                      ? "ENGLISH" : "ESPANOL",
                                  languageChip.x + languageChip.w / 2,
                                  languageChip.y + 18,
                                  gnx::gfx::FontSize::Small,
                                  gnx::gfx::kText);

                SDL_Rect logoutRow = drawCard(rowOf(SettingsRowId::Logout), true);
                gfx.text(ui("Cerrar sesion", "Sign out"),
                         logoutRow.x + 40, logoutRow.y + 70,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text("(A)", logoutRow.x + logoutRow.w - 160,
                         logoutRow.y + 70,
                         gnx::gfx::FontSize::Body, gnx::gfx::kError);

                SDL_RenderSetClipRect(gfx.renderer(), nullptr);

                if (maxScroll > 0) {
                    SDL_Rect track = {settingsCard.x + settingsCard.w - 30,
                                      kSettingsViewTop, 8, viewH};
                    gfx.fill(track, gnx::gfx::kChipEdge);
                    const int thumbH = std::max(48, viewH * viewH / contentH);
                    const int thumbY = kSettingsViewTop +
                        (viewH - thumbH) * settingsScroll / maxScroll;
                    SDL_Rect thumb = {track.x, thumbY, track.w, thumbH};
                    gfx.fill(thumb, gnx::gfx::kFocus);
                }

                if (serverConfigInFlight) {
                    gfx.text(ui("Cargando ubicaciones oficiales de Boosteroid...",
                                "Loading official Boosteroid locations..."),
                             440, 925, gnx::gfx::FontSize::Small,
                             gnx::gfx::kWarn);
                } else if (serverSaveInFlight) {
                    gfx.text(ui("Guardando en tu cuenta de Boosteroid...",
                                "Saving to your Boosteroid account..."),
                             440, 925, gnx::gfx::FontSize::Small,
                             gnx::gfx::kWarn);
                } else if (!serverConfigError.empty()) {
                    gfx.text(fit_text(gfx, serverConfigError, 1000), 440, 925,
                             gnx::gfx::FontSize::Small, gnx::gfx::kError);
                } else {
                    gfx.text(ui("Izquierda/derecha cambia; (A) selecciona o guarda.",
                                "Left/right changes; (A) selects or saves."),
                             440, 925, gnx::gfx::FontSize::Small,
                             gnx::gfx::kTextDim);
                }
            } else if (launchPanel) {
                SDL_Rect cardRect = {250, 190, 1420, 660};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                const bool failed =
                    stream->state() == gnx::stream::EngineState::Failed;
                gfx.frame(cardRect, failed ? gnx::gfx::kError
                                           : gnx::gfx::kFocus, 3);
                gfx.text(failed ? ui("No se pudo iniciar el juego",
                                     "Could not start the game")
                                : ui("Iniciando streaming",
                                     "Starting stream"),
                         330, 255, gnx::gfx::FontSize::Title,
                         failed ? gnx::gfx::kError : gnx::gfx::kFocus);
                gfx.text(fit_text(gfx, launchingTitle, 1240,
                                  gnx::gfx::FontSize::Body),
                         330, 355, gnx::gfx::FontSize::Body,
                         gnx::gfx::kText);
                const std::string streamStatus = failed
                    ? stream->error()
                    : stream->status();
                gfx.text(fit_text(gfx, streamStatus, 1240,
                                  gnx::gfx::FontSize::Body),
                         330, 465, gnx::gfx::FontSize::Body,
                         failed ? gnx::gfx::kError : gnx::gfx::kWarn);
                const std::string route = std::string(ui("Servidor: ", "Server: ")) +
                    serverSettings.preferredLocationLabel +
                    (serverSettings.allowDistantRegions
                        ? ui(" | regiones lejanas permitidas",
                             " | distant regions allowed")
                        : ui(" | solo mi region", " | my region only"));
                gfx.text(fit_text(gfx, route, 1240), 330, 570,
                         gnx::gfx::FontSize::Small, gnx::gfx::kTextDim);
                gfx.text(ui("La asignacion de una maquina puede tardar varios minutos.",
                            "Machine assignment may take several minutes."),
                         330, 650, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);
                gfx.text(ui("(B) Cancelar / volver a la biblioteca",
                            "(B) Cancel / return to library"), 330, 745,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
            } else if (libraryInFlight || !libraryReady) {
                SDL_Rect cardRect = {160, 150, 1600, 780};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kFocus, 2);

                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now.time_since_epoch()).count();
                const int dotCount = static_cast<int>((elapsed / 450) % 4);
                gfx.text(ui("Sesion iniciada correctamente",
                            "Signed in successfully"), 220, 220,
                         gnx::gfx::FontSize::Title, gnx::gfx::kFocus);
                gfx.text(std::string(ui("Cargando tu biblioteca",
                                        "Loading your library")) +
                             std::string(dotCount, '.'), 220, 310,
                         gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Consultando tus juegos sin bloquear la interfaz.",
                            "Fetching your games without freezing the interface."),
                         220, 390,
                         gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
                if (!libraryNotice.empty()) {
                    gfx.text(fit_text(gfx, libraryNotice, 1450), 220, 470,
                             gnx::gfx::FontSize::Small, gnx::gfx::kWarn);
                }
            } else if (!libraryOk) {
                SDL_Rect cardRect = {160, 150, 1600, 780};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kError, 3);
                gfx.text(ui("No se pudo cargar la biblioteca",
                            "Could not load the library"), 220, 220,
                         gnx::gfx::FontSize::Title, gnx::gfx::kError);
                gfx.text(fit_text(gfx, libraryError, 1450,
                                  gnx::gfx::FontSize::Body),
                         220, 320, gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Presiona (Y) para reintentar o entra en Settings para cerrar sesion.",
                            "Press (Y) to retry or open Settings to sign out."),
                         220, 420, gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
            } else if (libraryGames.empty()) {
                SDL_Rect cardRect = {160, 150, 1600, 780};
                gfx.fill(cardRect, gnx::gfx::kSurface);
                gfx.frame(cardRect, gnx::gfx::kFocus, 2);
                gfx.text(ui("Tu biblioteca esta conectada",
                            "Your library is connected"), 220, 220,
                         gnx::gfx::FontSize::Title, gnx::gfx::kFocus);
                gfx.text(ui("Boosteroid no devolvio juegos instalados en esta cuenta.",
                            "Boosteroid returned no installed games for this account."),
                         220, 320, gnx::gfx::FontSize::Body, gnx::gfx::kText);
                gfx.text(ui("Agrega juegos a Mi biblioteca y presiona R3 para recargar.",
                            "Add games to My Library and press R3 to refresh."),
                         220, 400, gnx::gfx::FontSize::Body, gnx::gfx::kTextDim);
            } else {
                constexpr int kColumns = 5;
                constexpr int kRows = 2;
                constexpr int kPageSize = kColumns * kRows;
                constexpr int kCardWidth = 288;
                constexpr int kCardHeight = 305;
                constexpr int kGapX = 20;
                constexpr int kGapY = 20;
                constexpr int kGridX = 200;
                constexpr int kGridY = 280;

                const std::string owner = libraryUser.nickname.empty()
                    ? ui("Mi biblioteca", "My library")
                    : std::string(ui("Biblioteca de ", "Library of ")) +
                          libraryUser.nickname;
                gfx.text(owner, 180, 112, gnx::gfx::FontSize::Title,
                         gnx::gfx::kText);
                gfx.text(std::to_string(visibleGames.size()) + " / " +
                             std::to_string(libraryGames.size()) + " " +
                             ui("juegos", "games"),
                         1440, 118,
                         gnx::gfx::FontSize::Body, gnx::gfx::kFocus);

                const auto drawTab = [&](LibraryTab tab, int x,
                                         const std::string& label) {
                    SDL_Rect tabRect = {x, 160, 215, 48};
                    const bool active = libraryTab == tab;
                    gfx.fill(tabRect, active ? gnx::gfx::kSurfaceHi
                                             : gnx::gfx::kChip);
                    gfx.frame(tabRect, active ? gnx::gfx::kFocus
                                              : gnx::gfx::kChipEdge,
                              active ? 3 : 1);
                    gfx.text_centered(label, tabRect.x + tabRect.w / 2,
                                      tabRect.y + 10,
                                      gnx::gfx::FontSize::Small,
                                      active ? gnx::gfx::kText
                                             : gnx::gfx::kTextDim);
                };
                drawTab(LibraryTab::All, 180, ui("TODOS", "ALL"));
                drawTab(LibraryTab::Favorites, 410,
                        ui("FAVORITOS", "FAVORITES"));
                drawTab(LibraryTab::Recent, 640,
                        ui("RECIENTES", "RECENT"));

                SDL_Rect searchRect = {900, 160, 840, 48};
                gfx.fill(searchRect, gnx::gfx::kBar);
                gfx.frame(searchRect, searchQuery.empty()
                    ? gnx::gfx::kChipEdge : gnx::gfx::kFocus, 2);
                const std::string searchLabel = searchQuery.empty()
                    ? ui("(Y) Buscar juegos", "(Y) Search games")
                    : std::string(ui("Busqueda: ", "Search: ")) + searchQuery;
                gfx.text(fit_text(gfx, searchLabel, searchRect.w - 35),
                         searchRect.x + 18, searchRect.y + 10,
                         gnx::gfx::FontSize::Small,
                         searchQuery.empty() ? gnx::gfx::kTextDim
                                             : gnx::gfx::kText);

                gfx.text(ui("ZL/ZR: pestana   L/R: pagina   A: jugar   X: favorito   R3: recargar",
                            "ZL/ZR: tab   L/R: page   A: play   X: favorite   R3: refresh"),
                         180, 225, gnx::gfx::FontSize::Small,
                         gnx::gfx::kTextDim);

                if (visibleGames.empty()) {
                    SDL_Rect emptyRect = {300, 350, 1320, 350};
                    gfx.fill(emptyRect, gnx::gfx::kSurface);
                    gfx.frame(emptyRect, gnx::gfx::kChipEdge, 2);
                    std::string emptyTitle;
                    std::string emptyHelp;
                    if (!searchQuery.empty()) {
                        emptyTitle = ui("No encontramos juegos con esa busqueda",
                                        "No games matched your search");
                        emptyHelp = ui("Presiona (Y) y borra el texto para mostrar todo.",
                                       "Press (Y) and clear the text to show everything.");
                    } else if (libraryTab == LibraryTab::Favorites) {
                        emptyTitle = ui("Todavia no tienes favoritos",
                                        "You do not have favorites yet");
                        emptyHelp = ui("En Todos o Recientes, presiona (X) sobre un juego.",
                                       "In All or Recent, press (X) on a game.");
                    } else {
                        emptyTitle = ui("Todavia no hay juegos recientes",
                                        "There are no recent games yet");
                        emptyHelp = ui("Los juegos apareceran aqui cuando los inicies.",
                                       "Games appear here after you launch them.");
                    }
                    gfx.text_centered(emptyTitle,
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 105,
                                      gnx::gfx::FontSize::Title,
                                      gnx::gfx::kText);
                    gfx.text_centered(emptyHelp,
                                      emptyRect.x + emptyRect.w / 2,
                                      emptyRect.y + 215,
                                      gnx::gfx::FontSize::Body,
                                      gnx::gfx::kTextDim);
                } else {
                    const std::size_t pageStart =
                        (selectedGame / static_cast<std::size_t>(kPageSize)) *
                        kPageSize;
                    const std::size_t pageEnd = std::min(
                        visibleGames.size(),
                        pageStart + static_cast<std::size_t>(kPageSize));
                    for (std::size_t index = pageStart; index < pageEnd; ++index) {
                        const int slot = static_cast<int>(index - pageStart);
                        const int column = slot % kColumns;
                        const int row = slot / kColumns;
                        SDL_Rect card = {
                            kGridX + column * (kCardWidth + kGapX),
                            kGridY + row * (kCardHeight + kGapY),
                            kCardWidth,
                            kCardHeight,
                        };
                        const bool selected = index == selectedGame;
                        gfx.fill(card, selected ? gnx::gfx::kSurfaceHi
                                                : gnx::gfx::kSurface);
                        gfx.frame(card, selected ? gnx::gfx::kFocus
                                                 : gnx::gfx::kChipEdge,
                                  selected ? 4 : 2);

                        const ZERODROID::GameItem& game =
                            libraryGames[visibleGames[index]];
                        const std::string& coverUrl = game.posterUrl.empty()
                            ? game.bannerUrl
                            : game.posterUrl;
                        const std::string coverKey = std::to_string(game.id);
                        SDL_Rect coverRect = {card.x + 8, card.y + 8,
                                              card.w - 16, 190};
                        SDL_Texture* cover = covers->get(coverKey, coverUrl);
                        if (cover) {
                            draw_cover_texture(gfx.renderer(), cover, coverRect);
                        } else {
                            gfx.fill(coverRect, gnx::gfx::kChip);
                            const bool unavailable = coverUrl.empty() ||
                                                     covers->has_result(coverKey);
                            gfx.text_centered(
                                unavailable ? ui("Sin caratula", "No cover")
                                            : ui("Cargando imagen", "Loading image"),
                                coverRect.x + coverRect.w / 2,
                                coverRect.y + 70,
                                gnx::gfx::FontSize::Small,
                                gnx::gfx::kFaint);
                        }

                        if (contains_game_id(serverSettings.favoriteGameIds,
                                             game.id)) {
                            SDL_Rect favoriteChip = {
                                card.x + card.w - 74, card.y + 16, 58, 38};
                            gfx.fill(favoriteChip, gnx::gfx::Color{104, 67, 145});
                            gfx.frame(favoriteChip, gnx::gfx::kFocus, 1);
                            gfx.text_centered("FAV",
                                              favoriteChip.x + favoriteChip.w / 2,
                                              favoriteChip.y + 7,
                                              gnx::gfx::FontSize::Small,
                                              gnx::gfx::kText);
                        }

                        gfx.text(fit_text(gfx, game.title, card.w - 22),
                                 card.x + 11, card.y + 208,
                                 gnx::gfx::FontSize::Small,
                                 gnx::gfx::kText);
                        std::string platform = game.store.empty()
                            ? game.platform : game.store;
                        if (platform.empty()) platform = "Boosteroid Cloud";
                        gfx.text(fit_text(gfx, platform, card.w - 22),
                                 card.x + 11, card.y + 258,
                                 gnx::gfx::FontSize::Small,
                                 selected ? gnx::gfx::kFocus
                                          : gnx::gfx::kTextDim);
                    }

                    const std::size_t page = selectedGame / kPageSize + 1;
                    const std::size_t pages =
                        (visibleGames.size() + kPageSize - 1) / kPageSize;
                    gfx.text(std::string(ui("Pagina ", "Page ")) +
                                 std::to_string(page) + " " + ui("de", "of") +
                                 " " + std::to_string(pages),
                             180, 940, gnx::gfx::FontSize::Small,
                             gnx::gfx::kTextDim);
                }

                if (!libraryNotice.empty()) {
                    gfx.text(fit_text(gfx, libraryNotice, 1180), 560, 940,
                             gnx::gfx::FontSize::Small, gnx::gfx::kWarn);
                }
            }
        }

        // Draw Footer Glass Bar
        SDL_Rect footerRect = {0, gnx::gfx::kHeight - 70, gnx::gfx::kWidth, 70};
        gfx.fill(footerRect, gnx::gfx::kBar);
        gfx.text(hasSession
                      ? (settingsOpen
                           ? ui("Arriba/abajo: opcion   Izq./der.: cambiar   (A) Elegir   (B) Volver",
                                "Up/down: option   Left/right: change   (A) Select   (B) Back")
                           : (launchPanel
                                ? ui("(B) Cancelar inicio   (+) Salir",
                                     "(B) Cancel launch   (+) Exit")
                                : ui("(Y) Buscar   (X) Favorito   (-) Settings   (+) Salir",
                                     "(Y) Search   (X) Favorite   (-) Settings   (+) Exit")))
                     : ui("(Y) Renovar QR   (+) Salir al menu principal",
                          "(Y) Refresh QR   (+) Exit to HOME menu"),
                 40, gnx::gfx::kHeight - 50, gnx::gfx::FontSize::Body,
                 gnx::gfx::kTextDim);

        // End Graphical Frame & Present SDL2 Buffer
        gfx.end_frame();
    }

    if (pollInFlight) pollFuture.wait();
    if (libraryInFlight) libraryFuture.wait();
    if (serverConfigInFlight) serverConfigFuture.wait();
    if (serverSaveInFlight) serverSaveFuture.wait();
    std::fprintf(stderr, "ZERODROID shutting down\n");
    if (dekoStreaming) {
        stream->end_deko_output();
        stream->stop();
        gfx.resume();
    }
    stream.reset();
    gnx::stream::Engine::global_shutdown();
    covers.reset();
    gfx.shutdown();
    gnx::Http::global_cleanup();
    if (romfsReady) romfsExit();
    if (plReady) plExit();
    if (socketsReady) socketExit();
    return 0;
}
