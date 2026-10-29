// SPDX-License-Identifier: GPL-3.0-or-later
#include "claude/CrashLog.h"

#include <Arduino.h>
#include <user_interface.h>

namespace {
constexpr uint32_t CRASH_MAGIC = 0x43524153;  // "CRAS"
// RTC user memory block offset; RescueMode uses the first blocks
constexpr uint32_t RTC_BLOCK = 32;
constexpr size_t MAX_ADDRS = 16;
constexpr uint32_t STACK_SCAN_BYTES = 1024;
constexpr uint32_t IROM_START = 0x40200000;
constexpr uint32_t IROM_END = 0x40300000;
constexpr uint32_t IRAM_START = 0x40100000;
constexpr uint32_t IRAM_END = 0x40108000;

struct CrashRecord {
    uint32_t magic;
    uint32_t reason;
    uint32_t exccause;
    uint32_t epc1;
    uint32_t excvaddr;
    uint32_t stack;
    uint32_t stackEnd;
    uint32_t count;
    uint32_t addrs[MAX_ADDRS];
};

auto isCodeAddress(uint32_t value) -> bool {
    return (value >= IROM_START && value < IROM_END) || (value >= IRAM_START && value < IRAM_END);
}
}  // namespace

// Called by the core's postmortem handler for exceptions, WDT resets, panic() and abort()
extern "C" void custom_crash_callback(struct rst_info* info, uint32_t stack, uint32_t stackEnd) {
    CrashRecord rec{};
    rec.magic = CRASH_MAGIC;
    rec.reason = info->reason;
    rec.exccause = info->exccause;
    rec.epc1 = info->epc1;
    rec.excvaddr = info->excvaddr;
    rec.stack = stack;
    rec.stackEnd = stackEnd;

    const uint32_t limit = (stackEnd > stack && stackEnd - stack < STACK_SCAN_BYTES) ? stackEnd : stack + STACK_SCAN_BYTES;
    for (uint32_t pos = stack; pos + 4 <= limit && rec.count < MAX_ADDRS; pos += 4) {
        const uint32_t value = *reinterpret_cast<const uint32_t*>(pos);  // NOLINT(performance-no-int-to-ptr)
        if (isCodeAddress(value)) {
            rec.addrs[rec.count++] = value;
        }
    }
    ESP.rtcUserMemoryWrite(RTC_BLOCK, reinterpret_cast<uint32_t*>(&rec), sizeof(rec));
}

void CrashLog::toJson(JsonObject target) {
    CrashRecord rec{};
    // NOLINTNEXTLINE(readability-static-accessed-through-instance)
    if (!ESP.rtcUserMemoryRead(RTC_BLOCK, reinterpret_cast<uint32_t*>(&rec), sizeof(rec)) ||
        rec.magic != CRASH_MAGIC) {
        target["present"] = false;
        return;
    }
    char hex[12];
    target["present"] = true;
    target["reason"] = rec.reason;
    target["exccause"] = rec.exccause;
    snprintf(hex, sizeof(hex), "0x%08x", rec.epc1);
    target["epc1"] = hex;
    snprintf(hex, sizeof(hex), "0x%08x", rec.excvaddr);
    target["excvaddr"] = hex;
    snprintf(hex, sizeof(hex), "0x%08x", rec.stack);
    target["sp"] = hex;
    JsonArray addrs = target["addrs"].to<JsonArray>();
    for (uint32_t i = 0; i < rec.count && i < MAX_ADDRS; i++) {
        snprintf(hex, sizeof(hex), "0x%08x", rec.addrs[i]);
        addrs.add(hex);
    }
}
