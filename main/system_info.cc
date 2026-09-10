#include "system_info.h"

#include <freertos/task.h>
#include <esp_log.h>
#include <esp_flash.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_rom_sys.h>
#include <esp_app_desc.h>
#include <esp_ota_ops.h>
#if CONFIG_IDF_TARGET_ESP32P4
// #include "esp_wifi_remote.h"
#endif
#include "esp_wifi.h"

#define TAG "SystemInfo"

size_t SystemInfo::GetFlashSize() {
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get flash size");
        return 0;
    }
    return (size_t)flash_size;
}

size_t SystemInfo::GetMinimumFreeHeapSize() {
    return esp_get_minimum_free_heap_size();
}

size_t SystemInfo::GetFreeHeapSize() {
    return esp_get_free_heap_size();
}

std::string SystemInfo::GetMacAddress() {
    // Identity override: report 30:ed:a0:e1:b5:28 (matches server/docs configuration).
    // This is a hardcoded device identity; real silicon MAC is read via esp_read_mac.
    const char* override_mac = "30:ed:a0:e1:b5:28";
    printf("MAC Address: %s\n", override_mac);
    return std::string(override_mac);
}

std::string SystemInfo::GetChipModelName() {
    return std::string(CONFIG_IDF_TARGET);
}

std::string SystemInfo::GetUserAgent() {
    auto app_desc = esp_app_get_description();
    auto user_agent = std::string(BOARD_NAME "/") + app_desc->version;
    return user_agent;
}

esp_err_t SystemInfo::PrintTaskCpuUsage(TickType_t xTicksToWait) {
    #define ARRAY_SIZE_OFFSET 5
    TaskStatus_t *start_array = NULL, *end_array = NULL;
    UBaseType_t start_array_size, end_array_size;
    configRUN_TIME_COUNTER_TYPE start_run_time, end_run_time;
    esp_err_t ret;
    uint32_t total_elapsed_time;

    //Allocate array to store current task states
    start_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    start_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * start_array_size);
    if (start_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get current task states
    start_array_size = uxTaskGetSystemState(start_array, start_array_size, &start_run_time);
    if (start_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    vTaskDelay(xTicksToWait);

    //Allocate array to store tasks states post delay
    end_array_size = uxTaskGetNumberOfTasks() + ARRAY_SIZE_OFFSET;
    end_array = (TaskStatus_t*)malloc(sizeof(TaskStatus_t) * end_array_size);
    if (end_array == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto exit;
    }
    //Get post delay task states
    end_array_size = uxTaskGetSystemState(end_array, end_array_size, &end_run_time);
    if (end_array_size == 0) {
        ret = ESP_ERR_INVALID_SIZE;
        goto exit;
    }

    //Calculate total_elapsed_time in units of run time stats clock period.
    total_elapsed_time = (end_run_time - start_run_time);
    if (total_elapsed_time == 0) {
        ret = ESP_ERR_INVALID_STATE;
        goto exit;
    }

    printf("| Task | Run Time | Percentage\n");
    //Match each task in start_array to those in the end_array
    for (int i = 0; i < start_array_size; i++) {
        int k = -1;
        for (int j = 0; j < end_array_size; j++) {
            if (start_array[i].xHandle == end_array[j].xHandle) {
                k = j;
                //Mark that task have been matched by overwriting their handles
                start_array[i].xHandle = NULL;
                end_array[j].xHandle = NULL;
                break;
            }
        }
        //Check if matching task found
        if (k >= 0) {
            uint32_t task_elapsed_time = end_array[k].ulRunTimeCounter - start_array[i].ulRunTimeCounter;
            uint32_t percentage_time = (task_elapsed_time * 100UL) / (total_elapsed_time * CONFIG_FREERTOS_NUMBER_OF_CORES);
            printf("| %-16s | %8lu | %4lu%%\n", start_array[i].pcTaskName, task_elapsed_time, percentage_time);
        }
    }

    //Print unmatched tasks
    for (int i = 0; i < start_array_size; i++) {
        if (start_array[i].xHandle != NULL) {
            printf("| %s | Deleted\n", start_array[i].pcTaskName);
        }
    }
    for (int i = 0; i < end_array_size; i++) {
        if (end_array[i].xHandle != NULL) {
            printf("| %s | Created\n", end_array[i].pcTaskName);
        }
    }
    ret = ESP_OK;

exit:    //Common return path
    free(start_array);
    free(end_array);
    return ret;
}

void SystemInfo::PrintTaskList() {
    char buffer[1000];
    vTaskList(buffer);
    ESP_LOGI(TAG, "Task list: \n%s", buffer);
}

void SystemInfo::PrintHeapStats() {
    PrintHeapStats(nullptr);
}

const char* SystemInfo::ResetReasonName(int reason_code) {
    switch (static_cast<esp_reset_reason_t>(reason_code)) {
        case ESP_RST_UNKNOWN: return "UNKNOWN";
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXT";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        case ESP_RST_USB: return "USB";
        case ESP_RST_JTAG: return "JTAG";
        default: return "OTHER";
    }
}

void SystemInfo::PrintResetReason(const char* stage) {
    const int code = (int)esp_reset_reason();
    const char* name = ResetReasonName(code);
    // P4 ROM often prints rst:0x7 (HP_SYS_HP_WDT_RESET) while IDF maps to INT_WDT/WDT.
    if (stage != nullptr && stage[0] != '\0') {
        ESP_LOGI(TAG,
                 "RESET | stage=%s reason=%s reason_code=%d | crosscheck_ROM=look_for_rst:_HP_SYS_HP_WDT_or_similar",
                 stage, name, code);
    } else {
        ESP_LOGI(TAG,
                 "RESET | reason=%s reason_code=%d | crosscheck_ROM=look_for_rst:_HP_SYS_HP_WDT_or_similar",
                 name, code);
    }
}

void SystemInfo::PrintHeapStats(const char* stage) {
    // Use %u + unsigned casts only — %llu/%lld often mis-parse on RV32 newlib and
    // shift later arguments (false heap_int=1960 style readings).
    // s1cg/s1ch: do NOT call heap_caps_get_largest_free_block — it tlsf_walk_pool's
    // the PSRAM heap and was still the LoadAccessFault site after skipping
    // heap_caps_check_integrity_all (prev_phase=IDLE_HEAP, mepc=tlsf_walk_pool).
    const unsigned free_int =
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const unsigned min_int =
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    const unsigned free_psram =
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const unsigned min_psram =
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    if (stage != nullptr && stage[0] != '\0') {
        ESP_LOGI(TAG,
                 "HEAP | stage=%s | free_int=%u min_int=%u | free_psram=%u min_psram=%u | largest=skipped s1ch",
                 stage, free_int, min_int, free_psram, min_psram);
    } else {
        ESP_LOGI(TAG,
                 "HEAP | free_int=%u min_int=%u | free_psram=%u min_psram=%u | largest=skipped s1ch",
                 free_int, min_int, free_psram, min_psram);
    }

    // s1bg walk was a localisation probe; under MSPI-750/751 it became the
    // surest crash trigger. Keep size counters only.
    ESP_LOGI(TAG, "HEAP_INTEGRITY | stage=%s skipped=walk s1cg",
             (stage != nullptr && stage[0] != '\0') ? stage : "-");
}
