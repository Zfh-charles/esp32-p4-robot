#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Panic register capture — serial filter: PANIC_LAST
 *
 * esp_panic_handler() arms a 1s TG0 watchdog with WDT_STAGE_ACTION_RESET_SYSTEM
 * (IDF panic.c:245) before it prints anything. When the console dump does not
 * fit in that budget the chip resets as rst:0x7 (HP_SYS_HP_WDT_RESET) with no
 * output at all, so the crash cannot be attributed.
 *
 * This module wraps esp_panic_handler and mirrors the exception frame into RTC
 * memory first, so the next boot can always report where the crash happened.
 * Feed mepc/ra to riscv32-esp-elf-addr2line against the matching build.
 */
void PanicCaptureReport(void);

#ifdef __cplusplus
}
#endif
