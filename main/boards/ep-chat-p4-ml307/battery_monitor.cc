/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "i2c_bus.h"
#include "battery_monitor.h"

#define BATTERY_MONITOR_I2C_SDA_PIN  GPIO_NUM_7
#define BATTERY_MONITOR_I2C_SCL_PIN  GPIO_NUM_8

#define BATTERY_SHUTDOWN_SOC (10)

static const gauging_config_t g_gauge_config = {
    .CCT = 1,
    .CSYNC = 0,
    .EDV_CMP = 0,
    .SC = 1,
    .FIXED_EDV0 = 0,
    .FCC_LIM = 1,
    .FC_FOR_VDQ = 1,
    .IGNORE_SD = 1,
    .SME0 = 0,
};

static const parameter_cedv_t g_cedv = {
    .full_charge_cap = 650,
    .design_cap = 650,
    .reserve_cap = 0,
    .near_full = 200,
    .self_discharge_rate = 20,
    .EDV0 = 3490,
    .EDV1 = 3511,
    .EDV2 = 3535,
    .EMF = 3670,
    .C0 = 115,
    .R0 = 968,
    .T0 = 4547,
    .R1 = 4764,
    .TC = 11,
    .C1 = 0,
    .DOD0 = 4147,
    .DOD10 = 4002,
    .DOD20 = 3969,
    .DOD30 = 3938,
    .DOD40 = 3880,
    .DOD50 = 3824,
    .DOD60 = 3794,
    .DOD70 = 3753,
    .DOD80 = 3677,
    .DOD90 = 3574,
    .DOD100 = 3490,
};

static const char *TAG = "battery_monitor";

void BatteryMonitor::check_shutdown()
{
    if (battery_status.DSG == 0) {
        return;
    }
    if (this->getBatterySOC() <= BATTERY_SHUTDOWN_SOC) {
        ESP_LOGW(TAG, "Battery SOC is low, going to sleep");
        this->printInfo();
        if (this->shutdown_cb) {
            this->shutdown_cb();
        }
        esp_deep_sleep_start();
    }
}

void BatteryMonitor::printInfo() const
{
    battery_status_t status = {};
    bq27220_get_battery_status(bq27220Handle, &status);
    ESP_LOGI(TAG, "Battery Status - DSG: %d, SYSDWN: %d, TDA: %d, BATTPRES: %d, AUTH_GD: %d, OCVGD: %d, TCA: %d, RSVD: %d, CHGINH: %d, FC: %d, OTD: %d, OTC: %d, SLEEP: %d, OCVFAIL: %d, OCVCOMP: %d, FD: %d",
             status.DSG, status.SYSDWN, status.TDA, status.BATTPRES,
             status.AUTH_GD, status.OCVGD, status.TCA, status.RSVD,
             status.CHGINH, status.FC, status.OTD, status.OTC,
             status.SLEEP, status.OCVFAIL, status.OCVCOMP, status.FD);

    uint16_t vol = bq27220_get_voltage(bq27220Handle);
    int16_t current = bq27220_get_current(bq27220Handle);
    uint16_t rc = bq27220_get_remaining_capacity(bq27220Handle);
    uint16_t full_cap = bq27220_get_full_charge_capacity(bq27220Handle);
    uint16_t temp = bq27220_get_temperature(bq27220Handle) / 10 - 273;
    uint16_t cycle_cnt = bq27220_get_cycle_count(bq27220Handle);
    uint16_t soc = bq27220_get_state_of_charge(bq27220Handle);
    int16_t avg_power = bq27220_get_average_power(bq27220Handle);
    int16_t max_load = bq27220_get_maxload_current(bq27220Handle);
    uint16_t time_to_empty = bq27220_get_time_to_empty(bq27220Handle);
    uint16_t time_to_full = bq27220_get_time_to_full(bq27220Handle);

    ESP_LOGI(TAG, "Battery Info - Vol: %dmv, Current: %dmA, Power: %dmW, Remaining Capacity: %dmAh, Full Charge Capacity: %dmAh, Temperature: %dC, Cycle Count: %d, SOC: %d%%, Max Load: %dmA, Time to empty: %dmin, Time to full: %dmin",
             vol, current, avg_power, rc, full_cap, temp, cycle_cnt, soc, max_load, time_to_empty, time_to_full);
}

BatteryMonitor::BatteryMonitor() : bq27220Handle(nullptr),
    timer(nullptr),
    battery_status(),
    status_cb(nullptr),
    shutdown_cb(nullptr),
    period_cb(nullptr) {}

