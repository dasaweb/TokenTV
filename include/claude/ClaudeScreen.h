// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

class ClaudeScreen {
   public:
    static constexpr uint8_t LAYOUT_RINGS = 0;
    static constexpr uint8_t LAYOUT_BARS = 1;
    static constexpr uint8_t LAYOUT_COUNT = 2;

    static void begin(const String& deviceIp);
    static void update();
    static void invalidate();
    static void setLayout(uint8_t newLayout);
    static auto layout() -> uint8_t;
};
