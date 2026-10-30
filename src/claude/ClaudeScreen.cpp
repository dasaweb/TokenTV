// SPDX-License-Identifier: GPL-3.0-or-later
#include "claude/ClaudeScreen.h"

#include <Arduino_GFX_Library.h>
#include <U8g2lib.h>  // only for the font data, Arduino_GFX renders it
#include <algorithm>
#include <array>
#include <cmath>
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
constexpr uint16_t COLOR_CARD = 0x18E3;
constexpr uint16_t COLOR_BAR = 0x39E7;
constexpr uint16_t COLOR_FRAME = 0x6B4D;
constexpr uint16_t COLOR_CLAUDE = 0xDBAA;
constexpr uint16_t COLOR_OK = 0x362B;
constexpr uint16_t COLOR_WARN = 0xFD84;
constexpr uint16_t COLOR_CRIT = 0xFA27;

constexpr float WARN_PERCENT = 70.0F;
constexpr float CRIT_PERCENT = 90.0F;
constexpr size_t TEXT_BUF = 40;
constexpr long SESSION_WINDOW_S = 5L * 3600;
constexpr long WEEKLY_WINDOW_S = 7L * 86400;

String ip;
bool active = false;
bool gifWasPlaying = false;
uint8_t currentLayout = ClaudeScreen::LAYOUT_RINGS;
bool showPace = true;
bool outlineCards = false;
uint32_t drawnRevision = UINT32_MAX;
time_t drawnMinute = 0;

// The screen is a fixed 240x240 layout; literal geometry stays next to the drawing code.
// NOLINTBEGIN(readability-magic-numbers,bugprone-narrowing-conversions)
constexpr int16_t HEADER_H = 26;
constexpr int16_t RING_Y = 94;
constexpr int16_t RING_OUTER = 46;
constexpr int16_t RING_INNER = 40;
constexpr int16_t PACE_OVERHANG = 3;
constexpr std::array<int16_t, 2> RING_X = {62, 178};
constexpr int16_t LABEL_Y = 158;
constexpr int16_t RESET_Y = 177;
constexpr int16_t FOOTER_Y = 228;
constexpr int16_t CARD_X = 6;
constexpr int16_t CARD_W = 228;
constexpr int16_t CARD_H = 88;
constexpr int16_t CARD_PAD = 10;
constexpr std::array<int16_t, 2> CARD_Y = {32, 124};
constexpr int16_t BAR_H = 12;

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

