// SPDX-License-Identifier: GPL-3.0-or-later
#include "display/Backlight.h"

#include <algorithm>
#include <cmath>

#include "config/ConfigManager.h"

extern ConfigManager configManager;

namespace {
constexpr uint16_t PWM_RANGE = 1023;
constexpr unsigned long UPDATE_INTERVAL_MS = 30000UL;
constexpr time_t REASONABLE_EPOCH = 1700000000;

// Auto mode blends from the night to the day level while the sun climbs from the end of civil twilight to a few
// degrees above the horizon, and back in the evening
constexpr double NIGHT_ELEVATION_DEG = -6.0;
constexpr double DAY_ELEVATION_DEG = 6.0;
constexpr double SUNRISE_ELEVATION_DEG = -0.833;  // refraction and solar radius

constexpr double SECONDS_PER_DAY = 86400.0;
constexpr double J2000_UNIX_DAYS = 10957.5;  // 2000-01-01 12:00 UTC
constexpr double SIDEREAL_DEG_PER_DAY = 360.98564736629;
constexpr double FULL_CIRCLE_DEG = 360.0;
constexpr double HALF_CIRCLE_DEG = 180.0;

uint8_t currentPercent = Backlight::MAX_PERCENT;
int currentDuty = -1;
unsigned long lastUpdate = 0;

auto toRad(double degrees) -> double { return degrees * M_PI / HALF_CIRCLE_DEG; }
auto toDeg(double radians) -> double { return radians * HALF_CIRCLE_DEG / M_PI; }

auto wrap180(double degrees) -> double {
    double out = std::fmod(degrees, FULL_CIRCLE_DEG);
    if (out > HALF_CIRCLE_DEG) {
        out -= FULL_CIRCLE_DEG;
    } else if (out < -HALF_CIRCLE_DEG) {
        out += FULL_CIRCLE_DEG;
    }
    return out;
}

struct SunPosition {
    double declination;  // degrees
    double hourAngle;    // degrees, 0 at solar noon, positive in the afternoon
};

// Low-precision solar position from the Astronomical Almanac, good to about a minute for sunrise and sunset.
// Double precision is needed: the sidereal angle grows by 361 degrees per day since J2000.
// NOLINTBEGIN(readability-magic-numbers)
auto sunPosition(time_t when, double longitude) -> SunPosition {
    const double days = static_cast<double>(when) / SECONDS_PER_DAY - J2000_UNIX_DAYS;
    const double meanAnomaly = toRad(std::fmod(357.529 + 0.98560028 * days, FULL_CIRCLE_DEG));
    const double meanLongitude = std::fmod(280.459 + 0.98564736 * days, FULL_CIRCLE_DEG);
    const double eclipticLongitude =
        toRad(meanLongitude + 1.915 * std::sin(meanAnomaly) + 0.020 * std::sin(2 * meanAnomaly));
    const double obliquity = toRad(23.439 - 0.00000036 * days);
    const double rightAscension =
        toDeg(std::atan2(std::cos(obliquity) * std::sin(eclipticLongitude), std::cos(eclipticLongitude)));
    const double sidereal = std::fmod(280.46061837 + SIDEREAL_DEG_PER_DAY * days, FULL_CIRCLE_DEG);

    SunPosition pos{};
    pos.declination = toDeg(std::asin(std::sin(obliquity) * std::sin(eclipticLongitude)));
    pos.hourAngle = wrap180(sidereal + longitude - rightAscension);
    return pos;
}
// NOLINTEND(readability-magic-numbers)

auto sunElevation(time_t when) -> double {
    const SunPosition pos = sunPosition(when, configManager.longitude);
    const double lat = toRad(configManager.latitude);
    const double dec = toRad(pos.declination);
    return toDeg(
        std::asin(std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(toRad(pos.hourAngle))));
}

auto targetPercent() -> uint8_t {
    const uint8_t day = std::min(configManager.brightness, Backlight::MAX_PERCENT);
    const time_t now = time(nullptr);
    if (!configManager.brightness_auto || now < REASONABLE_EPOCH) {
        return day;
    }
    const uint8_t night = std::min(configManager.brightness_night, Backlight::MAX_PERCENT);
    const double blend =
        std::clamp((sunElevation(now) - NIGHT_ELEVATION_DEG) / (DAY_ELEVATION_DEG - NIGHT_ELEVATION_DEG), 0.0, 1.0);
    return static_cast<uint8_t>(std::lround(night + (day - night) * blend));
}

void write(uint8_t percent) {
    currentPercent = percent;
    // Squared so the steps look even; anything above zero stays at least one step lit
    const double level = static_cast<double>(percent) / Backlight::MAX_PERCENT;
    int duty = static_cast<int>(std::lround(level * level * PWM_RANGE));
    if (percent > 0) {
        duty = std::max(duty, 1);
    }
    if (duty == currentDuty) {
        return;
    }
    currentDuty = duty;
    // 0 and PWM_RANGE make the core drive the pin as plain GPIO, so full brightness runs without PWM
    analogWrite(static_cast<uint8_t>(LCD_BACKLIGHT_GPIO), LCD_BACKLIGHT_ACTIVE_LOW ? PWM_RANGE - duty : duty);
}
}  // namespace

/**
 * @brief Set up the PWM range and apply the configured level
 *
 * @return void
 */
void Backlight::begin() {
    analogWriteRange(PWM_RANGE);
    apply();
}

/**
 * @brief Follow the sun in auto mode
 *
 * @return void
 */
void Backlight::loop() {
    if (millis() - lastUpdate >= UPDATE_INTERVAL_MS) {
        apply();
    }
}

/**
 * @brief Compute and set the level for the current settings and time
 *
 * @return void
 */
void Backlight::apply() {
    lastUpdate = millis();
    write(targetPercent());
}

/**
 * @brief Get the level currently set
 *
 * @return Brightness in percent
 */
auto Backlight::percent() -> uint8_t { return currentPercent; }

/**
 * @brief Compute sunrise and sunset for the solar day closest to now at the configured location
 *
 * @param sunrise Receives the sunrise time (UTC epoch)
 * @param sunset Receives the sunset time (UTC epoch)
 *
 * @return false without valid time or when the sun does not cross the horizon that day
 */
auto Backlight::sunTimes(time_t& sunrise, time_t& sunset) -> bool {
    const time_t now = time(nullptr);
    if (now < REASONABLE_EPOCH) {
        return false;
    }
    // Step back to solar noon, then once more since the hour angle drifts slightly within a day
    time_t noon = now;
    SunPosition pos{};
    for (int i = 0; i < 2; ++i) {
        pos = sunPosition(noon, configManager.longitude);
        noon -= static_cast<time_t>(std::lround(pos.hourAngle / SIDEREAL_DEG_PER_DAY * SECONDS_PER_DAY));
    }
    const double lat = toRad(configManager.latitude);
    const double dec = toRad(pos.declination);
    const double cosHalfDay = (std::sin(toRad(SUNRISE_ELEVATION_DEG)) - std::sin(lat) * std::sin(dec)) /
                              (std::cos(lat) * std::cos(dec));
    if (cosHalfDay < -1.0 || cosHalfDay > 1.0) {
        return false;
    }
    const auto halfDay =
        static_cast<time_t>(std::lround(toDeg(std::acos(cosHalfDay)) / SIDEREAL_DEG_PER_DAY * SECONDS_PER_DAY));
    sunrise = noon - halfDay;
    sunset = noon + halfDay;
    return true;
}
