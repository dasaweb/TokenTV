// SPDX-License-Identifier: GPL-3.0-or-later
#include "web/ClaudeApi.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <cmath>

#include "claude/ClaudeScreen.h"
#include "claude/ClaudeUsage.h"
#include "claude/CrashLog.h"
#include "config/ConfigManager.h"
#include "display/Backlight.h"
#include "web/Api.h"
#include "web/ClaudePage.h"
#include "web/IndexPage.h"

extern ConfigManager configManager;

namespace {
void sendJson(Webserver* webserver, int code, const JsonDocument& doc) {
    String json;
    serializeJson(doc, json);
    setCorsHeaders(webserver);
    webserver->raw().send(code, "application/json", json);
}

void sendError(Webserver* webserver, int code, const String& message) {
    JsonDocument doc;
    doc["status"] = "error";
    doc["message"] = message;
    sendJson(webserver, code, doc);
}

auto parseBody(Webserver* webserver, JsonDocument& doc) -> bool {
    if (!webserver->raw().hasArg("plain")) {
        return false;
    }
    return !deserializeJson(doc, webserver->raw().arg("plain"));
}

void putLimit(JsonObject target, const ClaudeLimit& limit) {
    target["valid"] = limit.valid;
    target["percent"] = limit.percent;
    target["resets_at"] = static_cast<uint32_t>(limit.resetsAt);
    target["warning"] = limit.warning;
}

void putBrightness(JsonDocument& doc) {
    doc["brightness"] = configManager.brightness;
    doc["brightness_night"] = configManager.brightness_night;
    doc["brightness_auto"] = configManager.brightness_auto;
    doc["latitude"] = configManager.latitude;
    doc["longitude"] = configManager.longitude;
    doc["brightness_now"] = Backlight::percent();
    time_t sunrise = 0;
    time_t sunset = 0;
    if (Backlight::sunTimes(sunrise, sunset)) {
        doc["sunrise"] = static_cast<uint32_t>(sunrise);
        doc["sunset"] = static_cast<uint32_t>(sunset);
    }
}

auto validPercent(JsonVariantConst value) -> bool {
    return value.isNull() ||
           (value.is<int>() && value.as<int>() >= 0 && value.as<int>() <= Backlight::MAX_PERCENT);
}

auto validDegrees(JsonVariantConst value, float limit) -> bool {
    return value.isNull() || (value.is<float>() && std::fabs(value.as<float>()) <= limit);
}

void handleClaudePage(Webserver* webserver) { webserver->raw().send_P(HTTP_CODE_OK, "text/html", CLAUDE_PAGE_HTML); }

void handleIndexPage(Webserver* webserver) { webserver->raw().send_P(HTTP_CODE_OK, "text/html", INDEX_PAGE_HTML); }

void handleClaudeStatus(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    const ClaudeUsageData& usage = ClaudeUsage::data();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["state"] = ClaudeUsage::stateName();
    doc["logged_in"] = ClaudeUsage::isLoggedIn();
    doc["login_pending"] = ClaudeUsage::loginPending();
    doc["login_error"] = ClaudeUsage::loginError();
    doc["error"] = ClaudeUsage::lastError();
    doc["scope"] = ClaudeUsage::scope();
    doc["token_expires_at"] = static_cast<uint32_t>(ClaudeUsage::tokenExpiresAt());
    doc["fetched_at"] = static_cast<uint32_t>(usage.fetchedAt);
    doc["now"] = static_cast<uint32_t>(time(nullptr));
    putLimit(doc["session"].to<JsonObject>(), usage.session);
    putLimit(doc["weekly"].to<JsonObject>(), usage.weekly);
    doc["free_heap"] = ESP.getFreeHeap();             // NOLINT(readability-static-accessed-through-instance)
    doc["max_free_block"] = ESP.getMaxFreeBlockSize();  // NOLINT(readability-static-accessed-through-instance)
    doc["has_refresh_token"] = ClaudeUsage::hasRefreshToken();
    doc["layout"] = ClaudeScreen::layout();
    doc["pace"] = ClaudeScreen::paceMarker();
    putBrightness(doc);
    doc["uptime_s"] = millis() / 1000;
    doc["reset_reason"] = ESP.getResetReason();  // NOLINT(readability-static-accessed-through-instance)
    doc["reset_info"] = ESP.getResetInfo();      // NOLINT(readability-static-accessed-through-instance)
    CrashLog::toJson(doc["last_crash"].to<JsonObject>());
    sendJson(webserver, HTTP_CODE_OK, doc);
}

void handleClaudeLoginStart(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument body;
    parseBody(webserver, body);
    const String scope = body["scope"] | "";

    JsonDocument doc;
    doc["status"] = "ok";
    doc["url"] = ClaudeUsage::beginLogin(scope);
    sendJson(webserver, HTTP_CODE_OK, doc);
}

void handleClaudeLoginFinish(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    JsonDocument body;
    if (!parseBody(webserver, body) || !body["code"].is<const char*>()) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, "Missing code");
        return;
    }
    String error;
    if (!ClaudeUsage::finishLogin(body["code"].as<String>(), error)) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, error);
        return;
    }
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Code accepted, exchanging";
    sendJson(webserver, HTTP_CODE_OK, doc);
}