void drawRight(const char* text, int16_t rightX, int16_t baseline, const uint8_t* font, uint16_t color) {
    auto* lcd = gfx();
    lcd->setFont(font);
    int16_t boundX = 0;
    int16_t boundY = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    lcd->getTextBounds(text, 0, baseline, &boundX, &boundY, &width, &height);
    lcd->setTextColor(color);
    lcd->setCursor(rightX - static_cast<int16_t>(width) - boundX, baseline);
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

void formatDuration(long seconds, char* out, size_t len) {
    const long days = seconds / 86400;
    const long hours = (seconds % 86400) / 3600;
    const long minutes = (seconds % 3600) / 60;
    if (days > 0) {
        snprintf(out, len, "%ldd %ldh", days, hours);
    } else if (hours > 0) {
        snprintf(out, len, "%ldh %02ldm", hours, minutes);
    } else {
        snprintf(out, len, "%ld min", minutes);
    }
}

void formatCountdown(time_t target, char* out, size_t len) {
    if (target <= 0) {
        snprintf(out, len, "--");
        return;
    }
    std::array<char, TEXT_BUF> duration{};
    formatDuration(std::max<long>(0, static_cast<long>(target - time(nullptr))), duration.data(), duration.size());
    snprintf(out, len, "in %s", duration.data());
}

// Share of the limit window that has already passed (0..1), negative when the reset time is unknown
auto elapsedFraction(const ClaudeLimit& limit, long windowS) -> float {
    if (!limit.valid || limit.resetsAt <= 0) {
        return -1.0F;
    }
    const auto remaining = static_cast<float>(limit.resetsAt - time(nullptr));
    return std::min(std::max(1.0F - (remaining / static_cast<float>(windowS)), 0.0F), 1.0F);
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

void drawRing(size_t idx, const ClaudeLimit& limit, const char* label, long windowS) {
    auto* lcd = gfx();
    const int16_t centerX = RING_X[idx];
    lcd->fillCircle(centerX, RING_Y, RING_OUTER + PACE_OVERHANG, COLOR_BG);
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

    // Pace marker: a radial tick at the share of the window that has passed
    const float elapsed = elapsedFraction(limit, windowS);
    if (showPace && elapsed >= 0.0F) {
        const float angle = 270.0F + (elapsed * 360.0F);
        lcd->fillArc(centerX, RING_Y, RING_OUTER + PACE_OVERHANG, RING_INNER - PACE_OVERHANG, angle - 2.5F,
                     angle + 2.5F, COLOR_TEXT);
    }
    drawCentered(text.data(), centerX, RING_Y + 7, u8g2_font_fur17_tr, COLOR_TEXT);
    drawCentered(label, centerX, LABEL_Y, u8g2_font_helvR08_tr, COLOR_MUTED);
}

void drawRings() {
    const ClaudeUsageData& usage = ClaudeUsage::data();
    drawRing(0, usage.session, "5 Stunden", SESSION_WINDOW_S);
    drawRing(1, usage.weekly, "Woche", WEEKLY_WINDOW_S);
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

// Projects the current rate to the reset: either the limit lasts, or it runs out after the returned duration
void formatProjection(const ClaudeLimit& limit, float elapsed, long windowS, char* out, size_t len,
                      uint16_t& color) {
    const float used = std::min(std::max(limit.percent, 0.0F), 100.0F);
    const float elapsedS = elapsed * static_cast<float>(windowS);
    const float remainingS = static_cast<float>(windowS) - elapsedS;
    color = COLOR_OK;
    if (used >= 100.0F) {
        color = COLOR_CRIT;
        snprintf(out, len, "Limit erreicht");
        return;
    }
    if (used < 0.5F || elapsedS < 60.0F) {
        snprintf(out, len, "reicht bis Reset");
        return;
    }
    const float toFullS = (100.0F - used) * elapsedS / used;
    if (toFullS >= remainingS) {
        snprintf(out, len, "reicht bis Reset");
        return;
    }
    color = colorFor(limit) == COLOR_CRIT ? COLOR_CRIT : COLOR_WARN;
    std::array<char, TEXT_BUF> duration{};
    formatDuration(static_cast<long>(toFullS), duration.data(), duration.size());
    snprintf(out, len, "leer in %s", duration.data());
}

void drawBarCard(size_t idx, const ClaudeLimit& limit, const char* label, long windowS) {
    auto* lcd = gfx();
    const int16_t top = CARD_Y[idx];
    const int16_t left = CARD_X + CARD_PAD;
    const int16_t right = CARD_X + CARD_W - CARD_PAD;
    const int16_t barW = right - left;
    const int16_t barY = top + 36;
    if (outlineCards) {
        // Black card with a grey frame for more contrast than the filled card
        lcd->fillRoundRect(CARD_X, top, CARD_W, CARD_H, 6, COLOR_BG);
        lcd->drawRoundRect(CARD_X, top, CARD_W, CARD_H, 6, COLOR_FRAME);
    } else {
        lcd->fillRoundRect(CARD_X, top, CARD_W, CARD_H, 6, COLOR_CARD);
    }
    drawLeft(label, left, top + 20, u8g2_font_helvB10_tr, COLOR_MUTED);
    lcd->fillRect(left, barY, barW, BAR_H, COLOR_BAR);

    if (!limit.valid) {
        drawRight("--", right, top + 28, u8g2_font_fur20_tr, COLOR_TEXT);
        return;
    }

    const float used = std::min(std::max(limit.percent, 0.0F), 100.0F);
    std::array<char, TEXT_BUF> text{};
    snprintf(text.data(), text.size(), "%d%%", static_cast<int>(limit.percent + 0.5F));
    drawRight(text.data(), right, top + 28, u8g2_font_fur20_tr, COLOR_TEXT);

    const auto fillW = static_cast<int16_t>(static_cast<float>(barW) * used / 100.0F);
    if (fillW > 0) {
        lcd->fillRect(left, barY, fillW, BAR_H, colorFor(limit));
    }

    // Pace marker: where usage would be if spread evenly over the window
    const float elapsed = elapsedFraction(limit, windowS);
    if (showPace && elapsed >= 0.0F) {
        const auto markX = static_cast<int16_t>(left + static_cast<int16_t>(static_cast<float>(barW) * elapsed) - 1);
        lcd->fillRect(std::min<int16_t>(std::max(markX, left), right - 3), barY - 3, 3, BAR_H + 6, COLOR_TEXT);
    }

    snprintf(text.data(), text.size(), "%d%% frei", static_cast<int>(100.0F - used + 0.5F));
    drawLeft(text.data(), left, top + 64, u8g2_font_helvR10_tr, COLOR_TEXT);
    std::array<char, TEXT_BUF> countdown{};
    formatCountdown(limit.resetsAt, countdown.data(), countdown.size());
    snprintf(text.data(), text.size(), "Reset %s", countdown.data());
    drawRight(text.data(), right, top + 64, u8g2_font_helvR10_tr, COLOR_MUTED);

    if (elapsed < 0.0F) {
        return;
    }
    if (showPace) {
        const float reserve = (elapsed * 100.0F) - used;
        const uint16_t paceColor = reserve >= 0.0F ? COLOR_OK : COLOR_WARN;
        lcd->fillCircle(left + 3, top + 79, 3, paceColor);
        snprintf(text.data(), text.size(), reserve >= 0.0F ? "%d%% Reserve" : "%d%% zu schnell",
                 static_cast<int>(std::fabs(reserve) + 0.5F));
        drawLeft(text.data(), left + 10, top + 83, u8g2_font_helvR08_tr, paceColor);
    }

    uint16_t projColor = COLOR_OK;
    formatProjection(limit, elapsed, windowS, text.data(), text.size(), projColor);
    drawRight(text.data(), right, top + 83, u8g2_font_helvR08_tr, projColor);
}

void drawBarCards() {
    const ClaudeUsageData& usage = ClaudeUsage::data();
    drawBarCard(0, usage.session, "5 Stunden", SESSION_WINDOW_S);
    drawBarCard(1, usage.weekly, "Woche", WEEKLY_WINDOW_S);
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
    } else if (currentLayout == ClaudeScreen::LAYOUT_BARS) {
        drawBarCards();
        drawFooter();
    } else {
        drawRings();
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
        if (currentLayout == ClaudeScreen::LAYOUT_BARS) {
            drawBarCards();  // the pace marker and projection move with time
        } else {
            if (showPace) {
                drawRings();
            }
            drawResetLine();
        }
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

void ClaudeScreen::setLayout(uint8_t newLayout) {
    currentLayout = newLayout < LAYOUT_COUNT ? newLayout : LAYOUT_RINGS;
    invalidate();
}

auto ClaudeScreen::layout() -> uint8_t { return currentLayout; }

void ClaudeScreen::setPaceMarker(bool enabled) {
    showPace = enabled;
    invalidate();
}

auto ClaudeScreen::paceMarker() -> bool { return showPace; }

void ClaudeScreen::setOutline(bool enabled) {
    outlineCards = enabled;
    invalidate();
}

auto ClaudeScreen::outline() -> bool { return outlineCards; }

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
