// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <ArduinoJson.h>

/**
 * Keeps the last crash (reset reason, exception registers and candidate return addresses from the
 * stack) in RTC memory so it survives the reboot and can be read over the API. Decode the addresses
 * with xtensa-lx106-elf-addr2line against the matching firmware.elf.
 */
class CrashLog {
   public:
    static void toJson(JsonObject target);
};
