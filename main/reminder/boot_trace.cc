#include <sdkconfig.h>
#include "reminder/boot_trace.h"

#if CONFIG_USE_REMINDER_POLL && CONFIG_REMINDER_BOOT_TRACE

#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include "debug/afe_fetch_gate.h"
#include "ota.h"

#define TAG "BootTrace"
// G4 PresentSink isolated production flip; Decoder and Clock remain legacy.
#define FW_MARKER "boot_trace_v10_s1gu_p3_idle_arm_order"

namespace {

constexpr size_t kPhaseRingCap = 24;
constexpr size_t kBootCountRtcWords = 2;

RTC_NOINIT_ATTR uint32_t g_rtc_boot_magic;
RTC_NOINIT_ATTR uint32_t g_rtc_boot_count;
RTC_NOINIT_ATTR uint32_t g_rtc_crash_streak;
RTC_NOINIT_ATTR char g_rtc_last_phase[20];
RTC_NOINIT_ATTR uint32_t g_rtc_last_phase_ms;

struct PhaseEntry {
    char phase[20];
    char detail[32];
    uint32_t t_ms;
    uint32_t heap_int;
    uint32_t heap_psram;
    uint32_t heap_largest;
};

int64_t g_boot_us = 0;
bool g_marker_echoed = false;
bool g_afe_first_fetch_seen = false;
bool g_local_ota_confirmed = false;
// BootTraceMark is called from Core0 audio/network tasks and the Core1 face worker.
// A std::deque here used to mutate its allocator state concurrently and eventually
// crashed in RecordPhase with heap poison (0xBAAD5678).  Keep tracing allocation-free.
PhaseEntry g_phases[kPhaseRingCap];
size_t g_phase_head = 0;   // Next insertion slot.
size_t g_phase_count = 0;
portMUX_TYPE g_phase_mux = portMUX_INITIALIZER_UNLOCKED;
char g_last_phase[20] = "BOOT";

uint32_t BootMs() {
    if (g_boot_us <= 0) {
        return 0;
    }
    return static_cast<uint32_t>((esp_timer_get_time() - g_boot_us) / 1000LL);
}

void CopyField(char* dst, size_t len, const char* src) {
    if (dst == nullptr || len == 0) {
        return;
    }
    if (src == nullptr) {
        dst[0] = '\0';
        return;
    }
    std::strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

const char* ResetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_EXT:
            return "EXT";
        case ESP_RST_SW:
            return "SW";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
        case ESP_RST_USB:
            return "USB";
        case ESP_RST_JTAG:
            return "JTAG";
        default:
            return "UNKNOWN";
    }
}

void PersistCrashHint(const char* phase) {
    g_rtc_boot_magic = 0xB0070002;
    CopyField(g_rtc_last_phase, sizeof(g_rtc_last_phase), phase);
    g_rtc_last_phase_ms = BootMs();
}

void RecordPhase(const char* phase, const char* detail, bool log_heap) {
    PhaseEntry entry{};
    CopyField(entry.phase, sizeof(entry.phase), phase);
    CopyField(entry.detail, sizeof(entry.detail), detail);
    entry.t_ms = BootMs();
    entry.heap_int = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    entry.heap_psram = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    // s1ch: largest_free_block walks tlsf — skip under MSPI errata (same as PrintHeapStats).
    entry.heap_largest = 0;
    portENTER_CRITICAL(&g_phase_mux);
    if (std::strcmp(phase, "AFE_FIRST_FETCH") == 0) {
        g_afe_first_fetch_seen = true;
    }
    g_phases[g_phase_head] = entry;
    g_phase_head = (g_phase_head + 1) % kPhaseRingCap;
    if (g_phase_count < kPhaseRingCap) {
        ++g_phase_count;
    }
    CopyField(g_last_phase, sizeof(g_last_phase), phase);
    PersistCrashHint(phase);
    portEXIT_CRITICAL(&g_phase_mux);

    if (log_heap) {
        ESP_LOGI(TAG,
                 "PH | t=%ums phase=%s detail=%s | free_int=%u free_psram=%u largest_int=%u",
                 (unsigned)entry.t_ms,
                 entry.phase,
                 entry.detail[0] ? entry.detail : "-",
                 (unsigned)entry.heap_int,
                 (unsigned)entry.heap_psram,
                 (unsigned)entry.heap_largest);
    } else {
        ESP_LOGI(TAG, "PH | t=%ums phase=%s detail=%s",
                 (unsigned)entry.t_ms,
                 entry.phase,
                 entry.detail[0] ? entry.detail : "-");
    }
}

void RegisterCrashHandlers() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;

    esp_register_shutdown_handler([]() {
        const esp_reset_reason_t pending = esp_reset_reason();
        (void)pending;
        ESP_LOGW(TAG, "CRASH | shutdown last_phase=%s t=%ums",
                 g_last_phase, (unsigned)BootMs());
        BootTraceDumpSummary("shutdown");
    });
}

}  // namespace

