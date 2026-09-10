#include "eezui_display_adapter.h"
#include "face_mouth_layer.h"
#include "face_asset_source.h"
#include "face_speech_core_bank.h"
#include "face_route_v2.h"
#include "visual_budget_v2.h"
#include "screen_presenter.h"
#include "wdt_contention_diag.h"

#include <esp_log.h>
#include <esp_err.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <esp_rom_sys.h>
#include <esp_lvgl_port.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <strings.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include "ui.h"
#include "lvgl.h"
#include "src/misc/lv_timer.h"
#include "sd_scanner.h"
#include <sys/stat.h>
#include "application.h"
#include "board.h"
#include "font_awesome.h"
#include "reminder/boot_trace.h"
#include "afe_fetch_gate.h"

extern "C" {
#include "emotion_video_player.h"



}

#define TAG "EezuiDisplayAdapter"

// 逐字显示效果的延迟时间（毫秒）
#define TYPEWRITER_DELAY_MS 50

// MID 时间片的 ROI 帧预算。s1aj 曾从 18 回退到 12，依据是「加帧导致 WDT」；
// s1ba 已证伪该因果（崩溃在 WakeNet PSRAM 内核，与帧数无关），故恢复 18。
// 说话期的收缩仍由节奏（FaceCadenceMs 的退档）承担，不再靠砍帧数。
static constexpr int kFaceMidFrames = 18;
// s1bo: MID cadence steps (0 = target, then backoff) and the present cost that triggers one.
static constexpr int kFaceCadenceSteps = 3;
// s1da: current generated eye ROI moves brows/upper face as a rigid tile.
// Keep the pose-bank assets loaded for future masked resources, but disable the
// coupled blink/pose choreography so dialogue falls back to emotion enter + mouth.
static constexpr bool kBlinkPoseChoreographyEnabled = false;
/** s1cb: speak backs off earlier so TTS keeps CPU/bus headroom (was 35). */
static constexpr int kFacePresentSlowMs = 28;
/** Dialogue + idle breathe share the same row cap. s1cb tried breathe=200 for AFE
 *  headroom → head/body frankenstein (top/bot alternate on full dirty). Reverted. */
static constexpr int kBandMaxRowsCap = 400;
/** LVGL draw buffer quantum = full width × these rows (see AGENTS.md). */
static constexpr int kLvglBandRows = 50;
static constexpr int kBandDiffColStep = 8;
/** s1cp-g: fixed enter cadence — uniform enter, not FaceCadence backoff. */
static constexpr int kEnterCadenceMs = 180;
/** s1am: speaking settle before ROI MID. */
static constexpr int64_t kSpeakSettleUs = 700 * 1000;
/** s1cr-h: mouth follow tick interval. */
static constexpr int64_t kMouthFollowTickUs = 80 * 1000;
/** s1ef: low-frequency life pose; deliberately slower than speech articulation. */
static constexpr int64_t kLifeFirstDueUs = 2400 * 1000LL;
static constexpr int64_t kLifeHoldUs = 4200 * 1000LL;
static constexpr int64_t kLifeBlendStepUs = 360 * 1000LL;
/** Display fence wait under AFE / fullscreen gate. */
static constexpr uint32_t kFlushFenceWaitMs = 100;
/** Common SafeLVGLLock budgets (values unchanged; names for readability / smell extract). */
static constexpr int kLvglLockUiMs = 200;
static constexpr int kLvglLockQuickMs = 50;
static constexpr int kLvglLockPresentMs = 80;
static constexpr int kLvglLockMouthMs = 40;
static constexpr int kLvglLockStyleMs = 100;  /* caption / theme chrome */
static constexpr int kLvglLockTickMs = 10;    /* anim tick best-effort */
static constexpr int kLvglLockCbMs = 5;       /* short callback / timer path */

namespace {

// Optional-canary diagnostics are internal-DRAM strings.  This preserves the
// P4 v1.0 pre-IROM/TCM topology while adding no pixel buffer at runtime.
DRAM_ATTR const char kIdleFlashArmLog[] = "!!FACE_S1GT arm rows=48 flash=1\n";
DRAM_ATTR const char kIdleFlashFallbackLog[] = "!!FACE_S1GT asset=0 fallback=1\n";
DRAM_ATTR const char kIdleFlashFrameLog[] = "!!FACE_S1GT f=%u abort=%u err=%d next=%u\n";
DRAM_ATTR const char kIdleFlashDropLog[] = "!!FACE_S1GT drop=%u\n";

bool IsLifeLayerEmotion(const std::string& emotion) {
    return strcasecmp(emotion.c_str(), "happy") == 0 ||
           strcasecmp(emotion.c_str(), "neutral") == 0 ||
           strcasecmp(emotion.c_str(), "standby") == 0 ||
           strcasecmp(emotion.c_str(), "relaxed") == 0 ||
           strcasecmp(emotion.c_str(), "laughing") == 0 ||
           strcasecmp(emotion.c_str(), "funny") == 0;
}

struct BandRoiResult {
    int dirty_first = -1;
    int dirty_last = -1;
    int band_y1 = 0;
    int band_y2 = 0;
    bool capped = false;
    const char* cap_anchor = "full";
    const char* scan_mode = "full";
};

/** Pure geometry after dirty_first/last known. Mutates *cap_bot_toggle when capped. */
void ComputeCappedBandRoi(int dirty_first, int dirty_last, int frame_h, int band_max_rows,
                          bool* cap_bot_toggle, BandRoiResult* out) {
    int band_y1 = (dirty_first / kLvglBandRows) * kLvglBandRows;
    int band_y2 = ((dirty_last / kLvglBandRows) + 1) * kLvglBandRows - 1;
    if (band_y2 > frame_h - 1) {
        band_y2 = frame_h - 1;
    }
    bool capped = false;
    const char* cap_anchor = "full";
    if (band_y2 - band_y1 + 1 > band_max_rows) {
        capped = true;
        if (*cap_bot_toggle) {
            cap_anchor = "bot";
            band_y2 = dirty_last;
            if (band_y2 > frame_h - 1) {
                band_y2 = frame_h - 1;
            }
            band_y1 = band_y2 - band_max_rows + 1;
            if (band_y1 < 0) {
                band_y1 = 0;
                band_y2 = band_max_rows - 1;
                if (band_y2 > frame_h - 1) {
                    band_y2 = frame_h - 1;
                }
            }
        } else {
            cap_anchor = "top";
            band_y1 = dirty_first;
            if (band_y1 < 0) {
                band_y1 = 0;
            }
            band_y2 = band_y1 + band_max_rows - 1;
            if (band_y2 > frame_h - 1) {
                band_y2 = frame_h - 1;
            }
        }
        *cap_bot_toggle = !*cap_bot_toggle;
    }
    if (band_y2 - band_y1 + 1 > band_max_rows) {
        band_y2 = band_y1 + band_max_rows - 1;
        if (band_y2 > frame_h - 1) {
            band_y2 = frame_h - 1;
        }
        capped = true;
    }
    out->band_y1 = band_y1;
    out->band_y2 = band_y2;
    out->capped = capped;
    out->cap_anchor = cap_anchor;
}

void CopyRgb565BandRows(uint8_t* dst, uint32_t dst_stride_px, const uint8_t* src,
                        uint32_t src_stride_px, int y1, int y2, int x1, int row_px) {
    for (int y = y1; y <= y2; y++) {
        memcpy(dst + ((size_t)y * dst_stride_px + (size_t)x1) * 2,
               src + ((size_t)y * src_stride_px + (size_t)x1) * 2, (size_t)row_px * 2);
    }
}

void IdleVisualAfeEdgeNotify(void*) {
    // Normal audio-task context, never ISR: only overwrite/post the one-slot
    // face-worker token.  All gate checks and display work remain in the worker.
    FaceRouteV2_PostTick();
}

}  // namespace

// 注意：表情映射表已统一到emotion_video_player.c中，这里不再维护重复的映射

// ========== TimerData析构函数实现 ==========
TimerData::~TimerData() {
    if (hide_timer && adapter) {
        adapter->SetHideTimer(nullptr);
    }
}

EezuiDisplayAdapter::EezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : Display(), panel_io_(panel_io), panel_(panel), dialogue_box_(nullptr), 
      typewriter_index_(0), typewriter_timer_(nullptr), typewriter_active_(false),
      emotion_player_(nullptr), video_canvas_(nullptr) {
    width_ = width;
    height_ = height;
    
    
    // 初始化状态管理
    init_state_ = InitState::NONE;
    current_status_ = Status::kIdle;
    presenter_ = new ScreenPresenter(this);
    FaceRouteV2_BootLog();
    if (FaceRouteV2_WorkerEnabled()) {
        FaceRouteV2_EnsureWorker(&EezuiDisplayAdapter::FaceWorkerTickTrampoline, this);
    }
}

EezuiDisplayAdapter::~EezuiDisplayAdapter() {
        StopFacePanelAnim("dtor");
        if (idle_backlight_timer_ != nullptr) {
            esp_timer_delete(idle_backlight_timer_);
            idle_backlight_timer_ = nullptr;
        }
        if (face_anim_timer_ != nullptr) {
            esp_timer_delete(face_anim_timer_);
            face_anim_timer_ = nullptr;
        }
        StopTypewriterEffect();
        if (presenter_ != nullptr) {
            delete presenter_;
            presenter_ = nullptr;
        }
        if (last_bypass_rgb_ != nullptr) {
            heap_caps_free(last_bypass_rgb_);
            last_bypass_rgb_ = nullptr;
            last_bypass_cap_ = 0;
            last_bypass_size_ = 0;
        }
        DeinitEmotionSystem();

        if (hide_timer_ != nullptr) {
            void* timer_data = lv_timer_get_user_data(hide_timer_);
            if (timer_data) {
                delete static_cast<TimerData*>(timer_data);
            }
            lv_timer_del(hide_timer_);
            hide_timer_ = nullptr;
        }
    }

void EezuiDisplayAdapter::ShowFaceCanvasLayers() {
    // Any canonical face commit supersedes the idle child overlay.  This runs
    // under the caller's LVGL lock and closes the rare StopIdleBreathe try-lock miss.
    idle_flash_overlay_.Hide();
    if (main_image_ != nullptr) {
        lv_obj_add_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (video_canvas_ != nullptr) {
        lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(video_canvas_);
    }
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
    }
    if (battery_panel_ != nullptr) {
        lv_obj_move_foreground(battery_panel_);
    }
}