BatteryMonitor::~BatteryMonitor()
{
    ready_.store(false, std::memory_order_release);
    if (timer) {
        xTimerStop(timer, 0);
        xTimerDelete(timer, 0);
    }
    if (bq27220Handle) {
        bq27220_delete(bq27220Handle);
        bq27220Handle = nullptr;
    }
}

bool BatteryMonitor::init(i2c_master_bus_handle_t i2c_bus)
{
    ESP_LOGI(TAG, "Initializing battery monitor with shared I2C bus");

    if (i2c_bus == nullptr) {
        ESP_LOGE(TAG, "Invalid I2C bus handle");
        return false;
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BATTERY_MONITOR_I2C_SDA_PIN,
        .scl_io_num = BATTERY_MONITOR_I2C_SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed = 400 * 1000,
        },
    };

    i2c_bus_handle_t i2c_bus_handle = i2c_bus_create(I2C_NUM_0, &conf);
    if (i2c_bus_handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create i2c_bus wrapper");
        return false;
    }

    if (i2c_bus_get_internal_bus_handle(i2c_bus_handle) != i2c_bus) {
        ESP_LOGE(TAG, "i2c_bus wrapper does not match shared I2C bus");
        return false;
    }

    bq27220_config_t bq27220_cfg = {
        .i2c_bus = i2c_bus_handle,
        .cfg = &g_gauge_config,
        .cedv = &g_cedv,
    };

    bq27220Handle = bq27220_create(&bq27220_cfg);
    if (!bq27220Handle) {
        ESP_LOGE(TAG, "Failed to initialize BQ27220");
        return false;
    }

    if (getBatteryStatus(this->battery_status)) {
        charging_.store(this->battery_status.DSG == 0, std::memory_order_relaxed);
        discharging_.store(this->battery_status.DSG != 0, std::memory_order_relaxed);
    }
    const uint16_t initial_soc = bq27220_get_state_of_charge(bq27220Handle);
    if (initial_soc <= 100) {
        battery_soc_.store(static_cast<uint8_t>(initial_soc), std::memory_order_relaxed);
        check_shutdown();
    } else {
        ESP_LOGW(TAG, "Ignore invalid initial battery SOC: %u",
                 static_cast<unsigned>(initial_soc));
    }

    timer = xTimerCreate("battery_monitor", pdMS_TO_TICKS(1000), pdTRUE, this, monitor_period);
    if (timer == nullptr) {
        ESP_LOGE(TAG, "Failed to create battery monitor timer");
        bq27220_delete(bq27220Handle);
        bq27220Handle = nullptr;
        return false;
    }
    xTimerStart(timer, 0);
    ready_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Battery monitor initialized successfully");
    return true;
}

uint8_t BatteryMonitor::getBatterySOC() const
{
    return battery_soc_.load(std::memory_order_relaxed);
}

uint16_t BatteryMonitor::getCapacity() const
{
    return bq27220_get_design_capacity(bq27220Handle);
}

uint16_t BatteryMonitor::getFCC() const
{
    return bq27220_get_full_charge_capacity(bq27220Handle);
}

uint16_t BatteryMonitor::getVoltage() const
{
    return bq27220_get_voltage(bq27220Handle);
}

int16_t BatteryMonitor::getCurrent() const
{
    return bq27220_get_current(bq27220Handle);
}

uint16_t BatteryMonitor::getTemperature() const
{
    uint16_t temp = bq27220_get_temperature(bq27220Handle);
    return static_cast<int16_t>(temp / 10 - 273);
}

bool BatteryMonitor::getBatteryStatus(battery_status_t &status)
{
    return bq27220_get_battery_status(bq27220Handle, &status) == ESP_OK;
}

void BatteryMonitor::monitor_period(TimerHandle_t xTimer)
{
    BatteryMonitor &bm = *static_cast<BatteryMonitor *>(pvTimerGetTimerID(xTimer));
    if (!bm.is_ready()) {
        return;
    }
    if (bm.getBatteryStatus(bm.battery_status)) {
        bm.charging_.store(bm.battery_status.DSG == 0, std::memory_order_relaxed);
        bm.discharging_.store(bm.battery_status.DSG != 0, std::memory_order_relaxed);
    }
    if (bm.status_cb) {
        bm.status_cb(bm.battery_status);
    }

    static uint32_t count = 0;
    if (count++ % 5 == 0) {
        const uint16_t soc = bq27220_get_state_of_charge(bm.bq27220Handle);
        if (soc <= 100) {
            bm.battery_soc_.store(static_cast<uint8_t>(soc), std::memory_order_relaxed);
        } else {
            ESP_LOGW(TAG, "Ignore invalid battery SOC: %u", static_cast<unsigned>(soc));
        }
        bm.check_shutdown();
        if (bm.period_cb) {
            bm.period_cb();
        }
    }
}