void BootTraceInit() {
    g_boot_us = esp_timer_get_time();
    g_marker_echoed = false;
    g_afe_first_fetch_seen = false;
    g_local_ota_confirmed = false;
    RegisterCrashHandlers();

    if (g_rtc_boot_magic != 0xB0070002) {
        g_rtc_boot_count = 0;
        g_rtc_crash_streak = 0;
        g_rtc_boot_magic = 0xB0070002;
    }
    g_rtc_boot_count++;

    const esp_reset_reason_t reason = esp_reset_reason();
    // P4 HP_SYS_HP_WDT_RESET usually surfaces as INT_WDT / WDT / TASK_WDT.
    const bool crash_reboot = reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_PANIC ||
        reason == ESP_RST_BROWNOUT;
    if (crash_reboot) {
        g_rtc_crash_streak++;
    } else if (reason == ESP_RST_POWERON || reason == ESP_RST_SW || reason == ESP_RST_EXT ||
               reason == ESP_RST_USB || reason == ESP_RST_JTAG) {
        // USB/JTAG flash must clear streak — otherwise HoldFlashWindow fires forever
        // after WDT experiments and changes SD mount timing.
        g_rtc_crash_streak = 0;
    }
    ESP_LOGI(TAG, "FW_MARKER %s", FW_MARKER);
    // s1cf: bootloader INFO exceeds 0x6000 partition budget — print compile-time
    // SPIRAM speed here so the cold-start gate can verify it without bloating BL.
    // s1ck: also print nominal MEM_CLK (CPU360→MEM180 per IDF rtc_clk.c) for
    // MSPI-751 ratio notes; does not change clocks.
    {
        const int cpu_mhz = (int)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
        const int mem_mhz = (cpu_mhz >= 360) ? (cpu_mhz / 2) : cpu_mhz;
        ESP_LOGW(TAG, "SPIRAM_CFG speed_mhz=%d mode_hex=%d cpu_mhz=%d mem_mhz=%d s1cm",
                 (int)CONFIG_SPIRAM_SPEED,
#if CONFIG_SPIRAM_MODE_HEX
                 1,
#else
                 0,
#endif
                 cpu_mhz, mem_mhz);
    }
    ESP_LOGI(TAG, "BOOT | reason=%s reason_code=%d boot_count=%lu crash_streak=%lu prev_phase=%s prev_t=%lums",
             ResetReasonName(reason),
             (int)reason,
             (unsigned long)g_rtc_boot_count,
             (unsigned long)g_rtc_crash_streak,
             g_rtc_last_phase[0] ? g_rtc_last_phase : "-",
             (unsigned long)g_rtc_last_phase_ms);

    if (crash_reboot) {
        ESP_LOGW(TAG, "REBOOT_AFTER | reason=%s reason_code=%d prev_phase=%s prev_t=%lums",
                 ResetReasonName(reason),
                 (int)reason,
                 g_rtc_last_phase[0] ? g_rtc_last_phase : "-",
                 (unsigned long)g_rtc_last_phase_ms);
    }

    // s1cv: print and clear the previous boot's allocation-free audio/display
    // transaction breadcrumbs. This survives bare HP-WDT resets with no panic.
    AfeFetchGateBootReportAndReset();

    RecordPhase("APP_MAIN", ResetReasonName(reason), true);
}

void BootTraceMaybeEchoMarker() {
    // The P4 USB serial port can enumerate after the early boot marker has
    // already passed. Echo it once after startup settles so a soak log can be
    // attributed to the exact firmware without adding another timer or task.
    const uint32_t boot_ms = BootMs();
    if (!g_local_ota_confirmed && boot_ms >= 60000 && g_afe_first_fetch_seen) {
        g_local_ota_confirmed = OtaMarkCurrentVersionValid();
    }
    if (g_marker_echoed || boot_ms < 60000) {
        return;
    }
    g_marker_echoed = true;
    ESP_LOGI(TAG, "FW_MARKER %s echo=delayed_once", FW_MARKER);
}

void BootTraceMark(const char* phase, const char* detail) {
    RecordPhase(phase, detail, false);
}

void BootTraceMarkHeap(const char* phase) {
    RecordPhase(phase, nullptr, true);
}

void BootTraceDumpSummary(const char* reason) {
    PhaseEntry snapshot[kPhaseRingCap];
    size_t snapshot_count = 0;
    portENTER_CRITICAL(&g_phase_mux);
    snapshot_count = g_phase_count;
    const size_t oldest = (g_phase_head + kPhaseRingCap - g_phase_count) % kPhaseRingCap;
    for (size_t i = 0; i < snapshot_count; ++i) {
        snapshot[i] = g_phases[(oldest + i) % kPhaseRingCap];
    }
    char last_phase[sizeof(g_last_phase)];
    CopyField(last_phase, sizeof(last_phase), g_last_phase);
    portEXIT_CRITICAL(&g_phase_mux);

    std::string line;
    line.reserve(512);
    line += "SUMMARY | reason=";
    line += reason ? reason : "?";
    line += " boots=";
    line += std::to_string(g_rtc_boot_count);
    line += " last=";
    line += last_phase;
    line += " t=";
    line += std::to_string(BootMs());
    line += "ms | ring:";
    for (size_t i = 0; i < snapshot_count; ++i) {
        const auto& e = snapshot[i];
        line += " ";
        line += e.phase;
        line += "@";
        line += std::to_string(e.t_ms);
    }
    ESP_LOGW(TAG, "%s", line.c_str());
}

const char* BootTraceLastPhase() {
    return g_last_phase;
}

uint32_t BootTraceCrashStreak() {
    return g_rtc_crash_streak;
}

bool BootTraceInCrashStorm(uint32_t threshold) {
    if (threshold == 0) {
        return false;
    }
    return g_rtc_crash_streak >= threshold;
}

void BootTraceClearCrashStreak() {
    g_rtc_crash_streak = 0;
    ESP_LOGI(TAG, "crash_streak cleared");
}

#endif