void EezuiDisplayAdapter::SetupUI() {
    if (!SafeLVGLLock(kLvglLockUiMs)) {
        ESP_LOGE(TAG, "无法获取LVGL锁，放弃UI初始化");
        return;
    }


    // 设置默认显示器
    if (display_ != nullptr) {
        lv_disp_set_default(display_);
    } else {
        ESP_LOGE(TAG, "Display is null!");
        SafeLVGLUnlock();
        return;
    }

    // 初始化 eezui
    ui_init();
    
    // 获取 EEZ UI 对象引用
    dialogue_box_ = objects.dialogue_box;
    // EEZ renamed resident art to error_image (img_main). Must bind — nullptr left it
    // always visible under/over SD face canvas (invisible emotion / boot tear).
    main_image_ = objects.error_image;

    // 初始化时隐藏音量条
    if (objects.volume_bar) {
        lv_obj_add_flag(objects.volume_bar, LV_OBJ_FLAG_HIDDEN);
    }

    SetupBatteryUI();

    // 初始状态设置 — 与对话期同一套字幕样式（EnsureDialogueCaptionStyle）。
    if (dialogue_box_ != nullptr) {
        EnsureDialogueCaptionStyle();
        lv_label_set_text(dialogue_box_, "你好，小易！");
        lv_obj_add_event_cb(dialogue_box_, DialogueBoxClickCallback, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(dialogue_box_, LV_OBJ_FLAG_CLICKABLE);
        UpdateDialogueBoxHeight();
        ESP_LOGW(TAG, "CTRL UI dialogue unified style (idle==conversation)");
    } else {
        ESP_LOGE(TAG, "dialogue_box_ is null after ui_init!");
    }

    if (main_image_ != nullptr) {
        // Boot: keep resident art until SD seed commits; then ShowSdFaceCanvasOnly hides it.
        lv_obj_clear_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGW(TAG, "CTRL UI bind resident=error_image (img_main)");
    } else {
        ESP_LOGE(TAG, "error_image/main_image null after ui_init!");
    }
    
    SafeLVGLUnlock();
    
    

    
    // 设置UI就绪标志
    init_state_ = static_cast<InitState>(static_cast<int>(init_state_) | static_cast<int>(InitState::UI_READY));
    ESP_LOGI(TAG, "✅ UI初始化完成");
}

void EezuiDisplayAdapter::SetupBatteryUI() {
    if (objects.main == nullptr || battery_panel_ != nullptr) {
        return;
    }

    battery_panel_ = lv_obj_create(objects.main);
    lv_obj_set_size(battery_panel_, 66, 32);
    lv_obj_align(battery_panel_, LV_ALIGN_TOP_RIGHT, -120, 52);
    lv_obj_clear_flag(battery_panel_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(battery_panel_, 0, 0);
    lv_obj_set_style_border_width(battery_panel_, 0, 0);
    lv_obj_set_style_bg_opa(battery_panel_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(battery_panel_, 0, 0);

    battery_gauge_ = lv_obj_create(battery_panel_);
    lv_obj_set_size(battery_gauge_, 63, 28);
    lv_obj_align(battery_gauge_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(battery_gauge_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(battery_gauge_, 0, 0);
    lv_obj_set_style_border_width(battery_gauge_, 0, 0);
    lv_obj_set_style_bg_opa(battery_gauge_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(battery_gauge_, 0, 0);

    battery_body_ = lv_obj_create(battery_gauge_);
    lv_obj_set_size(battery_body_, 58, 26);
    lv_obj_align(battery_body_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(battery_body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(battery_body_, 4, 0);
    lv_obj_set_style_border_width(battery_body_, 2, 0);
    lv_obj_set_style_border_color(battery_body_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(battery_body_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(battery_body_, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(battery_body_, 0, 0);

    battery_fill_ = lv_obj_create(battery_body_);
    lv_obj_set_size(battery_fill_, 2, 3);
    lv_obj_align(battery_fill_, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_clear_flag(battery_fill_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(battery_fill_, 1, 0);
    lv_obj_set_style_border_width(battery_fill_, 0, 0);
    lv_obj_set_style_bg_color(battery_fill_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(battery_fill_, 0, 0);

    battery_tip_ = lv_obj_create(battery_gauge_);
    lv_obj_set_size(battery_tip_, 5, 12);
    lv_obj_align(battery_tip_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_clear_flag(battery_tip_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(battery_tip_, 2, 0);
    lv_obj_set_style_border_width(battery_tip_, 0, 0);
    lv_obj_set_style_bg_color(battery_tip_, lv_color_white(), 0);
    lv_obj_set_style_pad_all(battery_tip_, 0, 0);

    battery_percent_label_ = lv_label_create(battery_body_);
    lv_obj_set_style_text_font(battery_percent_label_, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_color(battery_percent_label_, lv_color_white(), 0);
    lv_obj_align(battery_percent_label_, LV_ALIGN_CENTER, 0, -2);
    lv_label_set_text(battery_percent_label_, "--%");

    lv_obj_add_flag(battery_panel_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(battery_panel_);
}

void EezuiDisplayAdapter::UpdateStatusBar(bool update_all) {
    const int64_t now_us = esp_timer_get_time();
    constexpr int64_t kBatteryRefreshIntervalUs = 5 * 1000000LL;
    if (!update_all && last_battery_update_us_ > 0 &&
        now_us - last_battery_update_us_ < kBatteryRefreshIntervalUs) {
        return;
    }
    last_battery_update_us_ = now_us;

    int level = 0;
    bool charging = false;
    bool discharging = false;
    const bool available = Board::GetInstance().GetBatteryLevel(level, charging, discharging);

    if (!SafeLVGLLock(kLvglLockQuickMs)) {
        return;
    }
    if (battery_panel_ == nullptr || battery_body_ == nullptr || battery_fill_ == nullptr ||
        battery_tip_ == nullptr || battery_percent_label_ == nullptr) {
        SafeLVGLUnlock();
        return;
    }
    if (!available) {
        lv_obj_add_flag(battery_panel_, LV_OBJ_FLAG_HIDDEN);
        SafeLVGLUnlock();
        return;
    }

    level = std::max(0, std::min(100, level));
    const bool low_battery = discharging && level <= 20;

    const bool changed = update_all || level != last_battery_level_ ||
                         charging != last_battery_charging_ || low_battery != last_battery_low_ ||
                         lv_obj_has_flag(battery_panel_, LV_OBJ_FLAG_HIDDEN);
    if (changed) {
        lv_color_t color = lv_color_white();
        if (charging) {
            color = lv_color_hex(0x58D68D);
        } else if (low_battery) {
            color = lv_color_hex(0xFF5A5F);
        }
        char level_text[8];
        snprintf(level_text, sizeof(level_text), charging ? "+%d%%" : "%d%%", level);
        lv_obj_set_width(battery_fill_, std::max(2, (48 * level) / 100));
        lv_label_set_text(battery_percent_label_, level_text);
        lv_obj_set_style_border_color(battery_body_, color, 0);
        lv_obj_set_style_bg_color(battery_fill_, color, 0);
        lv_obj_set_style_bg_color(battery_tip_, color, 0);
        lv_obj_set_style_text_color(battery_percent_label_, color, 0);
        lv_obj_clear_flag(battery_panel_, LV_OBJ_FLAG_HIDDEN);
        last_battery_level_ = level;
        last_battery_charging_ = charging;
        last_battery_low_ = low_battery;
    }
    lv_obj_move_foreground(battery_panel_);
    SafeLVGLUnlock();
}

void EezuiDisplayAdapter::EnsureDialogueCaptionStyle() {
    if (dialogue_box_ == nullptr) {
        return;
    }
    // Match EEZ screens.c baseline — identical idle vs conversation.
    lv_obj_set_style_text_align(dialogue_box_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(dialogue_box_, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(dialogue_box_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dialogue_box_, 50, 0);
    lv_obj_set_style_radius(dialogue_box_, 20, 0);
    lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(dialogue_box_);
}

void EezuiDisplayAdapter::CancelIdleHideTimer() {
    if (hide_timer_ != nullptr) {
        void* timer_data = lv_timer_get_user_data(hide_timer_);
        if (timer_data) {
            delete static_cast<TimerData*>(timer_data);
        }
        lv_timer_del(hide_timer_);
        hide_timer_ = nullptr;
    }
}

void EezuiDisplayAdapter::ApplyDialogueCaption(const char* content, bool use_typewriter) {
    if (dialogue_box_ == nullptr || content == nullptr) {
        return;
    }
    EnsureDialogueCaptionStyle();
    if (use_typewriter) {
        StartTypewriterEffect(std::string(content));
    } else {
        StopTypewriterEffect();
        lv_label_set_text(dialogue_box_, content);
        UpdateDialogueBoxHeight();
    }
}

void EezuiDisplayAdapter::StartTypewriterEffect(const std::string& text) {
    if (dialogue_box_ == nullptr) {
        return;
    }
    
    // 停止之前的逐字显示效果
    StopTypewriterEffect();
    
    typewriter_text_ = text;
    typewriter_index_ = 0;
    typewriter_active_ = true;
    
    EnsureDialogueCaptionStyle();
    // 清空对话框内容
    lv_label_set_text(dialogue_box_, "");
    
    // 创建定时器
    typewriter_timer_ = lv_timer_create(TypewriterTimerCallback, TYPEWRITER_DELAY_MS, this);
    
}

void EezuiDisplayAdapter::StopTypewriterEffect() {
    if (typewriter_timer_ != nullptr) {
        lv_timer_del(typewriter_timer_);
        typewriter_timer_ = nullptr;
    }
    typewriter_active_ = false;
}

void EezuiDisplayAdapter::UpdateDialogueBoxHeight() {
    if (dialogue_box_ == nullptr) {
        return;
    }
    
    // 设置对话框高度为内容高度
    lv_obj_set_height(dialogue_box_, LV_SIZE_CONTENT);
    lv_obj_invalidate(dialogue_box_);
}



void EezuiDisplayAdapter::TypewriterTimerCallback(lv_timer_t* timer) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(lv_timer_get_user_data(timer));
    if (adapter == nullptr || !adapter->typewriter_active_ || adapter->dialogue_box_ == nullptr) {
        return;
    }
    
    if (adapter->typewriter_index_ < adapter->typewriter_text_.length()) {
        // 显示到当前索引的文本
        std::string current_text = adapter->typewriter_text_.substr(0, adapter->typewriter_index_ + 1);
        lv_label_set_text(adapter->dialogue_box_, current_text.c_str());
        adapter->UpdateDialogueBoxHeight();
        adapter->typewriter_index_++;
    } else {
        // 逐字显示完成
        adapter->StopTypewriterEffect();
    }
}

void EezuiDisplayAdapter::DialogueBoxClickCallback(lv_event_t* e) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(lv_event_get_user_data(e));
    if (adapter == nullptr) {
        return;
    }
    
    
    // 如果正在逐字显示，点击后立即显示完整文本
    if (adapter->typewriter_active_) {
        adapter->StopTypewriterEffect();
        if (adapter->dialogue_box_ != nullptr) {
            lv_label_set_text(adapter->dialogue_box_, adapter->typewriter_text_.c_str());
            adapter->UpdateDialogueBoxHeight();
        }
    }
}

bool EezuiDisplayAdapter::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void EezuiDisplayAdapter::Unlock() {
    lvgl_port_unlock();
}

// ========== 安全的LVGL锁机制 ==========

// 全局锁持有者跟踪（线程安全）
static TaskHandle_t g_lvgl_lock_holder = nullptr;
static char g_lvgl_lock_holder_name[16] = "-";
static portMUX_TYPE g_lock_holder_spinlock = portMUX_INITIALIZER_UNLOCKED;

bool EezuiDisplayAdapter::SafeLVGLLock(int timeout_ms) {
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    
    // 原子检查当前任务是否已经持有锁，避免重入死锁
    portENTER_CRITICAL(&g_lock_holder_spinlock);
    bool already_holds_lock = (g_lvgl_lock_holder == current_task);
    portEXIT_CRITICAL(&g_lock_holder_spinlock);
    
    if (already_holds_lock) {
        return true; // 避免重入死锁
    }
    
    bool result = lvgl_port_lock(pdMS_TO_TICKS(timeout_ms));
    if (result) {
        portENTER_CRITICAL(&g_lock_holder_spinlock);
        g_lvgl_lock_holder = current_task;
        const char* n = pcTaskGetName(current_task);
        if (n && n[0]) {
            strncpy(g_lvgl_lock_holder_name, n, sizeof(g_lvgl_lock_holder_name) - 1);
            g_lvgl_lock_holder_name[sizeof(g_lvgl_lock_holder_name) - 1] = '\0';
        }
        portEXIT_CRITICAL(&g_lock_holder_spinlock);
    }
    return result;
}

void EezuiDisplayAdapter::SafeLVGLUnlock() {
    // 原子清除锁持有者标记
    portENTER_CRITICAL(&g_lock_holder_spinlock);
    g_lvgl_lock_holder = nullptr;
    portEXIT_CRITICAL(&g_lock_holder_spinlock);
    lvgl_port_unlock();
}



void EezuiDisplayAdapter::SetEmotion(const char* emotion) {
    const int64_t t0 = esp_timer_get_time();
    // 处理空字符串和空指针
    if (!emotion || strlen(emotion) == 0) {
        ESP_LOGW(TAG, "收到空表情，使用默认neutral");
       emotion = "neutral";
      
    }
    const char* raw_emotion = emotion;
    emotion = emotion_video_player_canonicalize_emotion(emotion);
    if (strcasecmp(raw_emotion, emotion) != 0) {
        ESP_LOGW(TAG, "s1ep-j canonical raw=%s mapped=%s", raw_emotion, emotion);
        esp_rom_printf("!!FACE_S1EP canonical %s>%s\n", raw_emotion, emotion);
    }

    // Listening/speaking: ignore standby so LLM face is not wiped ~1ms later.
    if (strcasecmp(emotion, "standby") == 0) {
        const auto st = Application::GetInstance().GetDeviceState();
        if (st == kDeviceStateListening || st == kDeviceStateSpeaking) {
            ESP_LOGW(TAG, "CTRL SetEmotion ignore standby during live cur=%s state=%d",
                     current_emotion_name_.empty() ? "-" : current_emotion_name_.c_str(),
                     (int)st);
            ESP_LOGW(TAG, "SAD_DIAG SetEmotion early_exit=ignore_standby cur=%s",
                     current_emotion_name_.empty() ? "-" : current_emotion_name_.c_str());
            return;
        }
    }
    
    ESP_LOGW(TAG, "CTRL SetEmotion req=%s cur=%s", emotion,
             current_emotion_name_.empty() ? "-" : current_emotion_name_.c_str());
    ESP_LOGW(TAG,
             "SAD_DIAG SetEmotion enter req=%s cur=%s present=%d paused=%d owner=%d "
             "lvgl_stop=%d task=%s",
             emotion, current_emotion_name_.empty() ? "-" : current_emotion_name_.c_str(),
             InConversationPresent() ? 1 : 0,
             (emotion_player_ && emotion_video_player_is_decode_paused(emotion_player_)) ? 1 : 0,
             static_cast<int>(panel_owner_), bypass_lvgl_stopped_ ? 1 : 0,
             pcTaskGetName(nullptr));

    // 对特殊表情（alarm、music、charge等）允许重新触发，因为它们可能已被释放缓存
    // 特殊表情列表
    static const char* special_emotions[] = {"alarm", "music", "charge", "logo", nullptr};
    bool is_special = false;
    for (int i = 0; special_emotions[i] != nullptr; i++) {
        if (strcmp(emotion, special_emotions[i]) == 0) {
            is_special = true;
            break;
        }
    }
    
    // 检查是否与当前表情相同（特殊表情除外）
    if (!is_special && !current_emotion_name_.empty() && current_emotion_name_ == emotion) {
        ESP_LOGW(TAG, "CTRL SetEmotion skip same=%s", emotion);
        ESP_LOGW(TAG, "SAD_DIAG SetEmotion early_exit=same emo=%s", emotion);
        return;
    }

    // 延迟初始化表情系统
    if (!IsEmotionSystemReady()) {
        if (!InitEmotionSystem()) {
            ESP_LOGE(TAG, "表情系统初始化失败，跳过表情显示");
            ESP_LOGW(TAG, "SAD_DIAG SetEmotion early_exit=init_fail emo=%s", emotion);
            return;
        }
    }

    // 播放表情动画（经 ScreenPresenter 单向通知）
    const char* path = "none";
    if (IsEmotionSystemReady() && emotion_player_ != nullptr) {
        if (presenter_ != nullptr) {
            path = "presenter";
            presenter_->NotifyEmotion(emotion);
            current_emotion_name_ = emotion;
        } else {
            path = "direct_play";
            esp_err_t ret = PlayMjpegEmotion(emotion);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "表情播放失败: %s，使用neutral", emotion);
                if (strcmp(emotion, "neutral") != 0) {
                    PlayMjpegEmotion("neutral");
                }
            } else {
                current_emotion_name_ = emotion;
            }
        }
    } else {
        ESP_LOGW(TAG, "表情系统未就绪，跳过: %s", emotion);
        path = "not_ready";
    }
    ESP_LOGW(TAG, "SAD_DIAG SetEmotion leave path=%s emo=%s cur=%s cost_ms=%d", path, emotion,
             current_emotion_name_.empty() ? "-" : current_emotion_name_.c_str(),
             (int)((esp_timer_get_time() - t0) / 1000));
}





void EezuiDisplayAdapter::SetChatMessage(const char* role, const char* content) {
    // L3/s1an: route via Presenter so caption_live / panel-hold queue is the single path.
    if (presenter_ != nullptr) {
        presenter_->NotifyDialogue(role, content);
        return;
    }
    PresenterApplyDialogue(role, content);
}

// 字符串版本状态设置
void EezuiDisplayAdapter::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    
    
    // 忽略时间格式（HH:MM）
    if (strlen(status) >= 5 && status[2] == ':' && 
        isdigit(status[0]) && isdigit(status[1]) && 
        isdigit(status[3]) && isdigit(status[4])) {
        return;
    }
    
    // 快速状态识别
    Status new_status = Status::kIdle;
    
    if (strstr(status, "listening") || strstr(status, "聆听")) {
        new_status = Status::kListening;
    } else if (strstr(status, "thinking") || strstr(status, "思考")) {
        new_status = Status::kThinking;
    } else if (strstr(status, "speaking") || strstr(status, "说话")) {
        new_status = Status::kSpeaking;
    } else if (strstr(status, "connecting") || strstr(status, "连接")) {
        new_status = Status::kConnecting;
    } else if (strstr(status, "error") || strstr(status, "错误")) {
        new_status = Status::kError;
    } else if (strstr(status, "standby") || strstr(status, "待命") || strstr(status, "待机")) {
        // 明确识别待命/待机状态，避免重复处理
        new_status = Status::kIdle;
    }
    
    // 直接调用枚举版本的SetStatus方法，避免歧义
    EezuiDisplayAdapter::SetStatus(new_status);
}

// 从范例移植的ShowNotification方法
void EezuiDisplayAdapter::ShowNotification(const char* notification, int duration_ms) {
    if (notification == nullptr || dialogue_box_ == nullptr) {
        return;
    }
    
    DisplayLockGuard lock(this);
    ApplyDialogueCaption(notification, false);
}

void EezuiDisplayAdapter::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

// 优化的定时器回调函数（改进内存管理）
static void OnIdleHideTimer(lv_timer_t* timer) {
    if (timer == nullptr) {
        return;
    }
    
    void* user_data = lv_timer_get_user_data(timer);
    if (user_data == nullptr) {
        lv_timer_del(timer);
        return;
    }
    
    // 获取EezuiDisplayAdapter指针和对话框
    TimerData* data = static_cast<TimerData*>(user_data);
    
    // 验证数据有效性
    if (!data->dialogue_box || !data->adapter) {
        lv_timer_del(timer);
        delete data;
        return;
    }
    
    // 检查是否为重要信息，如果是则不隐藏
    const char* current_text = lv_label_get_text(data->dialogue_box);
    const char* important_keywords[] = {"配网模式", "热点", "192.168.4.1", "验证码", "输入", "请", "code", "Code", "验证", "密码", "连接", "xiaozhi.me"};
    
    if (current_text) {
        for (int i = 0; i < sizeof(important_keywords)/sizeof(important_keywords[0]); i++) {
            if (strstr(current_text, important_keywords[i])) {
                // 安全清理定时器引用
                if (data->hide_timer == timer) {
                    data->adapter->SetHideTimer(nullptr);
                }
                lv_timer_del(timer);
                delete data;
                return;
            }
        }
    }
    
    // 隐藏对话框
    lv_obj_add_flag(data->dialogue_box, LV_OBJ_FLAG_HIDDEN);
    
    // 安全清理定时器引用
    if (data->hide_timer == timer) {
        data->adapter->SetHideTimer(nullptr);
    }
    
    // 清理资源
    lv_timer_del(timer);
    delete data;
    
}

void EezuiDisplayAdapter::SetStatus(Status status) {
    // 只有在状态变化时才更新，但是第一次idle状态需要强制更新
    if (current_status_ == status && !(status == Status::kIdle && current_status_ == Status::kIdle)) {
        if (status == Status::kIdle && dialogue_box_ != nullptr) {
            const char* current_text = lv_label_get_text(dialogue_box_);
            if (current_text != nullptr && strcmp(current_text, "请说：你好，小易！唤醒我吧！") == 0) {
                return;  // 文本已经是正确的，不需要更新
            }
        } else {
            return;
        }
    }

    current_status_ = status;

    // S1o: status captions = baseline direct LVGL (no Presenter NotifyStatusPhase).
    if (dialogue_box_ != nullptr) {
        DisplayLockGuard lock(this);

        if (status == Status::kIdle) {
            // 检查当前对话框内容和可见性
            const char* current_text = lv_label_get_text(dialogue_box_);
            bool is_hidden = lv_obj_has_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
            bool has_valid_content = false;
            
            // 如果对话框是隐藏的，说明之前的内容已经过期，应该显示待机提示
            if (!is_hidden && current_text && strlen(current_text) > 0) {
                // 检查是否是状态提示文本（这些文本在idle时应该被替换）
                const char* status_texts[] = {
                    "正在聆听...", "正在思考...", "正在说话...", "正在连接...", "出现错误"
                };
                bool is_status_text = false;
                for (int i = 0; i < sizeof(status_texts)/sizeof(status_texts[0]); i++) {
                    if (strcmp(current_text, status_texts[i]) == 0) {
                        is_status_text = true;
                        break;
                    }
                }
                
                // 如果不是状态文本且不是待机提示，说明是有效的对话内容，应该保留
                if (!is_status_text && strcmp(current_text, "请说：你好，小易！唤醒我吧！") != 0) {
                    has_valid_content = true;
                }
            }
            
            // 如果对话框被隐藏或没有有效内容，显示待机提示（样式与对话期一致，常显不自动隐藏）
            if (is_hidden || !has_valid_content) {
                ApplyDialogueCaption("请说：你好，小易！唤醒我吧！", false);
            } else {
                EnsureDialogueCaptionStyle();
            }

            // 与对话期一致：字幕保持可见，不再 3s 自动隐藏。
            CancelIdleHideTimer();
        } else if (status == Status::kListening) {
            CancelIdleHideTimer();
            ApplyDialogueCaption("正在聆听...", false);
        } else if (status == Status::kThinking) {
            CancelIdleHideTimer();
            ApplyDialogueCaption("正在思考...", false);
        } else if (status == Status::kSpeaking) {
            // s1cr: never cover assistant/user TTS text with the status placeholder.
            CancelIdleHideTimer();
            const char* cur = lv_label_get_text(dialogue_box_);
            const char* placeholders[] = {"正在聆听...", "正在思考...", "正在说话...",
                                          "正在连接...", "出现错误",
                                          "请说：你好，小易！唤醒我吧！"};
            bool is_placeholder = (cur == nullptr || cur[0] == '\0');
            if (!is_placeholder && cur != nullptr) {
                is_placeholder = false;
                for (const char* p : placeholders) {
                    if (strcmp(cur, p) == 0) {
                        is_placeholder = true;
                        break;
                    }
                }
            }
            if (is_placeholder) {
                ApplyDialogueCaption("正在说话...", false);
                ESP_LOGW(TAG, "SAD_DIAG caption_status speaking placeholder s1cr");
            } else {
                EnsureDialogueCaptionStyle();
                ESP_LOGW(TAG, "SAD_DIAG caption_keep speaking len=%u s1cr",
                         (unsigned)strlen(cur));
                esp_rom_printf("!!FACE_CAPTION keep s1cr\n");
            }
        } else if (status == Status::kConnecting) {
            CancelIdleHideTimer();
            ApplyDialogueCaption("正在连接...", false);
        } else if (status == Status::kError) {
            CancelIdleHideTimer();
            ApplyDialogueCaption("出现错误", false);
        } else {
            // 处理未预期的状态值
            ESP_LOGW(TAG, "未处理的枚举状态: %d", static_cast<int>(status));
        }
    }

}

void EezuiDisplayAdapter::SetStatus(const char* status, const char* time) {
    // 忽略时间参数，直接调用枚举版本的状态设置
    if (status == nullptr) {
        return;
    }
    
    // 快速状态识别并转换为枚举
    Status new_status = Status::kIdle;
    
    if (strstr(status, "listening") || strstr(status, "聆听")) {
        new_status = Status::kListening;
    } else if (strstr(status, "thinking") || strstr(status, "思考")) {
        new_status = Status::kThinking;
    } else if (strstr(status, "speaking") || strstr(status, "说话")) {
        new_status = Status::kSpeaking;
    } else if (strstr(status, "connecting") || strstr(status, "连接")) {
        new_status = Status::kConnecting;
    } else if (strstr(status, "error") || strstr(status, "错误")) {
        new_status = Status::kError;
    }
    
    // 调用枚举版本的SetStatus避免歧义
    EezuiDisplayAdapter::SetStatus(new_status);
}



// MipiEezuiDisplayAdapter 实现
MipiEezuiDisplayAdapter::MipiEezuiDisplayAdapter(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                                                 int width, int height, int offset_x, int offset_y,
                                                 bool mirror_x, bool mirror_y, bool swap_xy)
    : EezuiDisplayAdapter(panel_io, panel, width, height) {
    
    // 注意：LVGL端口初始化应该由Board类负责，这里不再重复初始化
    
    // 配置LVGL显示器
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width),
        .vres = static_cast<uint32_t>(height),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add MIPI DSI display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }

    // CONTEND: H1/H2 — flush + render boundaries (rom_printf survives better before WDT)
    auto flush_ev = [](lv_event_t* e) {
        const lv_event_code_t code = lv_event_get_code(e);
        lv_display_t* disp = (lv_display_t*)lv_event_get_target(e);
        uint32_t area_px = 0;
        // s1aq: use dirty flush area (was always 480² → false "full flush" signal).
        if (code == LV_EVENT_FLUSH_START || code == LV_EVENT_FLUSH_FINISH) {
            const lv_area_t* area = (const lv_area_t*)lv_event_get_param(e);
            if (area) {
                area_px = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);
            }
        } else if (disp) {
            area_px = (uint32_t)lv_display_get_horizontal_resolution(disp) *
                      (uint32_t)lv_display_get_vertical_resolution(disp);
        }
        if (code == LV_EVENT_FLUSH_START || code == LV_EVENT_FLUSH_FINISH) {
            AfeFetchGateNoteDisplayFlush(code == LV_EVENT_FLUSH_START);
            WdtContendNoteFlush(code == LV_EVENT_FLUSH_START, area_px);
        } else if (code == LV_EVENT_RENDER_START || code == LV_EVENT_RENDER_READY) {
            WdtContendNoteRender(code == LV_EVENT_RENDER_START, area_px);
        }
    };
    lv_display_add_event_cb(display_, flush_ev, LV_EVENT_FLUSH_START, nullptr);
    lv_display_add_event_cb(display_, flush_ev, LV_EVENT_FLUSH_FINISH, nullptr);
    lv_display_add_event_cb(display_, flush_ev, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display_, flush_ev, LV_EVENT_RENDER_READY, nullptr);

    // 初始化UI
    SetupUI();
}

// ==================== 简化的表情系统实现（与LcdDisplay统一）====================

bool EezuiDisplayAdapter::InitEmotionSystem() {
    if (IsEmotionSystemReady()) {
        return true;
    }


    // SD must already be mounted by board init. Do NOT remount here —
    // sd_scanner_init_and_scan() used to deinit first and could destroy a
    // working mount then fail OCR after USB reset.
    if (!sd_scanner_is_mounted()) {
        ESP_LOGE(TAG, "❌ SD卡未挂载，无法初始化表情系统");
        return false;
    }

    // 简化的视频播放器配置（与LcdDisplay一致，基于范例更新）
    emotion_video_config_t config = {
        .output_format = ESP_VIDEO_CODEC_PIXEL_FMT_RGB565_LE,
        .frame_rate = 30,
        .canvas_width = static_cast<uint32_t>(width_),        // 修复narrowing conversion
        .canvas_height = static_cast<uint32_t>(height_)       // 修复narrowing conversion
    };
    
    // 创建播放器（不再显示加载提示，因为已改为按需加载）
    esp_err_t ret = emotion_video_player_init(&config, &emotion_player_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "播放器初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    // 注册回调（参考LcdDisplay的简单回调）
    emotion_video_player_register_frame_callback(emotion_player_, EmotionVideoFrameCallback, this);
    emotion_video_player_register_event_callback(emotion_player_, EmotionVideoEventCallback, this);

    // 创建视频画布
    CreateVideoCanvas();

    // 设置表情系统就绪标志
    init_state_ = static_cast<InitState>(static_cast<int>(init_state_) | static_cast<int>(InitState::EMOTION_READY));
    current_emotion_name_.clear();

    // s1ct: do NOT NotifyEmotion/Play standby here — seeds are not ready yet, and
    // PlayMjpeg would unhide an empty black canvas while hiding boot main_image.
    // Keep resident art until PreloadBaseEmotionsSync paints the first standby seed.
    ESP_LOGW(TAG, "FACE_BOOT keep_main_image until standby seed s1ct");
    esp_rom_printf("!!FACE_BOOT keep_main s1ct\n");
    // Boot/AFE-safe: never leave MJPEG full-flush looping during later preload.
    PauseMjpegHeavyWork();

    return true;
}

void EezuiDisplayAdapter::PauseMjpegHeavyWork() {
    if (emotion_player_ != nullptr) {
        emotion_video_player_pause_loop(emotion_player_);
    }
}

void EezuiDisplayAdapter::ResumeMjpegHeavyWork() {
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }
    if (video_canvas_ != nullptr && SafeLVGLLock(kLvglLockQuickMs)) {
        ShowFaceCanvasLayers();
        SafeLVGLUnlock();
    }
    if (emotion_player_ != nullptr) {
        emotion_video_player_resume_loop(emotion_player_);
    }
}

void EezuiDisplayAdapter::ResumeMjpegHeavyWorkAtFps(uint32_t fps) {
    if (emotion_player_ != nullptr) {
        emotion_video_player_resume_loop_at_fps(emotion_player_, fps);
    }
}

void EezuiDisplayAdapter::StartDeferredEmotionPreload() {
    if (emotion_player_ != nullptr) {
        emotion_video_player_start_deferred_preload(emotion_player_);
    }
}

void EezuiDisplayAdapter::ShowSdFaceCanvasOnly() {
    if (!SafeLVGLLock(kLvglLockQuickMs)) {
        return;
    }
    // Above resident art, below captions.
    ShowFaceCanvasLayers();
    SafeLVGLUnlock();
}

esp_err_t EezuiDisplayAdapter::CommitSdSeedFace(const char* emotion_name, bool panel_blit) {
    if (!emotion_name) {
        emotion_name = "standby";
    }
    if (emotion_player_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }
    const uint8_t* rgb = nullptr;
    uint32_t size = 0, w = 0, h = 0;
    esp_err_t derr = emotion_video_player_get_seed_rgb565(emotion_player_, emotion_name, &rgb,
                                                         &size, &w, &h);
    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "CTRL SD_FACE seed miss emo=%s err=%s (keep main_image)", emotion_name,
                 esp_err_to_name(derr));
        // s1ct: never unhide empty canvas on miss — that is the boot black screen.
        return (derr == ESP_OK) ? ESP_ERR_NOT_FOUND : derr;
    }
    // Canvas holds SD pixels so LVGL won't fall back to boot/resident art.
    // Only swap main_image → canvas AFTER pixels are copied (no black gap).
    SyncCanvasFromRgb(rgb, size, w, h);
    ShowSdFaceCanvasOnly();
    CacheLastBypassFrame(rgb, size, w, h);
    // Always invalidate — panel=0 previously left canvas dirty-but-unflushed (invisible sad).
    if (video_canvas_ != nullptr && SafeLVGLLock(kLvglLockQuickMs)) {
        lv_obj_invalidate(video_canvas_);
        SafeLVGLUnlock();
    }
    esp_err_t berr = ESP_OK;
    if (panel_blit) {
        // Prefer one-shot full frame (seed is already compact). DPI retry inside DirectPanelBlit.
        berr = DirectPanelBlit(0, 0, (int)w, (int)h, reinterpret_cast<const uint16_t*>(rgb));
        if (berr != ESP_OK) {
            berr = BlitRgbFrameChunked(rgb, w, h);
        }
        vTaskDelay(pdMS_TO_TICKS(4));
    }
    current_emotion_name_ = emotion_name;
    ESP_LOGW(TAG, "CTRL SD_FACE commit emo=%s panel=%d err=%s", emotion_name, panel_blit ? 1 : 0,
             esp_err_to_name(berr));
    return berr;
}

bool EezuiDisplayAdapter::PreloadBaseEmotionsSync() {
    // s1at: stop any early idle breathe so it cannot steal SD decode during P2.
    StopIdleBreathe("p2_preload");
    if (!IsEmotionSystemReady()) {
        if (!InitEmotionSystem()) {
            ESP_LOGW(TAG, "CTRL P2 preload abort: emotion init failed");
            return false;
        }
    }
    if (emotion_player_ == nullptr) {
        return false;
    }

    // s1ct: paint standby as soon as that one still exists — before the long 5-clip SD load.
    esp_err_t early = emotion_video_player_seed_emotion_still(emotion_player_, "standby");
    if (early == ESP_OK) {
        const esp_err_t paint = CommitSdSeedFace("standby", false);
        ESP_LOGW(TAG, "FACE_BOOT early_paint standby err=%s s1ct", esp_err_to_name(paint));
        esp_rom_printf("!!FACE_BOOT early_paint s1ct\n");
    } else {
        ESP_LOGW(TAG, "FACE_BOOT early_seed_fail err=%s (keep main_image) s1ct",
                 esp_err_to_name(early));
    }

    esp_err_t ret = emotion_video_player_preload_base_sync(emotion_player_);
    if (ret == ESP_OK) {
        // Refresh standby after full seed set (idempotent if already painted).
        CommitSdSeedFace("standby", false);
        if (last_bypass_rgb_ == nullptr) {
            if (presenter_ != nullptr) {
                ESP_LOGW(TAG, "CTRL PRESENT commit emo=standby via=presenter why=preload s1bj");
                presenter_->NotifyEmotion("standby");
            } else {
                ESP_LOGW(TAG, "CTRL PRESENT commit emo=standby via=leaf why=preload_no_presenter s1bj");
                CommitFaceStillOnce("standby", false);
            }
        }
        ESP_LOGW(TAG, "FACE_SELFTEST skip_boot_burst (use speak still-only)");
        esp_rom_printf("!!FACE_SELFTEST skip_boot_burst\n");
        // s1cs/s1ct: restore idle BAND breathe — do not kill dynamics for WDT (B3/F6).
        FaceMouth_BootProbe();
        const bool speech_core_ready = FaceSpeechCore_Preload();
        ArmIdleBreathe("seed_ready");
    }
    return ret == ESP_OK;
}

esp_err_t EezuiDisplayAdapter::RunLvglFaceSelfTest(const char* emotion_name) {
    if (!emotion_name) {
        emotion_name = "sad";
    }
    if (emotion_player_ == nullptr || video_canvas_ == nullptr) {
        ESP_LOGW(TAG, "FACE_SELFTEST abort=bad_state emo=%s", emotion_name);
        esp_rom_printf("!!FACE_SELFTEST abort bad_state\n");
        return ESP_ERR_INVALID_STATE;
    }
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }

    ESP_LOGW(TAG, "FACE_SELFTEST begin emo=%s path=lvgl_seed_incr", emotion_name);
    esp_rom_printf("!!FACE_SELFTEST begin emo=%s\n", emotion_name);

    const uint8_t* rgb = nullptr;
    uint32_t size = 0, w = 0, h = 0;
    esp_err_t derr = emotion_video_player_decode_one_rgb565(emotion_player_, emotion_name, 0, &rgb,
                                                           &size, &w, &h);
    if (derr != ESP_OK || rgb == nullptr) {
        derr = emotion_video_player_get_seed_rgb565(emotion_player_, emotion_name, &rgb, &size, &w,
                                                   &h);
    }
    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "FACE_SELFTEST decode_fail emo=%s err=%s", emotion_name,
                 esp_err_to_name(derr));
        esp_rom_printf("!!FACE_SELFTEST decode_fail\n");
        return (derr == ESP_OK) ? ESP_FAIL : derr;
    }

    face_anim_emo_ = emotion_name;
    esp_err_t perr = PresentFaceFrameToLvgl(rgb, size, w, h, true);
    ESP_LOGW(TAG, "FACE_SELFTEST seed err=%s %ux%u", esp_err_to_name(perr), (unsigned)w,
             (unsigned)h);
    int ok_n = (perr == ESP_OK) ? 1 : 0;

    for (int i = 0; i < 5; i++) {
        vTaskDelay(pdMS_TO_TICKS(90));
        rgb = nullptr;
        size = w = h = 0;
        derr = emotion_video_player_decode_next_rgb565(emotion_player_, &rgb, &size, &w, &h);
        if (derr != ESP_OK || rgb == nullptr) {
            ESP_LOGW(TAG, "FACE_SELFTEST incr_fail i=%d err=%s", i, esp_err_to_name(derr));
            break;
        }
        perr = PresentFaceFrameToLvgl(rgb, size, w, h, false);
        if (perr == ESP_OK) {
            ok_n++;
        }
        ESP_LOGW(TAG, "FACE_SELFTEST incr i=%d err=%s", i, esp_err_to_name(perr));
    }

    // Give LVGL one flush window before restoring standby seed.
    vTaskDelay(pdMS_TO_TICKS(120));
    current_emotion_name_ = emotion_name;
    ESP_LOGW(TAG, "FACE_SELFTEST done emo=%s frames_ok=%d", emotion_name, ok_n);
    esp_rom_printf("!!FACE_SELFTEST done emo=%s ok=%d\n", emotion_name, ok_n);
    return (ok_n > 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t EezuiDisplayAdapter::DirectPanelBlit(int x, int y, int w, int h, const uint16_t* rgb565) {
    if (panel_ == nullptr || rgb565 == nullptr || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x + w > width_ || y + h > height_) {
        return ESP_ERR_INVALID_ARG;
    }
    // MIPI DPI + DMA2D: draw_bitmap takes draw_sem with timeout=0.
    // Back-to-back row blits → ESP_ERR_INVALID_STATE (SAD: xy=120,121 cost_ms=0).
    // Retry until previous DMA2D finishes (or give up).
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 80; attempt++) {
        err = esp_lcd_panel_draw_bitmap(panel_, x, y, x + w, y + h, rgb565);
        if (err != ESP_ERR_INVALID_STATE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CTRL BYPASS blit fail err=%s xy=%d,%d wh=%d,%d",
                 esp_err_to_name(err), x, y, w, h);
    }
    return err;
}

void EezuiDisplayAdapter::RunI1DirectBlitSmokeOnce() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;

    if (panel_ == nullptr || width_ <= 0 || height_ <= 0) {
        ESP_LOGW(TAG, "CTRL BYPASS I1 smoke skip: no panel");
        return;
    }

    const size_t px = (size_t)width_ * (size_t)height_;
    const size_t bytes = px * sizeof(uint16_t);
    auto* buf = static_cast<uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        buf = static_cast<uint16_t*>(malloc(bytes));
    }
    if (!buf) {
        ESP_LOGW(TAG, "CTRL BYPASS I1 smoke skip: no buf size=%u", (unsigned)bytes);
        return;
    }

    // Bright red RGB565 — unmistakable vs standby face.
    constexpr uint16_t kRed = 0xF800;
    for (size_t i = 0; i < px; i++) {
        buf[i] = kRed;
    }

    ESP_LOGW(TAG, "CTRL BYPASS I1 smoke begin stop_lvgl blit red %dx%d", width_, height_);
    lvgl_port_stop();
    const esp_err_t err = DirectPanelBlit(0, 0, width_, height_, buf);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "CTRL BYPASS I1 smoke blit ok hold_ms=1500");
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ESP_LOGW(TAG, "CTRL BYPASS I1 smoke blit fail err=%s", esp_err_to_name(err));
    }
    lvgl_port_resume();
    heap_caps_free(buf);
    RestoreUiAfterDirectBlit();
    ESP_LOGW(TAG, "CTRL BYPASS I1 smoke end resume_lvgl");
}

void EezuiDisplayAdapter::RestoreUiAfterDirectBlit(bool full_refr) {
    // Panel FB was overwritten outside LVGL — invalidate; optional sync refr.
    if (!SafeLVGLLock(kLvglLockUiMs)) {
        ESP_LOGW(TAG, "CTRL BYPASS restore skip=lock_fail");
        return;
    }
    if (display_ != nullptr) {
        lv_disp_set_default(display_);
    }
    lv_obj_t* scr = lv_scr_act();
    if (scr != nullptr) {
        lv_obj_invalidate(scr);
    }
    if (video_canvas_ != nullptr) {
        lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(video_canvas_);
    }
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
        lv_obj_invalidate(dialogue_box_);
    }
    if (full_refr) {
        lv_refr_now(display_);
    }
    SafeLVGLUnlock();
    ESP_LOGW(TAG, "CTRL BYPASS restore refr_now=%d owner=%d", full_refr ? 1 : 0,
             static_cast<int>(panel_owner_));
}

void EezuiDisplayAdapter::AcquirePanelForEmotion() {
    if (panel_owner_ == PanelOwner::kEmotion) {
        if (!bypass_lvgl_stopped_) {
            lvgl_port_stop();
            bypass_lvgl_stopped_ = true;
        }
        return;
    }
    lvgl_port_stop();
    bypass_lvgl_stopped_ = true;
    panel_owner_ = PanelOwner::kEmotion;
    ESP_LOGW(TAG, "CTRL BYPASS owner=Emotion");
}

void EezuiDisplayAdapter::ReleasePanelToLvgl(bool restore_ui) {
    if (bypass_lvgl_stopped_) {
        lvgl_port_resume();
        bypass_lvgl_stopped_ = false;
    }
    const bool was_emotion = (panel_owner_ == PanelOwner::kEmotion);
    panel_owner_ = PanelOwner::kLvgl;
    if (was_emotion) {
        ESP_LOGW(TAG, "CTRL BYPASS owner=Lvgl restore=%d", restore_ui ? 1 : 0);
    }
    if (restore_ui) {
        // Speaking/TTS and conversation listen: never lv_refr_now.
        // Sync refr after sad/angry on listen edge → AFE stall, UI stuck 正在聆听.
        const bool speaking =
            (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking);
        const bool conv = InConversationPresent();
        RestoreUiAfterDirectBlit(!speaking && !conv);
    }
}

void EezuiDisplayAdapter::YieldPanelForUi(const char* reason) {
    if (panel_owner_ != PanelOwner::kEmotion) {
        return;
    }
    // I4b: never full-screen LVGL restore while Emotion holds the panel.
    // Wake-armed + lv_refr_now every chat tick / anim slice → HP_WDT (root cause).
    // Dialogue label may update in LVGL memory; panel shows it on anim end Release.
    ESP_LOGW(TAG, "CTRL BYPASS owner yield deferred reason=%s (hold Emotion)",
             reason ? reason : "-");
}

bool EezuiDisplayAdapter::CacheLastBypassFrame(const uint8_t* rgb, uint32_t size, uint32_t w,
                                              uint32_t h) {
    if (!rgb || size == 0 || w == 0 || h == 0) {
        return false;
    }
    if (last_bypass_rgb_ == nullptr || last_bypass_cap_ < size) {
        if (last_bypass_rgb_ != nullptr) {
            heap_caps_free(last_bypass_rgb_);
            last_bypass_rgb_ = nullptr;
            last_bypass_cap_ = 0;
        }
        last_bypass_rgb_ = static_cast<uint8_t*>(
            heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!last_bypass_rgb_) {
            last_bypass_rgb_ = static_cast<uint8_t*>(malloc(size));
        }
        if (!last_bypass_rgb_) {
            return false;
        }
        last_bypass_cap_ = size;
    }
    memcpy(last_bypass_rgb_, rgb, size);
    last_bypass_size_ = size;
    last_bypass_w_ = w;
    last_bypass_h_ = h;
    return true;
}

esp_err_t EezuiDisplayAdapter::ReblitLastBypassFrame() {
    if (!last_bypass_rgb_ || last_bypass_size_ == 0 || last_bypass_w_ == 0 ||
        last_bypass_h_ == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return BlitRgbFrame(last_bypass_rgb_, last_bypass_w_, last_bypass_h_);
}

esp_err_t EezuiDisplayAdapter::SyncCanvasFromRgb(const uint8_t* rgb, uint32_t size, uint32_t w,
                                                 uint32_t h) {
    if (!rgb || !video_canvas_ || w == 0 || h == 0 || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    // LVGL may be stopped; still take lock when possible for canvas metadata safety.
    const bool locked = SafeLVGLLock(kLvglLockQuickMs);
    const lv_image_dsc_t* img = lv_canvas_get_image(video_canvas_);
    if (!img || !img->data) {
        if (locked) {
            SafeLVGLUnlock();
        }
        return ESP_ERR_INVALID_STATE;
    }
    auto* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img->data));
    const uint32_t cw = (uint32_t)lv_obj_get_width(video_canvas_);
    const uint32_t ch = (uint32_t)lv_obj_get_height(video_canvas_);
    const uint32_t copy_w = (w < cw) ? w : cw;
    const uint32_t copy_h = (h < ch) ? h : ch;
    const uint32_t need = copy_w * copy_h * 2;
    if (size < need) {
        if (locked) {
            SafeLVGLUnlock();
        }
        return ESP_ERR_INVALID_SIZE;
    }
    if (copy_w == cw && copy_h == ch) {
        memcpy(canvas_buf, rgb, need);
    } else {
        for (uint32_t y = 0; y < copy_h; y++) {
            memcpy(canvas_buf + y * cw * 2, rgb + y * copy_w * 2, copy_w * 2);
        }
    }
    lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
    if (locked) {
        SafeLVGLUnlock();
    }
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::PresentFaceFrameToLvgl(const uint8_t* rgb, uint32_t size, uint32_t w,
                                                      uint32_t h, bool seed_full, bool roi_only) {
    if (!rgb || !video_canvas_ || w == 0 || h == 0 || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }

    // s1cn-c: seed_full = fullscreen transaction — singleton + AFE safety window.
    bool took_fs_gate = false;
    if (seed_full && FaceRouteV2_FsGateEnabled()) {
        if (!FaceRouteV2_TryBeginFullscreen(face_anim_emo_.c_str(), face_holds_afe_gate_)) {
            return ESP_ERR_TIMEOUT;
        }
        took_fs_gate = true;
    }
    AfeFetchGateBeginFaceTxn();
    auto end_fs = [&]() {
        if (took_fs_gate) {
            FaceRouteV2_EndFullscreen();
            took_fs_gate = false;
        }
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_DONE);
    };
    const bool fence_flush = face_holds_afe_gate_ || took_fs_gate;
    afe_display_fence_t display_fence{};
    auto prepare_flush_fence = [&]() {
        if (fence_flush) {
            AfeFetchGatePrepareDisplayFence(&display_fence);
        }
    };
    auto wait_flush_fence = [&]() {
        if (!fence_flush) {
            return;
        }
        uint32_t wait_ms = 0;
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_FENCE_WAIT);
        const bool ok = AfeFetchGateWaitDisplayFence(&display_fence, kFlushFenceWaitMs, &wait_ms);
        static uint32_t s_fence_log_n = 0;
        const uint32_t n = ++s_fence_log_n;
        if (!ok || (n & 31u) == 1u) {
            ESP_LOGW(TAG, "FACE_FLUSH_FENCE ok=%d wait_ms=%u n=%u final_dma=1 s1cw",
                     ok ? 1 : 0, (unsigned)wait_ms, (unsigned)n);
            esp_rom_printf("!!FACE_FLUSH_FENCE ok=%d wait_ms=%u final_dma=1 s1cw\n",
                           ok ? 1 : 0, (unsigned)wait_ms);
        }
    };

    // Absolute coords for lv_obj_invalidate_area (intersects with obj->coords).
    int inv_x1 = 0, inv_y1 = 0, inv_x2 = (int)w - 1, inv_y2 = (int)h - 1;
    bool use_partial = false;
    const bool log_frame = FaceRouteV2_ShouldLogFrame();

    // s1bs: band rows from canvas diff (full width, snapped to LVGL strip).
    // s1bv: when capped, alternate top/bottom anchors (center-cap left shoulders stale).
    // Row count stays capped — full-height invalidate is the banned full-screen flush.
    // s1cb-rev: do NOT shrink idle breathe below dialogue cap (200 caused head/body split).
    if (roi_only && !seed_full) {
        const int kBandMaxRows = kBandMaxRowsCap;
        const int rw = (int)w;
        use_partial = true;

        if (!SafeLVGLLock(kLvglLockQuickMs)) {
            end_fs();
            return ESP_ERR_TIMEOUT;
        }
        const lv_image_dsc_t* img = lv_canvas_get_image(video_canvas_);
        if (!img || !img->data) {
            SafeLVGLUnlock();
            end_fs();
            return ESP_ERR_INVALID_STATE;
        }
        auto* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img->data));
        const uint32_t cw = (uint32_t)lv_obj_get_width(video_canvas_);
        const uint32_t ch = (uint32_t)lv_obj_get_height(video_canvas_);
        if (w > cw || h > ch) {
            SafeLVGLUnlock();
            end_fs();
            return ESP_ERR_INVALID_SIZE;
        }
        BandRoiResult band{};
        // s1ck: idle breathe almost always yields dirty=0..479 (serial) — the full
        // canvas PSRAM read-before-write is wasted work and widens MSPI-751
        // write-then-read windows. Skip scan; assume full-frame dirty and keep
        // capped top/bot alternate (same visual as today's capped=1 path).
        // Dialogue MID keeps the real scan (single-variable).
        if (idle_breathe_) {
            band.dirty_first = 0;
            band.dirty_last = (int)h - 1;
            band.scan_mode = "skip";
        } else {
            for (int y = 0; y < (int)h; y++) {
                const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb + (size_t)y * w * 2);
                const uint16_t* dst =
                    reinterpret_cast<const uint16_t*>(canvas_buf + (size_t)y * cw * 2);
                for (int x = 0; x < (int)w; x += kBandDiffColStep) {
                    if (src[x] != dst[x]) {
                        if (band.dirty_first < 0) {
                            band.dirty_first = y;
                        }
                        band.dirty_last = y;
                        break;
                    }
                }
            }
        }
        if (band.dirty_first < 0) {
            ShowFaceCanvasLayers();
            if (log_frame) {
                ESP_LOGW(TAG, "FACE_ROWS same emo=%s s1bs", face_anim_emo_.c_str());
            }
            SafeLVGLUnlock();
            BootTraceMark("FACE_PRESENT", "same");
            end_fs();
            return ESP_OK;
        }
        // s1bv: alternate top/bottom so shoulders are not permanently outside the band.
        // s1bw: bot path must NOT re-snap y1 to 50 after computing height — that made
        // rows=430 (e.g. y=50..479) and oversize flush/present right before Core1
        // InstrAccessFault in lv_draw_image (mepc=0).
        ComputeCappedBandRoi(band.dirty_first, band.dirty_last, (int)h, kBandMaxRows,
                             &face_band_cap_bot_, &band);
        inv_x1 = 0;
        inv_y1 = band.band_y1;
        inv_x2 = rw - 1;
        inv_y2 = band.band_y2;
        const int rh = inv_y2 - inv_y1 + 1;
        if (log_frame) {
            ESP_LOGW(TAG,
                     "FACE_ROWS dirty=%d..%d band=%d..%d rows=%d capped=%d anchor=%s emo=%s "
                     "breathe=%d scan=%s s1ck",
                     band.dirty_first, band.dirty_last, inv_y1, inv_y2, rh, band.capped ? 1 : 0,
                     band.cap_anchor, face_anim_emo_.c_str(), idle_breathe_ ? 1 : 0,
                     band.scan_mode);
        }
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_CANVAS_WRITE);
        CopyRgb565BandRows(canvas_buf, cw, rgb, w, inv_y1, inv_y2, inv_x1, rw);
        ShowFaceCanvasLayers();
        lv_area_t coords;
        lv_obj_get_coords(video_canvas_, &coords);
        lv_area_t area;
        area.x1 = coords.x1 + inv_x1;
        area.y1 = coords.y1 + inv_y1;
        area.x2 = coords.x1 + inv_x2;
        area.y2 = coords.y1 + inv_y2;
        prepare_flush_fence();
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_INVALIDATE);
        lv_obj_invalidate_area(video_canvas_, &area);
        if (log_frame) {
            ESP_LOGW(TAG, "FACE_ROI_GEO x=%d y=%d wxh=%dx%d emo=%s band s1bw", inv_x1, inv_y1, rw,
                     rh, face_anim_emo_.c_str());
            ESP_LOGW(TAG, "SAD_DIAG lvgl_face incr emo=%s roi=%d,%d %dx%d s1bw",
                     face_anim_emo_.c_str(), inv_x1, inv_y1, rw, rh);
            esp_rom_printf("!!FACE_ROI_GEO x=%d y=%d %dx%d band s1bw\n", inv_x1, inv_y1, rw, rh);
            esp_rom_printf("!!FACE_PIN inv emo=%s roi=%dx%d left=%d\n", face_anim_emo_.c_str(), rw,
                           rh, face_anim_left_);
        }
        BootTraceMark("FACE_INV", face_anim_emo_.c_str());
        // s1bk: same LVGL lock — flush deferred caption (no nested port_lock / no compose).
        if (presenter_ != nullptr && presenter_->FlushCaptionIfDirty()) {
            ESP_LOGW(TAG, "FACE_L3 coalesce roi+caption emo=%s s1bk", face_anim_emo_.c_str());
            esp_rom_printf("!!FACE_L3 coalesce s1bk\n");
        }
        // s1ap: patch last_bypass BEFORE unlock — serial WDT was after unlock_end in patch loop.
        // s1bz: idle breathe skips the mirror patch. It doubled PSRAM write traffic per tick
        // (band is usually capped at 400 rows => ~384KB extra) while all breathe frames are
        // standby look-alikes and the cycle rests back on the seed anyway. Dialogue MID keeps
        // the patch so a panel re-blit stays pixel-accurate.
        const bool patch_mirror = !idle_breathe_ && last_bypass_rgb_ != nullptr &&
                                  last_bypass_w_ == w && last_bypass_h_ == h &&
                                  last_bypass_size_ >= size;
        if (patch_mirror) {
            CopyRgb565BandRows(last_bypass_rgb_, w, rgb, w, inv_y1, inv_y2, inv_x1, rw);
        }
        // s1ca: sample the margin here, deep inside present — not after return (s1bh lesson).
        if (log_frame) {
            ESP_LOGW(TAG,
                     "FACE_BUS rows=%d bytes=%d patch=%d breathe=%d stack_free=%u task=%s s1cb", rh,
                     rh * rw * 2, patch_mirror ? 1 : 0, idle_breathe_ ? 1 : 0,
                     (unsigned)uxTaskGetStackHighWaterMark(nullptr), pcTaskGetName(nullptr));
            esp_rom_printf("!!FACE_PIN patch_ok\n");
        }
        BootTraceMark("FACE_PATCH", "ok");
        const uint32_t flush_n_before = WdtContendFlushCount();
        const int64_t tu0 = esp_timer_get_time();
        if (log_frame) {
            esp_rom_printf("!!FACE_PIN unlock_beg core=%d\n", (int)xPortGetCoreID());
        }
        BootTraceMark("FACE_UNLOCK", "beg");
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_UNLOCK);
        SafeLVGLUnlock();
        wait_flush_fence();
        const uint32_t unlock_ms = (uint32_t)((esp_timer_get_time() - tu0) / 1000);
        WdtContendNoteUnlock(unlock_ms, flush_n_before);
        if (log_frame) {
            esp_rom_printf("!!FACE_PIN unlock_end ms=%u flush_n=%u\n", (unsigned)unlock_ms,
                           (unsigned)WdtContendFlushCount());
            esp_rom_printf("!!FACE_PIN present_ret emo=%s\n", face_anim_emo_.c_str());
        }
        BootTraceMark("FACE_UNLOCK", "end");
        BootTraceMark("FACE_PRESENT", "ret");
        end_fs();
        return ESP_OK;
    }

    const esp_err_t serr = SyncCanvasFromRgb(rgb, size, w, h);
    if (serr != ESP_OK) {
        end_fs();
        return serr;
    }
    if (!seed_full && last_bypass_rgb_ != nullptr && last_bypass_w_ == w &&
        last_bypass_h_ == h && last_bypass_size_ >= size) {
        const uint16_t* prev = reinterpret_cast<const uint16_t*>(last_bypass_rgb_);
        const uint16_t* cur = reinterpret_cast<const uint16_t*>(rgb);
        int minx = (int)w, miny = (int)h, maxx = -1, maxy = -1;
        for (uint32_t y = 0; y < h; y++) {
            const uint16_t* a = prev + (size_t)y * w;
            const uint16_t* b = cur + (size_t)y * w;
            for (uint32_t x = 0; x < w; x++) {
                if (a[x] != b[x]) {
                    if ((int)x < minx) minx = (int)x;
                    if ((int)x > maxx) maxx = (int)x;
                    if ((int)y < miny) miny = (int)y;
                    if ((int)y > maxy) maxy = (int)y;
                }
            }
        }
        if (maxx < 0) {
            CacheLastBypassFrame(rgb, size, w, h);
            if (log_frame) {
                ESP_LOGW(TAG, "SAD_DIAG lvgl_face skip_identical emo=%s", face_anim_emo_.c_str());
            }
            end_fs();
            return ESP_OK;
        }
        minx = (minx > 2) ? (minx - 2) : 0;
        miny = (miny > 2) ? (miny - 2) : 0;
        maxx = (maxx + 2 < (int)w) ? (maxx + 2) : ((int)w - 1);
        maxy = (maxy + 2 < (int)h) ? (maxy + 2) : ((int)h - 1);
        const int dw = maxx - minx + 1;
        const int dh = maxy - miny + 1;
        const int area = dw * dh;
        const int full = (int)w * (int)h;
        if (area * 2 <= full) {
            inv_x1 = minx;
            inv_y1 = miny;
            inv_x2 = maxx;
            inv_y2 = maxy;
            use_partial = true;
        }
    }

    if (!SafeLVGLLock(kLvglLockPresentMs)) {
        CacheLastBypassFrame(rgb, size, w, h);
        end_fs();
        return ESP_ERR_TIMEOUT;
    }
    ShowFaceCanvasLayers();

    lv_area_t coords;
    lv_obj_get_coords(video_canvas_, &coords);
    prepare_flush_fence();
    if (seed_full || !use_partial) {
        lv_obj_invalidate(video_canvas_);
        ESP_LOGW(TAG, "SAD_DIAG lvgl_face %s emo=%s full=%ux%u s1cn-c",
                 seed_full ? "seed" : "full", face_anim_emo_.c_str(), (unsigned)w, (unsigned)h);
    } else {
        lv_area_t area;
        area.x1 = coords.x1 + inv_x1;
        area.y1 = coords.y1 + inv_y1;
        area.x2 = coords.x1 + inv_x2;
        area.y2 = coords.y1 + inv_y2;
        lv_obj_invalidate_area(video_canvas_, &area);
        if (log_frame) {
            ESP_LOGW(TAG, "SAD_DIAG lvgl_face incr emo=%s roi=%d,%d %dx%d",
                     face_anim_emo_.c_str(), inv_x1, inv_y1, inv_x2 - inv_x1 + 1,
                     inv_y2 - inv_y1 + 1);
            esp_rom_printf("!!FACE_ROI %d,%d %dx%d\n", inv_x1, inv_y1, inv_x2 - inv_x1 + 1,
                           inv_y2 - inv_y1 + 1);
        }
    }
    SafeLVGLUnlock();
    wait_flush_fence();

    CacheLastBypassFrame(rgb, size, w, h);
    end_fs();
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::BlitRgbFrame(const uint8_t* rgb, uint32_t w, uint32_t h) {
    if (!rgb || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int blit_w = (int)((w > (uint32_t)width_) ? (uint32_t)width_ : w);
    const int blit_h = (int)((h > (uint32_t)height_) ? (uint32_t)height_ : h);
    return DirectPanelBlit(0, 0, blit_w, blit_h, reinterpret_cast<const uint16_t*>(rgb));
}

esp_err_t EezuiDisplayAdapter::BlitRgbFrameChunked(const uint8_t* rgb, uint32_t w, uint32_t h) {
    if (!rgb || w == 0 || h == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const int blit_w = (int)((w > (uint32_t)width_) ? (uint32_t)width_ : w);
    const int blit_h = (int)((h > (uint32_t)height_) ? (uint32_t)height_ : h);
    // ~15KB/strip — full 460KB blast + lvgl_stop was killing AFE (S1b).
    constexpr int kRows = 16;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb);
    for (int y = 0; y < blit_h; y += kRows) {
        const int rows = (y + kRows > blit_h) ? (blit_h - y) : kRows;
        const esp_err_t err = DirectPanelBlit(0, y, blit_w, rows, src + (size_t)y * (size_t)w);
        if (err != ESP_OK) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::BlitRgbRoiRowwise(const uint8_t* rgb, uint32_t src_w, uint32_t src_h,
                                                 int roi_x, int roi_y, int roi_w, int roi_h) {
    if (!rgb || src_w == 0 || src_h == 0 || roi_w <= 0 || roi_h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (roi_x < 0 || roi_y < 0 || roi_x + roi_w > (int)src_w || roi_y + roi_h > (int)src_h) {
        return ESP_ERR_INVALID_ARG;
    }
    if (roi_x + roi_w > width_ || roi_y + roi_h > height_) {
        return ESP_ERR_INVALID_ARG;
    }
    // DPI DMA2D needs a compact 1D buffer (no src stride). Pack ROI then ONE draw_bitmap.
    const size_t bytes = (size_t)roi_w * (size_t)roi_h * sizeof(uint16_t);
    auto* packed = static_cast<uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!packed) {
        packed = static_cast<uint16_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!packed) {
        ESP_LOGW(TAG, "SAD_DIAG roi pack oom bytes=%u", (unsigned)bytes);
        return ESP_ERR_NO_MEM;
    }
    const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb);
    for (int row = 0; row < roi_h; row++) {
        const uint16_t* line = src + (size_t)(roi_y + row) * (size_t)src_w + (size_t)roi_x;
        memcpy(packed + (size_t)row * (size_t)roi_w, line, (size_t)roi_w * sizeof(uint16_t));
    }
    const esp_err_t err = DirectPanelBlit(roi_x, roi_y, roi_w, roi_h, packed);
    // Allow DMA2D to finish before caller/LVGL touches the panel again.
    vTaskDelay(pdMS_TO_TICKS(4));
    heap_caps_free(packed);
    return err;
}

void EezuiDisplayAdapter::SetFaceLayersHiddenForPartial(bool hide_face_layers) {
    if (!SafeLVGLLock(kLvglLockQuickMs)) {
        return;
    }
    if (video_canvas_ != nullptr) {
        if (hide_face_layers) {
            lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (main_image_ != nullptr) {
        // Always keep resident image hidden while face lives in panel FB.
        lv_obj_add_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
    }
    SafeLVGLUnlock();
}

void EezuiDisplayAdapter::HideVideoCanvasForDirectFace() {
    SetFaceLayersHiddenForPartial(true);
}

void EezuiDisplayAdapter::HideFaceLayersForPanelHold() {
    if (!SafeLVGLLock(kLvglLockQuickMs)) {
        return;
    }
    if (main_image_ != nullptr) {
        lv_obj_add_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (video_canvas_ != nullptr) {
        lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
    }
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
    }
    SafeLVGLUnlock();
}

void EezuiDisplayAdapter::StopIdleBreathe(const char* why) {
    AfeFetchGateCancelIdleVisualEdge();
    StopIdleBacklightBreathe(why);
    if (idle_breathe_flash_overlay_ && SafeLVGLLock(0)) {
        idle_flash_overlay_.Hide();
        SafeLVGLUnlock();
    }
    if (!idle_breathe_ && !idle_breathe_need_prime_ && idle_breathe_left_ <= 0 &&
        !idle_breathe_flash_overlay_) {
        return;
    }
    idle_breathe_ = false;
    idle_breathe_small_life_ = false;
    idle_breathe_flash_overlay_ = false;
    idle_life_track_ = 0;
    idle_life_frame_ = 0;
    idle_life_repair_attempts_ = 0;
    idle_life_repair_pending_ = false;
    idle_breathe_need_prime_ = false;
    idle_breathe_left_ = 0;
    idle_breathe_gen_++;
    ESP_LOGW(TAG, "FACE_BREATHE stop why=%s s1as", why ? why : "-");
    esp_rom_printf("!!FACE_BREATHE stop %s\n", why ? why : "-");
}

void EezuiDisplayAdapter::ArmIdleBreathe(const char* why) {
    if (emotion_player_ == nullptr || video_canvas_ == nullptr) {
        return;
    }
    // s1at: never arm before P2 seed_stills — serial showed breathe racing preload
    // so seed_stills never finished (free_psram stayed ~25MB, no ok=6/6).
    if (!emotion_video_player_seed_stills_ready(emotion_player_)) {
        ESP_LOGW(TAG, "FACE_BREATHE skip_arm why=%s seed_ready=0 s1at", why ? why : "-");
        return;
    }
    if (!emotion_video_player_is_decode_paused(emotion_player_)) {
        return;
    }
    const auto st = Application::GetInstance().GetDeviceState();
    if (st != kDeviceStateIdle || InConversationPresent()) {
        return;
    }
    // Do not overlap dialogue MID / settle.
    if (face_anim_left_ > 0 || face_anim_need_prime_ || face_anim_sustain_ ||
        face_anim_await_settle_) {
        return;
    }
    if (FaceRouteV2_IdleFlashOverlayEnabled() && idle_flash_overlay_.Ready()) {
        if (!EnsureFaceAnimTimer()) {
            return;
        }
        StopIdleBacklightBreathe("s1gt_flash_overlay");
        idle_breathe_ = true;
        idle_breathe_flash_overlay_ = true;
        idle_breathe_small_life_ = false;
        idle_life_frame_ = 0;
        idle_life_repair_attempts_ = 0;
        idle_life_repair_pending_ = false;
        idle_breathe_need_prime_ = false;
        idle_breathe_left_ = IdleFlashBandOverlay::kFrameCount;
        idle_breathe_gen_++;
        esp_timer_stop(face_anim_timer_);
        esp_timer_start_once(face_anim_timer_, 4200 * 1000LL);
        esp_rom_printf(kIdleFlashArmLog);
        return;
    }
    if (FaceRouteV2_IdleFlashOverlayEnabled()) {
        // Missing/corrupt assets must not reopen the rejected s1gs PSRAM token path.
        // Keep the s1fw visible fallback or static standby instead.
        if (FaceRouteV2_IdleBacklightBreatheEnabled()) {
            (void)ArmIdleBacklightBreathe("s1gt_asset_fallback");
        }
        esp_rom_printf(kIdleFlashFallbackLog);
        return;
    }
    // s1fu: the v5p3-approved lower standby track is the only framebuffer
    // idle-life candidate. The approved PC profile track1 is compacted to
    // runtime slot 0; qualify it again by its single-track/lower-face geometry.
    const bool token_canary = FaceRouteV2_IdleLifeTokenCanaryEnabled();
    bool small_life = false;
    if (token_canary && FaceRouteV2_StandbyLifeEnabled() &&
        FaceMouth_BindEmotion("standby") && FaceAsset_LifeReady() &&
        FaceAsset_LifeTrackCount() == 1) {
        small_life = FaceAsset_LifeFrameCount(0) > 1;
    }
    if (token_canary && small_life && FaceRouteV2_IdleBacklightBreatheEnabled()) {
        (void)ArmIdleBacklightBreathe("s1fw_visible_base");
    }
    if (!small_life && FaceRouteV2_IdleBacklightBreatheEnabled() &&
        ArmIdleBacklightBreathe(token_canary ? "s1fu_asset_fallback" : why)) {
        return;
    }
    if (token_canary && !small_life) {
        // Never let a missing/invalid layered contract reopen the legacy 400-row
        // idle path. Static standby is the safe final fallback for this canary.
        return;
    }
    // s1cs: FullStill still arms idle breathe, but ticks use BAND (not 480² seed_full).
    // s1cq killed all idle motion → looked "dead"; s1cp-f full-frame breathe caused WDT.
    // Dialogue bookend/enter keep seed_full (seam-free); idle accepts band micro-motion.
    if (!EnsureFaceAnimTimer()) {
        return;
    }
    if (!token_canary) {
        small_life = FaceRouteV2_StandbyLifeEnabled() &&
                     FaceMouth_BindEmotion("standby") && FaceAsset_LifeReady();
    }
    AfeFetchGateSetIdleVisualNotify(&IdleVisualAfeEdgeNotify, nullptr);
    idle_breathe_ = true;
    idle_breathe_need_prime_ = true;
    idle_breathe_small_life_ = small_life;
    idle_life_track_ = 0;
    idle_life_frame_ = 0;
    idle_life_repair_attempts_ = 0;
    idle_life_repair_pending_ = false;
    idle_breathe_left_ = idle_breathe_small_life_
                             ? (int)FaceAsset_LifeFrameCount(idle_life_track_)
                             : 6;  // budgeted cycle (not speak loop_continue)
    // s1bx: span standby clip like MID arc — not 6 consecutive frames at fc/3.
    idle_breathe_arc_step_ =
        emotion_video_player_mid_arc_step(emotion_player_, "standby", 6);
    if (idle_breathe_arc_step_ == 0) {
        idle_breathe_arc_step_ = 1;
    }
    idle_breathe_frame_idx_ = emotion_video_player_mid_arc_start(emotion_player_, "standby");
    idle_breathe_gen_++;
    esp_timer_stop(face_anim_timer_);
    ESP_LOGW(TAG, "s1ep-k FACE_BREATHE arm why=%s mode=%s frames=%d step=%u",
             why ? why : "-", idle_breathe_small_life_ ? "life48" : "legacy400",
             idle_breathe_left_, (unsigned)idle_breathe_arc_step_);
    esp_rom_printf("!!FACE_S1EP idle_life=%d frames=%d\n",
                   idle_breathe_small_life_ ? 1 : 0, idle_breathe_left_);
    // The canary spends one sparse token every 4–7 seconds; the legacy path
    // retains its existing short prime delay.
    esp_timer_start_once(face_anim_timer_,
                         token_canary ? (4200 * 1000LL) : (500 * 1000LL));
}

bool EezuiDisplayAdapter::ArmIdleBacklightBreathe(const char* why) {
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        return false;
    }
    if (idle_backlight_timer_ == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = &EezuiDisplayAdapter::IdleBacklightBreatheTimerCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "idle_bl_life",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &idle_backlight_timer_) != ESP_OK) {
            idle_backlight_timer_ = nullptr;
            ESP_LOGW(TAG, "s1fd idle_bl create_fail");
            return false;
        }
    }
    if (idle_backlight_breathe_) {
        return true;
    }
    const uint8_t base = backlight->brightness();
    if (base == 0) {
        // Backlight restore has not settled yet; retain the proven patch fallback.
        return false;
    }
    AfeFetchGateCancelIdleVisualEdge();
    idle_backlight_base_ = base;
    idle_backlight_last_command_ = base;
    idle_backlight_phase_ = 0;
    idle_backlight_breathe_ = true;
    esp_timer_stop(idle_backlight_timer_);
    if (esp_timer_start_periodic(idle_backlight_timer_, 300 * 1000LL) != ESP_OK) {
        idle_backlight_breathe_ = false;
        return false;
    }
    ESP_LOGW(TAG, "s1fd idle_bl arm why=%s base=%u cycle_ms=7200 psram=0",
             why ? why : "-", (unsigned)base);
    esp_rom_printf("!!FACE_S1FD t=arm base=%u\n", (unsigned)base);
    return true;
}

void EezuiDisplayAdapter::StopIdleBacklightBreathe(const char* why) {
    if (!idle_backlight_breathe_) {
        return;
    }
    idle_backlight_breathe_ = false;
    if (idle_backlight_timer_ != nullptr) {
        esp_timer_stop(idle_backlight_timer_);
    }
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight != nullptr) {
        backlight->SetBrightnessQuiet(idle_backlight_base_);
    }
    ESP_LOGW(TAG, "s1fd idle_bl stop why=%s restore=%u",
             why ? why : "-", (unsigned)idle_backlight_base_);
    esp_rom_printf("!!FACE_S1FD t=stop restore=%u\n", (unsigned)idle_backlight_base_);
}

