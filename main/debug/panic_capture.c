#include "panic_capture.h"

#include <stddef.h>
#include <stdint.h>

#include <esp_attr.h>
#include <esp_cpu.h>
#include <esp_log.h>

#include "esp_private/cache_utils.h"
#include "riscv/rvruntime-frames.h"

#define TAG "PanicCap"

#define PANIC_CAPTURE_MAGIC 0x50414E32u

typedef struct {
    uint32_t magic;
    uint32_t entries;   // how many cores reached the panic entry before reset
    uint32_t via;       // 1 = panicHandler, 2 = xt_unhandled_exception
    int32_t core;
    uint32_t mepc;
    uint32_t ra;
    uint32_t sp;
    uint32_t mcause;
    uint32_t mtval;
    // Second core, when both trap almost simultaneously and deadlock each
    // other inside panic_enable_cache().
    int32_t core2;
    uint32_t mepc2;
    uint32_t mcause2;
    uint32_t mtval2;
    // Cache state at trap time. A LoadAccessFault on a mapped PSRAM address has
    // only two explanations: the address is genuinely unreachable, or the cache
    // was suspended. spi_flash_disable_cache() suspends CACHE_LL_LEVEL_EXT_MEM /
    // CACHE_TYPE_ALL, which covers PSRAM as well as flash, so a flash write in
    // flight makes every PSRAM load fault. These fields tell the two apart.
    uint32_t flashop_depth;
    int32_t flashop_core;
    uint32_t flashop_seq;
    uint32_t cache_on;
} panic_capture_t;

RTC_NOINIT_ATTR panic_capture_t g_rtc_panic_capture;

// Plain statics live in internal DRAM, so these stay readable from the panic
// path while the external-memory cache is suspended.
static volatile uint32_t s_flashop_depth;
static volatile int32_t s_flashop_core = -1;
static volatile uint32_t s_flashop_seq;

void __real_panicHandler(void *frame);
void __real_xt_unhandled_exception(void *frame);
void __real_spi_flash_disable_interrupts_caches_and_other_cpu(void);
void __real_spi_flash_enable_interrupts_caches_and_other_cpu(void);
void __real_spi_flash_disable_interrupts_caches_and_other_cpu_no_os(void);
void __real_spi_flash_enable_interrupts_caches_no_os(void);

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu(void)
{
    s_flashop_core = (int32_t)esp_cpu_get_core_id();
    s_flashop_seq++;
    s_flashop_depth++;
    __real_spi_flash_disable_interrupts_caches_and_other_cpu();
}

void IRAM_ATTR __wrap_spi_flash_enable_interrupts_caches_and_other_cpu(void)
{
    __real_spi_flash_enable_interrupts_caches_and_other_cpu();
    if (s_flashop_depth > 0) {
        s_flashop_depth--;
    }
}

void IRAM_ATTR __wrap_spi_flash_disable_interrupts_caches_and_other_cpu_no_os(void)
{
    s_flashop_core = (int32_t)esp_cpu_get_core_id();
    s_flashop_seq++;
    s_flashop_depth++;
    __real_spi_flash_disable_interrupts_caches_and_other_cpu_no_os();
}

void IRAM_ATTR __wrap_spi_flash_enable_interrupts_caches_no_os(void)
{
    __real_spi_flash_enable_interrupts_caches_no_os();
    if (s_flashop_depth > 0) {
        s_flashop_depth--;
    }
}

/*
 * Runs at the very first C instruction of the panic path — before
 * panic_enable_cache(), which is where this board actually hangs until the
 * panic watchdog resets it as a bare rst:0x7. Cache may be disabled here, so
 * this must touch nothing but RTC memory and the exception frame: no string
 * literals (rodata lives in PSRAM), no logging, no FreeRTOS calls.
 */
