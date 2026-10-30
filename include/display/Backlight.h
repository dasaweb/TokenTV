// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>
#include <ctime>

/**
 * PWM dimming of the LCD backlight. Either a fixed level or, in auto mode, a blend between a day and a night
 * level that follows the sun's elevation at the configured location. Settings live in ConfigManager.
 */
class Backlight {
   public:
    static constexpr uint8_t MAX_PERCENT = 100;

    static void begin();
    static void loop();
    // Re-evaluates the level right away, call after changing the settings
    static void apply();
    static auto percent() -> uint8_t;
    // Sunrise and sunset around the current solar noon; false without valid time or during polar day/night
    static auto sunTimes(time_t& sunrise, time_t& sunset) -> bool;
};
