// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <ctime>

struct ClaudeLimit {
    bool valid = false;
    float percent = 0;
    time_t resetsAt = 0;
    bool warning = false;
};

struct ClaudeUsageData {
    ClaudeLimit session;
    ClaudeLimit weekly;
    time_t fetchedAt = 0;
};

enum class ClaudeState : uint8_t { LoggedOut, Waiting, Ok, Error, ReloginNeeded };

/**
 * Standalone Claude plan usage client: on-device OAuth (PKCE) login, token refresh
 * and periodic polling of the usage endpoint. Tokens are kept in SecureStorage.
 */
class ClaudeUsage {
   public:
    static void begin();
    static void loop();

    static auto beginLogin(const String& scope) -> String;
    // Validates and queues the pasted code; the token exchange runs in loop(), see loginError()
    static auto finishLogin(const String& pasted, String& error) -> bool;
    static void logout();
    static void requestFetch();

    static auto isLoggedIn() -> bool;
    static auto hasRefreshToken() -> bool;
    static auto loginError() -> const String&;
    static auto loginPending() -> bool;
    static auto state() -> ClaudeState;
    static auto stateName() -> const char*;
    static auto data() -> const ClaudeUsageData&;
    static auto lastError() -> const String&;
    static auto scope() -> const String&;
    static auto tokenExpiresAt() -> time_t;

    // Incremented on every data or state change so the screen only redraws when needed
    static auto revision() -> uint32_t;
};