void handleClaudeLogout(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    ClaudeUsage::logout();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Logged out";
    sendJson(webserver, HTTP_CODE_OK, doc);
}

void handleClaudeRefresh(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    ClaudeUsage::requestFetch();
    JsonDocument doc;
    doc["status"] = "ok";
    doc["message"] = "Fetch scheduled";
    sendJson(webserver, HTTP_CODE_OK, doc);
}
void handleClaudeDisplay(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    constexpr float MAX_LATITUDE = 90.0F;
    constexpr float MAX_LONGITUDE = 180.0F;
    JsonDocument body;
    parseBody(webserver, body);
    const JsonVariantConst layout = body["layout"];
    const JsonVariantConst pace = body["pace"];
    const JsonVariantConst brightness = body["brightness"];
    const JsonVariantConst night = body["brightness_night"];
    const JsonVariantConst autoMode = body["brightness_auto"];
    const JsonVariantConst latitude = body["latitude"];
    const JsonVariantConst longitude = body["longitude"];
    if (layout.isNull() && pace.isNull() && brightness.isNull() && night.isNull() && autoMode.isNull() &&
        latitude.isNull() && longitude.isNull()) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, "Expected layout, pace, brightness and/or location");
        return;
    }
    // Validate everything first so a bad request changes nothing
    const char* invalid = nullptr;
    if (!layout.isNull() &&
        !(layout.is<int>() && layout.as<int>() >= 0 && layout.as<int>() < ClaudeScreen::LAYOUT_COUNT)) {
        invalid = "Unknown layout";
    } else if ((!pace.isNull() && !pace.is<bool>()) || (!autoMode.isNull() && !autoMode.is<bool>())) {
        invalid = "pace and brightness_auto must be booleans";
    } else if (!validPercent(brightness) || !validPercent(night)) {
        invalid = "Brightness must be 0..100";
    } else if (!validDegrees(latitude, MAX_LATITUDE) || !validDegrees(longitude, MAX_LONGITUDE)) {
        invalid = "Invalid latitude or longitude";
    }
    if (invalid != nullptr) {
        sendError(webserver, HTTP_CODE_BAD_REQUEST, invalid);
        return;
    }

    if (!layout.isNull()) {
        configManager.claude_layout = layout.as<uint8_t>();
        ClaudeScreen::setLayout(configManager.claude_layout);
    }
    if (!pace.isNull()) {
        configManager.claude_pace = pace.as<bool>();
        ClaudeScreen::setPaceMarker(configManager.claude_pace);
    }
    if (!brightness.isNull()) {
        configManager.brightness = brightness.as<uint8_t>();
    }
    if (!night.isNull()) {
        configManager.brightness_night = night.as<uint8_t>();
    }
    if (!autoMode.isNull()) {
        configManager.brightness_auto = autoMode.as<bool>();
    }
    if (!latitude.isNull()) {
        configManager.latitude = latitude.as<float>();
    }
    if (!longitude.isNull()) {
        configManager.longitude = longitude.as<float>();
    }
    Backlight::apply();
    if (!configManager.save()) {
        sendError(webserver, HTTP_CODE_INTERNAL_ERROR, "Failed to save config");
        return;
    }
    JsonDocument doc;
    doc["status"] = "ok";
    doc["layout"] = ClaudeScreen::layout();
    doc["pace"] = ClaudeScreen::paceMarker();
    putBrightness(doc);
    sendJson(webserver, HTTP_CODE_OK, doc);
}

