// SPDX-License-Identifier: GPL-3.0-or-later
#include "claude/ClaudeScreen.h"

#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>  // only for the font data, Arduino_GFX renders it
#include <algorithm>
#include <array>
#include <ctime>

#include "claude/ClaudeUsage.h"
#include "display/DisplayManager.h"

// POSIX TZ rule used for the clock and reset times (NTP itself runs in UTC)
#ifndef CLAUDE_TZ
#define CLAUDE_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

namespace {
constexpr uint16_t COLOR_BG = 0x0000;
constexpr uint16_t COLOR_TEXT = 0xFFFF;
constexpr uint16_t COLOR_MUTED = 0x94B2;
constexpr uint16_t COLOR_TRACK = 0x2124;
constexpr uint16_t COLOR_CLAUDE = 0xDBAA;
constexpr uint16_t COLOR_OK = 0x362B;
constexpr uint16_t COLOR_WARN = 0xFD84;
constexpr uint16_t COLOR_CRIT = 0xFA27;

constexpr float WARN_PERCENT = 70.0F;
constexpr float CRIT_PERCENT = 90.0F;
constexpr size_t TEXT_BUF = 40;

String ip;
bool active = false;
bool gifWasPlaying = false;
uint32_t drawnRevision = UINT32_MAX;
time_t drawnMinute = 0;

// The screen is a fixed 240x240 layout; literal geometry stays next to the drawing code.
// NOLINTBEGIN(readability-magic-numbers,bugprone-narrowing-conversions)
constexpr int16_t HEADER_H = 26;
constexpr int16_t RING_Y = 94;
constexpr int16_t RING_OUTER = 46;
constexpr int16_t RING_INNER = 40;
constexpr std::array<int16_t, 2> RING_X = {62, 178};
constexpr int16_t LABEL_Y = 158;
constexpr int16_t RESET_Y = 177;
constexpr int16_t FOOTER_Y = 228;

auto gfx() -> Arduino_GFX* { return DisplayManager::getGfx(); }

// The boot screen leaves text size 2 behind, which doubles every U8g2 font, and wrapping would push an
// overflowing clock onto the next line
void beginText() {
    gfx()->setTextSize(1);
    gfx()->setTextWrap(false);
}

void endText() {
    gfx()->setFont(static_cast<const GFXfont*>(nullptr));  // leave the default font for other screens
    gfx()->setTextWrap(true);
}

auto localTime(time_t when) -> struct tm {
    setTZ(CLAUDE_TZ);  // configTime(0, 0, ...) in the NTP client resets TZ on every sync
    struct tm out {};
    localtime_r(&when, &out);
    return out;
}

void drawCentered(const char* text, int16_t centerX, int16_t baseline, const uint8_t* font, uint16_t color) {
    auto* lcd = gfx();
    lcd->setFont(font);
    int16_t boundX = 0;
    int16_t boundY = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    lcd->getTextBounds(text, 0, baseline, &boundX, &boundY, &width, &height);
    lcd->setTextColor(color);
    lcd->setCursor(centerX - static_cast<int16_t>(width / 2) - boundX, baseline);
    lcd->print(text);
}

void drawLeft(const char* text, int16_t xPos, int16_t baseline, const uint8_t* font, uint16_t color) {
    auto* lcd = gfx();
    lcd->setFont(font);
    lcd->setTextColor(color);
    lcd->setCursor(xPos, baseline);
    lcd->print(text);
}

auto colorFor(const ClaudeLimit& limit) -> uint16_t {
    if (limit.percent >= CRIT_PERCENT) {
        return COLOR_CRIT;
    }
    if (limit.percent >= WARN_PERCENT || limit.warning) {
        return COLOR_WARN;
    }
    return COLOR_OK;
}

void formatCountdown(time_t target, char* out, size_t len) {
    const time_t now = time(nullptr);
    if (target <= 0) {
        snprintf(out, len, "--");
        return;
    }
    const long remaining = std::max<long>(0, static_cast<long>(target - now));
    const long days = remaining / 86400;
    const long hours = (remaining % 86400) / 3600;
    const long minutes = (remaining % 3600) / 60;
    if (days > 0) {
        snprintf(out, len, "in %ldd %ldh", days, hours);
    } else if (hours > 0) {
        snprintf(out, len, "in %ldh %02ldm", hours, minutes);
    } else {
        snprintf(out, len, "in %ld min", minutes);
    }
}

void formatWeekday(time_t target, char* out, size_t len) {
    static constexpr std::array<const char*, 7> DAYS = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
    if (target <= 0) {
        snprintf(out, len, "--");
        return;
    }
    const struct tm when = localTime(target);
    snprintf(out, len, "%s %02d:%02d", DAYS[when.tm_wday], when.tm_hour, when.tm_min);
}

void drawHeader() {
    auto* lcd = gfx();
    lcd->fillRect(0, 0, 240, HEADER_H, COLOR_BG);
    lcd->fillCircle(15, 13, 4, COLOR_CLAUDE);
    drawLeft("Claude", 25, 18, u8g2_font_helvR10_tr, COLOR_TEXT);

    const time_t now = time(nullptr);
    if (now > 0) {
        const struct tm local = localTime(now);
        std::array<char, TEXT_BUF> clock{};
        snprintf(clock.data(), clock.size(), "%02d:%02d", local.tm_hour, local.tm_min);
        drawCentered(clock.data(), 212, 18, u8g2_font_helvR10_tr, COLOR_MUTED);
    }
    lcd->drawFastHLine(10, HEADER_H, 220, COLOR_TRACK);
}

void drawRing(size_t idx, const ClaudeLimit& limit, const char* label) {
    auto* lcd = gfx();
    const int16_t centerX = RING_X[idx];
    lcd->fillCircle(centerX, RING_Y, RING_OUTER, COLOR_BG);
    lcd->fillArc(centerX, RING_Y, RING_OUTER, RING_INNER, 0, 360, COLOR_TRACK);

    std::array<char, TEXT_BUF> text{};
    if (limit.valid) {
        const float clamped = std::min(std::max(limit.percent, 0.0F), 100.0F);
        if (clamped > 0.5F) {
            lcd->fillArc(centerX, RING_Y, RING_OUTER, RING_INNER, 270, 270 + (clamped * 3.6F), colorFor(limit));
        }
        snprintf(text.data(), text.size(), "%d%%", static_cast<int>(limit.percent + 0.5F));
    } else {
        snprintf(text.data(), text.size(), "--");
    }
    drawCentered(text.data(), centerX, RING_Y + 7, u8g2_font_fur17_tr, COLOR_TEXT);
    drawCentered(label, centerX, LABEL_Y, u8g2_font_helvR08_tr, COLOR_MUTED);
}

void drawResetLine() {
    const ClaudeUsageData& usage = ClaudeUsage::data();
    std::array<char, TEXT_BUF> text{};
    gfx()->fillRect(0, RESET_Y - 13, 240, 17, COLOR_BG);

    formatCountdown(usage.session.resetsAt, text.data(), text.size());
    drawCentered(text.data(), RING_X[0], RESET_Y, u8g2_font_fur11_tr, COLOR_TEXT);

    formatWeekday(usage.weekly.resetsAt, text.data(), text.size());
    drawCentered(text.data(), RING_X[1], RESET_Y, u8g2_font_fur11_tr, COLOR_TEXT);
}

void drawFooter() {
    gfx()->fillRect(0, FOOTER_Y - 12, 240, 16, COLOR_BG);
    std::array<char, TEXT_BUF * 2> text{};
    const ClaudeState state = ClaudeUsage::state();

    if (state == ClaudeState::Error) {
        snprintf(text.data(), text.size(), "%s", ClaudeUsage::lastError().substring(0, 44).c_str());
        drawCentered(text.data(), 120, FOOTER_Y, u8g2_font_helvR08_tr, COLOR_CRIT);
        return;
    }

    const time_t fetched = ClaudeUsage::data().fetchedAt;
    if (fetched <= 0) {
        return;
    }
    const long ageMin = static_cast<long>(time(nullptr) - fetched) / 60;
    if (ageMin < 1) {
        snprintf(text.data(), text.size(), "aktualisiert gerade eben");
    } else {
        snprintf(text.data(), text.size(), "aktualisiert vor %ld min", ageMin);
    }
    drawCentered(text.data(), 120, FOOTER_Y, u8g2_font_helvR08_tr, COLOR_MUTED);
}

void drawMessage(const char* title, const char* line1, const char* line2) {
    auto* lcd = gfx();
    lcd->fillRect(0, HEADER_H + 1, 240, 240 - HEADER_H - 1, COLOR_BG);
    drawCentered(title, 120, 95, u8g2_font_helvR12_tr, COLOR_TEXT);
    if (line1 != nullptr) {
        drawCentered(line1, 120, 130, u8g2_font_helvR08_tr, COLOR_MUTED);
    }
    if (line2 != nullptr) {
        drawCentered(line2, 120, 152, u8g2_font_helvR10_tr, COLOR_CLAUDE);
    }
}

void drawFull() {
    auto* lcd = gfx();
    beginText();
    lcd->fillScreen(COLOR_BG);
    drawHeader();

    const ClaudeState state = ClaudeUsage::state();
    const ClaudeUsageData& usage = ClaudeUsage::data();
    const String loginUrl = "http://" + ip + "/claude";

    if (state == ClaudeState::LoggedOut) {
        drawMessage("Nicht angemeldet", "Login im Browser:", loginUrl.c_str());
    } else if (state == ClaudeState::ReloginNeeded) {
        drawMessage("Login abgelaufen", "Neu anmelden:", loginUrl.c_str());
    } else if (usage.fetchedAt <= 0) {
        if (state == ClaudeState::Error) {
            drawMessage("Fehler", ClaudeUsage::lastError().substring(0, 36).c_str(), nullptr);
        } else {
            drawMessage("Lade Usage...", ClaudeUsage::lastError().c_str(), nullptr);
        }
    } else {
        drawRing(0, usage.session, "5 Stunden");
        drawRing(1, usage.weekly, "Woche");
        drawResetLine();
        drawFooter();
    }
    endText();
}

void drawMinuteTick() {
    beginText();
    drawHeader();
    if (ClaudeUsage::data().fetchedAt > 0 && ClaudeUsage::state() != ClaudeState::LoggedOut &&
        ClaudeUsage::state() != ClaudeState::ReloginNeeded) {
        drawResetLine();
        drawFooter();
    }
    endText();
}
// NOLINTEND(readability-magic-numbers,bugprone-narrowing-conversions)
}  // namespace

void ClaudeScreen::begin(const String& deviceIp) {
    ip = deviceIp;
    active = true;
    invalidate();
}

void ClaudeScreen::invalidate() { drawnRevision = UINT32_MAX; }

void ClaudeScreen::update() {
    if (!active) {
        return;
    }

    // A GIF started through the API owns the screen until it is stopped
    if (DisplayManager::isGifPlaying()) {
        gifWasPlaying = true;
        return;
    }
    if (gifWasPlaying) {
        gifWasPlaying = false;
        invalidate();
    }

    constexpr time_t SECONDS_PER_MINUTE = 60;
    const time_t minute = time(nullptr) / SECONDS_PER_MINUTE;
    if (drawnRevision != ClaudeUsage::revision()) {
        drawnRevision = ClaudeUsage::revision();
        drawnMinute = minute;
        drawFull();
        return;
    }

    if (minute != drawnMinute) {
        drawnMinute = minute;
        drawMinuteTick();
    }
}
