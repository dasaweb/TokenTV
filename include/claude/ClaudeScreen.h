// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <Arduino.h>

class ClaudeScreen {
   public:
    static void begin(const String& deviceIp);
    static void update();
    static void invalidate();
};