// Connectivity check from the device's point of view, to tell DNS, routing and HTTP problems apart
void handleClaudeDiag(Webserver* webserver) {
    if (!requireBearerToken(webserver)) {
        return;
    }
    constexpr uint16_t HTTP_PORT = 80;
    constexpr uint32_t READ_TIMEOUT_MS = 5000;
    JsonDocument doc;
    doc["ip"] = WiFi.localIP().toString();
    doc["gateway"] = WiFi.gatewayIP().toString();
    doc["subnet"] = WiFi.subnetMask().toString();
    doc["dns0"] = WiFi.dnsIP(0).toString();
    doc["dns1"] = WiFi.dnsIP(1).toString();
    doc["rssi"] = WiFi.RSSI();
    doc["time"] = static_cast<uint32_t>(time(nullptr));

    IPAddress resolved;
    const bool dnsOk = WiFi.hostByName("platform.claude.com", resolved) == 1;
    doc["dns_ok"] = dnsOk;
    doc["resolved"] = resolved.toString();

    if (dnsOk) {
        WiFiClient client;
        client.setTimeout(READ_TIMEOUT_MS);
        const bool tcpOk = client.connect(resolved, HTTP_PORT) != 0;
        doc["tcp80_ok"] = tcpOk;
        if (tcpOk) {
            client.print("HEAD / HTTP/1.1\r\nHost: platform.claude.com\r\nConnection: close\r\n\r\n");
            String head;
            const uint32_t start = millis();
            while (head.length() < 400 && millis() - start < READ_TIMEOUT_MS) {
                while (client.available() > 0 && head.length() < 400) {
                    head += static_cast<char>(client.read());
                }
                if (!client.connected()) {
                    break;
                }
                delay(10);
            }
            doc["http_head"] = head;
        }
        client.stop();
    }
    sendJson(webserver, HTTP_CODE_OK, doc);
}
}  // namespace

/**
 * @brief Register the Claude usage page and API endpoints
 * @param webserver Pointer to the Webserver instance
 *
 * @return void
 */
void registerClaudeEndpoints(Webserver* webserver) {
    // Served from flash so it works without a filesystem update. The home page is overridden as well so the
    // menu links to /claude even when the filesystem still holds the upstream index.html.
    webserver->raw().on("/claude", HTTP_GET, [webserver]() { handleClaudePage(webserver); });
    webserver->raw().on("/", HTTP_GET, [webserver]() { handleIndexPage(webserver); });
    webserver->raw().on("/index.html", HTTP_GET, [webserver]() { handleIndexPage(webserver); });

    // @openapi {get} /claude/status version=v1 group=Claude summary="Get Claude usage and login state"
    // requiresAuth=true responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/status", HTTP_GET, [webserver]() { handleClaudeStatus(webserver); });

    // @openapi {post} /claude/login/start version=v1 group=Claude summary="Start OAuth login, returns authorize URL"
    // requiresAuth=true requestBody=application/json requestBodySchema=scope:string example={"scope":"user:profile"}
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/login/start", HTTP_POST, [webserver]() { handleClaudeLoginStart(webserver); });

    // @openapi {post} /claude/login/finish version=v1 group=Claude summary="Finish OAuth login with pasted code"
    // requiresAuth=true requestBody=application/json requestBodySchema=code:string example={"code":"abc#state"}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/login/finish", HTTP_POST,
                        [webserver]() { handleClaudeLoginFinish(webserver); });

    // @openapi {post} /claude/logout version=v1 group=Claude summary="Forget Claude tokens" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/logout", HTTP_POST, [webserver]() { handleClaudeLogout(webserver); });

    // @openapi {get} /claude/diag version=v1 group=Claude summary="Network diagnostics" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/diag", HTTP_GET, [webserver]() { handleClaudeDiag(webserver); });

    // @openapi {post} /claude/display version=v1 group=Claude summary="Screen layout (0 rings, 1 bars), time
    // marker and brightness" requiresAuth=true requestBody=application/json
    // requestBodySchema=layout:integer,pace:boolean,brightness:integer,brightness_night:integer,brightness_auto:boolean,latitude:number,longitude:number
    // example={"layout":1,"pace":true,"brightness":80,"brightness_night":15,"brightness_auto":true}
    // responses=200:application/json,400:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/display", HTTP_POST, [webserver]() { handleClaudeDisplay(webserver); });

    // @openapi {post} /claude/refresh version=v1 group=Claude summary="Fetch usage now" requiresAuth=true
    // responses=200:application/json,401:application/json
    webserver->raw().on("/api/v1/claude/refresh", HTTP_POST, [webserver]() { handleClaudeRefresh(webserver); });
}
