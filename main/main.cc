#include <esp_log.h>
#include <esp_err.h>
#include <esp_system.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"
#include "debug/panic_capture.h"
#if CONFIG_USE_REMINDER_POLL
#include "reminder/boot_trace.h"
#endif

#define TAG "main"

// Consecutive crash reboots after which boot parks instead of starting the app.
static constexpr uint32_t kSafeModeCrashStreak = 5;

/**
 * After WDT/panic the USB-Serial/JTAG COM port often becomes unwritable until the
 * chip sits quiet long enough for the host to re-enumerate / for esptool to grab it.
 * Hold here BEFORE Application::Start() brings up display/AFE/MJPEG.
 *
 * s1bg: scale hold with crash_streak. A fixed 8s window is too short for a full
 * flash (~90s) during reboot storms, which left COM7 in Write-timeout /
 * PermissionError loops. Longer quiet on high streak is the flash-safe gate.
 */
static void HoldFlashWindowIfNeeded(void)
{
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool crash_reboot =
        reason == ESP_RST_WDT || reason == ESP_RST_TASK_WDT ||
        reason == ESP_RST_INT_WDT || reason == ESP_RST_PANIC ||
        reason == ESP_RST_BROWNOUT;
    // Only hold after an actual crash reboot. Do NOT hold on stale crash_storm
    // after USB flash — that delayed SD mount and broke emotion init.
    if (!crash_reboot) {
        return;
    }

#if CONFIG_USE_REMINDER_POLL
    const uint32_t streak = BootTraceCrashStreak();
#else
    const uint32_t streak = 0;
#endif

    // A reboot storm is the one state that can lock us out of the board: every
    // boot brings up display/AFE/MJPEG, crashes ~25s later and resets USB, so
    // esptool never gets the ~80s of quiet a full flash needs. Past this many
    // consecutive crashes, stop before Application::Start() and stay parked so
    // the port is always writable. RTC crash_streak is lost on power loss, so a
    // power-cycle is the way out.
    if (streak >= kSafeModeCrashStreak) {
        ESP_LOGE(TAG,
                 "SAFE_MODE | streak=%lu reason_code=%d — boot halted BEFORE app start. "
                 "USB stays writable: flash now. Power-cycle to leave safe mode.",
                 (unsigned long)streak, (int)reason);
        int parked_s = 0;
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            parked_s += 5;
            ESP_LOGE(TAG, "SAFE_MODE | parked=%ds streak=%lu — waiting for flash",
                     parked_s, (unsigned long)streak);
        }
    }

    int hold_s = 8;
    if (streak >= 3) {
        hold_s = 90;  // covers a full multi-image flash at 460800
    } else if (streak >= 2) {
        hold_s = 30;
    }

    ESP_LOGW(TAG,
             "FLASH_WINDOW | hold=%ds streak=%lu reason_code=%d | "
             "USB quiet for esptool — run flash now if needed",
             hold_s, (unsigned long)streak, (int)reason);
    for (int i = 1; i <= hold_s; ++i) {
        ESP_LOGW(TAG, "FLASH_WINDOW | %ds/%ds remaining_quiet", i, hold_s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGW(TAG, "FLASH_WINDOW | done — continuing boot");
}

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

#if CONFIG_USE_REMINDER_POLL
    BootTraceInit();
#endif

    PanicCaptureReport();

    HoldFlashWindowIfNeeded();

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
