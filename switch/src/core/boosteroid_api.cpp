#include "boosteroid_api.hpp"
#include "http.hpp"
#include "i18n.hpp"
#include "json.hpp"

#if defined(__SWITCH__)
#include "../switch/stream/websocket.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <random>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace ZERODROID {

namespace {

constexpr int kAndroidTvClientId = 6;
constexpr const char* kQrSyncEndpoint = "/api/v1/auth/login/qr-code/sync";
constexpr const char* kQrValidationEndpoint =
    "/api/v1/auth/login/qr-code/validate?auth-code=";

std::string make_uuid_v4() {
    std::array<unsigned char, 16> bytes{};
#if defined(__SWITCH__)
    randomGet(bytes.data(), bytes.size());
#else
    std::random_device random;
    for (auto& byte : bytes) byte = static_cast<unsigned char>(random());
#endif

    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

    char output[37]{};
    std::snprintf(
        output,
        sizeof(output),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        bytes[0], bytes[1], bytes[2], bytes[3],
        bytes[4], bytes[5], bytes[6], bytes[7],
        bytes[8], bytes[9], bytes[10], bytes[11],
        bytes[12], bytes[13], bytes[14], bytes[15]);
    return output;
}

int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int hex_digit(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::string decode_token(const std::string& token) {
    std::string decoded;
    decoded.reserve(token.size());
    for (std::size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '+') {
            decoded.push_back(' ');
        } else if (token[i] == '%' && i + 2 < token.size()) {
            const int high = hex_digit(token[i + 1]);
            const int low = hex_digit(token[i + 2]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            } else {
                decoded.push_back(token[i]);
            }
        } else {
            decoded.push_back(token[i]);
        }
    }

    const auto first = decoded.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = decoded.find_last_not_of(" \t\r\n");
    return decoded.substr(first, last - first + 1);
}

std::string normalize_authorization(const std::string& token) {
    const std::string decoded = decode_token(token);
    std::string prefix = decoded.substr(0, std::min<std::size_t>(7, decoded.size()));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (prefix == "bearer ") {
        return "Bearer " + decoded.substr(7);
    }

    const auto firstDot = decoded.find('.');
    const auto secondDot = firstDot == std::string::npos
        ? std::string::npos
        : decoded.find('.', firstDot + 1);
    if (firstDot != std::string::npos && secondDot != std::string::npos) {
        return "Bearer " + decoded;
    }
    return decoded;
}

std::string realtime_access_token(const std::string& token) {
    const std::string decoded = decode_token(token);
    std::string prefix = decoded.substr(0, std::min<std::size_t>(7, decoded.size()));
    std::transform(prefix.begin(), prefix.end(), prefix.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    if (prefix == "bearer ") {
        return decoded.substr(7);
    }
    return decoded;
}

std::string websocket_host(const std::string& baseUrl) {
    std::string host = baseUrl;
    const auto scheme = host.find("://");
    if (scheme != std::string::npos) host.erase(0, scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos) host.erase(slash);
    return host;
}

std::vector<std::string> android_tv_headers(
    bool jsonBody,
    const std::string& accessToken = {},
    const std::string& userDataToken = {},
    const std::string& deviceId = {}) {
    std::vector<std::string> headers = {
        "Accept: application/json",
        "User-Agent: BoosteroidAndroidTVClient v.2.5.10.tv; Android 12; Nintendo Switch",
        "Device-Name: ZERODROID Nintendo Switch 12",
        deviceId.empty() ? "Device-Uniq-Id;" : "Device-Uniq-Id: " + deviceId,
        "Nonce: 0",
        "Cookie: boosteroid_entrypoint_source=1;boosteroid_entrypoint_page=1",
    };
    if (jsonBody) headers.emplace_back("Content-Type: application/json");
    if (!accessToken.empty()) {
        headers.emplace_back("Authorization: " + normalize_authorization(accessToken));
    }
    if (!userDataToken.empty()) {
        headers.emplace_back("Authorization-Data: " + userDataToken);
    }
    return headers;
}

const nlohmann::json* unwrap_payload(const nlohmann::json& payload) {
    if (payload.is_object()) {
        const auto data = payload.find("data");
        if (data != payload.end() && data->is_object()) return &(*data);
    }
    return &payload;
}

std::string json_string(const nlohmann::json& object, const char* key) {
    const auto value = object.find(key);
    if (value == object.end() || value->is_null()) return {};
    if (value->is_string()) return value->get<std::string>();
    return value->dump();
}

std::string auth_data_token(const nlohmann::json& payload) {
    for (const char* key : {
             "authorization_data", "authorizationData",
             "boosteroid_auth", "boosteroidAuth"}) {
        const std::string direct = json_string(payload, key);
        if (!direct.empty()) return direct;
    }

    const auto userData = payload.find("user_data");
    if (userData == payload.end() || userData->is_null()) return {};
    if (userData->is_string()) return userData->get<std::string>();
    if (!userData->is_object()) return {};
    for (const char* key : {
             "authorization_data", "authorizationData",
             "boosteroid_auth", "boosteroidAuth"}) {
        const std::string nested = json_string(*userData, key);
        if (!nested.empty()) return nested;
    }
    return {};
}

std::string first_string(
    const nlohmann::json& object,
    std::initializer_list<const char*> keys) {
    if (!object.is_object()) return {};
    for (const char* key : keys) {
        const auto value = object.find(key);
        if (value == object.end() || value->is_null()) continue;
        if (value->is_string()) return value->get<std::string>();
        if (value->is_number_integer()) {
            return std::to_string(value->get<long long>());
        }
        if (value->is_number_unsigned()) {
            return std::to_string(value->get<unsigned long long>());
        }
    }
    return {};
}

int first_positive_int(
    const nlohmann::json& object,
    std::initializer_list<const char*> keys) {
    if (!object.is_object()) return 0;
    for (const char* key : keys) {
        const auto value = object.find(key);
        if (value == object.end() || value->is_null()) continue;
        try {
            int parsed = 0;
            if (value->is_number_integer() || value->is_number_unsigned()) {
                parsed = value->get<int>();
            } else if (value->is_string()) {
                parsed = std::stoi(value->get<std::string>());
            }
            if (parsed > 0) return parsed;
        } catch (...) {
        }
    }
    return 0;
}

const nlohmann::json* find_game_array(
    const nlohmann::json& value,
    int depth = 0) {
    if (depth > 6) return nullptr;
    if (value.is_array()) return &value;
    if (!value.is_object()) return nullptr;

    for (const char* key : {
             "data", "applications", "games", "installed", "items", "results"}) {
        const auto nested = value.find(key);
        if (nested == value.end()) continue;
        if (nested->is_array()) return &(*nested);
        if (const auto* result = find_game_array(*nested, depth + 1)) {
            return result;
        }
    }
    return nullptr;
}

const nlohmann::json& nested_game(const nlohmann::json& value) {
    if (!value.is_object()) return value;
    for (const char* key : {"application", "app", "game", "item"}) {
        const auto nested = value.find(key);
        if (nested != value.end() && nested->is_object()) return *nested;
    }
    return value;
}

std::string image_value(const nlohmann::json& value, int depth = 0) {
    if (depth > 4 || value.is_null()) return {};
    if (value.is_string()) return value.get<std::string>();
    if (value.is_array()) {
        for (const auto& entry : value) {
            const std::string found = image_value(entry, depth + 1);
            if (!found.empty()) return found;
        }
        return {};
    }
    if (!value.is_object()) return {};

    for (const char* key : {"url", "src", "href", "imageUrl", "image_url"}) {
        const auto nested = value.find(key);
        if (nested == value.end()) continue;
        const std::string found = image_value(*nested, depth + 1);
        if (!found.empty()) return found;
    }
    for (auto nested = value.begin(); nested != value.end(); ++nested) {
        const std::string found = image_value(nested.value(), depth + 1);
        if (!found.empty() &&
            (found.rfind("http://", 0) == 0 || found.rfind("https://", 0) == 0 ||
             found.rfind("//", 0) == 0 || found.front() == '/')) {
            return found;
        }
    }
    return {};
}

std::string image_from(
    const nlohmann::json& game,
    std::initializer_list<const char*> preferredKeys) {
    if (!game.is_object()) return {};
    for (const char* key : preferredKeys) {
        const auto value = game.find(key);
        if (value == game.end()) continue;
        const std::string found = image_value(*value);
        if (!found.empty()) return found;
    }
    for (const char* key : {"media", "images", "assets"}) {
        const auto value = game.find(key);
        if (value == game.end()) continue;
        const std::string found = image_value(*value);
        if (!found.empty()) return found;
    }
    return {};
}

std::string object_label(
    const nlohmann::json& object,
    std::initializer_list<const char*> keys) {
    if (!object.is_object()) return {};
    for (const char* key : keys) {
        const auto value = object.find(key);
        if (value == object.end() || value->is_null()) continue;
        if (value->is_string()) return value->get<std::string>();
        if (value->is_object()) {
            const std::string label =
                first_string(*value, {"name", "title", "slug", "displayName"});
            if (!label.empty()) return label;
        }
    }
    return {};
}

std::string absolute_image_url(
    const std::string& baseUrl,
    const std::string& imageUrl) {
    if (imageUrl.rfind("//", 0) == 0) return "https:" + imageUrl;
    if (!imageUrl.empty() && imageUrl.front() == '/') return baseUrl + imageUrl;
    return imageUrl;
}

bool parse_games(
    const nlohmann::json& payload,
    const std::string& baseUrl,
    std::vector<GameItem>& outGames) {
    const nlohmann::json* items = find_game_array(payload);
    if (!items) return false;

    std::unordered_set<int> seen;
    for (const auto& raw : *items) {
        const nlohmann::json& source = nested_game(raw);
        int id = first_positive_int(
            source, {"id", "appId", "applicationId", "gameId"});
        if (!id) {
            id = first_positive_int(
                raw, {"id", "appId", "applicationId", "gameId"});
        }
        if (!id || !seen.insert(id).second) continue;

        GameItem game;
        game.id = id;
        game.title = first_string(source, {"name", "title", "displayName"});
        if (game.title.empty()) {
            game.title = first_string(raw, {"name", "title", "displayName"});
        }
        if (game.title.empty()) game.title = gnx::i18n::tr("Aplicacion ", "App ") + std::to_string(id);

        game.posterUrl = absolute_image_url(
            baseUrl,
            image_from(source, {
                "poster", "posterUrl", "poster_url", "cover", "coverUrl",
                "cover_url", "coverVertical", "cover_vertical", "verticalCover",
                "coverImage", "cover_image", "image", "imageUrl", "image_url",
                "icon", "logo"}));
        game.bannerUrl = absolute_image_url(
            baseUrl,
            image_from(source, {
                "banner", "bannerUrl", "banner_url", "background",
                "backgroundImage", "background_url", "hero", "heroImage",
                "coverHorizontal", "cover_horizontal", "horizontalCover", "cover",
                "coverUrl", "cover_url"}));
        if (game.posterUrl.empty() && &source != &raw) {
            game.posterUrl = absolute_image_url(
                baseUrl,
                image_from(raw, {
                    "poster", "posterUrl", "poster_url", "cover", "coverUrl",
                    "cover_url", "coverVertical", "cover_vertical", "image",
                    "imageUrl", "image_url", "icon", "logo"}));
        }
        if (game.bannerUrl.empty() && &source != &raw) {
            game.bannerUrl = absolute_image_url(
                baseUrl,
                image_from(raw, {
                    "banner", "bannerUrl", "banner_url", "background",
                    "backgroundImage", "background_url", "coverHorizontal",
                    "cover_horizontal", "cover", "coverUrl", "cover_url"}));
        }
        if (game.posterUrl.empty()) game.posterUrl = game.bannerUrl;
        if (game.bannerUrl.empty()) game.bannerUrl = game.posterUrl;

        game.store = object_label(source, {"store", "storeName"});
        game.platform = object_label(source, {"platform", "platformName"});
        if (game.store.empty()) game.store = object_label(raw, {"store", "storeName"});
        if (game.platform.empty()) {
            game.platform = object_label(raw, {"platform", "platformName"});
        }
        game.isInstalled = true;
        outGames.push_back(std::move(game));
    }
    return true;
}

std::string upstream_error_message(const std::string& body) {
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) return {};
    for (const char* key : {"error_message", "message"}) {
        const auto message = parsed.find(key);
        if (message != parsed.end() && message->is_string()) {
            return message->get<std::string>();
        }
    }
    const auto error = parsed.find("error");
    if (error != parsed.end() && error->is_object()) {
        const auto message = error->find("message");
        if (message != error->end() && message->is_string()) {
            return message->get<std::string>();
        }
    }
    return {};
}

void walk_json(
    const nlohmann::json& value,
    const std::function<void(const nlohmann::json&, const std::string&)>& visit,
    const std::string& key = {},
    int depth = 0) {
    if (depth > 12) return;
    visit(value, key);
    if (value.is_object()) {
        for (auto item = value.begin(); item != value.end(); ++item) {
            walk_json(item.value(), visit, item.key(), depth + 1);
        }
    } else if (value.is_array()) {
        for (const auto& item : value) walk_json(item, visit, key, depth + 1);
    }
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

void push_unique(std::vector<std::string>& values, const std::string& value) {
    if (!value.empty() &&
        std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::string normalize_query(const std::string& raw) {
    std::string value = raw;
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    value.erase(0, first);
    const auto last = value.find_last_not_of(" \t\r\n");
    value.erase(last + 1);

    const auto question = value.find('?');
    if (question != std::string::npos) value.erase(0, question + 1);
    if (!value.empty() && value.front() == '?') value.erase(0, 1);
    if (value.find('=') == std::string::npos) return {};
    const std::string lowered = lower_ascii(value);
    if (lowered.find("sessionid=") == std::string::npos &&
        lowered.find("session=") == std::string::npos) {
        return {};
    }
    return value;
}

std::string query_parameter(const std::string& query, const std::string& name) {
    const std::string wanted = lower_ascii(name);
    std::size_t at = 0;
    while (at <= query.size()) {
        const std::size_t end = query.find('&', at);
        const std::string part = query.substr(
            at, end == std::string::npos ? std::string::npos : end - at);
        const std::size_t equals = part.find('=');
        if (equals != std::string::npos &&
            lower_ascii(part.substr(0, equals)) == wanted) {
            return decode_token(part.substr(equals + 1));
        }
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return {};
}

std::string session_id_from_query(const std::string& query) {
    std::string id = query_parameter(query, "sessionId");
    if (id.empty()) id = query_parameter(query, "sessionid");
    if (id.empty()) id = query_parameter(query, "session");
    return id;
}

void collect_stream_signals(
    const nlohmann::json& payload,
    std::vector<std::string>& tokens,
    std::vector<std::string>& queries,
    std::vector<std::string>& sessionIds) {
    walk_json(payload, [&](const nlohmann::json& value, const std::string& key) {
        const std::string loweredKey = lower_ascii(key);
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if ((loweredKey == "token" || loweredKey == "sessiontoken") &&
                text.size() >= 8) {
                push_unique(tokens, text);
            }
            const std::string query = normalize_query(text);
            if (!query.empty()) {
                push_unique(queries, query);
                push_unique(sessionIds, session_id_from_query(query));
            }
            if (loweredKey == "sessionid" || loweredKey == "sid") {
                push_unique(sessionIds, text);
            }
        } else if ((value.is_number_integer() || value.is_number_unsigned()) &&
                   (loweredKey == "sessionid" || loweredKey == "sid")) {
            push_unique(sessionIds, value.dump());
        }
    });
}

bool parse_server_location(const nlohmann::json& value, ServerLocation& location) {
    if (!value.is_object()) return false;
    location.id = first_positive_int(value, {"id", "playgroundId", "playground_id"});
    location.title = first_string(value, {"title", "name", "label"});
    const auto geo = value.find("location");
    if (geo != value.end() && geo->is_object()) {
        location.country = first_string(*geo, {"country", "countryName", "name"});
    }
    if (location.country.empty()) {
        location.country = first_string(value, {"country", "countryName", "region"});
    }
    const auto active = value.find("active");
    if (active != value.end()) {
        if (active->is_boolean()) location.active = active->get<bool>();
        else if (active->is_number_integer()) location.active = active->get<int>() != 0;
    }
    const std::string status = lower_ascii(first_string(value, {"status", "state"}));
    location.available = location.active &&
        (status.empty() || status == "up" || status == "online" || status == "active");
    return location.id > 0 && !location.title.empty();
}

void collect_server_locations(const nlohmann::json& payload,
                              std::vector<ServerLocation>& locations) {
    walk_json(payload, [&](const nlohmann::json& value, const std::string&) {
        ServerLocation location;
        if (!parse_server_location(value, location)) return;
        const auto duplicate = std::find_if(
            locations.begin(), locations.end(), [&](const ServerLocation& current) {
                return current.id == location.id;
            });
        if (duplicate == locations.end()) locations.push_back(std::move(location));
    });
}

void collect_server_preferences(const nlohmann::json& payload,
                                ServerPreferences& preferences) {
    walk_json(payload, [&](const nlohmann::json& value, const std::string& key) {
        const std::string lowered = lower_ascii(key);
        if ((lowered == "onlymyregion" || lowered == "only_my_region") &&
            value.is_boolean()) {
            preferences.allowDistantRegions = !value.get<bool>();
        }
        if (lowered != "preferredplaygrounds" &&
            lowered != "preferred_playgrounds") return;
        if (value.is_array() && !value.empty()) {
            const auto& first = value.front();
            if (first.is_number_integer() || first.is_number_unsigned()) {
                preferences.preferredLocationId = first.get<int>();
            } else if (first.is_object()) {
                preferences.preferredLocationId = first_positive_int(
                    first, {"id", "playgroundId", "playground_id"});
            }
        }
    });
}

bool collect_realtime_queue_event(
    const nlohmann::json& payload, int appId,
    std::vector<std::string>& tokens,
    std::vector<std::string>& queries,
    std::vector<std::string>& sessionIds,
    std::string& status,
    std::string& failure) {
    bool handled = false;
    walk_json(payload, [&](const nlohmann::json& value, const std::string&) {
        if (handled || !value.is_object()) return;
        if (lower_ascii(first_string(value, {"type"})) != "queues") return;
        const std::string action = lower_ascii(first_string(value, {"action"}));
        const auto eventValue = value.find("value");
        const nlohmann::json& body =
            eventValue != value.end() ? *eventValue : value;
        const int eventAppId = first_positive_int(
            body, {"appId", "app_id", "applicationId"});
        if (eventAppId > 0 && eventAppId != appId) return;

        if (action == "state") {
            const int position = first_positive_int(body, {"position"});
            const int eta = first_positive_int(body, {"eta"});
            status = gnx::i18n::tr("Esperando maquina", "Waiting for a machine");
            if (position > 0) status += " - posicion " + std::to_string(position);
            if (eta > 0) status += " (aprox. " + std::to_string(eta) + " s)";
            handled = true;
            return;
        }
        if (action == "start") {
            collect_stream_signals(body, tokens, queries, sessionIds);
            status = gnx::i18n::tr("Maquina asignada; iniciando sesion...", "Machine assigned; signing in...");
            handled = true;
            return;
        }
        if (action == "unavailable" || action == "removed" ||
            action == "canceled") {
            failure = first_string(body, {"reason", "message", "error"});
            if (failure.empty()) {
                failure = gnx::i18n::tr(
                    "Boosteroid retiro la solicitud de la cola.",
                    "Boosteroid removed the queue request.");
            }
            handled = true;
        }
    });
    return handled;
}

int find_error_code(const nlohmann::json& payload) {
    int result = 0;
    walk_json(payload, [&](const nlohmann::json& value, const std::string& key) {
        if (result || (lower_ascii(key) != "error_code" &&
                       lower_ascii(key) != "errorcode")) return;
        try {
            if (value.is_number_integer() || value.is_number_unsigned()) {
                result = value.get<int>();
            } else if (value.is_string()) {
                result = std::stoi(value.get<std::string>());
            }
        } catch (...) {
        }
    });
    return result;
}

std::string clean_gateway_host(std::string host) {
    const auto scheme = host.find("://");
    if (scheme != std::string::npos) host.erase(0, scheme + 3);
    const auto slash = host.find('/');
    if (slash != std::string::npos) host.erase(slash);
    while (!host.empty() && std::isspace(static_cast<unsigned char>(host.back()))) {
        host.pop_back();
    }
    return host;
}

bool parse_gateway_object(const nlohmann::json& value, StreamGateway& gateway) {
    if (!value.is_object()) return false;
    gateway.id = first_string(value, {"id", "gatewayId", "uuid"});
    gateway.host = clean_gateway_host(first_string(
        value, {"host", "hostname", "address", "url", "gateway", "domain"}));
    gateway.port = first_positive_int(value, {"port", "httpsPort", "wssPort"});
    gateway.region = first_string(
        value, {"region", "regionName", "location", "country", "city"});
    gateway.label = first_string(
        value, {"label", "name", "title", "displayName"});
    if (gateway.host.empty()) return false;
    if (gateway.port > 0 && gateway.host.find(':') == std::string::npos) {
        gateway.host += ":" + std::to_string(gateway.port);
    }
    if (gateway.label.empty()) gateway.label = gateway.region;
    if (gateway.label.empty()) gateway.label = gateway.host;
    return true;
}

void collect_gateways(const nlohmann::json& payload,
                      std::vector<StreamGateway>& gateways) {
    walk_json(payload, [&](const nlohmann::json& value, const std::string&) {
        StreamGateway gateway;
        if (!parse_gateway_object(value, gateway)) return;
        const auto duplicate = std::find_if(
            gateways.begin(), gateways.end(), [&](const StreamGateway& existing) {
                return existing.host == gateway.host;
            });
        if (duplicate == gateways.end()) gateways.push_back(std::move(gateway));
    });
}

nlohmann::json parse_json_response(const gnx::HttpResponse& response) {
    return nlohmann::json::parse(response.body, nullptr, false);
}

bool is_success(const gnx::HttpResponse& response) {
    return response.status >= 200 && response.status < 300;
}

bool cancelled(std::atomic<bool>* flag) {
    return flag && flag->load();
}

bool wait_or_cancel(std::atomic<bool>* flag, int milliseconds) {
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 50) {
        if (cancelled(flag)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(
            std::min(50, milliseconds - elapsed)));
    }
    return !cancelled(flag);
}

}  // namespace

BoosteroidAPI::BoosteroidAPI(const std::string& baseUrl)
    : m_baseUrl(baseUrl), m_deviceId(make_uuid_v4()) {
    while (!m_baseUrl.empty() && m_baseUrl.back() == '/') m_baseUrl.pop_back();
#if defined(__SWITCH__)
    m_sessionFilePath = "sdmc:/switch/ZERODROID/session.json";
    m_lastSessionFilePath = "sdmc:/switch/ZERODROID/last_session.json";
#else
    m_sessionFilePath = "session.json";
    m_lastSessionFilePath = "last_session.json";
#endif
    loadSessionFromSD();
    loadLastSessionFromSD();
}

BoosteroidAPI::~BoosteroidAPI() {}

bool BoosteroidAPI::loadSessionFromSD() {
    std::ifstream file(m_sessionFilePath);
    if (!file.is_open()) return false;

    std::ostringstream content;
    content << file.rdbuf();
    const std::string stored = content.str();
    if (stored.empty()) return false;

    const auto session = nlohmann::json::parse(stored, nullptr, false);
    if (!session.is_discarded() && session.is_object()) {
        m_authToken = json_string(session, "access_token");
        m_refreshToken = json_string(session, "refresh_token");
        m_userDataToken = json_string(session, "user_data");
        const std::string storedDeviceId = json_string(session, "device_id");
        if (!storedDeviceId.empty()) m_deviceId = storedDeviceId;
        return !m_authToken.empty();
    }

    // Compatibility with the original prototype, which stored one token line.
    std::istringstream legacy(stored);
    std::getline(legacy, m_authToken);
    return !m_authToken.empty();
}

bool BoosteroidAPI::saveSessionToSD() {
#if defined(__SWITCH__)
    if (mkdir("sdmc:/switch/ZERODROID", 0777) != 0 && errno != EEXIST) {
        m_lastError = gnx::i18n::tr("No se pudo crear la carpeta de sesion en la SD.", "Could not create the session folder on the SD card.");
        return false;
    }
#endif
    std::ofstream file(m_sessionFilePath);
    if (!file.is_open()) {
        m_lastError = gnx::i18n::tr("No se pudo guardar la sesion en la SD.", "Could not save the session to the SD card.");
        return false;
    }

    const nlohmann::json session = {
        {"version", 2},
        {"access_token", m_authToken},
        {"refresh_token", m_refreshToken},
        {"user_data", m_userDataToken},
        {"device_id", m_deviceId},
    };
    file << session.dump(2);
    return file.good();
}

void BoosteroidAPI::loadLastSessionFromSD() {
    m_lastSession = {};
    std::ifstream file(m_lastSessionFilePath);
    if (!file.is_open()) return;
    std::ostringstream content;
    content << file.rdbuf();
    const auto stored = nlohmann::json::parse(content.str(), nullptr, false);
    if (stored.is_discarded() || !stored.is_object()) return;
    m_lastSession.sessionId = json_string(stored, "sessionId");
    m_lastSession.appId = stored.value("appId", 0);
    m_lastSession.usedAt = stored.value("usedAt", static_cast<int64_t>(0));
}

bool BoosteroidAPI::saveLastSessionToSD() {
#if defined(__SWITCH__)
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/ZERODROID", 0777);
#endif
    std::ofstream file(m_lastSessionFilePath);
    if (!file.is_open()) return false;
    file << nlohmann::json({
        {"sessionId", m_lastSession.sessionId},
        {"appId", m_lastSession.appId},
        {"usedAt", m_lastSession.usedAt},
    }).dump(2);
    return file.good();
}

bool BoosteroidAPI::requestDeviceCode(DeviceCodeAuthArtifact& outArtifact) {
    m_lastError.clear();
    outArtifact.userCode.clear();
    outArtifact.deviceCode = make_uuid_v4();
    if (outArtifact.deviceCode.empty()) {
        m_lastError = gnx::i18n::tr("No se pudo generar el codigo QR.", "Could not generate the QR code.");
        return false;
    }
    outArtifact.verificationUrl =
        m_baseUrl + kQrValidationEndpoint + outArtifact.deviceCode;
    outArtifact.qrUrl = outArtifact.verificationUrl;
    outArtifact.expiresInSeconds = 300;
    outArtifact.intervalSeconds = 3;
    return true;
}

DeviceCodePollResult BoosteroidAPI::pollDeviceCodeStatus(
    const std::string& deviceCode,
    std::string& outAuthToken) {
    outAuthToken.clear();
    if (deviceCode.empty()) {
        m_lastError = gnx::i18n::tr("El codigo QR no es valido.", "The QR code is not valid.");
        return DeviceCodePollResult::Error;
    }

    try {
        gnx::Http http;
        const nlohmann::json request = {
            {"auth-code", deviceCode},
            {"clientId", kAndroidTvClientId},
        };
        const auto response = http.post(
            m_baseUrl + kQrSyncEndpoint,
            request.dump(),
            android_tv_headers(true, {}, {}, m_deviceId));

        // The official TV client receives 401 while the phone has not yet
        // approved the UUID. It is a normal polling state, not a login error.
        if (response.status == 401 || response.status == 202 || response.status == 204) {
            m_lastError.clear();
            return DeviceCodePollResult::Pending;
        }
        if (response.status == 422) {
            m_lastError = gnx::i18n::tr("El codigo QR vencio o ya no es valido.", "The QR code expired or is no longer valid.");
            return DeviceCodePollResult::Expired;
        }
        if (response.status != 200) {
            m_lastError = upstream_error_message(response.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("Boosteroid respondio HTTP ", "Boosteroid responded HTTP ") + std::to_string(response.status) + ".";
            }
            return DeviceCodePollResult::Error;
        }

        const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) {
            m_lastError = gnx::i18n::tr("Boosteroid devolvio una respuesta de login no valida.", "Boosteroid returned an invalid login response.");
            return DeviceCodePollResult::Error;
        }
        const nlohmann::json& payload = *unwrap_payload(parsed);
        const std::string accessToken = json_string(payload, "access_token");
        const std::string refreshToken = json_string(payload, "refresh_token");
        if (accessToken.empty() || refreshToken.empty()) {
            m_lastError = gnx::i18n::tr("El QR fue aceptado, pero faltan los tokens de sesion.", "The QR code was accepted, but the session tokens are missing.");
            return DeviceCodePollResult::Error;
        }

        m_authToken = accessToken;
        m_refreshToken = refreshToken;
        m_userDataToken = auth_data_token(payload);
        outAuthToken = accessToken;
        if (!saveSessionToSD()) return DeviceCodePollResult::Error;
        m_lastError.clear();
        return DeviceCodePollResult::Succeeded;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return DeviceCodePollResult::Error;
    } catch (...) {
        m_lastError = gnx::i18n::tr("Fallo desconocido al consultar el inicio de sesion.", "Unknown failure while checking sign-in.");
        return DeviceCodePollResult::Error;
    }
}

bool BoosteroidAPI::setAuthToken(const std::string& token) {
    m_authToken = token;
    saveSessionToSD();
    return !m_authToken.empty();
}

bool BoosteroidAPI::isAuthenticated() const {
    return !m_authToken.empty();
}

void BoosteroidAPI::logout() {
    m_authToken.clear();
    m_refreshToken.clear();
    m_userDataToken.clear();
    m_lastError.clear();
    m_currentUser = BoosteroidUser();
    remove(m_sessionFilePath.c_str());
    m_lastSession = {};
    remove(m_lastSessionFilePath.c_str());
}

bool BoosteroidAPI::getUserProfile(BoosteroidUser& outUser) {
    m_lastError.clear();
    if (!isAuthenticated()) {
        m_lastError = gnx::i18n::tr("No hay una sesion de Boosteroid activa.", "There is no active Boosteroid session.");
        return false;
    }

    try {
        gnx::Http http;
        const auto headers = android_tv_headers(
            false, m_authToken, m_userDataToken, m_deviceId);
        const auto response = http.get(m_baseUrl + "/api/v1/user", headers);
        if (response.status != 200) {
            m_lastError = upstream_error_message(response.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("El perfil respondio HTTP ", "The profile responded HTTP ") +
                              std::to_string(response.status) + ".";
            }
            return false;
        }

        const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) {
            m_lastError = gnx::i18n::tr("Boosteroid devolvio un perfil no valido.", "Boosteroid returned an invalid profile.");
            return false;
        }
        const nlohmann::json& user = *unwrap_payload(parsed);
        outUser.id = json_string(user, "id");
        outUser.email = json_string(user, "email");
        outUser.nickname = json_string(user, "nickname");
        if (outUser.nickname.empty()) outUser.nickname = json_string(user, "name");
        outUser.avatarUrl = json_string(user, "avatar_url");
        if (outUser.avatarUrl.empty()) outUser.avatarUrl = json_string(user, "avatar");
        m_currentUser = outUser;
        m_lastError.clear();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return false;
    } catch (...) {
        m_lastError = gnx::i18n::tr("Fallo desconocido al cargar el perfil.", "Unknown failure while loading the profile.");
        return false;
    }
}

