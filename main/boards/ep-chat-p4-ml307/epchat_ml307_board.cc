#include "ml307_board.h"
#include "codecs/box_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
#include "eezui_display_adapter.h"
// #include "display/no_display.h"
#include "button.h"
#include "mcp_server.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"

#include "esp_lcd_st7701.h"
#include "config.h"

// #include <wifi_station.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <driver/i2c_master.h>
#include <esp_lvgl_port.h>
#if CONFIG_USE_REMINDER_POLL
#include "reminder/boot_trace.h"
#endif
#include "driver/uart.h"
#include "driver/gpio.h"

// C 语言头文件放在 extern "C" 中
extern "C" {
#include "sd_scanner.h"
}

// C++ 类头文件放在 extern "C" 外面
#include "battery_monitor.h"

#define TAG "EpChatP4ML307"

LV_FONT_DECLARE(font_puhui_30_4);
LV_FONT_DECLARE(font_awesome_30_4);
extern const lv_font_t* font_emoji_64_init();

class EpChatP4ML307 : public Ml307Board {
private:
    i2c_master_bus_handle_t i2c_bus_;  // 共享的 I2C 总线
    Button boot_button_;
    MipiEezuiDisplayAdapter *display_;
    BatteryMonitor ep_battery_level_;
    esp_lcd_panel_io_handle_t io_;
    esp_lcd_panel_handle_t panel_;
    