void EezuiDisplayAdapter::IdleBacklightBreatheTimerCb(void* arg) {
    auto* self = static_cast<EezuiDisplayAdapter*>(arg);
    if (self != nullptr) {
        self->IdleBacklightBreatheTick();
    }
}

void EezuiDisplayAdapter::IdleBacklightBreatheTick() {
    if (!idle_backlight_breathe_) {
        return;
    }
    const auto st = Application::GetInstance().GetDeviceState();
    if (st != kDeviceStateIdle || InConversationPresent()) {
        StopIdleBacklightBreathe("leave_idle");
        return;
    }
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        StopIdleBacklightBreathe("no_backlight");
        return;
    }
    // Detect an external brightness command between our 300ms steps and adopt
    // it as the new base instead of fighting the user's setting.
    const uint8_t current = backlight->brightness();
    if (current != idle_backlight_last_command_) {
        idle_backlight_base_ = current;
        idle_backlight_last_command_ = current;
        idle_backlight_phase_ = 0;
    }
    // 24 x 300ms = 7.2s. s1fd's four-point ceiling was imperceptible on this
    // panel; s1fw keeps the same smooth envelope but allows an eight-point peak.
    // This remains PSRAM-free and scales down automatically at low brightness.
    static constexpr uint8_t kEnvelope[24] = {
        0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 4, 4,
        4, 4, 3, 3, 2, 2, 1, 1, 0, 0, 0, 0,
    };
    uint8_t amplitude = idle_backlight_base_ / 12;
    if (amplitude < 1) amplitude = 1;
    if (amplitude > 8) amplitude = 8;
    const uint8_t depth =
        (uint8_t)((kEnvelope[idle_backlight_phase_] * amplitude + 2) / 4);
    const uint8_t target = idle_backlight_base_ > depth
                               ? (uint8_t)(idle_backlight_base_ - depth)
                               : 1;
    backlight->SetBrightnessQuiet(target);
    idle_backlight_last_command_ = target;
    idle_backlight_phase_ = (uint8_t)((idle_backlight_phase_ + 1) % 24);
}