static void IRAM_ATTR CaptureFrame(const void *frame_ptr, uint32_t via)
{
    const RvExcFrame *frame = (const RvExcFrame *)frame_ptr;

    if (g_rtc_panic_capture.magic == PANIC_CAPTURE_MAGIC) {
        // A core already recorded its frame during this same panic; keep the
        // first one and log the second separately.
        g_rtc_panic_capture.entries++;
        if (frame != NULL) {
            g_rtc_panic_capture.core2 = (int32_t)frame->mhartid;
            g_rtc_panic_capture.mepc2 = (uint32_t)frame->mepc;
            g_rtc_panic_capture.mcause2 = (uint32_t)frame->mcause;
            g_rtc_panic_capture.mtval2 = (uint32_t)frame->mtval;
        }
        return;
    }

    g_rtc_panic_capture.magic = PANIC_CAPTURE_MAGIC;
    g_rtc_panic_capture.entries = 1;
    g_rtc_panic_capture.via = via;
    g_rtc_panic_capture.core = -1;
    g_rtc_panic_capture.mepc = 0;
    g_rtc_panic_capture.ra = 0;
    g_rtc_panic_capture.sp = 0;
    g_rtc_panic_capture.mcause = 0;
    g_rtc_panic_capture.mtval = 0;
    g_rtc_panic_capture.core2 = -1;
    g_rtc_panic_capture.mepc2 = 0;
    g_rtc_panic_capture.mcause2 = 0;
    g_rtc_panic_capture.mtval2 = 0;
    g_rtc_panic_capture.flashop_depth = s_flashop_depth;
    g_rtc_panic_capture.flashop_core = s_flashop_core;
    g_rtc_panic_capture.flashop_seq = s_flashop_seq;
    g_rtc_panic_capture.cache_on = 0xffffffffu;

    if (frame != NULL) {
        g_rtc_panic_capture.core = (int32_t)frame->mhartid;
        g_rtc_panic_capture.mepc = (uint32_t)frame->mepc;
        g_rtc_panic_capture.ra = (uint32_t)frame->ra;
        g_rtc_panic_capture.sp = (uint32_t)frame->sp;
        g_rtc_panic_capture.mcause = (uint32_t)frame->mcause;
        g_rtc_panic_capture.mtval = (uint32_t)frame->mtval;
    }

    // Read last: cache_hal lives in IRAM so this is safe with the cache down,
    // but if it ever is not, the frame above is already committed and the stale
    // 0xffffffff marks the read as the thing that failed.
    g_rtc_panic_capture.cache_on = spi_flash_cache_enabled() ? 1u : 0u;
}

void IRAM_ATTR __wrap_panicHandler(void *frame)
{
    CaptureFrame(frame, 1);
    __real_panicHandler(frame);
}

void IRAM_ATTR __wrap_xt_unhandled_exception(void *frame)
{
    CaptureFrame(frame, 2);
    __real_xt_unhandled_exception(frame);
}

static const char *CauseName(uint32_t mcause)
{
    switch (mcause) {
        case 1:
            return "InstrAccessFault";
        case 2:
            return "IllegalInstr";
        case 3:
            return "Breakpoint";
        case 5:
            return "LoadAccessFault";
        case 7:
            return "StoreAccessFault";
        case 11:
            return "EnvCall";
        default:
            return "other";
    }
}

void PanicCaptureReport(void)
{
    if (g_rtc_panic_capture.magic != PANIC_CAPTURE_MAGIC) {
        // Printed every boot so that a missing record is never confused with a
        // missing capture hook.
        ESP_LOGI(TAG, "PANIC_LAST | none armed=1 hook=panicHandler");
        return;
    }
    g_rtc_panic_capture.magic = 0;

    ESP_LOGW(TAG,
             "PANIC_LAST | core=%d via=%lu cores_trapped=%lu mcause=%lu(%s) "
             "mepc=0x%08lx ra=0x%08lx sp=0x%08lx mtval=0x%08lx",
             (int)g_rtc_panic_capture.core,
             (unsigned long)g_rtc_panic_capture.via,
             (unsigned long)g_rtc_panic_capture.entries,
             (unsigned long)g_rtc_panic_capture.mcause,
             CauseName(g_rtc_panic_capture.mcause),
             (unsigned long)g_rtc_panic_capture.mepc,
             (unsigned long)g_rtc_panic_capture.ra,
             (unsigned long)g_rtc_panic_capture.sp,
             (unsigned long)g_rtc_panic_capture.mtval);

    if (g_rtc_panic_capture.entries > 1) {
        ESP_LOGE(TAG,
                 "PANIC_LAST | DUAL_CORE_TRAP core2=%d mcause2=%lu(%s) mepc2=0x%08lx mtval2=0x%08lx",
                 (int)g_rtc_panic_capture.core2,
                 (unsigned long)g_rtc_panic_capture.mcause2,
                 CauseName(g_rtc_panic_capture.mcause2),
                 (unsigned long)g_rtc_panic_capture.mepc2,
                 (unsigned long)g_rtc_panic_capture.mtval2);
    }

    // flashop_depth>0 or cache_on=0 means the fault address was unreachable
    // because the external-memory cache was suspended, not because PSRAM itself
    // failed. Both zero points back at the PSRAM timing / 200M config.
    ESP_LOGW(TAG,
             "PANIC_LAST | CACHE cache_on=%lu flashop_depth=%lu flashop_core=%d flashop_seq=%lu",
             (unsigned long)g_rtc_panic_capture.cache_on,
             (unsigned long)g_rtc_panic_capture.flashop_depth,
             (int)g_rtc_panic_capture.flashop_core,
             (unsigned long)g_rtc_panic_capture.flashop_seq);
}
