// SPDX-License-Identifier: GPL-3.0-or-later
#include "claude/ClaudeUsage.h"

#include <ArduinoJson.h>
#include <BearSSLHelpers.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <Logger.h>
#include <WiFiClientSecureBearSSL.h>
#include <lwip/dns.h>
#include <algorithm>
#include <array>
#include <memory>
#include <sys/time.h>

#include "claude/ClaudeCerts.h"
#include "config/ConfigManager.h"

extern ConfigManager configManager;

namespace {
constexpr const char* TAG = "Claude";

// Same public OAuth client and endpoints as Claude Code itself (verified against Claude Code 2.1.312)
constexpr const char* OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const char* OAUTH_AUTHORIZE_URL = "https://claude.com/cai/oauth/authorize";
constexpr const char* OAUTH_REDIRECT_URI = "https://platform.claude.com/oauth/code/callback";
constexpr const char* OAUTH_TOKEN_HOST = "platform.claude.com";
constexpr const char* OAUTH_TOKEN_PATH = "/v1/oauth/token";
constexpr const char* USAGE_HOST = "api.anthropic.com";
constexpr const char* USAGE_PATH = "/api/oauth/usage";
constexpr const char* DEFAULT_SCOPE = "user:profile";
constexpr const char* USER_AGENT = "geekmagic-claude-usage/1.0";

constexpr const char* KEY_ACCESS = "claude_at";
constexpr const char* KEY_REFRESH = "claude_rt";
constexpr const char* KEY_EXPIRES = "claude_exp";
constexpr const char* KEY_SCOPE = "claude_scope";

constexpr uint32_t FETCH_INTERVAL_MS = 5UL * 60UL * 1000UL;
constexpr uint32_t RETRY_MIN_MS = 30UL * 1000UL;
constexpr uint32_t RETRY_MAX_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t NTP_WAIT_RETRY_MS = 5UL * 1000UL;
constexpr uint32_t HTTP_TIME_RETRY_MS = 30UL * 1000UL;
constexpr uint32_t LOGIN_TTL_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t MS_PER_S = 1000UL;
constexpr time_t REFRESH_MARGIN_S = 300;
constexpr time_t REASONABLE_EPOCH = 1700000000;
constexpr uint16_t HTTP_TIMEOUT_MS = 15000;
constexpr uint16_t HTTPS_PORT = 443;

// Neither host negotiates max fragment length, but both send small records (measured 2026-10: largest is
// the 3.4k certificate chain of platform.claude.com, data comes in ~1.4k records). 4k instead of the default
// 16k leaves enough heap for the handshake.
constexpr int TLS_RX_BUF = 4096;
constexpr int TLS_TX_BUF = 512;
constexpr uint32_t MIN_TLS_BLOCK = 8000;
constexpr uint32_t MIN_TLS_HEAP = 16000;
constexpr int ERR_LOW_MEMORY = -100;
constexpr int ERR_BEGIN = -101;

constexpr size_t VERIFIER_BYTES = 32;
constexpr size_t STATE_BYTES = 24;
constexpr size_t SHA256_LEN = 32;
constexpr size_t B64_BUF = 72;

struct PendingLogin {
    bool active = false;
    String verifier;
    String state;
    String scope;
    String code;  // set once the user pasted it; exchanged from loop() so no web request is open during TLS
    uint32_t startedMs = 0;
} pending;

String loginErrorText;

struct HttpResult {
    int status = 0;
    String body;
    uint32_t retryAfterS = 0;
};

String accessToken;
String refreshToken;
String tokenScope;
time_t expiresAt = 0;

ClaudeUsageData usage;
ClaudeState currentState = ClaudeState::LoggedOut;
String errorText;
uint32_t rev = 0;
uint32_t nextAttemptMs = 0;
uint32_t backoffMs = RETRY_MIN_MS;
uint32_t lastHttpTimeMs = 0;
bool httpTimeTried = false;


void setState(ClaudeState newState, const String& err = String()) {
    if (newState != currentState || err != errorText) {
        currentState = newState;
        errorText = err;
        rev++;
    }
}

void scheduleIn(uint32_t delayMs) { nextAttemptMs = millis() + delayMs; }

void scheduleRetry(uint32_t minDelayMs = 0) {
    scheduleIn(std::max(backoffMs, minDelayMs));
    backoffMs = std::min(backoffMs * 2, RETRY_MAX_MS);
}

auto base64url(const uint8_t* data, size_t len) -> String {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    String out;
    out.reserve(static_cast<unsigned int>((len * 4 / 3) + 4));
    size_t idx = 0;
    while (idx + 2 < len) {
        const uint32_t triple = (data[idx] << 16) | (data[idx + 1] << 8) | data[idx + 2];
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
        out += alphabet[triple & 0x3F];
        idx += 3;
    }
    const size_t rest = len - idx;
    if (rest == 1) {
        const uint32_t triple = data[idx] << 16;
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
    } else if (rest == 2) {
        const uint32_t triple = (data[idx] << 16) | (data[idx + 1] << 8);
        out += alphabet[(triple >> 18) & 0x3F];
        out += alphabet[(triple >> 12) & 0x3F];
        out += alphabet[(triple >> 6) & 0x3F];
    }
    return out;
}

auto randomBase64url(size_t nbytes) -> String {
    std::array<uint8_t, B64_BUF> buf{};
    nbytes = std::min(nbytes, buf.size());
    ESP.random(buf.data(), nbytes);  // hardware RNG, NOLINT(readability-static-accessed-through-instance)
    return base64url(buf.data(), nbytes);
}

auto pkceChallenge(const String& verifier) -> String {
    std::array<uint8_t, SHA256_LEN> hash{};
    br_sha256_context ctx;
    br_sha256_init(&ctx);
    br_sha256_update(&ctx, verifier.c_str(), verifier.length());
    br_sha256_out(&ctx, hash.data());
    return base64url(hash.data(), hash.size());
}

auto urlEncode(const String& str) -> String {
    static const char* hex = "0123456789ABCDEF";
    String out;
    out.reserve(str.length() * 3);
    for (size_t i = 0; i < str.length(); i++) {
        const char chr = str[i];
        if (isalnum(static_cast<unsigned char>(chr)) || chr == '-' || chr == '_' || chr == '.' || chr == '~') {
            out += chr;
        } else {
            out += '%';
            out += hex[(chr >> 4) & 0xF];
            out += hex[chr & 0xF];
        }
    }
    return out;
}

// Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil)
auto daysFromCivil(int year, unsigned month, unsigned day) -> int32_t {
    year -= month <= 2 ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int32_t>(doe) - 719468;
}

// Parses "2026-10-30T01:39:59.950683+00:00" (fraction and offset optional) to a UTC epoch
auto parseIso8601(const char* text) -> time_t {
    if (text == nullptr) {
        return 0;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day, &hour, &minute, &second) != 6) {
        return 0;
    }
    const char* tail = text + 19;
    while (*tail == '.' || isdigit(static_cast<unsigned char>(*tail))) {
        tail++;
    }
    int offsetS = 0;
    if (*tail == '+' || *tail == '-') {
        int offH = 0;
        int offM = 0;
        sscanf(tail + 1, "%2d:%2d", &offH, &offM);
        offsetS = (offH * 3600 + offM * 60) * (*tail == '-' ? -1 : 1);
    }
    const int32_t days = daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
    return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second - offsetS;
}