void EezuiDisplayAdapter::IdleBreatheTick() {
    if (!idle_breathe_) {
        return;
    }
    if (emotion_player_ == nullptr || video_canvas_ == nullptr) {
        StopIdleBreathe("tick_null");
        return;
    }
    const auto st = Application::GetInstance().GetDeviceState();
    if (st != kDeviceStateIdle || InConversationPresent() ||
        !emotion_video_player_is_decode_paused(emotion_player_)) {
        StopIdleBreathe("leave_idle");
        return;
    }
    if (!emotion_video_player_seed_stills_ready(emotion_player_)) {
        StopIdleBreathe("seed_not_ready");
        return;
    }
    if (face_anim_left_ > 0 || face_anim_need_prime_ || face_anim_await_settle_) {
        StopIdleBreathe("mid_takeover");
        return;
    }

    if (idle_breathe_flash_overlay_) {
        static constexpr uint16_t kRestMs[] = {4200, 5300, 6400, 6900};
        constexpr uint16_t kFrameMs = 240;
        constexpr uint16_t kRepairMs = 40;
        const bool visual_ok =
            VisualBudgetV2_Current() == VisualBudgetLevel::TransitionCandidate;
        bool gate_held = false;
        bool abort_to_base = idle_life_repair_pending_;
        uint8_t target = abort_to_base ? (IdleFlashBandOverlay::kFrameCount - 1)
                                       : idle_life_frame_;
        if (!abort_to_base && visual_ok) {
            gate_held = MspiBudgetGateTryEnterIdleVisual();
        }
        if (!abort_to_base && !gate_held) {
            if (idle_life_frame_ == 0) {
                const uint16_t delay = kRestMs[(++idle_breathe_gen_) & 3U];
                esp_timer_start_once(face_anim_timer_, (int64_t)delay * 1000LL);
                esp_rom_printf(kIdleFlashDropLog, visual_ok ? 1U : 2U);
                return;
            }
            abort_to_base = true;
            target = IdleFlashBandOverlay::kFrameCount - 1;
        }

        const esp_err_t perr = PresentIdleFlashBand(target);
        if (gate_held) {
            MspiBudgetGateLeaveIdleVisual();
        }
        const bool submitted = perr == ESP_OK || perr == ESP_ERR_NOT_FINISHED;
        uint16_t delay = kFrameMs;
        if (submitted) {
            idle_life_repair_pending_ = false;
            idle_life_repair_attempts_ = 0;
            if (abort_to_base || target == IdleFlashBandOverlay::kFrameCount - 1) {
                idle_life_frame_ = 0;
                delay = kRestMs[(++idle_breathe_gen_) & 3U];
            } else {
                idle_life_frame_ = target + 1;
            }
        } else if (idle_life_frame_ > 0 && idle_life_repair_attempts_ < 2) {
            idle_life_repair_pending_ = true;
            idle_life_repair_attempts_++;
            delay = kRepairMs;
        } else {
            idle_life_frame_ = 0;
            idle_life_repair_pending_ = false;
            idle_life_repair_attempts_ = 0;
            delay = kRestMs[(++idle_breathe_gen_) & 3U];
        }
        esp_rom_printf(kIdleFlashFrameLog, static_cast<unsigned>(target),
                       abort_to_base ? 1U : 0U, static_cast<int>(perr),
                       static_cast<unsigned>(delay));
        esp_timer_start_once(face_anim_timer_, (int64_t)delay * 1000LL);
        return;
    }

    if (FaceRouteV2_IdleLifeTokenCanaryEnabled()) {
        // Deterministic sparse jitter avoids a fixed AFE cadence while keeping
        // every attempt within the promised 4–7 second envelope.
        static constexpr uint16_t kTokenDelayMs[] = {4200, 5300, 6400, 6900};
        const uint8_t count = FaceAsset_LifeFrameCount(0);
        if (!idle_breathe_small_life_ || count < 5 || FaceAsset_LifeTrackCount() != 1) {
            StopIdleBreathe("s1fu_asset_lost");
            ArmIdleBacklightBreathe("s1fu_asset_lost");
            return;
        }
        const uint8_t keyframes[4] = {
            1, (uint8_t)(count / 2), (uint8_t)(count - 2), (uint8_t)(count - 1)};
        const uint8_t target_frame = idle_life_repair_pending_
                                         ? keyframes[3]
                                         : keyframes[idle_life_frame_];
        bool presented = false;
        if (VisualBudgetV2_Current() == VisualBudgetLevel::TransitionCandidate &&
            MspiBudgetGateTryEnterIdleVisual()) {
            if (AfeFetchGateTryLock(0)) {
                face_holds_afe_gate_ = true;
                if (FaceMouth_Lock(0)) {
                    const esp_err_t perr = PresentLifeBand(0, target_frame, 0);
                    presented = perr == ESP_OK || perr == ESP_ERR_NOT_FINISHED;
                    ESP_LOGW(TAG, "s1fu idle_token blit f=%u/%u e=%d",
                             (unsigned)target_frame, (unsigned)count, (int)perr);
                    FaceMouth_Unlock();
                }
                face_holds_afe_gate_ = false;
                AfeFetchGateUnlock();
            }
            MspiBudgetGateLeaveIdleVisual();
        }
        uint16_t delay_ms = 240;
        if (presented) {
            idle_life_repair_attempts_ = 0;
            if (idle_life_repair_pending_) {
                idle_life_repair_pending_ = false;
                idle_life_frame_ = 0;
            } else {
                idle_life_frame_++;
                if (idle_life_frame_ >= 4) idle_life_frame_ = 0;
            }
        } else {
            idle_life_repair_pending_ = idle_life_repair_pending_ || idle_life_frame_ > 0;
            idle_life_frame_ = 0;
            if (idle_life_repair_pending_ && idle_life_repair_attempts_ < 2) {
                idle_life_repair_attempts_++;
            } else if (idle_life_repair_pending_) {
                delay_ms = kTokenDelayMs[(++idle_breathe_gen_) & 3U];
            }
        }
        if (idle_life_frame_ == 0 && !idle_life_repair_pending_) {
            delay_ms = kTokenDelayMs[(++idle_breathe_gen_) & 3U];
        }
        if (face_anim_timer_ != nullptr) {
            esp_timer_start_once(face_anim_timer_, (int64_t)delay_ms * 1000LL);
        }
        return;
    }


    constexpr int kCycleFrames = 6;
    // s1cs: under FullStill idle uses band (cheaper than 480²); slightly slower cadence.
    const int64_t kIntervalUs = FaceRouteV2_FullStillEnabled() ? (900 * 1000) : (600 * 1000);
    constexpr int64_t kRestUs = 1800 * 1000;
    // Band present when FullStill: avoid s1cp-f WDT path; seam only on idle (not dialogue).
    const bool idle_band = FaceRouteV2_FullStillEnabled();
    // s1ez: visual never queues behind audio.  It takes a free gate or drops this
    // tick; audio_detection (prio 3) always outranks face_worker (prio 2).
    // The legacy 60ms wait remains behind the R0 switch.
    const uint32_t kLockWaitMs = FaceRouteV2_LifeAfeFenceEnabled() ? 0 : 60;
    // s1fa: a 120ms miss loop generated ~28k pointless Core1 wakeups/hour while
    // AFE owned the gate. MQTT contention keeps the coarse fixed backoff.
    constexpr int64_t kMqttRetryUs =
        S1FA_Q_MQTT_IDLE_QUIET ? (900 * 1000LL) : (120 * 1000LL);

    if (!MspiBudgetGateTryEnterIdleVisual()) {
        static uint32_t s_mqtt_quiet_skip_n = 0;
        static int64_t s_mqtt_quiet_last_log_us = 0;
        s_mqtt_quiet_skip_n++;
        const int64_t now = esp_timer_get_time();
        if (s_mqtt_quiet_skip_n == 1 || (now - s_mqtt_quiet_last_log_us) >= 2000000LL) {
            s_mqtt_quiet_last_log_us = now;
            ESP_LOGW(TAG, "s1fa idle_life_drop mqtt_window n=%u",
                     (unsigned)s_mqtt_quiet_skip_n);
            esp_rom_printf("!!FACE_S1FA q=mqtt_skip\n");
        }
        if (face_anim_timer_ != nullptr) {
            esp_timer_start_once(face_anim_timer_, kMqttRetryUs);
        }
        return;
    }
    struct IdleVisualBudgetUnlock {
        ~IdleVisualBudgetUnlock() { MspiBudgetGateLeaveIdleVisual(); }
    } idle_visual_budget_unlock;

    if (!AfeFetchGateTryLock(kLockWaitMs)) {
        static uint32_t s_skip_n = 0;
        static int64_t s_last_log_us = 0;
        s_skip_n++;
        // s1fb: 900ms is 30 * the observed ~30ms AFE fetch cadence, so a
        // miss can retry forever at the same busy phase.  These odd,
        // pairwise-distinct intervals keep the same ~4k wakeups/hour budget
        // while walking across common 20/30/40ms audio periods.
        static constexpr uint16_t kAfeRetryDitherMs[] = {731, 887, 1019};
        const uint32_t afe_retry_ms = S1FB_R_IDLE_AFE_DITHER
                                          ? kAfeRetryDitherMs[(s_skip_n - 1) % 3]
                                          : (S1FA_Q_MQTT_IDLE_QUIET ? 900U : 120U);
        const int64_t now = esp_timer_get_time();
        if (s_skip_n == 1 || (now - s_last_log_us) >= 2000000LL) {
            s_last_log_us = now;
            ESP_LOGW(TAG,
                     "FACE_BREATHE skip_afe n=%u left=%d wait_ms=%u retry_ms=%u s1fb",
                     (unsigned)s_skip_n, idle_breathe_left_, (unsigned)kLockWaitMs,
                     (unsigned)afe_retry_ms);
            esp_rom_printf("!!FACE_S1FB r=afe_skip retry_ms=%u n=%u\n",
                           (unsigned)afe_retry_ms, (unsigned)s_skip_n);
        }
        if (S1FC_S_AFE_EDGE_TOKEN) {
            // One pending latest-value request.  AFE releases its gate first,
            // then emits at most one token per 900ms; visual still try-locks and
            // drops if audio or MQTT reclaimed the budget.
            AfeFetchGateRequestIdleVisualEdge(900);
            esp_rom_printf("!!FACE_S1FC s=edge_wait n=%u\n", (unsigned)s_skip_n);
        } else if (face_anim_timer_ != nullptr) {
            esp_timer_start_once(face_anim_timer_, (int64_t)afe_retry_ms * 1000LL);
        }
        return;
    }
    face_holds_afe_gate_ = true;

    if (idle_breathe_small_life_) {
        // A Speaking edge may rebind the shared patch bank immediately after
        // StopIdleBreathe().  Serialize this last in-flight idle tick with that
        // rebind so PresentLifeBand never observes freed patch storage.
        if (!FaceMouth_Lock(100)) {
            face_holds_afe_gate_ = false;
            AfeFetchGateUnlock();
            ESP_LOGW(TAG, "s1ew idle_life_drop patch_mutex");
            if (face_anim_timer_ != nullptr) {
                esp_timer_start_once(face_anim_timer_, kMqttRetryUs);
            }
            return;
        }
        struct IdleLifeUnlock { ~IdleLifeUnlock() { FaceMouth_Unlock(); } } idle_life_unlock;
        const uint8_t count = FaceMouth_LifeFrameCount(idle_life_track_);
        if (count < 2) {
            face_holds_afe_gate_ = false;
            AfeFetchGateUnlock();
            StopIdleBreathe("idle_life_lost");
            return;
        }
        if (idle_life_frame_ >= count) {
            idle_life_frame_ = 0;
            if (FaceMouth_LifeTrackCount() > 1) {
                idle_life_track_ = (uint8_t)((idle_life_track_ + 1) % FaceMouth_LifeTrackCount());
            }
            face_holds_afe_gate_ = false;
            AfeFetchGateUnlock();
            const int64_t rest_us = 4200 * 1000LL;
            if (face_anim_timer_ != nullptr) esp_timer_start_once(face_anim_timer_, rest_us);
            return;
        }
        const uint8_t presented_track = idle_life_track_;
        const uint8_t presented_frame = idle_life_frame_;
        const esp_err_t perr = PresentLifeBand(presented_track, presented_frame);
        // NOT_FINISHED means the frame was submitted but its fence exceeded the
        // bounded wait.  Do not replay it later; stale visual frames are droppable.
        if (perr == ESP_OK || perr == ESP_ERR_NOT_FINISHED) idle_life_frame_++;
        face_holds_afe_gate_ = false;
        AfeFetchGateUnlock();
        // Do not hold the audio gate while formatting or writing UART logs.
        ESP_LOGW(TAG, "s1ez idle_life_blit track=%u frame=%u/%u err=%s gate=released",
                 (unsigned)presented_track, (unsigned)presented_frame, (unsigned)count,
                 esp_err_to_name(perr));
        const int64_t frame_us = 260 * 1000LL;
        if (face_anim_timer_ != nullptr) esp_timer_start_once(face_anim_timer_, frame_us);
        return;
    }

    const uint8_t* rgb = nullptr;
    uint32_t size = 0, w = 0, h = 0;
    esp_err_t derr = ESP_FAIL;
    int decode_ms = 0;

    if (idle_breathe_left_ <= 0) {
        // Cycle end → open-eye seed rest (not frame0; SD head often blink/closed).
        derr = emotion_video_player_get_seed_rgb565(emotion_player_, "standby", &rgb, &size, &w, &h);
        if (derr != ESP_OK || rgb == nullptr) {
            derr = emotion_video_player_decode_at_rgb565(
                emotion_player_, "standby",
                emotion_video_player_mid_stride_skip(emotion_player_, "standby"), &rgb, &size, &w,
                &h);
        }
        if (derr == ESP_OK && rgb != nullptr) {
            current_emotion_name_ = "standby";
            face_anim_emo_ = "standby";
            const bool has_fb = last_bypass_rgb_ != nullptr;
            PresentFaceFrameToLvgl(rgb, size, w, h, !has_fb, has_fb);
            (void)idle_band;
        }
        idle_breathe_need_prime_ = true;
        idle_breathe_left_ = kCycleFrames;
        idle_breathe_frame_idx_ = emotion_video_player_mid_arc_start(emotion_player_, "standby");
        ESP_LOGW(TAG, "FACE_BREATHE rest→seed then cycle frames=%d band=%d s1cs", kCycleFrames,
                 idle_band ? 1 : 0);
        esp_rom_printf("!!FACE_BREATHE rest s1cs\n");
        face_holds_afe_gate_ = false;
        AfeFetchGateUnlock();
        if (face_anim_timer_ != nullptr) {
            esp_timer_start_once(face_anim_timer_, kRestUs);
        }
        return;
    }

    const int64_t td0 = esp_timer_get_time();
    if (idle_breathe_need_prime_) {
        idle_breathe_need_prime_ = false;
        if (idle_breathe_arc_step_ == 0) {
            idle_breathe_arc_step_ =
                emotion_video_player_mid_arc_step(emotion_player_, "standby", kCycleFrames);
            if (idle_breathe_arc_step_ == 0) {
                idle_breathe_arc_step_ = 1;
            }
        }
        idle_breathe_frame_idx_ = emotion_video_player_mid_arc_start(emotion_player_, "standby");
    } else {
        idle_breathe_frame_idx_ += idle_breathe_arc_step_;
    }
    // s1by: one JPEG via index seek — was decode_next_n(step≈13) → 13× JPEG / tick → reboot.
    derr = emotion_video_player_decode_at_rgb565(emotion_player_, "standby", idle_breathe_frame_idx_,
                                                 &rgb, &size, &w, &h);
    decode_ms = (int)((esp_timer_get_time() - td0) / 1000);
    const bool log_breathe = FaceRouteV2_ShouldLogFrame();
    if (log_breathe) {
        ESP_LOGW(TAG, "FACE_BREATHE decode_at idx=%u step=%u left=%d decode_ms=%d s1by",
                 (unsigned)idle_breathe_frame_idx_, (unsigned)idle_breathe_arc_step_,
                 idle_breathe_left_, decode_ms);
    }

    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "FACE_BREATHE decode_fail err=%s → rest s1by", esp_err_to_name(derr));
        idle_breathe_left_ = 0;
        face_holds_afe_gate_ = false;
        AfeFetchGateUnlock();
        if (face_anim_timer_ != nullptr) {
            esp_timer_start_once(face_anim_timer_, kRestUs);
        }
        return;
    }

    current_emotion_name_ = "standby";
    face_anim_emo_ = "standby";
    const int64_t tp0 = esp_timer_get_time();
    // s1cs: never seed_full on idle breathe under FullStill (that was the WDT path).
    const esp_err_t perr = PresentFaceFrameToLvgl(rgb, size, w, h, false, true);
    const int present_ms = (int)((esp_timer_get_time() - tp0) / 1000);
    idle_breathe_left_--;
    if (log_breathe) {
        ESP_LOGW(TAG, "FACE_BREATHE tick left=%d decode_ms=%d present_ms=%d band=1 err=%s s1cs",
                 idle_breathe_left_, decode_ms, present_ms, esp_err_to_name(perr));
        esp_rom_printf("!!FACE_BREATHE tick left=%d s1cs\n", idle_breathe_left_);
    }
    face_holds_afe_gate_ = false;
    AfeFetchGateUnlock();
    if (face_anim_timer_ != nullptr) {
        esp_timer_start_once(face_anim_timer_, kIntervalUs);
    }
}