bool BoosteroidAPI::getInstalledGames(std::vector<GameItem>& outGames) {
    outGames.clear();
    m_lastError.clear();
    if (!isAuthenticated()) {
        m_lastError = gnx::i18n::tr("No hay una sesion de Boosteroid activa.", "There is no active Boosteroid session.");
        return false;
    }

    try {
        gnx::Http http;
        const auto headers = android_tv_headers(
            false, m_authToken, m_userDataToken, m_deviceId);
        const auto response = http.get(
            m_baseUrl +
                "/api/v1/boostore/applications/installed?page=1&paginate=50",
            headers);
        std::fprintf(stderr, "Library HTTP %ld (%zu bytes)\n",
                     response.status, response.body.size());
        if (response.status != 200) {
            m_lastError = upstream_error_message(response.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("La biblioteca respondio HTTP ", "The library responded HTTP ") +
                              std::to_string(response.status) + ".";
            }
            return false;
        }

        const auto parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (parsed.is_discarded()) {
            m_lastError = gnx::i18n::tr("Boosteroid devolvio una biblioteca no valida.", "Boosteroid returned an invalid library.");
            return false;
        }
        if (!parse_games(parsed, m_baseUrl, outGames)) {
            m_lastError = gnx::i18n::tr("No se reconocio el formato de la biblioteca de Boosteroid.", "The Boosteroid library format was not recognized.");
            return false;
        }
        std::fprintf(stderr, "Library parsed: %zu games\n", outGames.size());
        m_lastError.clear();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return false;
    } catch (...) {
        m_lastError = gnx::i18n::tr("Fallo desconocido al cargar la biblioteca.", "Unknown failure while loading the library.");
        return false;
    }
}