    void InitializeSharedI2c() {
        ESP_LOGI(TAG, "初始化共享 I2C 总线");
        
        // 初始化共享的 I2C 总线，供音频编解码器和电池监控器使用
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,  // 使用 I2C 端口 0
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        
        esp_err_t ret = i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "❌ I2C 总线初始化失败: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "✅ 共享 I2C 总线初始化成功 (I2C_NUM_0, SDA=%d, SCL=%d)", 
                     AUDIO_CODEC_I2C_SDA_PIN, AUDIO_CODEC_I2C_SCL_PIN);
        }
    }

    static esp_err_t bsp_enable_dsi_phy_power(void) {
#if MIPI_DSI_PHY_PWR_LDO_CHAN > 0
        // Turn on the power for MIPI DSI PHY, so it can go from "No Power" state to "Shutdown" state
        static esp_ldo_channel_handle_t phy_pwr_chan = NULL;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan);
        ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif // BSP_MIPI_DSI_PHY_PWR_LDO_CHAN > 0

        return ESP_OK;
    }

    void InitializeLCD() {
        bsp_enable_dsi_phy_power();
        esp_lcd_dsi_bus_handle_t mipi_dsi_bus = NULL;
        esp_lcd_dsi_bus_config_t bus_config = {
            .bus_id = 0,
            .num_data_lanes = 2,
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = 400,  // 降低位速率提高稳定性
        };
        esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus);


        ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
        // we use DBI interface to send LCD commands and parameters
        esp_lcd_dbi_io_config_t dbi_config = ST7701_PANEL_IO_DBI_CONFIG();
        esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_);

        esp_lcd_dpi_panel_config_t dpi_config = {
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = 20, // 降低时钟频率提高稳定性
            .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
            .num_fbs = 1,
            .video_timing = {
                .h_size = DISPLAY_WIDTH,
                .v_size = DISPLAY_HEIGHT,
                .hsync_pulse_width = 20,
                .hsync_back_porch = 40,
                .hsync_front_porch = 40,
                .vsync_pulse_width = 20,
                .vsync_back_porch = 30,
                .vsync_front_porch = 30,
            },
            .flags = {
                .use_dma2d = true,
            },
        };
        st7701_vendor_config_t vendor_config = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(st7701_lcd_init_cmd_t),
            .mipi_config = {
                .dsi_bus = mipi_dsi_bus,
                .dpi_config = &dpi_config,
                // .lane_num = 2,
            },
            .flags = {
                .use_mipi_interface = 1,
            },
        };

        const esp_lcd_panel_dev_config_t lcd_dev_config = {
            .reset_gpio_num = PIN_NUM_LCD_RST,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };
        esp_lcd_new_panel_st7701(io_, &lcd_dev_config, &panel_);
        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_reset(panel_);  // 初始化前再次重置屏幕
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_disp_on_off(panel_, true);
        ESP_LOGI(TAG, "LCD面板初始化完成");
    }

    void InitializeLvglDisplay() {
        // 初始化LVGL端口
        ESP_LOGI(TAG, "初始化LVGL端口");
        // s1ap: pin LVGL to HP core1 — audio_input is pinned core0; serial WDT showed
        // taskLVGL+face unlock on core0 under TTS (same-core starve).
        const lvgl_port_cfg_t lvgl_cfg = {
            .task_priority = 4,
            .task_stack = 8192,
            .task_affinity = 1,
            .task_max_sleep_ms = 500,
            .timer_period_ms = 5
        };
        ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
        ESP_LOGW(TAG, "LVGL port affinity=1 (s1ap isolate from audio_input@0)");
        
        // 创建MIPI EEZ UI显示器实例
        ESP_LOGI(TAG, "创建MIPI EEZ UI显示器实例");
        display_ = new MipiEezuiDisplayAdapter(io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                               DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
                                               DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "MIPI EEZ UI显示器初始化完成");
    }

    void InitializeSDCard() {
        ESP_LOGI(TAG, "初始化SD卡扫描器");
#if CONFIG_USE_REMINDER_POLL
        BootTraceMark("SD_MOUNT", "begin");
#endif
        // USB 烧录硬复位后 SDMMC 常 ESP_ERR_TIMEOUT；先等电源轨再挂，失败再重试。
        {
            const esp_reset_reason_t rr = esp_reset_reason();
            if (rr == ESP_RST_USB || rr == ESP_RST_POWERON || rr == ESP_RST_JTAG) {
                ESP_LOGW(TAG, "SD settle delay after reset_reason=%d", (int)rr);
                vTaskDelay(pdMS_TO_TICKS(800));
            }
        }
        esp_err_t ret = ESP_FAIL;
        for (int attempt = 1; attempt <= 3; attempt++) {
            ret = sd_scanner_init_and_scan();
            if (ret == ESP_OK) {
                break;
            }
            ESP_LOGW(TAG, "SD mount attempt %d/3 fail: %s", attempt, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "⚠️ SD卡初始化失败: %s", esp_err_to_name(ret));
            const esp_reset_reason_t rr = esp_reset_reason();
            if (rr == ESP_RST_USB || rr == ESP_RST_JTAG) {
                // USB/esptool 硬复位后 SDMMC OCR 常 TIMEOUT；冷启（断电上电）才能稳挂。
                // 勿在此误判为「表情读取代码坏了」——见 .cursor/rules 烧录后强制门。
                ESP_LOGE(TAG,
                         "SD_POWER_CYCLE_REQUIRED | reset_reason=%d mount=%s | "
                         "USB flash reset left SD bus dead — unplug POWER then replug "
                         "(not RST). Wait SD_MOUNT_OK + seed_stills ok=6/6 before face test.",
                         (int)rr, esp_err_to_name(ret));
                esp_rom_printf("!!SD_POWER_CYCLE_REQUIRED\n");
            }
#if CONFIG_USE_REMINDER_POLL
            BootTraceMark("SD_MOUNT", "fail");
#endif
        } else {
            ESP_LOGI(TAG, "✅ SD卡初始化成功");
#if CONFIG_USE_REMINDER_POLL
            BootTraceMarkHeap("SD_MOUNT_OK");
            // Do NOT clear crash_streak here — SD OK does not mean WDT storm is over
            // (serial: clear then ASSETS MJPEG → rst:0x7 loop). Clear after wake stable.
#endif
        }
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            app.ToggleChatState();
        });
    }

    void InitializeBatteryMonitor() {
        ESP_LOGI(TAG, "初始化电池监控器（使用共享 I2C 总线）");

        // The gauge occasionally does not answer immediately after a cold
        // power-up.  Retry here, while the shared bus is otherwise idle;
        // retrying later would contend with the audio codecs on the same bus.
        constexpr TickType_t kRetryDelay[] = {
            0,
            pdMS_TO_TICKS(500),
            pdMS_TO_TICKS(1200),
            pdMS_TO_TICKS(2500),
        };
        constexpr size_t kAttemptCount = sizeof(kRetryDelay) / sizeof(kRetryDelay[0]);
        for (size_t attempt = 0; attempt < kAttemptCount; ++attempt) {
            if (kRetryDelay[attempt] > 0) {
                ESP_LOGW(TAG, "BATTERY_INIT_RETRY wait_ms=%lu attempt=%u/%u",
                         static_cast<unsigned long>(kRetryDelay[attempt] * portTICK_PERIOD_MS),
                         static_cast<unsigned>(attempt + 1),
                         static_cast<unsigned>(kAttemptCount));
                vTaskDelay(kRetryDelay[attempt]);
            }
            if (ep_battery_level_.init(i2c_bus_)) {
                ESP_LOGI(TAG, "BATTERY_INIT_OK attempt=%u/%u soc=%u",
                         static_cast<unsigned>(attempt + 1),
                         static_cast<unsigned>(kAttemptCount),
                         static_cast<unsigned>(ep_battery_level_.getBatterySOC()));
                ep_battery_level_.printInfo();
                return;
            }
            ESP_LOGW(TAG, "BATTERY_INIT_FAIL attempt=%u/%u",
                     static_cast<unsigned>(attempt + 1),
                     static_cast<unsigned>(kAttemptCount));
        }
        ESP_LOGW(TAG, "BATTERY_INIT_GIVE_UP attempts=%u; battery remains unavailable",
                 static_cast<unsigned>(kAttemptCount));
    }