void EezuiDisplayAdapter::StopFacePanelAnim(const char* reason) {
    StopIdleBreathe(reason);
    // Keep mouth follow across enter_hold — ArmMouthFollow re-arms immediately after.
    if (reason == nullptr || strcmp(reason, "enter_hold") != 0) {
        StopMouthFollow(reason);
    }
    const bool was_active = face_anim_left_ > 0 || face_anim_need_prime_ || face_anim_sustain_ ||
                            face_anim_await_settle_ || emotion_release_active_;
    const bool notify_end =
        was_active || (reason != nullptr && strcmp(reason, "done") == 0) ||
        (reason != nullptr && strcmp(reason, "enter_hold") == 0);
    face_anim_left_ = 0;
    face_anim_roi_budget_ = 0;
    face_anim_fast_left_ = 0;
    face_anim_sustain_ = false;
    face_anim_await_settle_ = false;
    face_anim_pending_follow_ = 0;
    face_anim_need_prime_ = false;
    face_anim_roi_only_ = false;
    face_anim_enter_mode_ = false;
    emotion_release_active_ = false;
    emotion_release_entering_ = false;
    emotion_release_frame_ = 0;
    face_anim_gen_++;
    if (face_anim_timer_ != nullptr && !mouth_follow_ && !emotion_release_active_) {
        esp_timer_stop(face_anim_timer_);
    }
    if (notify_end) {
        ESP_LOGW(TAG, "SAD_DIAG short_anim stop reason=%s emo=%s", reason ? reason : "-",
                 face_anim_emo_.c_str());
        if (presenter_ != nullptr) {
            presenter_->OnBypassAnimEnded();
            // s1cn-b: slice done = safe commit point for overwritten pending emotion.
            presenter_->FlushPendingEmotion(reason ? reason : "slice_end");
        }
    }
}

void EezuiDisplayAdapter::FaceWorkerTickTrampoline(void* ctx) {
    auto* self = static_cast<EezuiDisplayAdapter*>(ctx);
    if (self != nullptr) {
        self->FacePanelAnimTick();
    }
}

void EezuiDisplayAdapter::FacePanelAnimTimerCb(void* arg) {
    auto* self = static_cast<EezuiDisplayAdapter*>(arg);
    if (self == nullptr) {
        return;
    }
    // s1bi: NEVER run hw_decode / PresentFace on ESP_TIMER_TASK (stack=3584).
    // s1cn-a: prefer face_worker (queue len 1, Core1, stack 12288); flag off → old Schedule path.
    if (FaceRouteV2_WorkerEnabled()) {
        FaceRouteV2_EnsureWorker(&EezuiDisplayAdapter::FaceWorkerTickTrampoline, self);
        FaceRouteV2_PostTick();
        return;
    }
    Application::GetInstance().Schedule([self]() { self->FacePanelAnimTick(); });
}

bool EezuiDisplayAdapter::EnsureFaceAnimTimer() {
    if (face_anim_timer_ != nullptr) {
        return true;
    }
    esp_timer_create_args_t args = {
        .callback = &EezuiDisplayAdapter::FacePanelAnimTimerCb,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "face_panim",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &face_anim_timer_) != ESP_OK) {
        face_anim_timer_ = nullptr;
        ESP_LOGW(TAG, "SAD_DIAG face_timer_create_fail");
        return false;
    }
    return true;
}

void EezuiDisplayAdapter::FacePanelAnimTick() {
    if (emotion_player_ == nullptr || video_canvas_ == nullptr) {
        StopFacePanelAnim("tick_idle");
        return;
    }
    if (strong_emotion_hold_pending_) {
        const auto hold_state = Application::GetInstance().GetDeviceState();
        const int64_t now_us = esp_timer_get_time();
        if (hold_state == kDeviceStateListening && now_us < strong_emotion_hold_until_us_) {
            if (face_anim_timer_ != nullptr) {
                esp_timer_start_once(face_anim_timer_, strong_emotion_hold_until_us_ - now_us);
            }
            return;
        }
        strong_emotion_hold_pending_ = false;
        strong_emotion_hold_until_us_ = 0;
        if (hold_state == kDeviceStateListening) {
            ESP_LOGW(TAG, "s1fg strong_hold_done emo=%s", current_emotion_name_.c_str());
            PresentStandbyBookend("strong_hold_done");
            StopFacePanelAnim("strong_hold_done");
        }
        return;
    }
    // s1et release owns the shared timer across Speaking->Listening/Idle.
    if (emotion_release_active_) {
        EmotionReleaseTick();
        return;
    }
    // s1cr-h: mouth follow shares timer; must not be aborted by listening idle gate.
    if (mouth_follow_) {
        MouthFollowTick();
        return;
    }
    // s1as: idle breathe uses the same timer; must run before idle-state abort.
    if (idle_breathe_) {
        IdleBreatheTick();
        return;
    }
    const auto st = Application::GetInstance().GetDeviceState();
    // s1cp-g: enter arc may run in listening/speaking; only idle/connecting abort it.
    if (face_anim_enter_mode_) {
        if (st == kDeviceStateConnecting || st == kDeviceStateIdle) {
            PresentStandbyBookend("enter_interrupt");
            StopFacePanelAnim("tick_state");
            return;
        }
    } else if (st == kDeviceStateConnecting || st == kDeviceStateListening ||
               st == kDeviceStateIdle) {
        if (face_anim_roi_only_ || face_anim_sustain_ || face_anim_left_ > 0 ||
            face_anim_await_settle_) {
            PresentStandbyBookend("slice_interrupt");
        }
        StopFacePanelAnim("tick_state");
        return;
    }

    const bool speaking = (st == kDeviceStateSpeaking);
    // s1cp-g: fixed enter cadence — uniform enter, not FaceCadence backoff slideshow.
    const int cadence_ms = face_anim_enter_mode_ ? kEnterCadenceMs : FaceCadenceMs(speaking);
    const int64_t kRoiIntervalUs = (int64_t)cadence_ms * 1000;

    // s1am: settle gate — OPEN/skip done; wait out TTS/I2S edge before ROI decode.
    if (face_anim_await_settle_) {
        face_anim_await_settle_ = false;
        const int n = face_anim_pending_follow_ > 0 ? face_anim_pending_follow_ : kFaceMidFrames;
        face_anim_pending_follow_ = 0;
        ESP_LOGW(TAG, "FACE_SPEAK settle_done→mid emo=%s frames=%d fast=%d s1am",
                 face_anim_emo_.c_str(), n, face_anim_fast_left_);
        esp_rom_printf("!!FACE_SPEAK mid s1am\n");
        StartFaceRoiFollow(n);
        return;
    }

    auto slice_close = [&](const char* tag) {
        face_anim_sustain_ = false;
        if (face_anim_enter_mode_) {
            // Hold emotion seed (not standby bookend / band).
            const uint8_t* hold_rgb = nullptr;
            uint32_t hold_size = 0, hold_w = 0, hold_h = 0;
            esp_err_t herr = emotion_video_player_get_seed_rgb565(
                emotion_player_, face_anim_emo_.c_str(), &hold_rgb, &hold_size, &hold_w, &hold_h);
            if (herr == ESP_OK && hold_rgb != nullptr) {
                PresentFaceFrameToLvgl(hold_rgb, hold_size, hold_w, hold_h, true, false);
            }
            ESP_LOGW(TAG, "s1cp-g enter_hold emo=%s tag=%s", face_anim_emo_.c_str(),
                     tag ? tag : "-");
            esp_rom_printf("!!FACE_S1CP g=enter_hold emo=%s\n", face_anim_emo_.c_str());
            const std::string emo = face_anim_emo_;
            StopFacePanelAnim("enter_hold");
            ArmMouthFollow(emo.c_str());
            return;
        }
        PresentStandbyBookend(tag);
        StopFacePanelAnim("done");
    };

    if (face_anim_sustain_) {
        face_anim_sustain_ = false;
        slice_close("slice_close");
        return;
    }

    if (face_anim_left_ <= 0) {
        slice_close("slice_close");
        return;
    }

    const uint8_t* rgb = nullptr;
    uint32_t size = 0, w = 0, h = 0;
    esp_err_t derr = ESP_FAIL;
    const bool is_prime = face_anim_need_prime_;
    int decode_ms = 0;
    if (face_anim_need_prime_) {
        face_anim_need_prime_ = false;
        // MID starts from the seed mid-clip (fc/3) so the slice reads as motion, not a
        // second still. s1av had disabled this under TTS because decode_ms≈94 preceded a
        // WDT; s1ba showed that WDT came from the WakeNet PSRAM kernel, so the stride is
        // restored on both paths and decode_ms stays visible in FACE_STRIDE.
        uint32_t stride_skip = 0;
        if (face_anim_enter_mode_) {
            // Enter: frame0 → seed (fc/3), uniform step; then hold seed.
            const uint32_t seed_idx =
                emotion_video_player_mid_stride_skip(emotion_player_, face_anim_emo_.c_str());
            const uint32_t n = face_anim_left_ > 0 ? (uint32_t)face_anim_left_ : 6u;
            face_anim_arc_step_ = (seed_idx / n) > 0 ? (seed_idx / n) : 1u;
            face_anim_frame_idx_ = 0;
            stride_skip = 0;
            ESP_LOGW(TAG, "FACE_ARC enter emo=%s seed=%u step=%u n=%u s1cp", face_anim_emo_.c_str(),
                     (unsigned)seed_idx, (unsigned)face_anim_arc_step_, (unsigned)n);
            esp_rom_printf("!!FACE_S1CP g=enter_arc seed=%u step=%u n=%u\n", (unsigned)seed_idx,
                           (unsigned)face_anim_arc_step_, (unsigned)n);
        } else if (face_anim_roi_only_) {
            stride_skip =
                emotion_video_player_mid_arc_start(emotion_player_, face_anim_emo_.c_str());
            face_anim_arc_step_ = emotion_video_player_mid_arc_step(
                emotion_player_, face_anim_emo_.c_str(), (uint32_t)kFaceMidFrames);
            face_anim_frame_idx_ = stride_skip;
            ESP_LOGW(TAG, "FACE_ARC emo=%s start=%u step=%u n=%d s1bq", face_anim_emo_.c_str(),
                     (unsigned)stride_skip, (unsigned)face_anim_arc_step_, kFaceMidFrames);
            esp_rom_printf("!!FACE_ARC start=%u step=%u n=%d s1bq\n", (unsigned)stride_skip,
                           (unsigned)face_anim_arc_step_, kFaceMidFrames);
        } else {
            face_anim_arc_step_ = 1;
            face_anim_frame_idx_ = 0;
        }
        const int64_t td0 = esp_timer_get_time();
        // s1cb/E: prime also via decode_at (same O(1) path as mid ticks).
        derr = emotion_video_player_decode_at_rgb565(emotion_player_, face_anim_emo_.c_str(),
                                                    face_anim_frame_idx_, &rgb, &size, &w, &h);
        if (derr != ESP_OK || rgb == nullptr) {
            derr = emotion_video_player_get_seed_rgb565(emotion_player_, face_anim_emo_.c_str(),
                                                       &rgb, &size, &w, &h);
            face_anim_left_ = 1;  // one still then done
        }
        decode_ms = (int)((esp_timer_get_time() - td0) / 1000);
        if (face_anim_roi_only_) {
            ESP_LOGW(TAG, "FACE_STRIDE emo=%s skip=%u left=%d decode_ms=%d speak=%d s1bc",
                     face_anim_emo_.c_str(), (unsigned)stride_skip, face_anim_left_, decode_ms,
                     speaking ? 1 : 0);
            esp_rom_printf("!!FACE_STRIDE emo=%s skip=%u speak=%d\n", face_anim_emo_.c_str(),
                           (unsigned)stride_skip, speaking ? 1 : 0);
        }
        // s1cp-g: enter prime paints seed_full (no band); s1an ROI prime unchanged.
        if ((face_anim_roi_only_ || face_anim_enter_mode_) && derr == ESP_OK && rgb != nullptr) {
            if (speaking) {
                WdtContendArmFaceSpeak();
            }
            const int64_t tp0 = esp_timer_get_time();
            BootTraceMark("FACE_PRIME", "beg");
            const esp_err_t perr = face_anim_enter_mode_
                                      ? PresentFaceFrameToLvgl(rgb, size, w, h, true, false)
                                      : PresentFaceFrameToLvgl(rgb, size, w, h, false, true);
            const int present_ms = (int)((esp_timer_get_time() - tp0) / 1000);
            NoteFacePresentPressure(present_ms, perr, "prime");
            face_anim_left_--;
            if (face_anim_roi_budget_ > 0) {
                face_anim_roi_budget_--;
            }
            ESP_LOGW(TAG,
                     "SAD_DIAG short_anim prime_present_roi emo=%s left=%d fast=%d present_ms=%d "
                     "err=%s s1ar",
                     face_anim_emo_.c_str(), face_anim_left_, face_anim_fast_left_, present_ms,
                     esp_err_to_name(perr));
            ESP_LOGW(TAG,
                     "FACE_COST emo=%s phase=prime decode_ms=%d present_ms=%d roi=1 speak=%d "
                     "left=%d flush_n=%u s1ar",
                     face_anim_emo_.c_str(), decode_ms, present_ms, speaking ? 1 : 0,
                     face_anim_left_, (unsigned)WdtContendFlushCount());
            esp_rom_printf("!!FACE_PRIME_ROI emo=%s\n", face_anim_emo_.c_str());
            BootTraceMark("FACE_PRIME", "end");
            if (face_anim_left_ <= 0) {
                ESP_LOGW(TAG, "FACE_SLICE mid_done→close emo=%s speaking=%d s1an",
                         face_anim_emo_.c_str(), speaking ? 1 : 0);
                slice_close("slice_close");
                return;
            }
            if (face_anim_timer_ != nullptr) {
                esp_timer_start_once(face_anim_timer_, kRoiIntervalUs);
            }
            return;
        }
        ESP_LOGW(TAG, "SAD_DIAG short_anim prime_async emo=%s err=%s left=%d",
                 face_anim_emo_.c_str(), esp_err_to_name(derr), face_anim_left_);
    } else {
        const int64_t td0 = esp_timer_get_time();
        // s1cb/E: one JPEG via index seek — was decode_next_n(step) → step×JPEG / tick under TTS.
        if ((face_anim_roi_only_ || face_anim_enter_mode_) && face_anim_arc_step_ > 0) {
            face_anim_frame_idx_ += face_anim_arc_step_;
        } else {
            face_anim_frame_idx_ += 1;
        }
        derr = emotion_video_player_decode_at_rgb565(emotion_player_, face_anim_emo_.c_str(),
                                                    face_anim_frame_idx_, &rgb, &size, &w, &h);
        decode_ms = (int)((esp_timer_get_time() - td0) / 1000);
        ESP_LOGW(TAG, "FACE_MID decode_at idx=%u step=%u left=%d decode_ms=%d speak=%d s1cb",
                 (unsigned)face_anim_frame_idx_, (unsigned)face_anim_arc_step_, face_anim_left_,
                 decode_ms, speaking ? 1 : 0);
    }
    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "SAD_DIAG short_anim decode_fail emo=%s err=%s left=%d",
                 face_anim_emo_.c_str(), esp_err_to_name(derr), face_anim_left_);
        slice_close("slice_decode_fail");
        return;
    }

    const int64_t t0 = esp_timer_get_time();
    const bool seed_full = face_anim_enter_mode_ || (is_prime && !face_anim_roi_only_);
    const bool use_roi = face_anim_roi_only_ && !face_anim_enter_mode_;
    esp_rom_printf("!!FACE_PIN tick_present_beg left=%d\n", face_anim_left_);
    BootTraceMark("FACE_TICK", "present_beg");
    const esp_err_t perr = PresentFaceFrameToLvgl(rgb, size, w, h, seed_full, use_roi);
    const int present_ms = (int)((esp_timer_get_time() - t0) / 1000);
    NoteFacePresentPressure(present_ms, perr, "mid");
    face_anim_left_--;
    if (face_anim_roi_only_ && face_anim_roi_budget_ > 0) {
        face_anim_roi_budget_--;
    }
    if (FaceRouteV2_ShouldLogFrame()) {
        ESP_LOGW(TAG,
                 "SAD_DIAG short_anim frame emo=%s left=%d budget=%d fast=%d present_ms=%d seed=%d "
                 "roi=%d err=%s",
                 face_anim_emo_.c_str(), face_anim_left_, face_anim_roi_budget_,
                 face_anim_fast_left_, present_ms, seed_full ? 1 : 0, face_anim_roi_only_ ? 1 : 0,
                 esp_err_to_name(perr));
        esp_rom_printf("!!FACE_PIN cost_beg left=%d\n", face_anim_left_);
        ESP_LOGW(TAG,
                 "FACE_COST emo=%s phase=mid decode_ms=%d present_ms=%d roi=%d speak=%d left=%d "
                 "flush_n=%u s1ao",
                 face_anim_emo_.c_str(), decode_ms, present_ms, face_anim_roi_only_ ? 1 : 0,
                 speaking ? 1 : 0, face_anim_left_, (unsigned)WdtContendFlushCount());
        esp_rom_printf("!!FACE_PIN cost_end left=%d flush_n=%u\n", face_anim_left_,
                       (unsigned)WdtContendFlushCount());
    }
    BootTraceMark("FACE_COST", "beg");
    BootTraceMark("FACE_COST", "end");

    if (face_anim_left_ <= 0) {
        ESP_LOGW(TAG, "FACE_SLICE mid_done→close emo=%s speaking=%d s1ao", face_anim_emo_.c_str(),
                 speaking ? 1 : 0);
        esp_rom_printf("!!FACE_PIN close_beg s1ao\n");
        BootTraceMark("FACE_CLOSE", "beg");
        slice_close("slice_close");
        BootTraceMark("FACE_CLOSE", "end");
        return;
    }
    if (face_anim_timer_ != nullptr) {
        int64_t interval_us = 80 * 1000;
        if (face_anim_enter_mode_) {
            interval_us = kRoiIntervalUs;  // fixed enter cadence
        } else if (face_anim_roi_only_) {
            if (face_anim_fast_left_ > 0) {
                face_anim_fast_left_--;
            }
            interval_us = kRoiIntervalUs;
            if (face_rate_log_gen_ != face_anim_gen_) {
                face_rate_log_gen_ = face_anim_gen_;
                ESP_LOGW(TAG, "FACE_RATE emo=%s interval_ms=%d step=%d speak=%d s1bo",
                         face_anim_emo_.c_str(), cadence_ms, face_cadence_step_,
                         speaking ? 1 : 0);
                esp_rom_printf("!!FACE_RATE ms=%d step=%d s1bo\n", cadence_ms,
                               face_cadence_step_);
            }
        }
        esp_timer_start_once(face_anim_timer_, interval_us);
    }
}

int EezuiDisplayAdapter::FaceCadenceMs(bool speaking) const {
    // s1bo: 18 frames at 450ms read as a slideshow (~2.2 fps); motion fusion needs ~12 fps.
    // Step 0 is the target; 1/2 are entered only from measured present/lock pressure (s1am's
    // 280/450 was a fixed guess made before ROI became a local row copy).
    static constexpr int kSpeakCadenceMs[kFaceCadenceSteps] = {160, 260, 450};
    static constexpr int kIdleCadenceMs[kFaceCadenceSteps] = {140, 220, 320};
    int step = face_cadence_step_;
    if (step < 0) {
        step = 0;
    } else if (step > kFaceCadenceSteps - 1) {
        step = kFaceCadenceSteps - 1;
    }
    return speaking ? kSpeakCadenceMs[step] : kIdleCadenceMs[step];
}

void EezuiDisplayAdapter::NoteFacePresentPressure(int present_ms, esp_err_t perr,
                                                  const char* phase) {
    // A lock timeout means LVGL was already busy — treat it as two slow frames so the
    // cadence backs off on the first hit instead of after a second one.
    if (perr == ESP_ERR_TIMEOUT) {
        face_slow_streak_ += 2;
    } else if (present_ms > kFacePresentSlowMs) {
        face_slow_streak_++;
    } else {
        face_slow_streak_ = 0;
        return;
    }
    if (face_slow_streak_ < 2 || face_cadence_step_ >= kFaceCadenceSteps - 1) {
        return;
    }
    face_slow_streak_ = 0;
    face_cadence_step_++;
    ESP_LOGW(TAG, "FACE_BACKOFF emo=%s phase=%s present_ms=%d err=%s step=%d s1bo",
             face_anim_emo_.c_str(), phase ? phase : "-", present_ms, esp_err_to_name(perr),
             face_cadence_step_);
    esp_rom_printf("!!FACE_BACKOFF step=%d present_ms=%d s1bo\n", face_cadence_step_, present_ms);
}