bool BoosteroidAPI::getBoostoreGames(const std::string& query, std::vector<GameItem>& outGames) {
    outGames.clear();
    gnx::Http http;
    const auto headers = android_tv_headers(
        false, m_authToken, m_userDataToken, m_deviceId);

    std::string url = m_baseUrl + "/api/v1/boostore/applications/search?query=" + gnx::Http::urlencode(query);
    auto response = http.get(url, headers);
    return (response.status == 200);
}

bool BoosteroidAPI::getStreamingGateways(
    std::vector<StreamGateway>& outGateways) {
    outGateways.clear();
    m_lastError.clear();
    if (!isAuthenticated()) {
        m_lastError = gnx::i18n::tr("No hay una sesion de Boosteroid activa.", "There is no active Boosteroid session.");
        return false;
    }

    try {
        gnx::Http http;
        const auto response = http.get(
            m_baseUrl + "/api/v1/streaming/gateways",
            android_tv_headers(false, m_authToken, m_userDataToken, m_deviceId));
        if (!is_success(response)) {
            m_lastError = upstream_error_message(response.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("La lista de servidores respondio HTTP ", "The server list responded HTTP ") +
                              std::to_string(response.status) + ".";
            }
            return false;
        }
        const auto parsed = parse_json_response(response);
        if (parsed.is_discarded()) {
            m_lastError = gnx::i18n::tr("Boosteroid devolvio una lista de servidores no valida.", "Boosteroid returned an invalid server list.");
            return false;
        }
        collect_gateways(parsed, outGateways);
        if (outGateways.empty()) {
            m_lastError = gnx::i18n::tr("Boosteroid no devolvio gateways disponibles.", "Boosteroid returned no available gateways.");
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return false;
    }
}

bool BoosteroidAPI::getServerConfiguration(
    std::vector<ServerLocation>& outLocations,
    ServerPreferences& outPreferences) {
    outLocations.clear();
    outPreferences = ServerPreferences();
    m_lastError.clear();
    if (!isAuthenticated()) {
        m_lastError = gnx::i18n::tr("No hay una sesion de Boosteroid activa.", "There is no active Boosteroid session.");
        return false;
    }

    try {
        gnx::Http http;
        const auto headers = android_tv_headers(
            false, m_authToken, m_userDataToken, m_deviceId);
        const auto userResponse = http.get(m_baseUrl + "/api/v1/user", headers);
        if (!is_success(userResponse)) {
            m_lastError = gnx::i18n::tr("No se pudieron leer las preferencias de la cuenta (HTTP ", "Could not read account preferences (HTTP ") +
                          std::to_string(userResponse.status) + ").";
            return false;
        }
        const auto userJson = parse_json_response(userResponse);
        if (!userJson.is_discarded()) {
            collect_server_preferences(userJson, outPreferences);
        }

        const auto locationsResponse = http.get(
            m_baseUrl + "/api/v1/streaming/playgrounds", headers);
        if (!is_success(locationsResponse)) {
            m_lastError = gnx::i18n::tr("No se pudieron cargar las ubicaciones (HTTP ", "Could not load locations (HTTP ") +
                          std::to_string(locationsResponse.status) + ").";
            return false;
        }
        const auto locationsJson = parse_json_response(locationsResponse);
        if (locationsJson.is_discarded()) {
            m_lastError = gnx::i18n::tr("Boosteroid devolvio ubicaciones no validas.", "Boosteroid returned invalid locations.");
            return false;
        }
        collect_server_locations(locationsJson, outLocations);
        outLocations.erase(std::remove_if(
            outLocations.begin(), outLocations.end(), [&](const ServerLocation& value) {
                return !value.available && value.id != outPreferences.preferredLocationId;
            }), outLocations.end());
        std::sort(outLocations.begin(), outLocations.end(),
                  [](const ServerLocation& left, const ServerLocation& right) {
            return left.title < right.title;
        });
        if (outLocations.empty()) {
            m_lastError = gnx::i18n::tr("Boosteroid no devolvio ubicaciones de servidor disponibles.", "Boosteroid returned no available server locations.");
            return false;
        }
        m_lastError.clear();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return false;
    }
}

bool BoosteroidAPI::updateServerConfiguration(
    const ServerPreferences& preferences) {
    m_lastError.clear();
    if (!isAuthenticated()) {
        m_lastError = gnx::i18n::tr("No hay una sesion de Boosteroid activa.", "There is no active Boosteroid session.");
        return false;
    }

    try {
        gnx::Http http;
        const auto headers = android_tv_headers(
            true, m_authToken, m_userDataToken, m_deviceId);
        const auto distantResponse = http.patch(
            m_baseUrl + "/api/v1/user/update/streaming-regions",
            nlohmann::json({
                {"onlyMyRegion", !preferences.allowDistantRegions}}).dump(),
            headers);
        if (!is_success(distantResponse)) {
            m_lastError = upstream_error_message(distantResponse.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("No se pudo guardar regiones lejanas (HTTP ", "Could not save distant regions (HTTP ") +
                              std::to_string(distantResponse.status) + ").";
            }
            return false;
        }

        nlohmann::json preferred = nlohmann::json::array();
        if (preferences.preferredLocationId > 0) {
            preferred.push_back(preferences.preferredLocationId);
        }
        const auto preferredResponse = http.patch(
            m_baseUrl + "/api/v1/user/update/preferred-playgrounds",
            nlohmann::json({{"preferredPlaygrounds", preferred}}).dump(),
            headers);
        if (!is_success(preferredResponse)) {
            m_lastError = upstream_error_message(preferredResponse.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("No se pudo guardar la ubicacion (HTTP ", "Could not save the location (HTTP ") +
                              std::to_string(preferredResponse.status) + ").";
            }
            return false;
        }
        m_lastError.clear();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return false;
    }
}

bool BoosteroidAPI::startStreamingSession(
    int appId, StreamSessionConfig& outConfig,
    std::atomic<bool>* cancelFlag,
    const std::function<void(const std::string&)>& log) {
    return startStreamingSessionImpl(appId, outConfig, cancelFlag, true, log);
}

bool BoosteroidAPI::tryResumeLastSession(
    int appId, StreamSessionConfig& outConfig,
    std::atomic<bool>* cancelFlag,
    const std::function<void(const std::string&)>& log) {
    if (m_lastSession.sessionId.empty() || m_lastSession.appId != appId ||
        m_lastSession.usedAt <= 0) {
        return false;
    }
    const int64_t now = now_seconds();
    if (now - m_lastSession.usedAt > 15 * 60) return false;

    const auto report = [&](const std::string& message) {
        if (log) log(message);
    };
    report(gnx::i18n::tr("Intentando recuperar la sesion anterior...",
                         "Trying to resume the previous session..."));

    gnx::Http http;
    http.set_abort_flag(cancelFlag);
    const auto headers = android_tv_headers(
        false, m_authToken, m_userDataToken, m_deviceId);
    std::string assignedGateway;
    std::string signedQuery;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!cancelled(cancelFlag) &&
           std::chrono::steady_clock::now() < deadline) {
        try {
            const auto response = http.post(
                m_baseUrl + "/api/v1/streaming/session/details?session=" +
                    gnx::Http::urlencode(m_lastSession.sessionId),
                "", headers);
            if (is_success(response)) {
                const auto parsed = parse_json_response(response);
                if (!parsed.is_discarded()) {
                    const nlohmann::json& details = *unwrap_payload(parsed);
                    assignedGateway = clean_gateway_host(
                        first_string(details, {"gw"}));
                    signedQuery = first_string(
                        details, {"queryString", "query_string"});
                }
                if (!signedQuery.empty() && !assignedGateway.empty()) break;
            } else if (response.status >= 400 && response.status < 500) {
                break;  // session expired or belongs to another machine
            }
        } catch (...) {
        }
        if (!wait_or_cancel(cancelFlag, 500)) return false;
    }
    if (cancelled(cancelFlag)) return false;
    if (signedQuery.empty() || assignedGateway.empty()) {
        report(gnx::i18n::tr(
            "No se pudo recuperar la sesion anterior. Iniciando una nueva...",
            "Could not resume the previous session. Starting a new one..."));
        m_lastError.clear();
        return false;
    }

    StreamSessionConfig resumed;
    resumed.sessionId = m_lastSession.sessionId;
    resumed.homeUrl = m_baseUrl;
    resumed.assignedGateway = std::move(assignedGateway);
    resumed.signedQuery = std::move(signedQuery);
    resumed.accessToken = m_authToken;
    resumed.authDataToken = m_userDataToken;
    resumed.preferredCodec = "h264";
    outConfig = std::move(resumed);
    m_lastSession.usedAt = now_seconds();
    saveLastSessionToSD();
    report(gnx::i18n::tr("Sesion anterior recuperada.",
                         "Previous session resumed."));
    m_lastError.clear();
    return true;
}