public:
    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();
        
        // 获取电池电量百分比
        mcp_server.AddTool("self.battery.get_level", 
            "获取电池电量百分比(0-100%)",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                if (!ep_battery_level_.is_ready()) {
                    return "battery monitor unavailable";
                }
                int level = ep_battery_level_.getBatterySOC();
                ESP_LOGI(TAG, "获取电池电量: %d%%", level);
                return level;
            });

        // 获取电池电压
        mcp_server.AddTool("self.battery.get_voltage", 
            "获取电池电压(mV)",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                if (!ep_battery_level_.is_ready()) {
                    return "battery monitor unavailable";
                }
                int voltage = ep_battery_level_.getVoltage();
                ESP_LOGI(TAG, "获取电池电压: %dmV", voltage);
                return voltage;
            });

        // 获取电池温度
        mcp_server.AddTool("self.battery.get_temperature", 
            "获取电池温度(°C)",
            PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                if (!ep_battery_level_.is_ready()) {
                    return "battery monitor unavailable";
                }
                int temp = ep_battery_level_.getTemperature();
                ESP_LOGI(TAG, "获取电池温度: %d°C", temp);
                return temp;
            });

      
    }

    EpChatP4ML307() : Ml307Board(Module_4G_TX_PIN, Module_4G_RX_PIN, ML307_DTR_PIN),
        i2c_bus_(nullptr),
        boot_button_(BOOT_BUTTON_GPIO) {
        // 1. 先初始化共享 I2C 总线
        InitializeSharedI2c();
        
        // 2. 初始化电池监控器（使用共享 I2C 总线）
        InitializeBatteryMonitor();

        // 3. SD before MIPI display — avoid LDO3/display rail starving SD LDO4 at OCR.
        InitializeSDCard();
        
        // 4. 显示
        InitializeLCD();
        InitializeLvglDisplay();
        InitializeButtons();
        
        RegisterMcpTools();
        GetBacklight()->RestoreBrightness();
        
    }

    virtual AudioCodec* GetAudioCodec() override {
        // 音频编解码器使用同一个共享的 I2C 总线
        static BoxAudioCodec audio_codec(
            i2c_bus_,  // 使用共享的 I2C 总线
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!ep_battery_level_.is_ready()) {
            level = 0;
            charging = false;
            discharging = false;
            return false;
        }
        level = static_cast<int>(ep_battery_level_.getBatterySOC());
        if (level < 0) {
            level = 0;
        } else if (level > 100) {
            level = 100;
        }
        charging = ep_battery_level_.is_charging();
        discharging = ep_battery_level_.is_discharging();
        return true;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
};

DECLARE_BOARD(EpChatP4ML307);