void EezuiDisplayAdapter::StartFaceRoiFollow(int follow_frames) {
    if (follow_frames <= 0 || emotion_player_ == nullptr || video_canvas_ == nullptr) {
        return;
    }
    // Do not StopFacePanelAnim(notify) — keeps caption queue; just re-arm follow.
    StopIdleBreathe("roi_follow");
    face_anim_left_ = 0;
    face_anim_need_prime_ = false;
    face_anim_await_settle_ = false;
    face_anim_enter_mode_ = false;
    if (face_anim_timer_ != nullptr) {
        esp_timer_stop(face_anim_timer_);
    }
    if (!EnsureFaceAnimTimer()) {
        return;
    }

    const bool speaking =
        Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
    face_anim_left_ = follow_frames;
    // Arm clip once so decode_next stays on the committed emotion (seed alone does not seek).
    face_anim_need_prime_ = true;
    face_anim_roi_only_ = true;
    face_anim_arc_step_ = emotion_video_player_mid_arc_step(
        emotion_player_, face_anim_emo_.c_str(), (uint32_t)kFaceMidFrames);
    face_anim_frame_idx_ =
        emotion_video_player_mid_arc_start(emotion_player_, face_anim_emo_.c_str());
    // s1cb: speaking starts one cadence step slower — leave TTS CPU/bus headroom by default.
    // Idle/listen MID still targets step 0; backoff still climbs to 450 on pressure.
    face_cadence_step_ = speaking ? 1 : 0;
    face_slow_streak_ = 0;
    face_anim_gen_++;
    // s1ao diag-only: open CONTEND/FACE_FLUSH window for speak-face path (was silent before).
    if (speaking) {
        WdtContendArmFaceSpeak();
    }
    ESP_LOGW(TAG, "FACE_ROI_FOLLOW emo=%s frames=%d budget=%d fast=%d speak=%d (MID s1ao)",
             face_anim_emo_.c_str(), follow_frames, face_anim_roi_budget_, face_anim_fast_left_,
             speaking ? 1 : 0);
    esp_rom_printf("!!FACE_ROI_FOLLOW n=%d s1ao\n", follow_frames);
    BootTraceMark("FACE_MID_ARM", speaking ? "speak" : "nospeak");
    const int64_t first_us = (int64_t)FaceCadenceMs(speaking) * 1000;
    esp_timer_start_once(face_anim_timer_, first_us);
}

void EezuiDisplayAdapter::StartFaceEnterTransition(const char* emotion_name) {
    // s1cs: slightly longer enter so emotion change is visible before hold (mouth pack still needed).
    constexpr int kEnterFrames = 8;
    constexpr int64_t kEnterIntervalUs = 160 * 1000;
    if (emotion_name == nullptr || emotion_player_ == nullptr || video_canvas_ == nullptr) {
        return;
    }
    StopIdleBreathe("enter_arc");
    StopMouthFollow("enter_restart");
    if (face_anim_timer_ != nullptr) {
        esp_timer_stop(face_anim_timer_);
    }
    if (!EnsureFaceAnimTimer()) {
        // Fallback: one full seed paint.
        const uint8_t* rgb = nullptr;
        uint32_t size = 0, w = 0, h = 0;
        if (emotion_video_player_get_seed_rgb565(emotion_player_, emotion_name, &rgb, &size, &w,
                                                 &h) == ESP_OK &&
            rgb != nullptr) {
            PresentFaceFrameToLvgl(rgb, size, w, h, true, false);
        }
        if (presenter_ != nullptr) {
            presenter_->OnBypassAnimEnded();
        }
        ArmMouthFollow(emotion_name);
        return;
    }
    face_anim_emo_ = emotion_name;
    current_emotion_name_ = emotion_name;
    face_anim_left_ = kEnterFrames;
    face_anim_need_prime_ = true;
    face_anim_roi_only_ = false;
    face_anim_enter_mode_ = true;
    face_anim_sustain_ = false;
    face_anim_await_settle_ = false;
    face_anim_pending_follow_ = 0;
    face_anim_roi_budget_ = 0;
    face_anim_fast_left_ = 0;
    face_cadence_step_ = 0;
    face_slow_streak_ = 0;
    face_anim_gen_++;
    ShowSdFaceCanvasOnly();
    ESP_LOGW(TAG, "s1cp-g enter_arm emo=%s frames=%d interval_ms=160 s1cs", emotion_name,
             kEnterFrames);
    esp_rom_printf("!!FACE_S1CP g=enter_arm emo=%s n=%d\n", emotion_name, kEnterFrames);
    esp_timer_start_once(face_anim_timer_, kEnterIntervalUs);
}

void EezuiDisplayAdapter::StopMouthFollow(const char* why) {
    if (!mouth_follow_) {
        return;
    }
    mouth_follow_ = false;
    mouth_visual_level_ = 0;
    mouth_budget_phase_ = 0;
    mouth_budget_logged_level_ = 0xff;
    life_budget_deferred_ = false;
    pose_blink_stage_ = 0;
    pose_blink_will_swap_ = false;
    mouth_pose_ = 0;
    life_target_pose_ = 0;
    life_blend_stage_ = 0;
    life_due_us_ = 0;
    ESP_LOGW(TAG, "s1cr-h mouth_stop why=%s", why ? why : "-");
    esp_rom_printf("!!FACE_S1CR mouth_stop\n");
}

bool EezuiDisplayAdapter::ArmMouthFollow(const char* emotion_name, bool base_already_present) {
    if (emotion_name == nullptr || !FaceMouth_Enabled()) {
        return false;
    }
    if (!FaceMouth_Lock(1500)) {
        ESP_LOGW(TAG, "s1ew mouth_skip emo=%s (patch mutex)", emotion_name);
        return false;
    }
    struct MouthUnlock { ~MouthUnlock() { FaceMouth_Unlock(); } } mouth_unlock;
    StopEmotionRelease("mouth_arm");
    const bool core_ready = FaceSpeechCore_Ready(emotion_name);
    if (core_ready && !base_already_present &&
        (speech_core_tts_started_.load(std::memory_order_acquire) ||
         speech_core_full_committed_)) {
        pending_face_emo_ = emotion_video_player_canonicalize_emotion(emotion_name);
        speech_core_pending_generation_ = speech_core_generation_;
        ESP_LOGW(TAG, "s1gl core_defer emo=%s tts=%d full=%d latest_only=1",
                 emotion_name,
                 speech_core_tts_started_.load(std::memory_order_relaxed) ? 1 : 0,
                 speech_core_full_committed_ ? 1 : 0);
        return false;
    }
    const bool bound = core_ready ? FaceSpeechCore_Select(emotion_name)
                                  : FaceMouth_BindEmotion(emotion_name);
    const bool core_selected = core_ready && FaceSpeechCore_Active();
    if (!bound || !FaceAsset_Ready()) {
        ESP_LOGW(TAG, "s1cr-h mouth_skip emo=%s (no layered pack)", emotion_name);
        return false;
    }
    if (!EnsureFaceAnimTimer()) {
        return false;
    }
    uint16_t base_w = 0, base_h = 0;
    const uint8_t* base = FaceAsset_PoseBase(0, &base_w, &base_h);
    if (base == nullptr || base_w == 0 || base_h == 0) {
        ESP_LOGW(TAG, "s1cx mouth_skip emo=%s (no canonical base)", emotion_name);
        return false;
    }
    const esp_err_t base_err = base_already_present ? ESP_OK : PresentFaceFrameToLvgl(
        base, (uint32_t)base_w * (uint32_t)base_h * 2U, base_w, base_h, true, false);
    if (base_err != ESP_OK) {
        if (core_selected) {
            FaceSpeechCore_Deactivate();
            return false;
        }
        // Speaking is intentionally allowed to reject a second fullscreen base
        // transaction. Keep the already-present emotion hold and still arm the
        // small mouth layer; dropping the whole layer freezes long TTS replies.
        ESP_LOGW(TAG, "s1ee mouth_base_defer emo=%s base_present=%s (arm_on_hold)",
                 emotion_name, esp_err_to_name(base_err));
        // Strong-emotion animation tails are not geometrically identical to
        // the layered pack's canonical base. Pasting its mouth onto that tail
        // creates an obvious split face. Neutral/happy-like holds are the only
        // measured-compatible fallback until manifest v2 carries a base hash.
        if (!IsLifeLayerEmotion(emotion_name)) {
            ESP_LOGW(TAG, "s1eg mouth_gen_reject emo=%s base_present=%s", emotion_name,
                     esp_err_to_name(base_err));
            return false;
        }
    }
    if (core_selected && !base_already_present) {
        speech_core_full_committed_ = true;
        ESP_LOGW(TAG, "s1gl core_full_commit emo=%s gen=%u count=1",
                 emotion_name, static_cast<unsigned>(speech_core_generation_));
    }
    current_emotion_name_ = emotion_video_player_canonicalize_emotion(emotion_name);
    mouth_follow_ = true;
    mouth_pose_ = 0;
    mouth_visual_level_ = 0;
    mouth_budget_phase_ = 0;
    mouth_budget_logged_level_ = 0xff;
    life_budget_deferred_ = false;
    life_target_pose_ = 0;
    life_blend_stage_ = 0;
    life_due_us_ = esp_timer_get_time() + kLifeFirstDueUs;
    pose_blink_stage_ = 0;
    pose_blink_will_swap_ = false;
    eye_blink_due_us_ = esp_timer_get_time() + 1800 * 1000LL;
    pose_switch_due_us_ = esp_timer_get_time() + 3600 * 1000LL;
    face_anim_emo_ = emotion_name;
    ESP_LOGW(TAG, "s1cy mouth_arm emo=%s poses=%u", emotion_name, (unsigned)FaceAsset_PoseCount());
    esp_rom_printf("!!FACE_S1CY pose_arm emo=%s poses=%u\n", emotion_name,
                   (unsigned)FaceAsset_PoseCount());
    esp_timer_stop(face_anim_timer_);
    esp_timer_start_once(face_anim_timer_, kMouthFollowTickUs);
    return true;
}

void EezuiDisplayAdapter::MouthFollowTick() {
    if (!mouth_follow_) {
        return;
    }
    if (!FaceMouth_Lock(100)) {
        ESP_LOGW(TAG, "s1ew mouth_tick_drop patch_mutex");
        if (face_anim_timer_ != nullptr) {
            esp_timer_start_once(face_anim_timer_, kMouthFollowTickUs);
        }
        return;
    }
    struct MouthUnlock { ~MouthUnlock() { FaceMouth_Unlock(); } } mouth_unlock;
    const auto st = Application::GetInstance().GetDeviceState();
    if (st != kDeviceStateSpeaking && st != kDeviceStateListening) {
        StopMouthFollow("leave_speak");
        return;
    }
    if (!FaceAsset_Ready() || video_canvas_ == nullptr) {
        StopMouthFollow("not_ready");
        return;
    }
    const int64_t now = esp_timer_get_time();
    const uint8_t target_level = FaceMouth_Level();
    bool mouth_blit_this_tick = false;
    bool mouth_deferred_by_budget = false;
    const bool budget_apply = FaceRouteV2_VisualBudgetShadowEnabled() &&
                              FaceRouteV2_VisualBudgetMouthApplyEnabled();
    const VisualBudgetLevel budget = VisualBudgetV2_Current();
    if (budget_apply && mouth_budget_logged_level_ != static_cast<uint8_t>(budget)) {
        ESP_LOGI(TAG, "s1fm mouth_budget level=%s apply=1",
                 VisualBudgetV2_LevelName(budget));
        mouth_budget_logged_level_ = static_cast<uint8_t>(budget);
    }
    bool allow_mouth_step = true;
    if (budget_apply && target_level > mouth_visual_level_) {
        if (budget == VisualBudgetLevel::MouthOnly) {
            mouth_budget_phase_ = 0;
        } else if (budget == VisualBudgetLevel::MouthReduced) {
            // First attack stays responsive; later rising steps use every
            // other tick. Targets are latest-value and are never replayed.
            allow_mouth_step = (mouth_budget_phase_++ & 1U) == 0;
        } else {
            allow_mouth_step = false;
            mouth_budget_phase_ = 0;
        }
        mouth_deferred_by_budget = !allow_mouth_step;
    } else if (target_level <= mouth_visual_level_) {
        // Closing always wins so a downgrade cannot strand an open mouth.
        mouth_budget_phase_ = 0;
    }
    if (target_level != mouth_visual_level_ && allow_mouth_step) {
        // Attack is deliberately soft (one pose/tick). Release may close two
        // poses/tick so silence does not leave an open mouth hanging onscreen.
        if (target_level > mouth_visual_level_) {
            mouth_visual_level_++;
        } else {
            const uint8_t delta = mouth_visual_level_ - target_level;
            mouth_visual_level_ -= delta > 1 ? 2 : 1;
        }
        mouth_blit_this_tick = PresentMouthPatch(mouth_visual_level_) == ESP_OK;
    }
    const uint8_t level = mouth_visual_level_;
    const bool life_source_eligible = FaceRouteV2_LifeLayerEnabled() &&
                                      st == kDeviceStateSpeaking &&
                                      FaceAsset_LifeReady() &&
                                      IsLifeLayerEmotion(face_anim_emo_);
    const bool life_budget_apply = FaceRouteV2_VisualBudgetShadowEnabled() &&
                                   FaceRouteV2_VisualBudgetLifeApplyEnabled();
    // M2c: preserve the accepted composite animation while audio has headroom,
    // but spend a thin queue only on the latest mouth target. A partial life
    // track is discarded rather than replayed after the queue recovers.
    const bool life_budget_allows = !life_budget_apply ||
                                    budget == VisualBudgetLevel::MouthOnly;
    const bool life_eligible = life_source_eligible && life_budget_allows;
    if (life_source_eligible && !life_budget_allows) {
        if (!life_budget_deferred_) {
            ESP_LOGI(TAG, "s1fn life_budget defer level=%s drop_partial=1",
                     VisualBudgetV2_LevelName(budget));
        }
        life_budget_deferred_ = true;
        life_blend_stage_ = 0;
        life_target_pose_ = 0;
        life_due_us_ = now + 600 * 1000LL;
    } else if (life_budget_deferred_) {
        ESP_LOGI(TAG, "s1fn life_budget resume level=%s latest_only=1",
                 VisualBudgetV2_LevelName(budget));
        life_budget_deferred_ = false;
    }
    if (life_eligible && now >= life_due_us_) {
        // One flush band per worker tick.  A non-overlapping life track may use
        // the next free tick even while speech keeps the mouth open; overlapping
        // tracks still wait for a closed/small mouth to avoid facial tearing.
        const bool overlaps_mouth = FaceAsset_LifeOverlapsMouth(life_target_pose_);
        if (mouth_blit_this_tick || mouth_deferred_by_budget) {
            life_due_us_ = now + kMouthFollowTickUs;
        } else if (overlaps_mouth && level > 1) {
            life_due_us_ = now + 240 * 1000LL;
        } else {
            const uint8_t count = FaceAsset_LifeFrameCount(life_target_pose_);
            const esp_err_t life_err = PresentLifeBand(life_target_pose_, life_blend_stage_);
            if (life_err == ESP_OK) {
                life_blend_stage_++;
                if (life_blend_stage_ >= count) {
                    life_blend_stage_ = 0;
                    if (FaceAsset_LifeTrackCount() > 1) {
                        life_target_pose_ = (uint8_t)((life_target_pose_ + 1) % FaceAsset_LifeTrackCount());
                    }
                    life_due_us_ = now + kLifeHoldUs;
                } else {
                    life_due_us_ = now + 160 * 1000LL;
                }
            } else {
                life_due_us_ = now + 600 * 1000LL;
            }
        }
    } else if (!life_source_eligible) {
        life_blend_stage_ = 0;
        life_target_pose_ = 0;
        life_budget_deferred_ = false;
    }
    if (kBlinkPoseChoreographyEnabled && FaceAsset_PoseCount() > 1) {
        const bool pose_due = st == kDeviceStateSpeaking && level == 0 && now >= pose_switch_due_us_;
        const bool blink_due = now >= eye_blink_due_us_;
        if (pose_blink_stage_ == 0 && (pose_due || blink_due)) {
            pose_blink_will_swap_ = pose_due;
            pose_blink_stage_ = 1;
            PresentEyePatch(1);
            ESP_LOGW(TAG, "s1cz blink begin pose=%u swap=%d state=%d", (unsigned)mouth_pose_,
                     pose_blink_will_swap_ ? 1 : 0, (int)st);
        } else if (pose_blink_stage_ == 1) {
            PresentEyePatch(2);
            pose_blink_stage_ = 2;
        } else if (pose_blink_stage_ == 2) {
            if (pose_blink_will_swap_) {
                const uint8_t next_pose = mouth_pose_ == 0 ? 1 : 0;
                if (PresentPoseBase(next_pose) == ESP_OK) {
                    mouth_pose_ = next_pose;
                    PresentEyePatch(2);
                    PresentMouthPatch(0);
                    ESP_LOGW(TAG, "s1cz pose_swap pose=%u", (unsigned)mouth_pose_);
                    esp_rom_printf("!!FACE_S1CZ pose_swap pose=%u\n", (unsigned)mouth_pose_);
                }
            }
            pose_blink_stage_ = 3;
        } else if (pose_blink_stage_ == 3) {
            PresentEyePatch(1);
            pose_blink_stage_ = 4;
        } else if (pose_blink_stage_ == 4) {
            PresentEyePatch(0);
            PresentMouthPatch(mouth_visual_level_);
            pose_blink_stage_ = 0;
            eye_blink_due_us_ = now + (2400 + mouth_pose_ * 700) * 1000LL;
            if (pose_blink_will_swap_) {
                pose_switch_due_us_ = now + (6500 + mouth_pose_ * 1500) * 1000LL;
            }
            pose_blink_will_swap_ = false;
        }
    }
    if (mouth_follow_ && face_anim_timer_ != nullptr) {
        esp_timer_start_once(face_anim_timer_, kMouthFollowTickUs);
    }
}

bool EezuiDisplayAdapter::ArmEmotionRelease(const char* why) {
    if (emotion_release_active_) return true;
    if (!FaceRouteV2_ReleaseHubEnabled() || !FaceMouth_ReleaseReady()) {
        return false;
    }
    if (strcasecmp(face_anim_emo_.c_str(), "angry") != 0 &&
        strcasecmp(face_anim_emo_.c_str(), "sad") != 0) {
        return false;
    }
    if (!EnsureFaceAnimTimer()) return false;
    const uint8_t count = FaceMouth_ReleaseFrameCount();
    if (count == 0) return false;

    // Transfer ownership of the shared timer from mouth follow to release.
    // Do not clear the bound pack: the release frames live in that generation.
    mouth_follow_ = false;
    mouth_visual_level_ = 0;
    pose_blink_stage_ = 0;
    life_blend_stage_ = 0;
    emotion_release_active_ = true;
    emotion_release_entering_ = false;
    emotion_release_frame_ = 0;
    esp_timer_stop(face_anim_timer_);
    ESP_LOGW(TAG, "s1et release_arm emo=%s frames=%u why=%s", face_anim_emo_.c_str(),
             (unsigned)count, why ? why : "-");
    esp_rom_printf("!!FACE_S1ET release_arm emo=%s frames=%u\n", face_anim_emo_.c_str(),
                   (unsigned)count);
    esp_timer_start_once(face_anim_timer_, 20 * 1000LL);
    return true;
}

bool EezuiDisplayAdapter::ArmEmotionEnter(const char* emotion_name) {
    if (emotion_release_active_ || emotion_name == nullptr ||
        !FaceRouteV2_ReleaseHubEnabled()) return false;
    if (strcasecmp(emotion_name, "angry") != 0 && strcasecmp(emotion_name, "sad") != 0) {
        return false;
    }
    if (!FaceMouth_BindEmotion(emotion_name) || !FaceMouth_EnterReady() ||
        !EnsureFaceAnimTimer()) return false;
    const uint8_t count = FaceMouth_EnterFrameCount();
    if (count == 0) return false;
    mouth_follow_ = false;
    emotion_release_active_ = true;
    emotion_release_entering_ = true;
    emotion_release_frame_ = 0;
    face_anim_emo_ = emotion_name;
    current_emotion_name_ = emotion_name;
    esp_timer_stop(face_anim_timer_);
    ESP_LOGW(TAG, "s1eu enter_arm emo=%s frames=%u", emotion_name, (unsigned)count);
    esp_rom_printf("!!FACE_S1EU enter_arm emo=%s frames=%u\n", emotion_name,
                   (unsigned)count);
    esp_timer_start_once(face_anim_timer_, 20 * 1000LL);
    return true;
}

void EezuiDisplayAdapter::StopEmotionRelease(const char* why) {
    if (!emotion_release_active_) return;
    emotion_release_active_ = false;
    emotion_release_entering_ = false;
    emotion_release_frame_ = 0;
    ESP_LOGW(TAG, "s1et release_stop why=%s", why ? why : "-");
    if (face_anim_timer_ != nullptr && !mouth_follow_ && !idle_breathe_ &&
        face_anim_left_ <= 0 && !face_anim_need_prime_) {
        esp_timer_stop(face_anim_timer_);
    }
}

void EezuiDisplayAdapter::EmotionReleaseTick() {
    if (!emotion_release_active_) return;
    const auto st = Application::GetInstance().GetDeviceState();
    if ((emotion_release_entering_ && st != kDeviceStateSpeaking) ||
        (!emotion_release_entering_ &&
         (st == kDeviceStateSpeaking || st == kDeviceStateConnecting))) {
        const bool was_entering = emotion_release_entering_;
        StopEmotionRelease("state_preempt");
        if (was_entering) PresentStandbyBookend("enter_hub_preempt");
        return;
    }
    const bool entering = emotion_release_entering_;
    const uint8_t count = entering ? FaceMouth_EnterFrameCount() : FaceMouth_ReleaseFrameCount();
    const bool ready = entering ? FaceMouth_EnterReady() : FaceMouth_ReleaseReady();
    if (!ready || emotion_release_frame_ >= count) {
        const std::string emo = face_anim_emo_;
        StopEmotionRelease("done");
        if (entering) {
            ESP_LOGW(TAG, "s1eu enter_done emo=%s exact_hub=1", emo.c_str());
            ArmMouthFollow(emo.c_str(), true);
        } else {
            current_emotion_name_ = "standby";
            face_anim_emo_ = "standby";
            ESP_LOGW(TAG, "s1eu release_done exact_standby=1 no_full_bookend");
            esp_rom_printf("!!FACE_S1EU release_done exact_standby=1\n");
            if (st == kDeviceStateIdle) ArmIdleBreathe("hub_release_done");
        }
        return;
    }
    const uint8_t frame = emotion_release_frame_;
    const esp_err_t err = PresentReleaseBand(frame);
    ESP_LOGW(TAG, "s1eu %s_blit emo=%s frame=%u/%u err=%s",
             entering ? "enter" : "release", face_anim_emo_.c_str(),
             (unsigned)frame, (unsigned)count, esp_err_to_name(err));
    if (err != ESP_OK) {
        StopEmotionRelease("present_fail");
        PresentStandbyBookend(st == kDeviceStateIdle ? "idle_standby" : "hub_fallback");
        return;
    }
    emotion_release_frame_++;
    if (face_anim_timer_ != nullptr) {
        esp_timer_start_once(face_anim_timer_,
                             (int64_t)(entering ? FaceMouth_EnterIntervalMs()
                                               : FaceMouth_ReleaseIntervalMs()) * 1000LL);
    }
}

esp_err_t EezuiDisplayAdapter::PresentReleaseBand(uint8_t frame) {
    const uint8_t* patch = nullptr;
    uint16_t pw = 0, ph = 0;
    int rx = 0, ry = 0;
    const bool got = emotion_release_entering_
                         ? FaceMouth_EnterFrame(frame, &patch, &pw, &ph, &rx, &ry)
                         : FaceMouth_ReleaseFrame(frame, &patch, &pw, &ph, &rx, &ry);
    if (!got ||
        patch == nullptr || video_canvas_ == nullptr || ph > 48) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!SafeLVGLLock(kLvglLockMouthMs)) return ESP_ERR_TIMEOUT;
    const lv_image_dsc_t* img = lv_canvas_get_image(video_canvas_);
    if (img == nullptr || img->data == nullptr) {
        SafeLVGLUnlock();
        return ESP_ERR_INVALID_STATE;
    }
    auto* canvas = const_cast<uint8_t*>(static_cast<const uint8_t*>(img->data));
    const uint32_t cw = (uint32_t)lv_obj_get_width(video_canvas_);
    const uint32_t ch = (uint32_t)lv_obj_get_height(video_canvas_);
    if (rx < 0 || ry < 0 || rx + pw > cw || ry + ph > ch) {
        SafeLVGLUnlock();
        return ESP_ERR_INVALID_SIZE;
    }
    for (uint32_t y = 0; y < ph; ++y) {
        memcpy(canvas + ((size_t)(ry + y) * cw + (size_t)rx) * 2U,
               patch + (size_t)y * pw * 2U, (size_t)pw * 2U);
    }
    if (dialogue_box_ != nullptr) lv_obj_move_foreground(dialogue_box_);
    lv_area_t coords;
    lv_obj_get_coords(video_canvas_, &coords);
    lv_area_t area{coords.x1, coords.y1 + ry, coords.x1 + (int32_t)cw - 1,
                   coords.y1 + ry + ph - 1};
    lv_obj_invalidate_area(video_canvas_, &area);
    if (presenter_ != nullptr) presenter_->FlushCaptionIfDirty();
    SafeLVGLUnlock();
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::PresentIdleFlashBand(uint8_t frame) {
    if (!idle_flash_overlay_.Ready() || frame >= IdleFlashBandOverlay::kFrameCount) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!SafeLVGLLock(0)) {
        return ESP_ERR_TIMEOUT;
    }
    afe_display_fence_t display_fence{};
    AfeFetchGatePrepareDisplayFence(&display_fence);
    const bool shown = idle_flash_overlay_.ShowFrame(frame);
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
    }
    SafeLVGLUnlock();
    if (!shown) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t waited_ms = 0;
    if (!AfeFetchGateWaitDisplayFence(&display_fence, kFlushFenceWaitMs, &waited_ms)) {
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}