bool BoosteroidAPI::startStreamingSessionImpl(
    int appId, StreamSessionConfig& outConfig,
    std::atomic<bool>* cancelFlag, bool allowRequeue,
    const std::function<void(const std::string&)>& log) {
    outConfig = StreamSessionConfig();
    m_lastError.clear();
    if (!isAuthenticated()) {
        m_lastError = gnx::i18n::tr("No hay una sesion de Boosteroid activa.", "There is no active Boosteroid session.");
        return false;
    }

    if (tryResumeLastSession(appId, outConfig, cancelFlag, log)) {
        return true;
    }
    if (cancelled(cancelFlag)) return false;

    try {
        const auto report = [&](const std::string& message) {
            if (log) log(message);
        };
        gnx::Http http;
        http.set_abort_flag(cancelFlag);
        const auto headers = android_tv_headers(
            true, m_authToken, m_userDataToken, m_deviceId);
        if (m_currentUser.id.empty()) {
            BoosteroidUser profile;
            if (!getUserProfile(profile)) {
                report(gnx::i18n::tr("No se pudo resolver uid para la cola en tiempo real.", "Could not resolve uid for the realtime queue."));
                m_lastError.clear();
            }
        }

#if defined(__SWITCH__)
        gnx::stream::WssClient realtime;
        bool realtimeReady = false;
        if (!m_currentUser.id.empty()) {
            const std::string host = websocket_host(m_baseUrl);
            const std::string token = realtime_access_token(m_authToken);
            const std::string path = "/ws?uid=" +
                gnx::Http::urlencode(m_currentUser.id) + "&token=" +
                gnx::Http::urlencode(token);
            std::string socketError;
            report(gnx::i18n::tr("Conectando a la cola en tiempo real...", "Connecting to the realtime queue..."));
            realtimeReady = realtime.connect(
                host, path, "romfs:/cacert.pem", socketError);
            if (realtimeReady) {
                report(gnx::i18n::tr("Cola en tiempo real conectada.", "Realtime queue connected."));
            } else {
                report(gnx::i18n::tr("Cola en tiempo real no disponible: ", "Realtime queue unavailable: ") + socketError);
            }
        }
#endif

        const std::string appBody = nlohmann::json({{"appId", appId}}).dump();
        report(gnx::i18n::tr("Solicitando maquina para la Switch...", "Requesting a machine for the Switch..."));
        auto enqueue = http.post(
            m_baseUrl + "/api/v2/streaming/session/enqueue", appBody, headers);
        report(gnx::i18n::tr("Solicitud de cola HTTP ", "Queue request HTTP ") + std::to_string(enqueue.status) + ".");
        if (!is_success(enqueue)) {
            m_lastError = upstream_error_message(enqueue.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("No se pudo entrar en la cola (HTTP ", "Could not enter the queue (HTTP ") +
                              std::to_string(enqueue.status) + ").";
            }
            return false;
        }

        auto enqueueJson = parse_json_response(enqueue);
        if (enqueueJson.is_discarded()) enqueueJson = nlohmann::json::object();
        std::vector<std::string> tokens;
        std::vector<std::string> queries;
        std::vector<std::string> sessionIds;
        collect_stream_signals(enqueueJson, tokens, queries, sessionIds);
        const auto hasReadyQuery = [&]() {
            return std::any_of(queries.begin(), queries.end(),
                               [](const std::string& query) {
                return query.find('&') != std::string::npos &&
                       !session_id_from_query(query).empty();
            });
        };

        // VM allocation is asynchronous. Wait in the queue for as long as the
        // user keeps the launch panel open; only cancellation ends the wait.
        auto nextRestPoll = std::chrono::steady_clock::now();
        auto nextPing = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
        std::string lastQueueStatus;
        while (!cancelled(cancelFlag) && tokens.empty() && !hasReadyQuery()) {
#if defined(__SWITCH__)
            if (realtimeReady && realtime.connected()) {
                std::string message;
                std::string socketError;
                if (realtime.read_text(message, 100, socketError)) {
                    const auto parsed = nlohmann::json::parse(
                        message, nullptr, false);
                    if (!parsed.is_discarded()) {
                        std::string queueStatus;
                        std::string queueFailure;
                        collect_realtime_queue_event(
                            parsed, appId, tokens, queries, sessionIds,
                            queueStatus, queueFailure);
                        if (!queueStatus.empty() && queueStatus != lastQueueStatus) {
                            lastQueueStatus = queueStatus;
                            report(queueStatus);
                        }
                        if (!queueFailure.empty()) {
                            m_lastError = queueFailure;
                            return false;
                        }
                    }
                } else if (!socketError.empty()) {
                    report(gnx::i18n::tr("Se perdio la cola en tiempo real: ", "Lost the realtime queue: ") + socketError);
                    realtimeReady = false;
                }
            }
            if (realtimeReady && realtime.connected() &&
                std::chrono::steady_clock::now() >= nextPing) {
                realtime.send_text(nlohmann::json({{"type", "ping"}}).dump());
                nextPing = std::chrono::steady_clock::now() +
                           std::chrono::seconds(5);
            }
#endif
            if (std::chrono::steady_clock::now() >= nextRestPoll) {
                for (const char* endpoint : {
                         "/api/v1/streaming/user/last-session/live",
                         "/api/v1/streaming/user/last-session",
                         "/api/v1/streaming/user/active-sessions"}) {
                    try {
                        const auto response = http.get(
                            m_baseUrl + endpoint,
                            android_tv_headers(
                                false, m_authToken, m_userDataToken, m_deviceId));
                        if (!is_success(response)) continue;
                        const auto parsed = parse_json_response(response);
                        if (!parsed.is_discarded()) {
                            collect_stream_signals(
                                parsed, tokens, queries, sessionIds);
                        }
                    } catch (...) {
                    }
                }
                nextRestPoll = std::chrono::steady_clock::now() +
                               std::chrono::seconds(2);
            }
            if (!tokens.empty() || hasReadyQuery()) break;
            if (!wait_or_cancel(cancelFlag, 50)) return false;
        }

        if (cancelled(cancelFlag)) return false;
        report(gnx::i18n::tr("Cola completada: tokens=", "Queue completed: tokens=") + std::to_string(tokens.size()) +
               " consultas=" + std::to_string(queries.size()) + ".");

        nlohmann::json startJson = nlohmann::json::object();
        bool started = false;
        bool useV1 = false;
        for (const std::string& token : tokens) {
            report(gnx::i18n::tr("Confirmando la maquina asignada...", "Confirming the assigned machine..."));
            const std::string startBody = nlohmann::json({
                {"appId", appId}, {"sessionToken", token}}).dump();
            const auto response = http.post(
                m_baseUrl + "/api/v2/streaming/session/start",
                startBody, headers);
            const auto parsed = parse_json_response(response);
            const int errorCode = parsed.is_discarded() ? 0 : find_error_code(parsed);
            report(gnx::i18n::tr("Inicio de sesion HTTP ", "Session start HTTP ") + std::to_string(response.status) +
                   gnx::i18n::tr(" codigo=", " code=") + std::to_string(errorCode) + ".");
            if (is_success(response)) {
                startJson = parsed.is_discarded() ? nlohmann::json::object() : parsed;
                started = true;
                break;
            }
            if (errorCode == 340005 && allowRequeue) {
                try {
                    http.post(
                        m_baseUrl + "/api/v2/streaming/session/dequeue", "{}",
                        headers);
                } catch (...) {
                }
                if (cancelled(cancelFlag)) return false;
                return startStreamingSessionImpl(
                    appId, outConfig, cancelFlag, false, log);
            }
            if (errorCode == 340006) {
                useV1 = true;
                break;
            }
            if (errorCode == 340007 || response.status == 400) continue;
            m_lastError = upstream_error_message(response.body);
            if (m_lastError.empty()) {
                m_lastError = gnx::i18n::tr("Boosteroid rechazo el inicio (HTTP ", "Boosteroid rejected the start (HTTP ") +
                              std::to_string(response.status) + ").";
            }
            return false;
        }

        // Older accounts/rollouts still advertise the v1 launch operation.
        if (!started && useV1) {
            report(gnx::i18n::tr("La cuenta solicito el inicio compatible v1.", "The account requested the v1-compatible start."));
            const auto response = http.post(
                m_baseUrl + "/api/v1/streaming/session/start", appBody, headers);
            if (is_success(response)) {
                startJson = parse_json_response(response);
                if (startJson.is_discarded()) startJson = nlohmann::json::object();
                started = true;
            }
        }
        if (!started && queries.empty()) {
            m_lastError = gnx::i18n::tr("Boosteroid no confirmo la maquina asignada.", "Boosteroid did not confirm the assigned machine.");
            return false;
        }

        collect_stream_signals(startJson, tokens, queries, sessionIds);
        std::string sessionId;
        walk_json(startJson, [&](const nlohmann::json& value,
                                 const std::string& key) {
            if (!sessionId.empty() || !value.is_string()) return;
            const std::string lowered = lower_ascii(key);
            if (lowered == "sessionid" || lowered == "sid") {
                sessionId = value.get<std::string>();
            }
        });
        if (sessionId.empty()) {
            for (const std::string& query : queries) {
                sessionId = session_id_from_query(query);
                if (!sessionId.empty()) break;
            }
        }
        if (sessionId.empty() && !sessionIds.empty()) sessionId = sessionIds.front();
        if (sessionId.empty()) {
            m_lastError = gnx::i18n::tr("Boosteroid inicio la maquina, pero no devolvio sessionId.", "Boosteroid started the machine but returned no sessionId.");
            return false;
        }
        report(gnx::i18n::tr("Sesion creada; obteniendo gateway firmado...", "Session created; obtaining signed gateway..."));

        collect_gateways(startJson, outConfig.gateways);
        if (outConfig.gateways.empty()) {
            getStreamingGateways(outConfig.gateways);
            m_lastError.clear();
        }

        // Android TV calls this exact Retrofit operation as an empty POST with
        // a query parameter named "session".  Its response pairs `gw` with
        // `queryString`; never mix that signature with a generic gateway.
        std::string assignedGateway;
        std::string signedQuery;
        const auto detailsDeadline = std::chrono::steady_clock::now() +
                                     std::chrono::seconds(60);
        while (!cancelled(cancelFlag) &&
               std::chrono::steady_clock::now() < detailsDeadline) {
            try {
                const auto response = http.post(
                    m_baseUrl +
                        "/api/v1/streaming/session/details?session=" +
                        gnx::Http::urlencode(sessionId),
                    "", android_tv_headers(
                        false, m_authToken, m_userDataToken, m_deviceId));
                if (is_success(response)) {
                    const auto parsed = parse_json_response(response);
                    if (!parsed.is_discarded()) {
                        const nlohmann::json& details = *unwrap_payload(parsed);
                        assignedGateway = clean_gateway_host(
                            first_string(details, {"gw"}));
                        signedQuery = first_string(
                            details, {"queryString", "query_string"});
                        collect_stream_signals(parsed, tokens, queries, sessionIds);
                        std::vector<StreamGateway> detailsGateways;
                        collect_gateways(parsed, detailsGateways);
                        if (!detailsGateways.empty()) {
                            outConfig.gateways = std::move(detailsGateways);
                        }
                    }
                }
                report(gnx::i18n::tr("Detalles de streaming HTTP ", "Streaming details HTTP ") +
                       std::to_string(response.status) + " gw=" +
                       (assignedGateway.empty() ? "ausente" : assignedGateway) +
                       " firmaBytes=" + std::to_string(signedQuery.size()) + ".");
            } catch (...) {
            }
            if (!signedQuery.empty() &&
                (!assignedGateway.empty() || !outConfig.gateways.empty())) break;
            if (!wait_or_cancel(cancelFlag, 2000)) return false;
        }

        if (cancelled(cancelFlag)) return false;

        outConfig.sessionId = sessionId;
        outConfig.homeUrl = m_baseUrl;
        outConfig.assignedGateway = std::move(assignedGateway);
        outConfig.signedQuery = std::move(signedQuery);
        outConfig.sessionQueries = std::move(queries);
        outConfig.accessToken = m_authToken;
        outConfig.authDataToken = m_userDataToken;
        outConfig.preferredCodec = "h264";
        if (outConfig.signedQuery.empty()) {
            m_lastError = gnx::i18n::tr("La maquina esta lista, pero falta la firma del gateway.", "The machine is ready, but the gateway signature is missing.");
            return false;
        }
        if (outConfig.assignedGateway.empty() && outConfig.gateways.empty()) {
            m_lastError = gnx::i18n::tr("La maquina esta lista, pero Boosteroid no devolvio su gateway.", "The machine is ready, but Boosteroid returned no gateway.");
            return false;
        }
        m_lastSession.sessionId = outConfig.sessionId;
        m_lastSession.appId = appId;
        m_lastSession.usedAt = now_seconds();
        saveLastSessionToSD();
        report(gnx::i18n::tr("Gateway firmado listo; preparando WebRTC...", "Signed gateway ready; preparing WebRTC..."));
        m_lastError.clear();
        return true;
    } catch (const std::exception& error) {
        m_lastError = error.what();
        return false;
    }
}

bool BoosteroidAPI::stopStreamingSession(const std::string& sessionId) {
    if (sessionId.empty()) return false;
    if (sessionId == m_lastSession.sessionId) {
        m_lastSession.usedAt = now_seconds();
        saveLastSessionToSD();
    }
    gnx::Http http;
    const auto headers = android_tv_headers(
        true, m_authToken, m_userDataToken, m_deviceId);
    const auto response = http.post(
        m_baseUrl + "/api/v2/streaming/session/dequeue", "{}", headers);
    return is_success(response);
}

} // namespace ZERODROID