// Parses an RFC 1123 HTTP date like "Thu, 29 Oct 2026 22:46:54 GMT" to a UTC epoch
auto parseHttpDate(const char* text) -> time_t {
    static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char month[4] = {0};
    if (sscanf(text, "%*3s, %d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6) {
        return 0;
    }
    const char* found = strstr(months, month);
    if (found == nullptr) {
        return 0;
    }
    const auto monthNum = static_cast<unsigned>((found - months) / 3 + 1);
    const int32_t days = daysFromCivil(year, monthNum, static_cast<unsigned>(day));
    return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

// Fallback for networks that block NTP (UDP 123): take the wall clock from a plain HTTP Date header.
// Unauthenticated like NTP itself; only needed so certificate validity can be checked at all.
auto syncTimeFromHttp() -> bool {
    WiFiClient client;
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    if (!http.begin(client, "http://platform.claude.com/")) {
        return false;
    }
    const char* collect[] = {"Date"};
    http.collectHeaders(collect, 1);
    const int status = http.sendRequest("HEAD");
    const String date = http.header("Date");
    http.end();

    const time_t parsed = status > 0 ? parseHttpDate(date.c_str()) : 0;
    if (parsed < REASONABLE_EPOCH) {
        Logger::warn("Could not get time from HTTP Date header", TAG);
        return false;
    }
    timeval now{};
    now.tv_sec = parsed;
    settimeofday(&now, nullptr);
    Logger::warn("NTP unavailable, time set from HTTP Date header", TAG);
    return true;
}

// Some DHCP servers (e.g. the Windows mobile hotspot) hand out no DNS server, which breaks NTP and HTTPS alike
void ensureDns() {
    if (WiFi.status() != WL_CONNECTED || WiFi.dnsIP(0).isSet()) {
        return;
    }
    const IPAddress primary(1, 1, 1, 1);
    const IPAddress secondary(8, 8, 8, 8);
    dns_setserver(0, primary);
    dns_setserver(1, secondary);
    Logger::warn("DHCP provided no DNS server, using 1.1.1.1 / 8.8.8.8", TAG);
}

void ensureTime() {
    if (time(nullptr) >= REASONABLE_EPOCH) {
        return;
    }
    if (httpTimeTried && millis() - lastHttpTimeMs < HTTP_TIME_RETRY_MS) {
        return;
    }
    httpTimeTried = true;
    lastHttpTimeMs = millis();
    syncTimeFromHttp();
}

auto httpsRequest(const char* host, const char* path, const char* bearer, const String* jsonBody) -> HttpResult {
    HttpResult res;

    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    if (ESP.getMaxFreeBlockSize() < MIN_TLS_BLOCK || ESP.getFreeHeap() < MIN_TLS_HEAP) {
        res.status = ERR_LOW_MEMORY;
        res.body = "low memory";
        return res;
    }

    // Declared before the client so it outlives it; freed again when the request is done
    const BearSSL::X509List trustAnchors(strcmp(host, USAGE_HOST) == 0 ? CLAUDE_CA_API : CLAUDE_CA_PLATFORM);

    auto client = std::make_unique<BearSSL::WiFiClientSecure>();
    client->setTrustAnchors(&trustAnchors);
    client->setBufferSizes(TLS_RX_BUF, TLS_TX_BUF);
    client->setTimeout(HTTP_TIMEOUT_MS);

    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent(USER_AGENT);

    if (!http.begin(*client, host, HTTPS_PORT, path, true)) {
        res.status = ERR_BEGIN;
        res.body = "http begin failed";
        return res;
    }

    const char* collect[] = {"Retry-After"};
    http.collectHeaders(collect, 1);
    http.addHeader("Accept", "application/json");

    if (bearer != nullptr) {
        http.addHeader("Authorization", String("Bearer ") + bearer);
        http.addHeader("anthropic-beta", "oauth-2025-04-20");
    }

    if (jsonBody != nullptr) {
        http.addHeader("Content-Type", "application/json");
        res.status = http.POST(*jsonBody);
    } else {
        res.status = http.GET();
    }

    if (res.status > 0) {
        res.body = http.getString();
        res.retryAfterS = static_cast<uint32_t>(http.header("Retry-After").toInt());
    } else {
        res.body = HTTPClient::errorToString(res.status);
        char sslErr[64];
        if (client->getLastSSLError(sslErr, sizeof(sslErr)) != 0) {
            res.body += String(" / ") + sslErr;
        }
    }

    http.end();

    return res;
}

auto describeFailure(const HttpResult& res) -> String {
    if (res.status <= 0) {
        return res.body;
    }
    // Error bodies are small: OAuth style {"error":"invalid_grant"} or API style {"error":{"message":...}}
    JsonDocument doc;
    String detail;
    if (!deserializeJson(doc, res.body)) {
        if (doc["error"].is<const char*>()) {
            detail = doc["error"].as<const char*>();
        } else if (doc["error"]["message"].is<const char*>()) {
            detail = doc["error"]["message"].as<const char*>();
        }
    }
    String out = "HTTP " + String(res.status);
    if (detail.length() > 0) {
        out += ": " + detail.substring(0, 60);
    }
    return out;
}

void persistTokens() {
    configManager.secure.put(KEY_ACCESS, accessToken.c_str());
    configManager.secure.put(KEY_REFRESH, refreshToken.c_str());
    configManager.secure.put(KEY_EXPIRES, String(static_cast<uint32_t>(expiresAt)).c_str());
    configManager.secure.put(KEY_SCOPE, tokenScope.c_str());
}

void clearTokens() {
    accessToken = "";
    refreshToken = "";
    tokenScope = "";
    expiresAt = 0;
    configManager.secure.remove(KEY_ACCESS);
    configManager.secure.remove(KEY_REFRESH);
    configManager.secure.remove(KEY_EXPIRES);
    configManager.secure.remove(KEY_SCOPE);
}

enum class TokenOutcome : uint8_t { Ok, Transient, InvalidGrant };

auto postToken(const String& body) -> TokenOutcome {
    const HttpResult res = httpsRequest(OAUTH_TOKEN_HOST, OAUTH_TOKEN_PATH, nullptr, &body);

    JsonDocument filter;
    filter["access_token"] = true;
    filter["refresh_token"] = true;
    filter["expires_in"] = true;
    filter["scope"] = true;
    filter["error"] = true;

    JsonDocument doc;
    const bool parsed = res.status > 0 && !deserializeJson(doc, res.body, DeserializationOption::Filter(filter));

    if (res.status == HTTP_CODE_OK && parsed && doc["access_token"].is<const char*>()) {
        accessToken = doc["access_token"].as<const char*>();
        if (doc["refresh_token"].is<const char*>()) {
            refreshToken = doc["refresh_token"].as<const char*>();
        }
        if (doc["scope"].is<const char*>()) {
            tokenScope = doc["scope"].as<const char*>();
        }
        const uint32_t expiresIn = doc["expires_in"] | 3600U;
        expiresAt = time(nullptr) + static_cast<time_t>(expiresIn);
        persistTokens();
        Logger::info(("Token ok, expires in " + String(expiresIn) + "s").c_str(), TAG);
        return TokenOutcome::Ok;
    }

    errorText = describeFailure(res);
    Logger::warn(("Token request failed: " + errorText).c_str(), TAG);

    const bool invalidGrant = parsed && doc["error"].is<const char*>() &&
                              (strcmp(doc["error"].as<const char*>(), "invalid_grant") == 0);
    return invalidGrant ? TokenOutcome::InvalidGrant : TokenOutcome::Transient;
}

auto refreshTokens() -> TokenOutcome {
    if (refreshToken.length() == 0) {
        return TokenOutcome::InvalidGrant;
    }
    JsonDocument body;
    body["grant_type"] = "refresh_token";
    body["refresh_token"] = refreshToken;
    body["client_id"] = OAUTH_CLIENT_ID;
    body["scope"] = tokenScope.length() > 0 ? tokenScope.c_str() : DEFAULT_SCOPE;
    String json;
    serializeJson(body, json);
    return postToken(json);
}

void applyLimit(ClaudeLimit& target, float percent, const char* resetsAt, bool warning) {
    target.valid = true;
    target.percent = percent;
    target.resetsAt = parseIso8601(resetsAt);
    target.warning = warning;
}

auto parseUsage(const String& body) -> bool {
    JsonDocument filter;
    for (const char* key : {"five_hour", "seven_day"}) {
        filter[key]["utilization"] = true;
        filter[key]["resets_at"] = true;
    }
    filter["limits"][0]["kind"] = true;
    filter["limits"][0]["percent"] = true;
    filter["limits"][0]["severity"] = true;
    filter["limits"][0]["resets_at"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
        return false;
    }

    ClaudeUsageData next;

    // Newer responses carry a "limits" list with a server-side severity, older ones only five_hour/seven_day
    for (JsonObject limit : doc["limits"].as<JsonArray>()) {
        const char* kind = limit["kind"] | "";
        const char* severity = limit["severity"] | "normal";
        const bool warning = strcmp(severity, "normal") != 0;
        if (strcmp(kind, "session") == 0) {
            applyLimit(next.session, limit["percent"] | 0.0F, limit["resets_at"], warning);
        } else if (strcmp(kind, "weekly_all") == 0) {
            applyLimit(next.weekly, limit["percent"] | 0.0F, limit["resets_at"], warning);
        }
    }

    if (!next.session.valid && !doc["five_hour"].isNull()) {
        applyLimit(next.session, doc["five_hour"]["utilization"] | 0.0F, doc["five_hour"]["resets_at"], false);
    }
    if (!next.weekly.valid && !doc["seven_day"].isNull()) {
        applyLimit(next.weekly, doc["seven_day"]["utilization"] | 0.0F, doc["seven_day"]["resets_at"], false);
    }

    if (!next.session.valid && !next.weekly.valid) {
        return false;
    }

    next.fetchedAt = time(nullptr);
    usage = next;
    return true;
}

void runCycle() {
    const time_t now = time(nullptr);

    if (now >= expiresAt - REFRESH_MARGIN_S) {
        const TokenOutcome outcome = refreshTokens();
        if (outcome == TokenOutcome::InvalidGrant) {
            setState(ClaudeState::ReloginNeeded, "Login expired");
            return;
        }
        if (outcome == TokenOutcome::Transient) {
            setState(ClaudeState::Error, "Refresh: " + errorText);
            scheduleRetry();
            return;
        }
    }

    HttpResult res = httpsRequest(USAGE_HOST, USAGE_PATH, accessToken.c_str(), nullptr);

    if (res.status == HTTP_CODE_UNAUTHORIZED) {
        const TokenOutcome outcome = refreshTokens();
        if (outcome == TokenOutcome::InvalidGrant) {
            setState(ClaudeState::ReloginNeeded, "Login expired");
            return;
        }
        if (outcome == TokenOutcome::Ok) {
            res = httpsRequest(USAGE_HOST, USAGE_PATH, accessToken.c_str(), nullptr);
        }
    }

    if (res.status == HTTP_CODE_OK && parseUsage(res.body)) {
        backoffMs = RETRY_MIN_MS;
        scheduleIn(FETCH_INTERVAL_MS);
        errorText = "";
        currentState = ClaudeState::Ok;
        rev++;
        return;
    }

    constexpr int HTTP_TOO_MANY_REQUESTS = 429;
    if (res.status == HTTP_TOO_MANY_REQUESTS) {
        setState(ClaudeState::Error, "Rate limited");
        scheduleRetry(std::max(res.retryAfterS * MS_PER_S, FETCH_INTERVAL_MS));
        return;
    }

    setState(ClaudeState::Error, res.status == HTTP_CODE_OK ? String("Unexpected response") : describeFailure(res));
    Logger::warn(("Usage fetch failed: " + errorText).c_str(), TAG);
    scheduleRetry();
}
void exchangeCode() {
    JsonDocument body;
    body["grant_type"] = "authorization_code";
    body["code"] = pending.code;
    body["redirect_uri"] = OAUTH_REDIRECT_URI;
    body["client_id"] = OAUTH_CLIENT_ID;
    body["code_verifier"] = pending.verifier;
    body["state"] = pending.state;
    String json;
    serializeJson(body, json);

    const String scope = pending.scope;
    pending = PendingLogin();  // authorization codes are single use, a failed attempt needs a new login

    const String previousScope = tokenScope;
    tokenScope = scope;
    if (postToken(json) != TokenOutcome::Ok) {
        tokenScope = previousScope;
        loginErrorText = errorText;
        return;
    }

    usage = ClaudeUsageData();
    backoffMs = RETRY_MIN_MS;
    setState(ClaudeState::Waiting);
    scheduleIn(0);
}
}  // namespace

void ClaudeUsage::begin() {
    accessToken = configManager.secure.get(KEY_ACCESS, "");
    refreshToken = configManager.secure.get(KEY_REFRESH, "");
    tokenScope = configManager.secure.get(KEY_SCOPE, DEFAULT_SCOPE);
    expiresAt = static_cast<time_t>(configManager.secure.get(KEY_EXPIRES, "0").toInt());

    ensureDns();  // before the NTP client's first sync in loop()
    setState(ClaudeUsage::isLoggedIn() ? ClaudeState::Waiting : ClaudeState::LoggedOut);
    scheduleIn(0);
}

void ClaudeUsage::loop() {
    if (pending.active && millis() - pending.startedMs > LOGIN_TTL_MS) {
        pending = PendingLogin();
    }

    // Needed for login and TLS too, so it runs before the logged-out check
    ensureDns();
    ensureTime();

    if (pending.active && pending.code.length() > 0) {
        exchangeCode();
        return;
    }

    if (currentState == ClaudeState::LoggedOut || currentState == ClaudeState::ReloginNeeded) {
        return;
    }

    if (static_cast<int32_t>(millis() - nextAttemptMs) < 0) {
        return;
    }

    if (time(nullptr) < REASONABLE_EPOCH) {
        // TLS certificate validation and token expiry both need wall clock time
        setState(ClaudeState::Waiting, "Waiting for time sync");
        scheduleIn(NTP_WAIT_RETRY_MS);
        return;
    }

    runCycle();
}

auto ClaudeUsage::beginLogin(const String& scope) -> String {
    pending.verifier = randomBase64url(VERIFIER_BYTES);
    pending.state = randomBase64url(STATE_BYTES);
    pending.scope = scope.length() > 0 ? scope : String(DEFAULT_SCOPE);
    pending.startedMs = millis();
    pending.active = true;

    String url = OAUTH_AUTHORIZE_URL;
    url += "?code=true&client_id=";
    url += OAUTH_CLIENT_ID;
    url += "&response_type=code&redirect_uri=";
    url += urlEncode(OAUTH_REDIRECT_URI);
    url += "&scope=";
    url += urlEncode(pending.scope);
    url += "&code_challenge=";
    url += pkceChallenge(pending.verifier);
    url += "&code_challenge_method=S256&state=";
    url += urlEncode(pending.state);
    return url;
}

auto ClaudeUsage::finishLogin(const String& pasted, String& error) -> bool {
    if (!pending.active) {
        error = "No login in progress, start again";
        return false;
    }
    if (time(nullptr) < REASONABLE_EPOCH && !syncTimeFromHttp()) {
        error = "Device time not synced yet (NTP)";
        return false;
    }

    String input = pasted;
    input.trim();
    String code;
    String state;

    // Accept "code#state" as shown by the callback page, or the whole callback URL
    const int codeParam = input.indexOf("code=");
    if (codeParam >= 0) {
        const int codeEnd = input.indexOf('&', codeParam);
        code = input.substring(codeParam + 5, codeEnd < 0 ? input.length() : codeEnd);
        const int stateParam = input.indexOf("state=");
        if (stateParam >= 0) {
            const int stateEnd = input.indexOf('&', stateParam);
            state = input.substring(stateParam + 6, stateEnd < 0 ? input.length() : stateEnd);
        }
    } else {
        const int hash = input.indexOf('#');
        code = hash < 0 ? input : input.substring(0, hash);
        state = hash < 0 ? String() : input.substring(hash + 1);
    }

    if (code.length() == 0) {
        error = "Empty code";
        return false;
    }
    if (state.length() > 0 && state != pending.state) {
        error = "State mismatch, start the login again";
        return false;
    }

    pending.code = code;
    loginErrorText = "";
    return true;
}

void ClaudeUsage::logout() {
    clearTokens();
    pending = PendingLogin();
    usage = ClaudeUsageData();
    setState(ClaudeState::LoggedOut);
}

void ClaudeUsage::requestFetch() {
    backoffMs = RETRY_MIN_MS;
    scheduleIn(0);
}

auto ClaudeUsage::isLoggedIn() -> bool { return refreshToken.length() > 0 || accessToken.length() > 0; }
auto ClaudeUsage::hasRefreshToken() -> bool { return refreshToken.length() > 0; }
auto ClaudeUsage::loginError() -> const String& { return loginErrorText; }
auto ClaudeUsage::loginPending() -> bool { return pending.active; }
auto ClaudeUsage::state() -> ClaudeState { return currentState; }
auto ClaudeUsage::data() -> const ClaudeUsageData& { return usage; }
auto ClaudeUsage::lastError() -> const String& { return errorText; }
auto ClaudeUsage::scope() -> const String& { return tokenScope; }
auto ClaudeUsage::tokenExpiresAt() -> time_t { return expiresAt; }
auto ClaudeUsage::revision() -> uint32_t { return rev; }

auto ClaudeUsage::stateName() -> const char* {
    switch (currentState) {
        case ClaudeState::LoggedOut:
            return "logged_out";
        case ClaudeState::Waiting:
            return "waiting";
        case ClaudeState::Ok:
            return "ok";
        case ClaudeState::Error:
            return "error";
        case ClaudeState::ReloginNeeded:
            return "relogin_needed";
    }
    return "unknown";
}