esp_err_t EezuiDisplayAdapter::PresentLifeBand(uint8_t track, uint8_t frame,
                                               uint32_t lock_wait_ms) {
    const uint8_t* patch = nullptr;
    const uint8_t* mask = nullptr;
    uint16_t pw = 0, ph = 0;
    int rx = 0, ry = 0;
    if (!FaceAsset_LifeFrame(track, frame, &patch, &mask, &pw, &ph, &rx, &ry) ||
        video_canvas_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!SafeLVGLLock(lock_wait_ms)) {
        return ESP_ERR_TIMEOUT;
    }
    const bool fence_dma = FaceRouteV2_LifeAfeFenceEnabled();
    afe_display_fence_t display_fence{};
    if (fence_dma) {
        AfeFetchGateBeginFaceTxn();
    }
    const lv_image_dsc_t* img = lv_canvas_get_image(video_canvas_);
    if (img == nullptr || img->data == nullptr) {
        SafeLVGLUnlock();
        if (fence_dma) AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_DONE);
        return ESP_ERR_INVALID_STATE;
    }
    auto* canvas = const_cast<uint8_t*>(static_cast<const uint8_t*>(img->data));
    const uint32_t cw = (uint32_t)lv_obj_get_width(video_canvas_);
    const uint32_t ch = (uint32_t)lv_obj_get_height(video_canvas_);
    if (rx < 0 || ry < 0 || rx + pw > cw || ry + ph > ch || ph > 48) {
        SafeLVGLUnlock();
        if (fence_dma) AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_DONE);
        return ESP_ERR_INVALID_SIZE;
    }
    auto blend565 = [](uint16_t dst, uint16_t src, uint16_t alpha) {
        const uint32_t inv = 256U - alpha;
        const uint32_t r = (((dst >> 11) & 0x1fU) * inv + ((src >> 11) & 0x1fU) * alpha) >> 8;
        const uint32_t g = (((dst >> 5) & 0x3fU) * inv + ((src >> 5) & 0x3fU) * alpha) >> 8;
        const uint32_t b = ((dst & 0x1fU) * inv + (src & 0x1fU) * alpha) >> 8;
        return (uint16_t)((r << 11) | (g << 5) | b);
    };
    if (fence_dma) AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_CANVAS_WRITE);
    for (uint32_t y = 0; y < ph; ++y) {
        auto* dst = reinterpret_cast<uint16_t*>(canvas + ((size_t)(ry + y) * cw + rx) * 2U);
        const auto* src = reinterpret_cast<const uint16_t*>(patch + (size_t)y * pw * 2U);
        if (FaceAsset_LifePrecomposited()) {
            memcpy(dst, src, (size_t)pw * 2U);
        } else {
            const auto* a = mask + (size_t)y * pw;
            for (uint32_t x = 0; x < pw; ++x) {
                dst[x] = blend565(dst[x], src[x], (uint16_t)a[x] + (a[x] == 255 ? 1 : 0));
            }
        }
    }
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
    }
    lv_area_t coords;
    lv_obj_get_coords(video_canvas_, &coords);
    lv_area_t area{coords.x1, coords.y1 + ry,
                   coords.x1 + (int32_t)cw - 1, coords.y1 + ry + ph - 1};
    if (fence_dma) {
        AfeFetchGatePrepareDisplayFence(&display_fence);
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_INVALIDATE);
    }
    lv_obj_invalidate_area(video_canvas_, &area);
    if (presenter_ != nullptr) {
        presenter_->FlushCaptionIfDirty();
    }
    if (fence_dma) AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_UNLOCK);
    SafeLVGLUnlock();
    if (fence_dma) {
        uint32_t waited_ms = 0;
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_FENCE_WAIT);
        const bool fence_ok =
            AfeFetchGateWaitDisplayFence(&display_fence, kFlushFenceWaitMs, &waited_ms);
        AfeFetchGateNoteFaceStage(AFE_FACE_STAGE_DONE);
        if (!fence_ok) {
            // Caller releases AfeFetchGate immediately after return.  A timeout
            // drops this visual result; it must never extend audio starvation.
            return ESP_ERR_NOT_FINISHED;
        }
    }
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::PresentMouthPatch(uint8_t level) {
    uint16_t pw = 0, ph = 0;
    const uint8_t* patch = FaceAsset_PosePatch(mouth_pose_, level, &pw, &ph);
    int rx = 0, ry = 0, rw = 0, rh = 0;
    FaceAsset_MouthRoi(&rx, &ry, &rw, &rh);
    if (patch == nullptr || pw == 0 || ph == 0 || video_canvas_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!SafeLVGLLock(kLvglLockMouthMs)) {
        return ESP_ERR_TIMEOUT;
    }
    const lv_image_dsc_t* img = lv_canvas_get_image(video_canvas_);
    if (img == nullptr || img->data == nullptr) {
        SafeLVGLUnlock();
        return ESP_ERR_INVALID_STATE;
    }
    auto* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img->data));
    const uint32_t cw = (uint32_t)lv_obj_get_width(video_canvas_);
    const uint32_t ch = (uint32_t)lv_obj_get_height(video_canvas_);
    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }
    if (rx + (int)pw > (int)cw) {
        pw = (uint16_t)(cw - (uint32_t)rx);
    }
    if (ry + (int)ph > (int)ch) {
        ph = (uint16_t)(ch - (uint32_t)ry);
    }
    // The current PC pack already composites each mouth pose against the same
    // canonical hold base.  Copying it is both deterministic and cheaper than
    // blending against the previous (possibly different) mouth pose.  Keep the
    // legacy four-row blend only for older packs without that manifest contract.
    constexpr uint16_t kFeatherRows = 4;
    auto blend565 = [](uint16_t dst, uint16_t src, uint16_t alpha256) {
        const uint32_t inv = 256U - alpha256;
        const uint32_t r = (((dst >> 11) & 0x1fU) * inv + ((src >> 11) & 0x1fU) * alpha256) >> 8;
        const uint32_t g = (((dst >> 5) & 0x3fU) * inv + ((src >> 5) & 0x3fU) * alpha256) >> 8;
        const uint32_t b = ((dst & 0x1fU) * inv + (src & 0x1fU) * alpha256) >> 8;
        return (uint16_t)((r << 11) | (g << 5) | b);
    };
    for (uint16_t y = 0; y < ph; y++) {
        auto* dst = reinterpret_cast<uint16_t*>(
            canvas_buf + ((size_t)(ry + y) * cw + (size_t)rx) * 2);
        const auto* src = reinterpret_cast<const uint16_t*>(patch + ((size_t)y * pw) * 2);
        const uint16_t edge = std::min<uint16_t>(y, (uint16_t)(ph - 1U - y));
        if (FaceAsset_MouthPrecomposited() || edge >= kFeatherRows) {
            memcpy(dst, src, (size_t)pw * 2);
        } else {
            // Row 0 stays on the current hold; rows 1..3 progressively merge.
            const uint16_t alpha256 = (uint16_t)((uint32_t)edge * 256U / kFeatherRows);
            for (uint16_t x = 0; x < pw; x++) {
                dst[x] = blend565(dst[x], src[x], alpha256);
            }
        }
    }
    if (dialogue_box_ != nullptr) {
        lv_obj_move_foreground(dialogue_box_);
    }
    lv_area_t coords;
    lv_obj_get_coords(video_canvas_, &coords);
    lv_area_t area;
    area.x1 = coords.x1;
    area.y1 = coords.y1 + ry;
    area.x2 = coords.x1 + (int32_t)cw - 1;
    area.y2 = coords.y1 + ry + (int32_t)ph - 1;
    lv_obj_invalidate_area(video_canvas_, &area);
    if (presenter_ != nullptr) {
        presenter_->FlushCaptionIfDirty();
    }
    SafeLVGLUnlock();
    if (FaceRouteV2_ShouldLogFrame()) {
        ESP_LOGW(TAG, "s1el mouth_blit level=%u roi=%d,%d %ux%u compose=%s", (unsigned)level, rx, ry,
                 (unsigned)pw, (unsigned)ph,
                 FaceAsset_MouthPrecomposited() ? "canonical" : "legacy_feather");
    }
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::PresentPoseBase(uint8_t pose) {
    uint16_t w = 0, h = 0;
    const uint8_t* base = FaceAsset_PoseBase(pose, &w, &h);
    if (base == nullptr || w == 0 || h == 0) return ESP_ERR_INVALID_STATE;
    return PresentFaceFrameToLvgl(base, (uint32_t)w * (uint32_t)h * 2U, w, h, true, false);
}

esp_err_t EezuiDisplayAdapter::PresentEyePatch(uint8_t level) {
    uint16_t pw = 0, ph = 0;
    const uint8_t* patch = FaceAsset_EyePatch(mouth_pose_, level, &pw, &ph);
    int rx = 0, ry = 0, rw = 0, rh = 0;
    FaceAsset_EyeRoi(&rx, &ry, &rw, &rh);
    if (patch == nullptr || pw == 0 || ph == 0 || video_canvas_ == nullptr) return ESP_ERR_INVALID_STATE;
    if (!SafeLVGLLock(kLvglLockMouthMs)) return ESP_ERR_TIMEOUT;
    const lv_image_dsc_t* img = lv_canvas_get_image(video_canvas_);
    if (img == nullptr || img->data == nullptr) {
        SafeLVGLUnlock();
        return ESP_ERR_INVALID_STATE;
    }
    auto* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img->data));
    const uint32_t cw = (uint32_t)lv_obj_get_width(video_canvas_);
    const uint32_t ch = (uint32_t)lv_obj_get_height(video_canvas_);
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx + (int)pw > (int)cw) pw = (uint16_t)(cw - (uint32_t)rx);
    if (ry + (int)ph > (int)ch) ph = (uint16_t)(ch - (uint32_t)ry);
    for (uint16_t y = 0; y < ph; y++) {
        memcpy(canvas_buf + ((size_t)(ry + y) * cw + (size_t)rx) * 2,
               patch + ((size_t)y * pw) * 2, (size_t)pw * 2);
    }
    if (dialogue_box_ != nullptr) lv_obj_move_foreground(dialogue_box_);
    lv_area_t coords;
    lv_obj_get_coords(video_canvas_, &coords);
    lv_area_t area{coords.x1, coords.y1 + ry, coords.x1 + (int32_t)cw - 1,
                   coords.y1 + ry + (int32_t)ph - 1};
    lv_obj_invalidate_area(video_canvas_, &area);
    if (presenter_ != nullptr) presenter_->FlushCaptionIfDirty();
    SafeLVGLUnlock();
    return ESP_OK;
}

void EezuiDisplayAdapter::StartFacePanelAnim(const char* emotion_name) {
    if (!emotion_name || emotion_player_ == nullptr || video_canvas_ == nullptr) {
        return;
    }
    StopFacePanelAnim("restart");

    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }

    if (face_anim_timer_ == nullptr) {
        esp_timer_create_args_t args = {
            .callback = &EezuiDisplayAdapter::FacePanelAnimTimerCb,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "face_panim",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &face_anim_timer_) != ESP_OK) {
            face_anim_timer_ = nullptr;
            ESP_LOGW(TAG, "SAD_DIAG short_anim timer_create_fail");
            return;
        }
    }

    face_anim_emo_ = emotion_name;
    // Legacy full path — unused in speak; keep for diagnostics.
    face_anim_left_ = 11;
    face_anim_need_prime_ = true;
    face_anim_roi_only_ = false;
    face_anim_gen_++;

    ShowSdFaceCanvasOnly();
    ESP_LOGW(TAG, "SAD_DIAG short_anim arm emo=%s frames=%d path=lvgl_seed_incr",
             emotion_name, face_anim_left_);

    if (face_anim_timer_ != nullptr) {
        esp_timer_start_once(face_anim_timer_, 1000);
    }
}

esp_err_t EezuiDisplayAdapter::PresentStandbyBookend(const char* tag) {
    if (emotion_player_ == nullptr || video_canvas_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t* rgb = nullptr;
    uint32_t size = 0, w = 0, h = 0;
    const int64_t td0 = esp_timer_get_time();
    // s1cd: bookend = mid-clip **seed** (fc/3) open-eye rest. s1cc used frame0 "contract"
    // but SD clip head is often blink/closed-eye → wake/dialogue looked eyes-shut.
    const auto state = Application::GetInstance().GetDeviceState();
    const bool conversation_core =
        state != kDeviceStateIdle && FaceSpeechCore_Ready("standby") &&
        FaceSpeechCore_Select("standby");
    esp_err_t derr = ESP_FAIL;
    if (conversation_core) {
        uint16_t core_w = 0, core_h = 0;
        rgb = FaceSpeechCore_Base(&core_w, &core_h);
        w = core_w;
        h = core_h;
        size = w * h * 2U;
        derr = rgb ? ESP_OK : ESP_FAIL;
    } else {
        derr = emotion_video_player_get_seed_rgb565(
            emotion_player_, "standby", &rgb, &size, &w, &h);
        if (derr != ESP_OK || rgb == nullptr) {
            derr = emotion_video_player_decode_at_rgb565(
                emotion_player_, "standby",
                emotion_video_player_mid_stride_skip(emotion_player_, "standby"),
                &rgb, &size, &w, &h);
        }
    }
    if (derr != ESP_OK || rgb == nullptr) {
        derr = emotion_video_player_decode_one_rgb565(emotion_player_, "standby", 0, &rgb, &size, &w,
                                                     &h);
    }
    const int decode_ms = (int)((esp_timer_get_time() - td0) / 1000);
    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "FACE_SLICE bookend_fail tag=%s err=%s", tag ? tag : "-",
                 esp_err_to_name(derr));
        return (derr == ESP_OK) ? ESP_FAIL : derr;
    }

    // Set before present so lvgl_face logs say standby (not leftover MID emo).
    current_emotion_name_ = "standby";
    face_anim_emo_ = "standby";

    const bool speaking =
        Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
    const bool has_fb = last_bypass_rgb_ != nullptr;
    // s1ap: present_listen/leave were falling through to full seed (speak=0) → WDT tip.
    // s1as: idle_* with FB → ROI so standby bookend matches dialogue CLOSE rest pose.
    const bool tag_roi =
        tag != nullptr &&
        (strstr(tag, "close") != nullptr || strstr(tag, "interrupt") != nullptr ||
         strstr(tag, "decode_fail") != nullptr || strstr(tag, "present_") != nullptr ||
         strstr(tag, "slice_") != nullptr || strstr(tag, "idle") != nullptr);
    bool use_roi = has_fb && (speaking || tag_roi || InConversationPresent());
    // s1cp-f: bookend always seed_full — band open/close was head seam source.
    if (FaceRouteV2_FullStillEnabled()) {
        use_roi = false;
    }
    const int64_t tp0 = esp_timer_get_time();
    const esp_err_t perr =
        PresentFaceFrameToLvgl(rgb, size, w, h, !use_roi /*seed_full*/, use_roi);
    const int present_ms = (int)((esp_timer_get_time() - tp0) / 1000);

    const char* mode = use_roi ? "close_roi" : "close_seed";
    if (tag != nullptr && strstr(tag, "open") != nullptr) {
        mode = use_roi ? "open_roi" : "open_seed";
    } else if (tag != nullptr && strstr(tag, "idle") != nullptr) {
        mode = use_roi ? "idle_roi" : "idle_seed";
    } else if (tag != nullptr && strstr(tag, "interrupt") != nullptr) {
        mode = use_roi ? "interrupt_roi" : "interrupt_seed";
    } else if (tag != nullptr && strstr(tag, "present_") != nullptr) {
        mode = use_roi ? "present_roi" : "present_seed";
    }
    ESP_LOGW(TAG, "FACE_SLICE %s mode=%s emo=standby present_err=%s seed_rest s1cd",
             tag ? tag : "bookend", mode, esp_err_to_name(perr));
    ESP_LOGW(TAG,
             "FACE_COST emo=standby phase=close decode_ms=%d present_ms=%d roi=%d speak=%d left=0 "
             "s1as",
             decode_ms, present_ms, use_roi ? 1 : 0, speaking ? 1 : 0);
    esp_rom_printf("!!FACE_SLICE %s %s\n", tag ? tag : "bookend", mode);
    // s1as: after idle bookend, start budgeted standby ROI breathe.
    if (perr == ESP_OK && tag != nullptr && strstr(tag, "idle_standby") != nullptr) {
        ArmIdleBreathe("idle_standby");
    }
    return perr;
}

esp_err_t EezuiDisplayAdapter::CommitFaceStillOnce(const char* emotion_name, bool roi_follow) {
    if (!emotion_name || emotion_player_ == nullptr || video_canvas_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    // s1co-e: dialogue StaticHold — no MID 400-row band until layered mouth exists.
    // Idle breathe keeps its own path; only dialogue ROI follow is strangled.
    const auto st0 = Application::GetInstance().GetDeviceState();
    const bool dialogue_ctx = InConversationPresent() || st0 == kDeviceStateSpeaking ||
                              st0 == kDeviceStateListening || st0 == kDeviceStateConnecting;
    bool static_hold = false;
    if (roi_follow && FaceRouteV2_StaticDialogueEnabled() && dialogue_ctx) {
        ESP_LOGW(TAG, "s1co-e static_hold emo=%s (skip MID band)", emotion_name);
        esp_rom_printf("!!FACE_S1CO e=static_hold emo=%s\n", emotion_name);
        roi_follow = false;
        static_hold = true;
    }

    // Same emotion already mid/settle/enter — keep slice, avoid re-OPEN full seed (WDT).
    if ((roi_follow || face_anim_enter_mode_) &&
        (face_anim_left_ > 0 || face_anim_sustain_ || face_anim_await_settle_ ||
         face_anim_need_prime_) &&
        strcasecmp(face_anim_emo_.c_str(), emotion_name) == 0) {
        ESP_LOGW(TAG, "FACE_SLICE keep emo=%s left=%d settle=%d enter=%d s1am", emotion_name,
                 face_anim_left_, face_anim_await_settle_ ? 1 : 0, face_anim_enter_mode_ ? 1 : 0);
        return ESP_OK;
    }

    StopFacePanelAnim("still_only");
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }

    if (strcasecmp(emotion_name, "standby") == 0) {
        const esp_err_t berr = PresentStandbyBookend("open_standby_only");
        if (presenter_ != nullptr) {
            presenter_->OnBypassAnimEnded();
        }
        return berr;
    }

    const bool speaking = (st0 == kDeviceStateSpeaking);
    bool canvas_was_standby =
        strcasecmp(current_emotion_name_.c_str(), "standby") == 0;
    // Skip OPEN full seed when canvas has FB — under TTS full 480 seed contends with I2S.
    const bool already_standby =
        strcasecmp(current_emotion_name_.c_str(), "standby") == 0 && last_bypass_rgb_ != nullptr;
    const bool skip_open = already_standby || (speaking && last_bypass_rgb_ != nullptr);
    if (!skip_open) {
        const esp_err_t open_err = PresentStandbyBookend("open");
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "FACE_SLICE open_fail emo=%s err=%s", emotion_name,
                     esp_err_to_name(open_err));
            return open_err;
        }
        canvas_was_standby = true;
    } else {
        ESP_LOGW(TAG, "FACE_SLICE open_skip emo=%s speak=%d has_fb=%d s1am", emotion_name,
                 speaking ? 1 : 0, last_bypass_rgb_ != nullptr ? 1 : 0);
    }

    face_anim_emo_ = emotion_name;
    current_emotion_name_ = emotion_name;
    ESP_LOGW(TAG, "FACE_SLICE mid_arm emo=%s roi_follow=%d speak=%d s1am", emotion_name,
             roi_follow ? 1 : 0, speaking ? 1 : 0);

    if (roi_follow) {
        // speaking → 700ms settle then ROI @280/450ms (not 150/320 under TTS). The TTS
        // gap widening is a design rule (shrink the step near the I2S edge), unlike the
        // frame-count cut that s1ba retired.
        face_anim_roi_budget_ = kFaceMidFrames;
        face_anim_fast_left_ = speaking ? 3 : 5;
        face_anim_sustain_ = false;
        if (speaking) {
            face_anim_await_settle_ = true;
            face_anim_pending_follow_ = kFaceMidFrames;
            if (!EnsureFaceAnimTimer()) {
                StartFaceRoiFollow(kFaceMidFrames);
                return ESP_OK;
            }
            esp_timer_stop(face_anim_timer_);
            ESP_LOGW(TAG, "FACE_SPEAK settle_arm emo=%s ms=700 fast=%d s1am", emotion_name,
                     face_anim_fast_left_);
            esp_rom_printf("!!FACE_SPEAK settle s1am\n");
            esp_timer_start_once(face_anim_timer_, kSpeakSettleUs);
        } else {
            StartFaceRoiFollow(kFaceMidFrames);
        }
    } else if (static_hold) {
        // Speech Core owns preloaded layered emotions. Never fall through to
        // native MJPEG seed/decode while speaking: a failed/late request keeps
        // the current safe hold and remains latest-only pending.
        if (speaking && FaceSpeechCore_Ready(emotion_name)) {
            const bool armed = ArmMouthFollow(emotion_name, false);
            if (presenter_ != nullptr) presenter_->OnBypassAnimEnded();
            if (armed &&
                (strcasecmp(emotion_name, "angry") == 0 ||
                 strcasecmp(emotion_name, "sad") == 0)) {
                strong_emotion_hold_until_us_ = esp_timer_get_time() + 1200 * 1000LL;
            }
            return ESP_OK;
        }
        // s1eu: strong emotions with a common standby hub grow only their
        // expression islands. Never present the globally shifted native seed.
        if (speaking && canvas_was_standby && ArmEmotionEnter(emotion_name)) {
            ESP_LOGW(TAG, "s1eu static_hold_hub emo=%s native_full_skipped=1", emotion_name);
            if (presenter_ != nullptr) presenter_->OnBypassAnimEnded();
            return ESP_OK;
        }
        // s1cp-g: short uniform full-frame enter, then hold seed (dialogue only).
        if (FaceRouteV2_EnterArcEnabled()) {
            StartFaceEnterTransition(emotion_name);
            return ESP_OK;
        }
        // Paint this emotion's seed still — do NOT fall through to standby bookend.
        const uint8_t* rgb = nullptr;
        uint32_t size = 0, w = 0, h = 0;
        esp_err_t derr =
            emotion_video_player_get_seed_rgb565(emotion_player_, emotion_name, &rgb, &size, &w, &h);
        if (derr != ESP_OK || rgb == nullptr) {
            derr = emotion_video_player_decode_at_rgb565(
                emotion_player_, emotion_name,
                emotion_video_player_mid_stride_skip(emotion_player_, emotion_name), &rgb, &size, &w,
                &h);
        }
        if (derr != ESP_OK || rgb == nullptr) {
            ESP_LOGW(TAG, "s1co-e static_hold_fail emo=%s err=%s", emotion_name,
                     esp_err_to_name(derr));
            return (derr == ESP_OK) ? ESP_FAIL : derr;
        }
        const bool has_fb = last_bypass_rgb_ != nullptr;
        const bool full = FaceRouteV2_FullStillEnabled() || !has_fb;
        const esp_err_t perr = PresentFaceFrameToLvgl(rgb, size, w, h, full, !full && has_fb);
        ESP_LOGW(TAG, "s1co-e static_hold_done emo=%s full=%d err=%s", emotion_name, full ? 1 : 0,
                 esp_err_to_name(perr));
        if (presenter_ != nullptr) {
            presenter_->OnBypassAnimEnded();
        }
        ArmMouthFollow(emotion_name);
        if (perr == ESP_OK &&
            (strcasecmp(emotion_name, "angry") == 0 || strcasecmp(emotion_name, "sad") == 0)) {
            // Cloud may answer an expression-only request with an emotion JSON and
            // immediate tts:stop (zero audio). Start the minimum dwell only after
            // the canonical layer is fully committed, so Listening cannot erase it
            // before a human can perceive it.
            strong_emotion_hold_until_us_ = esp_timer_get_time() + 1200 * 1000LL;
        }
        return perr;
    } else if (presenter_ != nullptr) {
        PresentStandbyBookend("close_no_mid");
        presenter_->OnBypassAnimEnded();
    }
    return ESP_OK;
}

esp_err_t EezuiDisplayAdapter::PaintSeedEmotionPartial(const char* emotion_name) {
    // S1v: conversation face = one LVGL still (React-style state commit).
    // Multi-frame burst during TTS caused HP_SYS_HP_WDT (s1u log @ happy short_anim).
    if (video_canvas_ == nullptr || emotion_player_ == nullptr || !emotion_name) {
        ESP_LOGW(TAG, "SAD_DIAG short_anim abort=bad_state emo=%s",
                 emotion_name ? emotion_name : "-");
        return ESP_ERR_INVALID_STATE;
    }
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }

    const auto st = Application::GetInstance().GetDeviceState();
    const bool wake_edge = (st == kDeviceStateConnecting || st == kDeviceStateListening);
    if (wake_edge) {
        StopFacePanelAnim("wake_edge");
        pending_face_emo_ = emotion_name;
        speech_core_pending_generation_ = 0;
        ESP_LOGW(TAG, "SAD_DIAG face_still defer+pending emo=%s state=%d", emotion_name,
                 (int)st);
        return ESP_OK;
    }

    pending_face_emo_.clear();
    // L2/s1bj leaf: under NotifyEmotion→PresenterPlayEmotion→PlayMjpeg→here.
    // Do not re-enter SetEmotion/NotifyEmotion (recursion).
    ESP_LOGW(TAG, "SAD_DIAG L2 host_commit emo=%s via=leaf s1bj", emotion_name);
    return CommitFaceStillOnce(emotion_name);
}

esp_err_t EezuiDisplayAdapter::ShowEmotionViaBypass(const char* emotion_name) {
    // S1d: product path must not panel-paint while wake/conversation is live.
    // Kept for rare idle diagnostics only; PlayMjpegEmotion no longer calls this when paused.
    if (panel_ == nullptr || emotion_player_ == nullptr || !emotion_name) {
        return ESP_ERR_INVALID_STATE;
    }
    const auto st = Application::GetInstance().GetDeviceState();
    if (InConversationPresent() || st == kDeviceStateListening || st == kDeviceStateSpeaking ||
        st == kDeviceStateConnecting || emotion_video_player_is_decode_paused(emotion_player_)) {
        ESP_LOGW(TAG, "CTRL BYPASS seed defer emo=%s (ShowEmotionViaBypass blocked)", emotion_name);
        return ESP_OK;
    }
    if (bypass_lvgl_stopped_ || panel_owner_ == PanelOwner::kEmotion) {
        ReleasePanelToLvgl(false);
    }
    // Ensure canvas visible — S1c HideVideoCanvas left 常驻 main_image tearing through.
    if (video_canvas_ != nullptr && SafeLVGLLock(kLvglLockQuickMs)) {
        ShowFaceCanvasLayers();
        SafeLVGLUnlock();
    }
    ESP_LOGW(TAG, "CTRL BYPASS idle noop emo=%s (use MJPEG play path)", emotion_name);
    return ESP_OK;
}

void EezuiDisplayAdapter::RunI2EmotionBypassOnce() {
    static bool s_done = false;
    if (s_done) {
        return;
    }
    s_done = true;

    if (panel_ == nullptr || emotion_player_ == nullptr) {
        ESP_LOGW(TAG, "CTRL BYPASS I2 skip: no panel/player");
        return;
    }

    const uint8_t* rgb = nullptr;
    uint32_t size = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    // Mid-clip frame is usually more expressive than frame0.
    const char* emo = "angry";
    constexpr uint32_t kSkip = 8;
    esp_err_t derr = emotion_video_player_decode_one_rgb565(
        emotion_player_, emo, kSkip, &rgb, &size, &w, &h);
    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "CTRL BYPASS I2 decode fail err=%s — try happy",
                 esp_err_to_name(derr));
        emo = "happy";
        derr = emotion_video_player_decode_one_rgb565(
            emotion_player_, emo, kSkip, &rgb, &size, &w, &h);
    }
    if (derr != ESP_OK || rgb == nullptr || w == 0 || h == 0) {
        ESP_LOGW(TAG, "CTRL BYPASS I2 abort decode err=%s", esp_err_to_name(derr));
        return;
    }

    // Cue flash (green) so the bypass window is unmistakable, then hold emotion longer.
    const size_t px = (size_t)width_ * (size_t)height_;
    auto* cue = static_cast<uint16_t*>(
        heap_caps_malloc(px * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (cue) {
        constexpr uint16_t kGreen = 0x07E0;
        for (size_t i = 0; i < px; i++) {
            cue[i] = kGreen;
        }
    }

    ESP_LOGW(TAG, "CTRL BYPASS I2 begin emo=%s %ux%u stop_lvgl", emo, (unsigned)w, (unsigned)h);
    lvgl_port_stop();
    if (cue) {
        DirectPanelBlit(0, 0, width_, height_, cue);
        ESP_LOGW(TAG, "CTRL BYPASS I2 cue green hold_ms=500");
        vTaskDelay(pdMS_TO_TICKS(500));
        heap_caps_free(cue);
    }
    const int blit_w = (int)((w > (uint32_t)width_) ? (uint32_t)width_ : w);
    const int blit_h = (int)((h > (uint32_t)height_) ? (uint32_t)height_ : h);
    const esp_err_t berr =
        DirectPanelBlit(0, 0, blit_w, blit_h, reinterpret_cast<const uint16_t*>(rgb));
    if (berr == ESP_OK) {
        ESP_LOGW(TAG, "CTRL BYPASS I2 blit ok emo=%s hold_ms=6000", emo);
        vTaskDelay(pdMS_TO_TICKS(6000));
    } else {
        ESP_LOGW(TAG, "CTRL BYPASS I2 blit fail err=%s", esp_err_to_name(berr));
    }
    lvgl_port_resume();
    RestoreUiAfterDirectBlit();
    ESP_LOGW(TAG, "CTRL BYPASS I2 end");
}

void EezuiDisplayAdapter::DeinitEmotionSystem() {
    if (!IsEmotionSystemReady()) {
        return;
    }


    // 停止播放
    StopEmotionPlayback();
    
    // 清理画布
    if (video_canvas_) {
        if (!SafeLVGLLock(kLvglLockTickMs)) {
            ESP_LOGW(TAG, "⚠️ 无法获取LVGL锁，跳过画布清理");
        } else {
            idle_flash_overlay_.Detach();
            lv_obj_del(video_canvas_);
            video_canvas_ = nullptr;
            SafeLVGLUnlock();
        }
    }
    
    // 清理播放器
    if (emotion_player_) {
        emotion_video_player_deinit(emotion_player_);
        emotion_player_ = nullptr;
    }

    // 清除表情系统就绪标志
    init_state_ = static_cast<InitState>(static_cast<int>(init_state_) & ~static_cast<int>(InitState::EMOTION_READY));
    current_emotion_name_.clear();

}

// FindMjpegFile函数已移除，映射逻辑统一到emotion_video_player中处理

esp_err_t EezuiDisplayAdapter::PlayMjpegEmotion(const char* emotion_name) {
    if (!IsEmotionSystemReady()) {
        ESP_LOGE(TAG, "表情系统未初始化");
        return ESP_ERR_INVALID_STATE;
    }

    // 处理空字符串和空指针
    if (!emotion_name || strlen(emotion_name) == 0) {
        ESP_LOGW(TAG, "PlayMjpegEmotion收到空表情，使用默认neutral");
        emotion_name = "neutral";
        
    }
    // s1ep-j: recovery/Presenter paths can deliberately enter at this leaf
    // without passing SetEmotion. Keep alias selection identical at the final
    // execution boundary as well; canonical input is unchanged.
    emotion_name = emotion_video_player_canonicalize_emotion(emotion_name);
    
    
    // Pause (wake coexist / conversation): no MJPEG decode.
    if (emotion_video_player_is_decode_paused(emotion_player_)) {
        current_emotion_name_ = emotion_name;
        const auto st = Application::GetInstance().GetDeviceState();
        const bool live = InConversationPresent() || st == kDeviceStateListening ||
                          st == kDeviceStateSpeaking || st == kDeviceStateConnecting;
        ESP_LOGW(TAG,
                 "SAD_DIAG PlayMjpeg paused=1 live=%d present=%d state=%d emo=%s seed_ready=%d "
                 "task=%s",
                 live ? 1 : 0, InConversationPresent() ? 1 : 0, (int)st, emotion_name,
                 emotion_video_player_seed_stills_ready(emotion_player_) ? 1 : 0,
                 pcTaskGetName(nullptr));
        if (live) {
            // S1e experiment: HW partial ROI blit (ST7701 draw_bitmap rect), not full 480².
            return PaintSeedEmotionPartial(emotion_name);
        }
        // Idle + wake pause: if FB never painted (boot/WDT) and seeds ready, LVGL still once.
        // No ROI follow / no panel DirectPanelBlit — avoids wake-coexist HP_WDT.
        const bool seed_ready = emotion_video_player_seed_stills_ready(emotion_player_);
        if (seed_ready && last_bypass_rgb_ == nullptr) {
            // s1bj leaf: already under PresenterPlayEmotion→PlayMjpeg; do NOT NotifyEmotion.
            ESP_LOGW(TAG, "FACE_IDLE_SEED emo=%s via=leaf (empty_fb after pause; s1bj)",
                     emotion_name);
            esp_rom_printf("!!FACE_IDLE_SEED emo=%s\n", emotion_name);
            return CommitFaceStillOnce(emotion_name, false);
        }
        // s1ah: idle standby must paint bookend — keep_fb left last emotion frame (not standby).
        if (strcasecmp(emotion_name, "standby") == 0) {
            ESP_LOGW(TAG, "FACE_IDLE_STANDBY paint bookend s1an (was keep_fb)");
            esp_rom_printf("!!FACE_IDLE_STANDBY s1an\n");
            return PresentStandbyBookend("idle_standby");
        }
        ESP_LOGW(TAG, "CTRL BYPASS seed defer emo=%s live=0 paused=1 (keep_fb)", emotion_name);
        ESP_LOGW(TAG, "SAD_DIAG PlayMjpeg branch=keep_fb emo=%s (no paint)", emotion_name);
        return ESP_OK;
    }

    // 显示视频画布，隐藏背景图片（EezUI特有逻辑）
    if (video_canvas_) {
        if (!SafeLVGLLock(kLvglLockStyleMs)) {  // 使用安全的锁机制
            ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI更新");
            // 继续尝试播放，不因UI锁问题阻止视频播放
        } else {
            ShowFaceCanvasLayers();
            SafeLVGLUnlock();
        }
    }

    esp_err_t ret = emotion_video_player_play_emotion(emotion_player_, emotion_name);
    if (ret == ESP_OK) {
        current_emotion_name_ = emotion_name;
    } else {
        ESP_LOGE(TAG, "表情播放失败: %s", esp_err_to_name(ret));
        // 恢复显示
        if (video_canvas_) {
            if (!SafeLVGLLock(kLvglLockStyleMs)) {
                ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI恢复");
            } else {
                lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
                if (main_image_) {
                    lv_obj_clear_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
                }
                SafeLVGLUnlock();
            }
        }
    }
    
    return ret;
}

esp_err_t EezuiDisplayAdapter::ForceStopAndSwitchEmotion(const char* emotion_name) {
    // s1bj: public switch also goes Presenter → leaf Play/Commit (no direct Play).
    if (presenter_ != nullptr) {
        ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=presenter why=force_switch s1bj",
                 emotion_name ? emotion_name : "-");
        presenter_->NotifyEmotion(emotion_name ? emotion_name : "neutral");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=leaf why=force_switch_no_presenter s1bj",
             emotion_name ? emotion_name : "-");
    return PlayMjpegEmotion(emotion_name);
}

void EezuiDisplayAdapter::StopEmotionPlayback() {
    if (!IsEmotionSystemReady()) {
        return;
    }
    
    
    if (emotion_player_) {
        emotion_video_player_play_emotion(emotion_player_, "standby");
    }
    
    // 恢复显示（EezUI特有逻辑）
    if (video_canvas_) {
        if (!SafeLVGLLock(kLvglLockStyleMs)) {
            ESP_LOGW(TAG, "无法获取LVGL锁，跳过UI恢复");
        } else {
            lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
            
            // 恢复背景图片
            if (main_image_) {
                lv_obj_clear_flag(main_image_, LV_OBJ_FLAG_HIDDEN);
            }
            
            SafeLVGLUnlock();
        }
    }
    
    current_emotion_name_.clear();
}

void EezuiDisplayAdapter::CreateVideoCanvas() {
    if (video_canvas_ != nullptr) {
        return;
    }
    lv_obj_t* parent_screen = nullptr;
    if (objects.main != nullptr) {
        parent_screen = objects.main;
    } else if (main_image_ && lv_obj_get_parent(main_image_)) {
        parent_screen = lv_obj_get_parent(main_image_);
    } else {
        parent_screen = lv_screen_active();
    }
    if (!parent_screen) {
        ESP_LOGE(TAG, "no parent");
        return;
    }
    if (!SafeLVGLLock(kLvglLockStyleMs)) {
        ESP_LOGE(TAG, "lvgl lock fail");
        return;
    }
    video_canvas_ = lv_canvas_create(parent_screen);
    if (!video_canvas_) {
        ESP_LOGE(TAG, "canvas create fail");
        SafeLVGLUnlock();
        return;
    }
    const uint32_t canvas_width = 480;
    const uint32_t canvas_height = 480;
    const uint32_t buf_size = canvas_width * canvas_height * 2;
    void* canvas_buf = heap_caps_aligned_calloc(64, 1, buf_size, MALLOC_CAP_SPIRAM);
    if (!canvas_buf) {
        canvas_buf = heap_caps_aligned_calloc(4, 1, buf_size, MALLOC_CAP_8BIT);
        if (!canvas_buf) {
            canvas_buf = malloc(buf_size);
        }
    }
    if (!canvas_buf) {
        ESP_LOGE(TAG, "canvas buf alloc fail (%u)", (unsigned)buf_size);
        lv_obj_del(video_canvas_);
        video_canvas_ = nullptr;
        SafeLVGLUnlock();
        return;
    }
    lv_canvas_set_buffer(video_canvas_, canvas_buf, canvas_width, canvas_height, LV_COLOR_FORMAT_RGB565);
    if (FaceRouteV2_IdleFlashOverlayEnabled()) {
        (void)idle_flash_overlay_.Initialize(video_canvas_);
    }
    lv_obj_add_flag(video_canvas_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(video_canvas_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(video_canvas_, lv_color_black(), 0);
    lv_obj_set_style_border_width(video_canvas_, 0, 0);
    lv_obj_set_style_pad_all(video_canvas_, 0, 0);
    lv_obj_set_size(video_canvas_, canvas_width, canvas_height);
    lv_obj_set_pos(video_canvas_, 0, 0);
    lv_obj_set_user_data(video_canvas_, canvas_buf);
    lv_obj_add_event_cb(video_canvas_, [](lv_event_t* e) {
        lv_obj_t* target = lv_event_get_target_obj(e);
        void* canvas_buf = lv_obj_get_user_data(target);
        if (canvas_buf) {
            heap_caps_free(canvas_buf);
        }
    }, LV_EVENT_DELETE, nullptr);
    SafeLVGLUnlock();
    EnsureUILayerOrder();
}

void EezuiDisplayAdapter::EnsureUILayerOrder() {
    if (!SafeLVGLLock(kLvglLockQuickMs)) {
        return;
    }
    if (dialogue_box_) {
        lv_obj_move_foreground(dialogue_box_);
        lv_obj_clear_flag(dialogue_box_, LV_OBJ_FLAG_HIDDEN);
    }
    if (battery_panel_) {
        lv_obj_move_foreground(battery_panel_);
    }
    SafeLVGLUnlock();
}

void EezuiDisplayAdapter::EmotionVideoFrameCallback(emotion_video_handle_t handle, uint8_t *frame_data,
                                                    uint32_t frame_size, uint32_t width, uint32_t height, void *user_data) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(user_data);
    if (!adapter || !adapter->IsEmotionSystemReady()) {
        return;
    }
    if (!adapter->video_canvas_ || !frame_data || frame_size == 0) {
        return;
    }
    (void)handle;
    if (WdtContendWindowActive()) {
        WdtContendBreadcrumb("cb_enter");
    }
    int64_t wait_begin_us = esp_timer_get_time();
    if (!adapter->SafeLVGLLock(kLvglLockCbMs)) {
        uint32_t wait_ms = (uint32_t)((esp_timer_get_time() - wait_begin_us) / 1000);
        WdtContendNoteLvglBusy(wait_ms, g_lvgl_lock_holder_name);
        return;
    }
    uint32_t wait_ms = (uint32_t)((esp_timer_get_time() - wait_begin_us) / 1000);
    int64_t t_get0 = esp_timer_get_time();
    const lv_image_dsc_t* img_dsc = lv_canvas_get_image(adapter->video_canvas_);
    if (!img_dsc || !img_dsc->data) {
        adapter->SafeLVGLUnlock();
        return;
    }
    uint8_t* canvas_buf = const_cast<uint8_t*>(static_cast<const uint8_t*>(img_dsc->data));
    lv_coord_t canvas_width = lv_obj_get_width(adapter->video_canvas_);
    lv_coord_t canvas_height = lv_obj_get_height(adapter->video_canvas_);
    uint32_t copy_width = (width < (uint32_t)canvas_width) ? width : (uint32_t)canvas_width;
    uint32_t copy_height = (height < (uint32_t)canvas_height) ? height : (uint32_t)canvas_height;
    uint32_t expected_size = copy_width * copy_height * 2;
    uint32_t getbuf_ms = (uint32_t)((esp_timer_get_time() - t_get0) / 1000);
    if (frame_size < expected_size) {
        adapter->SafeLVGLUnlock();
        return;
    }

    int64_t t_memcpy0 = esp_timer_get_time();
    if (copy_width == (uint32_t)canvas_width && copy_height == (uint32_t)canvas_height) {
        memcpy(canvas_buf, frame_data, expected_size);
    } else {
        uint8_t* dst_ptr = canvas_buf;
        uint8_t* src_ptr = frame_data;
        const uint32_t dst_stride = canvas_width * 2;
        const uint32_t src_stride = copy_width * 2;
        for (uint32_t y = 0; y < copy_height; y++) {
            memcpy(dst_ptr, src_ptr, src_stride);
            dst_ptr += dst_stride;
            src_ptr += src_stride;
        }
    }
    uint32_t canvas_memcpy_ms = (uint32_t)((esp_timer_get_time() - t_memcpy0) / 1000);
    if (WdtContendWindowActive()) {
        WdtContendBreadcrumb("cb_after_canvas_memcpy");
    }

    int64_t t_inv0 = esp_timer_get_time();
    lv_obj_invalidate(adapter->video_canvas_);
    if (adapter->dialogue_box_) {
        lv_obj_move_foreground(adapter->dialogue_box_);
    }
    uint32_t invalidate_ms = (uint32_t)((esp_timer_get_time() - t_inv0) / 1000);
    if (WdtContendWindowActive()) {
        WdtContendBreadcrumb("cb_after_invalidate");
    }
    const uint32_t flush_n_before = WdtContendFlushCount();
    if (WdtContendWindowActive()) {
        WdtContendBreadcrumb("cb_before_unlock");
    }
    int64_t t_un0 = esp_timer_get_time();
    adapter->SafeLVGLUnlock();
    uint32_t unlock_ms = (uint32_t)((esp_timer_get_time() - t_un0) / 1000);
    if (WdtContendWindowActive()) {
        WdtContendBreadcrumb("cb_after_unlock");
        WdtContendNoteUnlock(unlock_ms, flush_n_before);
    }
    WdtContendNoteCbPhases(wait_ms, getbuf_ms, canvas_memcpy_ms, invalidate_ms, unlock_ms, expected_size);
    uint32_t hold_ms = wait_ms + getbuf_ms + canvas_memcpy_ms + invalidate_ms + unlock_ms;
    WdtContendNoteLvglHold(hold_ms, expected_size, pcTaskGetName(NULL));
    if (WdtContendWindowActive()) {
        WdtContendBreadcrumb("cb_exit");
    }
}

void EezuiDisplayAdapter::EmotionVideoEventCallback(emotion_video_event_t event, void *user_data) {
    EezuiDisplayAdapter* adapter = static_cast<EezuiDisplayAdapter*>(user_data);
    if (!adapter) {
        return;
    }
    
    // 处理视频加载和播放事件
    switch (event) {
        case EMOTION_VIDEO_EVENT_LOADING_START:
            // 显示"正在加载视频资源"提示
            if (adapter->dialogue_box_) {
                if (adapter->SafeLVGLLock(kLvglLockUiMs)) {
                    adapter->ApplyDialogueCaption("正在加载视频资源", false);
                    adapter->SafeLVGLUnlock();
                }
            }
            break;
            
        case EMOTION_VIDEO_EVENT_LOADING_END:
            // 隐藏"正在加载视频资源"提示（除非是持久化消息）
            if (adapter->dialogue_box_ && !adapter->is_persistent_message_) {
                if (adapter->SafeLVGLLock(kLvglLockUiMs)) {
                    lv_obj_add_flag(adapter->dialogue_box_, LV_OBJ_FLAG_HIDDEN);
                    adapter->SafeLVGLUnlock();
                }
            }
            adapter->EnsureUILayerOrder();
            break;
            
        case EMOTION_VIDEO_EVENT_PLAY_START:
            adapter->EnsureUILayerOrder();
            break;
            
        case EMOTION_VIDEO_EVENT_PLAY_END:
            // 所有表情都常驻内存，无需异步加载
            adapter->EnsureUILayerOrder();
            break;
            
        // EMOTION_VIDEO_EVENT_RELEASE_CACHE 已被移除，不再需要处理
            
        default:
            ESP_LOGW(TAG, "未知视频事件: %d", event);
            break;
    }
}

bool EezuiDisplayAdapter::InConversationPresent() const {
    return presenter_ != nullptr && presenter_->IsConversationMode();
}

void EezuiDisplayAdapter::PresenterPlayEmotion(const char* emotion_name) {
    // Leaf under NotifyEmotion: PlayMjpeg → paused? CommitFaceStillOnce / time-slice.
    ESP_LOGW(TAG, "CTRL PRESENT commit emo=%s via=presenter s1bj",
             emotion_name ? emotion_name : "-");
    ESP_LOGW(TAG, "SAD_DIAG L2 PresenterPlayEmotion emo=%s s1bj",
             emotion_name ? emotion_name : "-");
    esp_err_t ret = PlayMjpegEmotion(emotion_name);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "CTRL PRESENT play fail emo=%s err=%s",
                 emotion_name ? emotion_name : "-", esp_err_to_name(ret));
        if (emotion_name && strcmp(emotion_name, "neutral") != 0) {
            PlayMjpegEmotion("neutral");
        }
    }
}

void EezuiDisplayAdapter::PresenterApplyDialogue(const char* role, const char* content) {
    // Leaf caption paint (called from Presenter NotifyDialogue / caption_live).
    if (dialogue_box_ == nullptr || content == nullptr) {
        return;
    }
    if (strlen(content) == 0 || strspn(content, " \t\n\r") == strlen(content)) {
        return;
    }

    DisplayLockGuard lock(this);
    PresenterApplyDialogueAssumingLock(role, content);
}

void EezuiDisplayAdapter::PresenterApplyDialogueAssumingLock(const char* role,
                                                             const char* content) {
    // s1bk: caller holds SafeLVGLLock — must not take DisplayLockGuard/lvgl_port_lock again.
    if (dialogue_box_ == nullptr || content == nullptr) {
        return;
    }
    if (strlen(content) == 0 || strspn(content, " \t\n\r") == strlen(content)) {
        return;
    }

    if (role != nullptr &&
        (strcmp(role, "alarm_note") == 0 || strcmp(role, "system_persistent") == 0)) {
        is_persistent_message_ = true;
    } else if (role != nullptr && strcmp(role, "lyrics") != 0) {
        is_persistent_message_ = false;
    }

    const char* important_keywords[] = {"配网模式", "热点", "192.168.4.1", "验证码", "输入", "请",
                                        "code", "Code", "验证", "密码", "连接", "xiaozhi.me"};
    bool is_important = false;
    for (size_t i = 0; i < sizeof(important_keywords) / sizeof(important_keywords[0]); i++) {
        if (strstr(content, important_keywords[i])) {
            is_important = true;
            break;
        }
    }
    if ((is_important || is_persistent_message_) && hide_timer_) {
        CancelIdleHideTimer();
    }

    const bool use_tw = (role != nullptr && strcmp(role, "assistant") == 0);
    ApplyDialogueCaption(content, use_tw);
}

esp_err_t EezuiDisplayAdapter::PresenterSeedFaceToScreen() {
    // C2c: do not blit/resume on enter — that either stalls AFE (LVGL flush) or
    // holds the panel and freezes captions. First live SetEmotion seed-blit holds.
    ESP_LOGW(TAG, "CTRL PRESENT seed skip=enter_keep_fb");
    return ESP_OK;
}

bool EezuiDisplayAdapter::PresenterLastFace(const uint8_t** rgb, uint32_t* size, uint32_t* w,
                                             uint32_t* h) const {
    if (!last_bypass_rgb_ || last_bypass_size_ == 0 || last_bypass_w_ == 0 ||
        last_bypass_h_ == 0) {
        return false;
    }
    if (rgb) {
        *rgb = last_bypass_rgb_;
    }
    if (size) {
        *size = last_bypass_size_;
    }
    if (w) {
        *w = last_bypass_w_;
    }
    if (h) {
        *h = last_bypass_h_;
    }
    return true;
}

void EezuiDisplayAdapter::PresenterAcquirePanel() {
    AcquirePanelForEmotion();
}

void EezuiDisplayAdapter::PresenterReleasePanel(bool restore_ui) {
    ReleasePanelToLvgl(restore_ui);
}

bool EezuiDisplayAdapter::PresenterSafeLVGLLock(int timeout_ms) {
    return SafeLVGLLock(timeout_ms);
}

void EezuiDisplayAdapter::PresenterSafeLVGLUnlock() {
    SafeLVGLUnlock();
}
